# T113 H.264 硬解播放器

全志 T113-s3（Cortex-A7，armv7l，musl libc）上的 H.264 硬件解码 + disp 直接送显播放器。
绕开 `libtplayer` / `xplayer`，直接调用 `libvdecoder` + Allwinner DISP2 显示驱动。

带 OSD 叠加：分辨率、帧率、播放时间/总时长、CPU 占用、剩余内存。

## 为什么不用现成的播放器

原方案走 `xplayer`（`libtplayer.so` → `layerControl`），播放时卡死：

```
E FbmReturnPicture return fail, bUsedByRender=0
E vdecoder <picNode is full when queue buffer>
E vdecoder <NOframeBuffer:>>> return NO_FRAME_BUFFER>
```

`layerControl` 的显示节点队列在 `layerControl_linux.cpp:32` 定义为
`NUM_OF_PICTURES_KEEP_IN_LIST = 2`，加上 `+1` 只有 **3 个节点**，G2D 流水线周转不开。

板上的 `libtplayer.so` 是 2022-10-12 的预编译产物，与 SDK 源码版本不完全对应，
改队列这条路验证不通。改成直接调底层 `libvdecoder`。

## 架构

三线程 + 两个 disp 层：

```
parser 线程 ──读 MP4──> AVCC→AnnexB ──SubmitVideoStreamData──> ┐
                                                              │
decoder 线程 <── DecodeVideoStream 循环 ───────────────────────┤
                                                              │
main 线程 ── RequestPicture ── memops[23] 取 dma-buf fd ──────┘
               │                        │
               │                        ├─> ch0/lyr0  DISP_LAYER_SET_CONFIG2  (YUV420SP, 视频)
               └── 持 2 帧再还           └─> ch1/lyr1  (ARGB8888, OSD，逐像素 alpha)
```

- **不放帧缓冲池**：`bGpuBufValid=0` 让解码器自己分配 FBM（12 块），
  主线程持有最近 2 帧不还，避免 disp 正在扫的缓冲被解码器复用。
- **时钟自算**：按 PTS + 墙上时间推进，不用音频时钟。循环时检测 PTS 回退并重置基准。
- **双 disp 层**：视频走 VI 通道（`ch0`，YUV 必须走这里），
  OSD 走 UI 通道（`ch1`），`zorder` 30 压在视频之上。

## 快速开始

### 1. 准备

需要：
- Tina SDK 源码包（取 cedarc 头文件）：`Tina-Linux-*.tar.gz`
- 交叉工具链：`toolchain-sunxi-musl`（**必须 musl**，见下方说明）
- 板子能 `adb` 连上

```bash
# 解压 SDK（只需要头文件，解压后约 200KB）
tar -xzf Tina-Linux-20250105.tar.gz

# 从板子上拉链接用的 .so（不进仓库）
make pull-libs
```

### 2. 编译

```bash
make TC=<toolchain>/bin SDK=<解压后的 Tina-Linux 路径>
```

例如：
```bash
make TC=/opt/toolchain-sunxi-musl/toolchain/bin \
     SDK=/opt/Tina-Linux
```

产物布局：

```
build/          所有中间文件（.o / .d），按源码目录结构镜像
  ├── player.o
  ├── mp4parse.o
  └── tools/
      └── dispclean.o
release/        最终可执行文件（已 strip）
  ├── player
  └── tools/
      ├── dispclean
      └── ...
```

- 中间文件和产物分离，源码目录保持干净
- 带头文件依赖跟踪（`-MMD -MP`），改了 `src/osd.h` 只会重编引用它的文件
- 链接后自动 strip（50K → 25K）

其他目标：

```bash
make tools      # 编译诊断工具到 release/tools/
make clean      # 清掉 build/ 和 release/
make deploy     # 推送到板子 /opt
make pull-libs  # 从板子拉链接用的 .so
```

### 3. 部署运行

```bash
make deploy
adb shell "cd /opt && ./player /opt/media/test.mp4 60"
```

第二个参数是播放秒数（默认 30）。播完自动退出，期间循环播放。

