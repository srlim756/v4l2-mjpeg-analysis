# V4L2 MJPEG Analysis

Linux V4L2 摄像头 MJPEG 采集分析项目，验证虚拟机环境下 USB 摄像头的传输瓶颈。

## 项目背景

在 VMware 虚拟机环境下使用 V4L2 采集 USB 摄像头 MJPEG 流时遇到严重花屏问题。通过逐字节分析 JPEG 码流结构，定位到两个独立问题：

1. **UVC 协议层**：摄像头固件为节省 USB 带宽，只在关键帧发送完整 JPEG 头（DQT/DHT），后续帧省略
2. **虚拟化层**：VMware USB 虚拟化层存在数据损坏，73.6% 的帧被破坏

通过 Windows 宿主机对比实验，证实花屏由虚拟机 USB 虚拟化导致，非摄像头或代码问题。

## 技术栈

- Linux V4L2 + MMAP 零拷贝
- UVC 协议分析
- JPEG 码流结构（SOI/SOF0/DQT/DHT/SOS/EOI）
- FFmpeg 管道解码
- VMware USB 虚拟化分析

## 核心成果

| 指标                | 数值                     |
| ----------------- | ---------------------- |
| MMAP vs read 性能提升 | 2.3 倍（85μs vs 195μs/帧） |
| CPU 搬运开销降低        | 56%                    |
| VM USB 帧损坏率       | 73.6%                  |
| 帧头补全算法版本          | v1→v4                  |

## 编译运行

```bash
# MJPG 采集（原始版本）
gcc -o capture src/main.c
./capture | ffplay -f mjpeg -i pipe:0

# MJPG 帧头补全版本
gcc -o capture_fix src/main_mjpg_fix.c
./capture_fix | ffplay -f mjpeg -i pipe:0

# YUYV 采集（需要物理机）
gcc -o capture_yuyv src/main_yuyv.c
./capture_yuyv | ffplay -f rawvideo -pixel_format yuyv422 -video_size 640x480 -framerate 20 -i pipe:0

# 抓帧分析工具
gcc -o dump_mjpg src/dump_mjpg.c
./dump_mjpg

# mmap vs read 基准测试
gcc -o bench bench/bench_mmap_vs_read.c -lrt
./bench
```

## 文件说明

| 文件                         | 说明                      |
| -------------------------- | ----------------------- |
| src/main.c                 | MJPG 采集（原始版本）           |
| src/main_mjpg_fix.c        | MJPG 帧头补全 v4（含完整性校验）    |
| src/main_yuyv.c            | YUYV 采集（VM 下超时）         |
| src/dump_mjpg.c            | 抓帧分析工具                  |
| tools/mjpeg_repair_tool.c  | MJPEG 修复工具（Huffman 表补全） |
| bench/bench_mmap_vs_read.c | mmap vs read 基准测试       |
| docs/VM_USB方案分析.md         | 虚拟机 USB 摄像头问题分析与方案对比    |

## 问题分析

### 问题 1：UVC 非关键帧缺表

摄像头 MJPG 流只在关键帧发送完整 JPEG 头，后续帧省略 DQT/DHT：

```
关键帧：SOI + SOF + DQT + DHT + SOS + 数据 + EOI（完整）
后续帧：[压缩数据]（无任何 JPEG 头）
```

### 问题 2：VM USB 数据损坏

VMware USB 虚拟化层损坏 73.6% 的帧数据：

| 环境          | 结果              |
| ----------- | --------------- |
| Windows 宿主机 | 画面正常            |
| Linux 虚拟机   | 花屏/卡顿，73.6% 帧损坏 |

### 解决方案

1. **帧头补全算法**：缓存关键帧头部，给后续帧动态补上
2. **架构改进**：物理机采集 + RTSP 分发，规避虚拟化瓶颈

## 对比实验

同一摄像头在 Windows 宿主机上测试：

```powershell
# Windows 上查看摄像头
ffmpeg -list_devices true -f dshow -i dummy

# 测试外置摄像头
ffplay -f dshow -i video="USB Camera"
```

| 环境          | 结果      | 结论            |
| ----------- | ------- | ------------- |
| Windows 宿主机 | ✓ 画面正常  | 摄像头没问题        |
| Linux 虚拟机   | ✗ 花屏/卡顿 | VM USB 传输损坏数据 |

## 主要分析：

### mmap vs read 区别

```
read：摄像头 DMA buffer → 内核中间缓冲区 → 用户 buffer（两次拷贝）
mmap：摄像头 DMA buffer ← MAP_SHARED → 用户 buffer（零拷贝）

实测：mmap 85μs/帧，read 195μs/帧，快 2.3 倍
```

### JPEG 段结构

```
SOI (FF D8)  → 帧开始
SOF (FF C0)  → 帧信息（宽高、颜色分量）
DQT (FF DB)  → 量化表
DHT (FF C4)  → Huffman 表
SOS (FF DA)  → 扫描开始（压缩数据从这里开始）
EOI (FF D9)  → 帧结束
```

### 为什么花屏

1. 摄像头只在关键帧发 DQT/DHT，后续帧省略
2. 程序启动时没赶上关键帧，拿到的全是缺表帧
3. ffplay 没有量化表和 Huffman 表，只能乱猜 → 花屏

## License

MIT
