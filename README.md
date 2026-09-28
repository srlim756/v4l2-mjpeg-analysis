Linux V4L2 摄像头 MJPEG 采集分析

验证虚拟机环境下 USB 摄像头的传输瓶颈。

## 项目背景

在 VMware 虚拟机环境下使用 V4L2 采集 USB 摄像头 MJPEG 流时遇到严重花屏问题。通过逐字节分析 JPEG 码流结构，定位到两个独立问题：

1. **UVC 协议层**：部分摄像头固件为节省 USB 带宽，只在关键帧发送完整 JPEG 头（DQT/DHT），后续帧省略
2. **虚拟化层**：VMware USB 2.0 控制器在 Hypervisor 分时调度下无法保证 UVC 等时传输的 125μs 微帧时序，导致 YUYV 永远超时、MJPEG 帧损坏（73.6%）

**✅ 最终解决方案**：将 VMware USB 控制器从 2.0 升级到 3.1，MJPG 和 YUYV 均可正常出图。

> 注：本项目使用的 HD Webcam 每帧都发送完整 JPEG 头（SOI+APP0+...），帧头补全算法对本摄像头不需要，仅对"增量帧缺头"的摄像头有效。

## 技术栈

- Linux V4L2 + MMAP 零拷贝
- UVC 协议分析
- JPEG 码流结构（SOI/SOF0/DQT/DHT/SOS/EOI）
- FFmpeg 管道解码
- VMware USB 虚拟化分析

## 核心成果

| 指标  | 数值  |
| --- | --- |
| MMAP vs read 性能提升 | 2.3 倍（85μs vs 195μs/帧） |
| CPU 搬运开销降低 | 56% |
| USB 2.0 下帧损坏率 | 73.6% |
| USB 2.0 → 3.1 后出图 | ✅ 正常 |

## 编译运行

```bash
# MJPG 采集（推荐，USB 3.1 下正常）
gcc -o capture src/main.c
./capture 2>/dev/null | ffplay -f mjpeg -framerate 30 -i pipe:0

# MJPG 帧头补全版本（仅对缺头型摄像头有效，本摄像头不需要）
gcc -o capture_fix src/main_mjpg_fix.c
./capture_fix 2>/dev/null | ffplay -f mjpeg -framerate 30 -i pipe:0

# YUYV 采集（USB 2.0 下超时，USB 3.1 可出图但有延迟）
gcc -o capture_yuyv src/main_yuyv.c
./capture_yuyv 2>/dev/null | ffplay -f rawvideo -pixel_format yuyv422 -video_size 640x480 -i pipe:0

# 抓帧分析工具
gcc -o dump_mjpg src/dump_mjpg.c
./dump_mjpg

# mmap vs read 基准测试
gcc -o bench bench/bench_mmap_vs_read.c -lrt
./bench
```

## 文件说明

| 文件  | 说明  |
| --- | --- |
| src/main.c | MJPG 采集（原始版本） |
| src/main_mjpg_fix.c | MJPG 帧头补全 v4（仅对缺头型摄像头有效） |
| src/main_yuyv.c | YUYV 采集 |
| src/dump_mjpg.c | 抓帧分析工具 |
| bench/bench_mmap_vs_read.c | mmap vs read 基准测试 |

## 问题分析

### 问题 1：UVC 非关键帧缺表（本摄像头不存在此问题）

部分摄像头 MJPG 流只在关键帧发送完整 JPEG 头，后续帧省略 DQT/DHT：

```
关键帧：SOI + SOF + DQT + DHT + SOS + 数据 + EOI（完整）
后续帧：[压缩数据]（无任何 JPEG 头）
```

判断方法：用 dump_mjpg 抓帧后 `xxd frame_0.jpg | head -3`，如果有 `FF D8 FF E0/C0/C4/DA` 说明头完整，不需要补全。

### 问题 2：VM USB 2.0 等时传输时序不足

VMware USB 2.0 控制器在 Hypervisor 分时调度下，无法保证 UVC 等时传输的 125μs 微帧时序，导致 YUYV 永远超时、MJPEG 帧损坏（73.6%）。

| 环境  | 结果  | 结论  |
| --- | --- | --- |
| USB 2.0 | ✗ 花屏/超时 | 时序不足 |
| USB 3.1 | ✅ 正常出图 | 时序足够 |

**✅ 解决方案**：VMware 设置 → USB 控制器 → 兼容性改为 USB 3.1

## 对比实验

| 环境  | 结果  | 结论  |
| --- | --- | --- |
| Windows 宿主机 | ✓ 画面正常 | 摄像头没问题 |
| Linux 虚拟机 USB 2.0 | ✗ 花屏/卡顿 | VM USB 2.0 时序不足 |
| Linux 虚拟机 USB 3.1 | ✓ 画面正常 | USB 3.1 解决时序问题 |

## 主要分析

### mmap vs read 区别

```
read：摄像头 DMA buffer → 内核中间缓冲区 → 用户 buffer（CPU 拷贝）
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

1. USB 2.0 等时传输时序不足，帧数据损坏
2. 部分摄像头只在关键帧发 DQT/DHT，后续帧省略
3. ffplay 收到损坏帧或缺表帧 → 花屏

## License

MIT
