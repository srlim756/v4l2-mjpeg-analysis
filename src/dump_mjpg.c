/*
 * V4L2 MJPG 抓帧工具 —— 抓取原始 MJPG 帧保存到文件
 *
 * 功能：抓取前 5 帧 MJPG 数据，保存为 frame_0.jpg ~ frame_4.jpg
 *
 * 编译：  gcc -o dump_mjpg dump_mjpg.c
 * 运行：  ./dump_mjpg
 * 分析：  xxd frame_0.jpg | head -5    （看前几个字节）
 *
 * 目的：确认摄像头输出的 MJPG 数据是否包含完整的 JPEG 头
 *   完整 JPEG：FF D8 FF DB(DQT) FF C0(SOF) FF C4(DHT) FF DA(SOS)
 *   缺头 JPEG：FF D8 直接就是压缩数据
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define WIDTH   640
#define HEIGHT  480
#define DEVICE  "/dev/video0"
#define N_BUFS  4
#define N_FRAMES 5
#define TIMEOUT_SEC  2

struct buffer {
    void   *ptr;
    size_t  length;
};

static void die(const char *msg)
{
    fprintf(stderr, "[错误] %s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

static int wait_for_data(int fd, int timeout_sec)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv;
    tv.tv_sec  = timeout_sec;
    tv.tv_usec = 0;
    return select(fd + 1, &fds, NULL, NULL, &tv);
}

int main(void)
{
    int fd = open(DEVICE, O_RDWR);
    if (fd < 0) die("open");

    /* QUERYCAP */
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) die("VIDIOC_QUERYCAP");
    fprintf(stderr, "[init] %s | %s\n", cap.driver, cap.card);

    /* S_FMT: MJPG */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = WIDTH;
    fmt.fmt.pix.height      = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) die("VIDIOC_S_FMT");
    fprintf(stderr, "[init] %dx%d MJPG, max %u bytes/frame\n",
            fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);

    /* REQBUFS */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = N_BUFS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) die("VIDIOC_REQBUFS");
    unsigned int n_bufs = req.count;

    /* mmap */
    struct buffer buffers[N_BUFS];
    for (unsigned int i = 0; i < n_bufs; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) die("VIDIOC_QUERYBUF");
        buffers[i].length = buf.length;
        buffers[i].ptr = mmap(NULL, buf.length,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].ptr == MAP_FAILED) die("mmap");
    }

    /* QBUF */
    for (unsigned int i = 0; i < n_bufs; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) die("VIDIOC_QBUF");
    }

    /* STREAMON */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) die("VIDIOC_STREAMON");

    /* 抓 N_FRAMES 帧保存到文件 */
    fprintf(stderr, "[run] 开始抓帧...\n");
    int saved = 0;

    while (saved < N_FRAMES) {
        if (wait_for_data(fd, TIMEOUT_SEC) <= 0) {
            fprintf(stderr, "[warn] 超时，跳过\n");
            continue;
        }

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            perror("VIDIOC_DQBUF");
            continue;
        }

        /* 保存到文件 */
        char filename[32];
        snprintf(filename, sizeof(filename), "frame_%d.jpg", saved);
        FILE *fp = fopen(filename, "wb");
        if (fp) {
            fwrite(buffers[buf.index].ptr, 1, buf.bytesused, fp);
            fclose(fp);
            fprintf(stderr, "[save] %s: %u bytes\n", filename, buf.bytesused);

            /* 打印前 16 字节（方便快速查看） */
            unsigned char *p = (unsigned char *)buffers[buf.index].ptr;
            fprintf(stderr, "       头部: ");
            for (int i = 0; i < 16 && i < (int)buf.bytesused; i++)
                fprintf(stderr, "%02X ", p[i]);
            fprintf(stderr, "\n");
        }

        ioctl(fd, VIDIOC_QBUF, &buf);
        saved++;
    }

    /* 清理 */
    fprintf(stderr, "[exit] 共保存 %d 帧\n", saved);
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (unsigned int i = 0; i < n_bufs; i++)
        munmap(buffers[i].ptr, buffers[i].length);
    close(fd);

    return 0;
}
