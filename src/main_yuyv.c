/*
 * V4L2 YUYV 实时采集 —— 管道输出到 ffplay
 *
 * 功能：循环采集 YUYV 帧，输出到 stdout，管道接 ffplay 实时显示
 *
 * 编译：  gcc -o capture_yuyv main_yuyv.c
 * 运行：  ./capture_yuyv | ffplay -f rawvideo -pixel_format yuyv422 -video_size 640x480 -framerate 20 -i pipe:0
 * 退出：  Ctrl+C 或关闭 ffplay 窗口
 *
 * 与 main.c 的区别：
 *   像素格式从 MJPG 改为 YUYV
 *   YUYV 是原始像素数据，不需要 JPEG 解码，不存在"缺头"花屏问题
 *   代价是数据量大（640x480×2 = 614KB/帧），帧率从 30fps 降到 20fps
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

/* ====== 配置 ====== */

#define WIDTH   640
#define HEIGHT  480
#define DEVICE  "/dev/video0"
#define N_BUFS  4
#define TIMEOUT_SEC  2

struct buffer {
    void   *ptr;
    size_t  length;
};

static volatile int keep_running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    keep_running = 0;
}

/* ====== 工具函数 ====== */

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

/* ====== 主函数 ====== */

int main(void)
{
    signal(SIGINT, sigint_handler);

    int fd = open(DEVICE, O_RDWR);
    if (fd < 0) die("open");

    /* QUERYCAP */
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) die("VIDIOC_QUERYCAP");
    fprintf(stderr, "[init] %s | %s\n", cap.driver, cap.card);

    /* S_FMT: YUYV 640x480 */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = WIDTH;
    fmt.fmt.pix.height      = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) die("VIDIOC_S_FMT");
    fprintf(stderr, "[init] %dx%d YUYV, %u bytes/frame\n",
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

    /* 丢前 2 帧预热 */
    for (int skip = 0; skip < 2; skip++) {
        if (wait_for_data(fd, TIMEOUT_SEC) <= 0) continue;
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) break;
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    /* 主采集循环 */
    fprintf(stderr, "[run] 开始采集，Ctrl+C 停止\n");
    long frame_count = 0;
    long drop_count  = 0;

    while (keep_running) {
        int ret = wait_for_data(fd, TIMEOUT_SEC);
        if (ret < 0) {
            if (errno == EINTR) break;
            die("select");
        }
        if (ret == 0) {
            drop_count++;
            continue;
        }

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EINTR) break;
            perror("VIDIOC_DQBUF");
            drop_count++;
            continue;
        }

        /*
         * YUYV 是原始像素数据，直接写到 stdout
         * ffplay -f rawvideo 会按指定的像素格式和分辨率读取
         */
        fwrite(buffers[buf.index].ptr, 1, buf.bytesused, stdout);
        fflush(stdout);

        ioctl(fd, VIDIOC_QBUF, &buf);

        frame_count++;
        if (frame_count % 30 == 0)
            fprintf(stderr, "[run] %ld 帧 (丢弃 %ld)\n", frame_count, drop_count);
    }

    /* 清理 */
    fprintf(stderr, "[exit] 共采集 %ld 帧, 丢弃 %ld 帧\n", frame_count, drop_count);
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (unsigned int i = 0; i < n_bufs; i++)
        munmap(buffers[i].ptr, buffers[i].length);
    close(fd);

    return 0;
}
