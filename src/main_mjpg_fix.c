/*
 * V4L2 MJPG 采集 + 帧头补全 v4 —— 增加非法标记扫描
 *
 * 编译：  gcc -o capture_fix main_mjpg_fix.c
 * 运行：  ./capture_fix | ffplay -f mjpeg -i pipe:0
 *
 * v4 改进：
 *   6. 扫描增量帧压缩数据内的非法 JPEG 标记（FF DA/C0/C4/DB/D9）
 *      有非法标记的帧直接丢弃，避免污染解码器状态
 *
 * v3 改进：
 *   1. 强化关键帧检测：SOI 后必须找到 FF C0/DB/C4，且帧大小 >= 50 字节
 *   2. 关键帧 EOI 校验：没有 EOI 的关键帧丢弃，不更新缓存
 *   3. SOS 长度校验：只在长度 == 12（标准 3 分量）时更新缓存
 *   4. 增量帧扫描有效数据：去除末尾填充字节
 *   5. 调试日志：关键帧/增量帧打印前 16 字节
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
#define SOS_LEN_NORMAL  12  /* 标准 3 分量 JPEG 的 SOS 段长度 */

struct buffer {
    void   *ptr;
    size_t  length;
};

static volatile int keep_running = 1;

/* 头部缓存 */
static uint8_t header_cache[4096];   /* SOI + SOF + DQT + DHT（SOS 之前） */
static size_t  header_len = 0;

static uint8_t sos_cache[64];        /* SOS 段头（FF DA + 长度 + 参数） */
static size_t  sos_len = 0;

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

/* 打印前 n 字节（调试用） */
static void print_hex(const uint8_t *data, size_t size, size_t n)
{
    if (n > size) n = size;
    for (size_t i = 0; i < n; i++)
        fprintf(stderr, "%02X ", data[i]);
    fprintf(stderr, "\n");
}

/*
 * 扫描帧数据，查找 EOI (FF D9)
 * 返回 EOI 结束位置（含 FF D9），未找到返回 frame_size
 */
static size_t find_eoi(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i + 1 < size; i++) {
        if (data[i] == 0xFF && data[i + 1] == 0xD9)
            return i + 2;
    }
    return size;  /* 未找到 */
}

/*
 * 强化关键帧检测
 *
 * 条件：
 *   1. 以 SOI (FF D8) 开头
 *   2. 帧大小 >= 50 字节
 *   3. SOI 后 100 字节内找到合法 JPEG 标记（FF C0/DB/C4/DA）
 */
static int is_keyframe(const uint8_t *data, size_t size)
{
    if (size < 50) return 0;
    if (data[0] != 0xFF || data[1] != 0xD8) return 0;

    /* SOI 后扫描前 100 字节，找合法标记 */
    size_t end = (size < 100) ? size : 100;
    for (size_t i = 2; i + 1 < end; i++) {
        if (data[i] == 0xFF) {
            uint8_t m = data[i + 1];
            if (m == 0xC0 || m == 0xC2) return 1;  /* SOF0/SOF2 */
            if (m == 0xDB) return 1;                /* DQT */
            if (m == 0xC4) return 1;                /* DHT */
            if (m == 0xDA) return 1;                /* SOS */
            if (m == 0xD9) return 0;                /* EOI = 无效 */
            if (m == 0x00) continue;                /* 填充字节 */
        }
    }
    return 0;
}

/*
 * 扫描压缩数据内是否包含非法 JPEG 标记
 *
 * 合法的 0xFF 后面跟 0x00（字节填充）
 * 非法的 0xFF 后面跟 DA/C0/C4/D9 等（垃圾数据）
 *
 * 返回值：1 有非法标记，0 正常
 */
static int has_illegal_markers(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i + 1 < size; i++) {
        if (data[i] == 0xFF) {
            uint8_t m = data[i + 1];
            if (m == 0x00) continue;                /* 合法的字节填充 */
            if (m == 0xDA) return 1;                /* 非法 SOS */
            if (m == 0xC0 || m == 0xC2) return 1;   /* 非法 SOF */
            if (m == 0xC4) return 1;                /* 非法 DHT */
            if (m == 0xDB) return 1;                /* 非法 DQT */
            if (m == 0xD9) return 1;                /* 非法 EOI */
            /* 其他标记忽略 */
        }
    }
    return 0;
}

/*
 * 从关键帧中提取 JPEG 头部
 *
 * 拆成两部分缓存：
 *   header_cache = SOI ~ SOS 之前（SOF + DQT + DHT）
 *   sos_cache    = SOS 段本身（FF DA + 长度 + 参数）
 *
 * 返回值：1 成功，0 失败（SOS 长度异常等）
 */