## 关键结论

这部分是踩坑记录，每条都有实测依据。详版见 [`docs/排查记录.md`](docs/排查记录.md)。

### 必须用 musl 工具链

板上只有 musl：
```
/lib/ld-musl-armhf.so.1 -> libc.so
```

用 glibc 工具链编出来的 ELF 解释器是 `/lib/ld-linux-armhf.so.3`，
板上一执行就 `not found`，**而 `ldd` 却显示一切正常**，极易误判。
用 `file` 看 interpreter 字段才准。

### `AddVDPlugin()` 必须最先调用

它注册 h264/h265 解码插件，声明在 `vdecoder.h:267`，实现在 `libvideoengine.so`。
不调用则 `VideoEngineCreate: unsupported format H264`。
链接时必须加 `-lvideoengine -lVE`，否则 undefined reference。

### `bGpuBufValid` 必须为 0

最隐蔽的一条。设 1 时：

```
FbmCreate, total fbm number: 12, decoder needed: 2      ← 只建结构
Fbm module of video stream 0 not create yet             ← 永远取不到帧
```

设 0 时：

```
FbmCreateBuffer: ** call allocate pic buf, i = 0, maxNum = 12   ← 真正分配
```

原因：`bGpuBufValid=1` 是让外部通过 `SetVideoFbmBufAddress` 提供缓冲的路径，
不提供就永远 `NO_FRAME_BUFFER`。

### AVCC → AnnexB 转换

MP4 的 sample 是 AVCC 长度前缀，解码器要 AnnexB 起始码。实测对照：

| 送什么 | `DecodeVideoStream` 返回 |
|---|---|
| 原始 AVCC | 5 (`NO_BITSTREAM`) |
| 转成 AnnexB | 3 (`KEYFRAME_DECODED`) → 1 (`FRAME_DECODED`) |

转换缓冲区容量要按 `len + len/8 + 256` 留，否则首帧 105KB 的 NAL 多会溢出。

### 取 dma-buf fd 靠 memops 虚表索引 23

`VideoPicture.nBufFd` 恒为 0，必须走 memops：

```c
typedef int (*fn_t)(void *);
fn_t fn = ((fn_t *)(void *)memops)[23];   // 32 位偏移 = 23*4 = 92
int fd = fn(pic->pData0);
```

验证结果：
```
帧1: get_fd(pData0) → fd=9    readlink=/dmabuf:   size=1413120
帧2: get_fd(pData0) → fd=10   readlink=/dmabuf:   size=1413120
理论值 1280 × 736 × 1.5 = 1413120   ← 吻合
```

### 结构体布局：两版头文件差近一倍

SDK 里有两套 cedarc 头文件，布局完全不同：

| | 旧版（2022） | 新版（2025） |
|---|---|---|
| `VConfig` | **116** 字节 | **196** 字节 |
| `VideoPicture` | **144** 字节 | **296** 字节 |
| `memops` 偏移 | 112 | 136 |

**用错就会所有字段错位**，表现为 `InitializeVideoDecoder` 返回 0 但一帧都解不出来。

判断板上库属于哪版：查 `libvdecoder.so` 的字符串表——
有 `nVeFreq` 且版本号是 `CedarC-v1.3.0` 就属于**新版**。

### 视频必须走 channel 0

```c
// de_feat.h:  CHN_NUM=4, VI_CHN_NUM 为 1 或 2
// de_rtmx.c:  if (channel < vi_chno) → VI（视频）路径，支持 YUV
//             else                   → UI 路径，数据被当 RGB 解释
```

放在 `channel=1`（UI 通道）时，NV12 被当 RGB 解，屏幕上是**紫色乱码 + 左右重复**。

### crop 用真实高度，不是缓冲高度

解码输出 `1280x736`，736 是 720 向上对齐到 16。实测那 16 行全是 0（黑）。
- `size[]` 用 736（物理布局）
- `crop` 用 **720**（实际显示区域）

否则黑边被一起拉伸，画面变形。

### 帧率：两个 timescale 别搞混

