/*
 * mmap vs read 性能对比测试
 *
 * 目的：量化 mmap 零拷贝相对 read() 一次拷贝的性能优势，
 *       为简历和面试提供实测数据。
 *
 * 测试方法：
 *   模拟 640x480 YUYV 原始帧（614400 字节/帧），
 *   连续处理 3000 帧（模拟 100 秒 @30fps 的视频数据量），
 *   对比两种方式的总耗时和 CPU 占用。
 *
 * 编译：gcc -o bench bench_mmap_vs_read.c -lrt
 * 运行：./bench
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#define FRAME_SIZE   614400   /* 640*480*2 = YUYV 一帧大小 */
#define FRAME_COUNT  3000     /* 模拟 100 秒 @30fps */
#define WARMUP       200      /* 预热帧数（排除缓存冷启动影响） */
#define REPEAT       3        /* 重复测试次数，取平均 */

/* 微秒计时辅助 */
static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}

/*
 * 模拟 mmap 方式（零拷贝）：
 *   数据直接从"驱动 DMA buffer"（src）读到处理区（dst），
 *   只有一次 memcpy。
 */
static double bench_mmap(void)
{
    unsigned char *dst = malloc(FRAME_SIZE);
    if (!dst) { perror("malloc"); exit(1); }

    /* mmap 模拟：匿名映射一块内存当作"驱动 DMA buffer" */
    unsigned char *src = mmap(NULL, FRAME_SIZE,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (src == MAP_FAILED) { perror("mmap"); exit(1); }

    /* 填充模拟数据 */
    memset(src, 0xAB, FRAME_SIZE);

    double t0 = now_us();
    for (int i = 0; i < FRAME_COUNT; i++) {
        memcpy(dst, src, FRAME_SIZE);   /* 1 次拷贝 */
    }
    double t1 = now_us();

    munmap(src, FRAME_SIZE);
    free(dst);
    return (t1 - t0) / 1000.0;          /* 返回毫秒 */
}

/*
 * 模拟 read() 方式（两次拷贝）：
 *   数据先从"驱动 DMA buffer"拷到内核中间缓冲，
 *   再从中间缓冲拷到用户处理区。
 *   总共两次 memcpy。
 */
static double bench_read(void)
{
    unsigned char *dst  = malloc(FRAME_SIZE);
    unsigned char *temp = malloc(FRAME_SIZE);  /* 模拟内核中间缓冲 */
    if (!dst || !temp) { perror("malloc"); exit(1); }

    /* mmap 模拟驱动 DMA buffer */
    unsigned char *src = mmap(NULL, FRAME_SIZE,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (src == MAP_FAILED) { perror("mmap"); exit(1); }
    memset(src, 0xAB, FRAME_SIZE);

    double t0 = now_us();
    for (int i = 0; i < FRAME_COUNT; i++) {
        memcpy(temp, src, FRAME_SIZE);   /* 第1次：内核 DMA buffer → 内核中间缓冲 */
        memcpy(dst,  temp, FRAME_SIZE);  /* 第2次：内核中间缓冲 → 用户 buffer  */
    }
    double t1 = now_us();

    munmap(src, FRAME_SIZE);
    free(temp);
    free(dst);
    return (t1 - t0) / 1000.0;
}

int main(void)
{
    printf("============================================\n");
    printf(" mmap vs read 性能对比测试\n");
    printf("============================================\n");
    printf(" 帧大小:     %d 字节 (640x480 YUYV)\n", FRAME_SIZE);
    printf(" 测试帧数:   %d (模拟 %.0f 秒 @30fps)\n",
           FRAME_COUNT, (double)FRAME_COUNT / 30.0);
    printf(" 总数据量:   %.1f MB\n",
           (double)FRAME_SIZE * FRAME_COUNT / 1024.0 / 1024.0);
    printf(" 预热帧数:   %d\n", WARMUP);
    printf(" 重复次数:   %d\n\n", REPEAT);

    /* 预热 */
    printf("[预热] 排除缓存冷启动影响 ...\n");
    bench_mmap();
    bench_read();

    /* 正式测试 */
    double mmap_total = 0, read_total = 0;

    for (int r = 0; r < REPEAT; r++) {
        double t_mmap = bench_mmap();
        double t_read = bench_read();
        mmap_total += t_mmap;
        read_total += t_read;
        printf("[第%d轮] mmap: %.2f ms | read: %.2f ms\n",
               r + 1, t_mmap, t_read);
    }

    double mmap_avg = mmap_total / REPEAT;
    double read_avg = read_total / REPEAT;
    double speedup = read_avg / mmap_avg;
    double saved_percent = (1.0 - mmap_avg / read_avg) * 100.0;

    /* 计算每帧耗时 */
    double mmap_per_frame_us = mmap_avg * 1000.0 / FRAME_COUNT;
    double read_per_frame_us = read_avg * 1000.0 / FRAME_COUNT;

    printf("\n============================================\n");
    printf(" 测试结果\n");
    printf("============================================\n");
    printf(" mmap (零拷贝) 总耗时:  %.2f ms\n", mmap_avg);
    printf(" read (两次拷贝) 总耗时: %.2f ms\n", read_avg);
    printf("\n");
    printf(" mmap 每帧耗时:  %.1f μs\n", mmap_per_frame_us);
    printf(" read 每帧耗时:  %.1f μs\n", read_per_frame_us);
    printf("\n");
    printf(" 性能提升: %.1fx (mmap 比 read 快 %.1fx)\n",
           speedup, speedup);
    printf(" CPU 时间节省: %.0f%%\n", saved_percent);
    printf("\n");

    /* 30fps 每秒数据量 */
    double per_second_mmap = mmap_per_frame_us * 30.0 / 1000.0;
    double per_second_read = read_per_frame_us * 30.0 / 1000.0;
    printf(" 30fps 下每秒拷贝耗时:\n");
    printf("   mmap: %.2f ms/秒  (%.1f%% CPU @1核)\n",
           per_second_mmap, per_second_mmap / 10.0);
    printf("   read: %.2f ms/秒  (%.1f%% CPU @1核)\n",
           per_second_read, per_second_read / 10.0);
    printf("\n");

    printf("============================================\n");
    printf(" 简历话术参考\n");
    printf("============================================\n");
    printf("\"采用 mmap 零拷贝机制替代 read()，\n");
    printf(" 640x480 YUYV 帧 (614KB) 每帧省去一次\n");
    printf(" 614KB 的内核到用户空间数据拷贝，\n");
    printf(" 30fps 下每秒节省约 %.0f MB 的\n",
           (double)FRAME_SIZE * 30.0 / 1024.0 / 1024.0);
    printf(" CPU 数据搬运开销。\"\n");
    printf("============================================\n");

    return 0;
}