static int extract_header(const uint8_t *data, size_t size)
{
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return 0;

    size_t pos = 2;

    while (pos + 4 < size) {
        if (data[pos] != 0xFF) {
            pos++;
            continue;
        }

        uint8_t marker = data[pos + 1];

        /* SOS 标记：FF DA */
        if (marker == 0xDA) {
            size_t seg_len = (data[pos + 2] << 8) | data[pos + 3];

            /* SOS 长度校验：标准 3 分量 JPEG 应为 12 */
            if (seg_len != SOS_LEN_NORMAL) {
                fprintf(stderr, "[warn] SOS 长度异常: %zu (期望 %d)，跳过此帧\n",
                        seg_len, SOS_LEN_NORMAL);
                return 0;
            }

            size_t sos_end = pos + 2 + seg_len;
            if (sos_end > size) sos_end = size;

            /* 缓存 SOS 段 */
            sos_len = sos_end - pos;
            if (sos_len > sizeof(sos_cache)) sos_len = sizeof(sos_cache);
            memcpy(sos_cache, data + pos, sos_len);

            /* 缓存 SOS 之前的头部（SOI + SOF + DQT + DHT） */
            header_len = pos;
            if (header_len > sizeof(header_cache)) header_len = sizeof(header_cache);
            memcpy(header_cache, data, header_len);

            fprintf(stderr, "[header] 缓存头部 %zu 字节 + SOS %zu 字节\n",
                    header_len, sos_len);

            /* 打印结构 */
            size_t p = 2;
            while (p < header_len) {
                if (data[p] == 0xFF && p + 4 <= header_len) {
                    uint8_t m = data[p + 1];
                    size_t ml = (data[p + 2] << 8) | data[p + 3];
                    const char *name = "???";
                    if (m == 0xC0) name = "SOF0";
                    else if (m == 0xC4) name = "DHT";
                    else if (m == 0xDB) name = "DQT";
                    else if (m == 0xDD) name = "DRI";
                    fprintf(stderr, "  %s: FF %02X (len=%zu)\n", name, m, ml);
                    p += 2 + ml;
                } else {
                    p++;
                }
            }
            fprintf(stderr, "  SOS: FF DA (len=%zu)\n", sos_len);

            return 1;
        }

        /* 其他标记 */
        if (marker == 0xD9 || marker == 0xD8) {
            pos += 2;
            continue;
        }

        if (pos + 4 >= size) break;
        size_t seg_len = (data[pos + 2] << 8) | data[pos + 3];
        pos += 2 + seg_len;
    }

    return 0;
}

static int has_sos(const uint8_t *data, size_t size)
{
    return (size >= 2 && data[0] == 0xFF && data[1] == 0xDA);
}

/* ====== 主函数 ====== */

