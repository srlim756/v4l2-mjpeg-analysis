#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 给缺失 DHT 段的 MJPG 码流补充标准 Huffman 表
 *
 * 用途：部分 USB 摄像头（尤其笔记本内置摄像头）输出 MJPEG 时省略了
 *       Huffman 表段（DHT），导致 libjpeg / ffplay 无法解码。
 *       此函数在码流头部插入 4 张标准 Huffman 表（Y-DC/Y-AC/UV-DC/UV-AC），
 *       使其变为合法 JPEG 文件。
 *
 * 入参：src     - 摄像头 DQBUF 拿到的原始 MJPG 数据
 *       src_len - 数据长度（buf.bytesused）
 *       out_len - [出参] 修复后数据的长度
 *
 * 返回：malloc 出来的新缓冲区（调用者负责 free），失败返回 NULL
 *
 * 使用示例：
 *   int fixed_len;
 *   unsigned char *fixed = jpeg_fix_huffman(raw, raw_len, &fixed_len);
 *   fwrite(fixed, 1, fixed_len, fp);
 *   free(fixed);
 */
unsigned char* jpeg_fix_huffman(const unsigned char *src,
                                 int src_len, int *out_len);

#endif