MP4 有两个 timescale：

| 来源 | 管什么 | 示例值 |
|---|---|---|
| `mvhd.timescale` | movie 时长 | 1000 |
| `mdhd.timescale` | **sample PTS 的单位** | **12800** |

`stts.delta = 512`，用 mdhd 换算 `512/12800 = 40ms` → **25fps**。
用 mvhd 的 1000 算成 512ms，慢 12.8 倍。

```c
target_us = base_wall + (pts - base_pts) * 1000000 / media_timescale;
```

## 实现文件

| 文件 | 说明 |
|---|---|
| `src/player.c` | 主程序：三线程解码 + disp 送显 + OSD + 播放时钟 |
| `src/mp4parse.c/h` | 轻量 MP4 解封装（stsd/stts/stsc/stsz/stco + mdhd timescale）|
| `src/avcc2annexb.c/h` | AVCC → AnnexB 格式转换 |
| `src/osd.c/h` | OSD 层：ARGB8888 层 + 逐像素 alpha + 位图字体渲染 |
| `src/font.h` | 1bpp 点阵字体（9×17，ASCII 32~126），由 `tools/genfont.py` 生成 |
| `tools/` | 排查用诊断工具，见下表 |

### 诊断工具

| 工具 | 用途 |
|---|---|
| `dispclean` | 清理测试残留的 disp 层（**保留系统 UI 层**）|
| `fdverify` | 验证 memops 索引 23 返回的确实是 dma-buf fd |
| `dumppic` | 导出裸 NV12 帧，用于离线还原核对画面 |
| `pixcheck` | 统计 Y 分量直方图，判断解码是否真出画 |
| `osddump` | 离线渲染 OSD 画布到 PPM，验证排版 |
| `tscheck` | 检查 timescale 与实际帧率 |

```bash
make tools TC=... SDK=...
```

## OSD 显示内容

```
1280x720 H264          分辨率 + 编码格式
fps 25.0 / 25.0        实测帧率 / 目标帧率
00:12 / 02:50  x2      播放时间 / 总时长 / 循环轮次
cpu 42% dec 509        整机 CPU 占用 / 已解码帧数
mem 75/108M free 9M    可用内存 / 总量 / 账面空闲
```

内存显示的是 `MemAvailable` 而不是 `MemFree` —— Linux 会把空闲内存拿去做页缓存，
`MemFree` 常年只有 9MB 左右，看着吓人但随时可回收，`MemAvailable` 才是真正能用的量。

可用内存低于 20MB 时该行变红。

## 实测性能

1920×1080 屏，播放 1280×720@25fps：

| 指标 | 数值 |
|---|---|
| 帧率 | 25.0 fps 稳定（200 秒 5001 帧）|
| CPU 占用 | player 进程 ~5%，整机 90% idle |
| 内存 | 播放时可用 52MB，结束后回到 78MB（无泄漏）|
| 循环 | PTS 从 2149888 跳回 14336，正常续播 |

## 注意事项

- **`dispclean` 会关掉所有视频层和 OSD 层**，但保留 `ch1/lyr0`（系统 fbdev 层）。
  如果不小心关了系统层导致黑屏，执行 `echo 0 > /sys/class/graphics/fb0/blank` 恢复。
- 视频层 `ch0/lyr0` 的名字里有 layer 0，但它属于**视频通道**，不是 UI 的 layer 0。
- 只支持 MP4 容器 + H.264/H.265。没有音频。

## 许可与声明

本项目代码为原创实现，可自由使用。

**不包含** Allwinner 的预编译库（`libvdecoder.so` 等）和 SDK 源码——
这些是厂商版权物，需自行从设备和 SDK 获取。`make pull-libs` 从设备上拉取，
`lib/` 已加入 `.gitignore`。

## 文档

| 文档 | 内容 |
|---|---|
| [`docs/排查记录.md`](docs/排查记录.md) | 完整排查过程：每个坑的现象、证据、结论 |
| [`docs/git-setup.md`](docs/git-setup.md) | Git 推送配置：SSH key、凭据文件、代理处理 |