/*
 * V4L2 MJPG 实时采集 —— 管道输出到 ffplay
 *
 * 功能：循环采集 MJPG 帧，输出到 stdout，管道接 ffplay 实时显示
 *
 * 编译：  gcc -o capture main.c
 * 运行：  ./capture | ffplay -f mjpeg -i pipe:0
 * 退出：  Ctrl+C 或关闭 ffplay 窗口
 *
 * 数据流：
 *   /dev/video0 → DQBUF → stdout → 管道 → ffplay → 屏幕
 *
 * 为什么写 stdout 而非文件？
 *   stdout 通过管道 | 直接喂给 ffplay 的 stdin，
 *   ffplay 边收边解边显示，实现实时预览。
 *   fflush(stdout) 是关键——不手动刷新的话，
 *   数据会堆在 libc 缓冲区里，ffplay 收不到。
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
#define TIMEOUT_SEC  2    /* 每帧等待超时（短一些，方便 Ctrl+C 响应） */

struct buffer {
    void   *ptr;
    size_t  length;
};

/* 全局标志：Ctrl+C 时设为 0，主循环退出 */
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
    signal(SIGINT, sigint_handler);   /* Ctrl+C 优雅退出 */

    int fd = open(DEVICE, O_RDWR);
    if (fd < 0) die("open");

    /* QUERYCAP */
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) die("VIDIOC_QUERYCAP");
    fprintf(stderr, "[init] %s | %s\n", cap.driver, cap.card);

    /* S_FMT: MJPG 640x480 */
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

    /*
     * 主采集循环
     *
     * 每一帧：
     *   select(等数据) → DQBUF(取出) → fwrite → stdout
     *   → fflush(stdout) [关键!] → QBUF(归还)
     */
    fprintf(stderr, "[run] 开始采集，Ctrl+C 停止\n");
    long frame_count = 0;
    long drop_count  = 0;

    while (keep_running) {
        int ret = wait_for_data(fd, TIMEOUT_SEC);
        if (ret < 0) {
            if (errno == EINTR) break;   /* 被 Ctrl+C 中断 */
            die("select");
        }
        if (ret == 0) {
            drop_count++;
            continue;                    /* 超时 → 跳过，等下一帧 */
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
         * 把这一帧 MJPG 数据写到 stdout
         * ffplay -f mjpeg 会自动识别每一帧的 JPEG 边界
         */
        fwrite(buffers[buf.index].ptr, 1, buf.bytesused, stdout);
        fflush(stdout);   /* ← 这条最关键！不刷新 ffplay 收不到数据 */

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
