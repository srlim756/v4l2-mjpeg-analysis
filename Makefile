# V4L2 MJPEG Analysis - Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lrt

# 源文件
SRC_DIR = src
TOOLS_DIR = tools
BENCH_DIR = bench

# 目标
TARGETS = capture capture_fix capture_yuyv dump_mjpg bench

.PHONY: all clean

all: $(TARGETS)

# MJPG 采集（原始版本）
capture: $(SRC_DIR)/main.c
	$(CC) $(CFLAGS) -o $@ $<

# MJPG 帧头补全版本
capture_fix: $(SRC_DIR)/main_mjpg_fix.c
	$(CC) $(CFLAGS) -o $@ $<

# YUYV 采集
capture_yuyv: $(SRC_DIR)/main_yuyv.c
	$(CC) $(CFLAGS) -o $@ $<

# 抓帧分析工具
dump_mjpg: $(SRC_DIR)/dump_mjpg.c
	$(CC) $(CFLAGS) -o $@ $<

# mmap vs read 基准测试
bench: $(BENCH_DIR)/bench_mmap_vs_read.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# MJPEG 修复工具
mjpeg_repair: $(TOOLS_DIR)/mjpeg_repair_tool.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS) mjpeg_repair
	rm -f frame_*.jpg