int main(void)
{
    signal(SIGINT, sigint_handler);

    int fd = open(DEVICE, O_RDWR);
    if (fd < 0) die("open");

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) die("VIDIOC_QUERYCAP");
    fprintf(stderr, "[init] %s | %s\n", cap.driver, cap.card);

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = WIDTH;
    fmt.fmt.pix.height      = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) die("VIDIOC_S_FMT");
    fprintf(stderr, "[init] %dx%d MJPG, max %u bytes/frame\n",
            fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = N_BUFS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) die("VIDIOC_REQBUFS");
    unsigned int n_bufs = req.count;

    struct buffer buffers[N_BUFS];
    for (unsigned int i = 0; i < n_bufs; i++) {
        struct v4l2_buffer vbuf;
        memset(&vbuf, 0, sizeof(vbuf));
        vbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vbuf.memory = V4L2_MEMORY_MMAP;
        vbuf.index  = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &vbuf) < 0) die("VIDIOC_QUERYBUF");
        buffers[i].length = vbuf.length;
        buffers[i].ptr = mmap(NULL, vbuf.length,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, vbuf.m.offset);
        if (buffers[i].ptr == MAP_FAILED) die("mmap");
    }

    for (unsigned int i = 0; i < n_bufs; i++) {
        struct v4l2_buffer vbuf;
        memset(&vbuf, 0, sizeof(vbuf));
        vbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vbuf.memory = V4L2_MEMORY_MMAP;
        vbuf.index  = i;
        if (ioctl(fd, VIDIOC_QBUF, &vbuf) < 0) die("VIDIOC_QBUF");
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) die("VIDIOC_STREAMON");

    for (int skip = 0; skip < 2; skip++) {
        if (wait_for_data(fd, TIMEOUT_SEC) <= 0) continue;
        struct v4l2_buffer vbuf;
        memset(&vbuf, 0, sizeof(vbuf));
        vbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vbuf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &vbuf) < 0) break;
        ioctl(fd, VIDIOC_QBUF, &vbuf);
    }

    fprintf(stderr, "[run] 开始采集，Ctrl+C 停止\n");
    long frame_count = 0;
    long drop_count  = 0;
    long key_count   = 0;
    long key_bad     = 0;   /* 关键帧校验失败 */
    long fix_sos     = 0;   /* 增量帧自带 SOS */
    long fix_raw     = 0;   /* 增量帧纯压缩数据 */
    long fix_bad     = 0;   /* 增量帧含非法标记 */

    const uint8_t eoi[2] = {0xFF, 0xD9};

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

        struct v4l2_buffer vbuf;
        memset(&vbuf, 0, sizeof(vbuf));
        vbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vbuf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &vbuf) < 0) {
            if (errno == EINTR) break;
            perror("VIDIOC_DQBUF");
            drop_count++;
            continue;
        }

        const uint8_t *frame_data = (const uint8_t *)buffers[vbuf.index].ptr;
        size_t frame_size = vbuf.bytesused;

        if (is_keyframe(frame_data, frame_size)) {
            /*
             * 关键帧校验：
             *   1. 必须有 EOI (FF D9)
             *   2. SOS 长度必须 == 12
             *   3. 提取头部成功
             */
            size_t eoi_pos = find_eoi(frame_data, frame_size);

            if (eoi_pos >= frame_size) {
                /* 没有 EOI，丢弃 */
                fprintf(stderr, "[drop] 关键帧无 EOI，丢弃 (前16字节: ");
                print_hex(frame_data, frame_size, 16);
                key_bad++;
                drop_count++;
            } else if (extract_header(frame_data, frame_size)) {
                /* 校验通过，输出有效数据（到 EOI 为止） */
                fwrite(frame_data, 1, eoi_pos, stdout);
                key_count++;
                fprintf(stderr, "[key] 关键帧 %zu 字节 (前16字节: ", eoi_pos);
                print_hex(frame_data, frame_size, 16);
            } else {
                /* 头部提取失败（SOS 长度异常等） */
                fprintf(stderr, "[drop] 关键帧头部异常，丢弃 (前16字节: ");
                print_hex(frame_data, frame_size, 16);
                key_bad++;
                drop_count++;
            }
        } else if (header_len > 0 && has_sos(frame_data, frame_size)) {
            /* 增量帧自带 SOS：补 [header_cache] + [帧数据] */
            size_t eoi_pos = find_eoi(frame_data, frame_size);
            fwrite(header_cache, 1, header_len, stdout);
            fwrite(frame_data, 1, eoi_pos, stdout);
            if (eoi_pos >= frame_size) fwrite(eoi, 1, 2, stdout);
            fix_sos++;
        } else if (header_len > 0) {
            /* 增量帧纯压缩数据：补 [header_cache] + [sos_cache] + [帧数据] + [EOI] */
            if (has_illegal_markers(frame_data, frame_size)) {
                /* 压缩数据内有非法标记，丢弃 */
                drop_count++;
                fix_bad++;
            } else {
                size_t eoi_pos = find_eoi(frame_data, frame_size);
                fwrite(header_cache, 1, header_len, stdout);
                fwrite(sos_cache, 1, sos_len, stdout);
                fwrite(frame_data, 1, eoi_pos, stdout);
                if (eoi_pos >= frame_size) fwrite(eoi, 1, 2, stdout);
                fix_raw++;
            }
        } else {
            fprintf(stderr, "[skip] 缓存未就绪\n");
        }

        fflush(stdout);
        ioctl(fd, VIDIOC_QBUF, &vbuf);

        frame_count++;
        if (frame_count % 30 == 0)
            fprintf(stderr, "[run] %ld 帧 (关键 %ld, 异常 %ld, 补SOS %ld, 补全 %ld, 坏标记 %ld, 丢 %ld)\n",
                    frame_count, key_count, key_bad, fix_sos, fix_raw, fix_bad, drop_count);
    }

    fprintf(stderr, "[exit] %ld 帧 (关键 %ld, 异常 %ld, 补SOS %ld, 补全 %ld, 坏标记 %ld, 丢 %ld)\n",
            frame_count, key_count, key_bad, fix_sos, fix_raw, fix_bad, drop_count);
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (unsigned int i = 0; i < n_bufs; i++)
        munmap(buffers[i].ptr, buffers[i].length);
    close(fd);

    return 0;
}
