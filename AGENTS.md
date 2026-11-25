# AGENTS.md

给 AI 协作者（以及新加入的人）的工作约定。**动手改代码前请先读完本文**。

这个仓库的代码**几乎每一行都是有原因的**——很多写法看起来奇怪，但都是实测撞出来的结论。
盲目「优化」或「重构」很容易把功能改坏，而且坏了之后很难查。

- 平台：全志 T113-s3（Cortex-A7，armv7l，**musl libc**，内核 5.4.61）
- 目标：H.264/H.265 硬解 + Allwinner DISP2 直接送显 + OSD 叠加
- 背景细节：见 [`docs/排查记录.md`](docs/排查记录.md)

---

## 一、绝对不要改的东西

下面这些是**硬约束**，改了必然出问题。每条都标注了违反后的具体症状，方便你自查。

### 1. `bGpuBufValid` 必须是 0

```c
conf.bGpuBufValid = 0;   // src/player.c
```

设为 1 时解码器不分配帧缓冲，只建空结构，等外部通过 `SetVideoFbmBufAddress` 提供。
本项目没有提供，结果就是**帧缓冲永远为空**。

**症状**：`InitializeVideoDecoder` 返回 0 看似正常，但 `DecodeVideoStream` 永远返回
`VDECODE_RESULT_NO_FRAME_BUFFER(4)`，`RequestPicture` 一直拿到 NULL，日志刷
`Fbm module of video stream 0 not create yet`。

### 2. 视频层必须用 `channel 0`

```c
#define DISP_CHANNEL  0
```

`channel < VI_CHN_NUM` 走 VI（视频）路径，支持 YUV；否则走 UI 路径，
数据被当 RGB 解释。

**症状**：画面变成**紫绿色乱码 + 左右重复两份**。因为 NV12 的字节被当成 RGB 分量了。

### 3. `crop` 高度用真实高度（720），不是缓冲高度（736）

```c
// size[] 用缓冲尺寸（含对齐填充），crop 用真实显示区域
cfg.info.fb.size[0].height = pic->nHeight;      // 736
cfg.info.fb.crop.height   = ((long long)crop_h) << 32;  // 720
```

解码输出高度是 720 向上对齐到 16 = 736，**多出的 16 行实测全是 0（黑）**。

**症状**：黑边被一起拉伸进画面，**画面纵向压扁变形**。

### 4. 帧率必须用 `mdhd.timescale`

```c
clock_start(&clk, c.mp4->media_timescale);   // 不是 c.mp4->timescale
```

MP4 有两个 timescale，`mvhd` 的管整体时长，`mdhd` 的才是 sample PTS 的单位。
本机测试文件：`mvhd=1000`，`mdhd=12800`，差 12.8 倍。

**症状**：播放速度**慢 12.8 倍**（或快，取决于两者大小关系）。

### 5. MP4 sample 必须 AVCC → AnnexB 转换

```c
alen = avcc_to_annexb(smp.data, (int)smp.len, abuf, acap, 4);
```

MP4 里是 4 字节长度前缀（AVCC），解码器要起始码（AnnexB）。
**转换缓冲区要按 `len + len/8 + 256` 留**，按 `len + 64` 会让大帧（首帧 105KB）转换失败。

**症状**：直接喂原始数据 → `DecodeVideoStream` 一直返回 `NO_BITSTREAM(5)`。

### 6. `AddVDPlugin()` 必须最先调用

```c
AddVDPlugin();   // 在 CreateVideoDecoder / InitializeVideoDecoder 之前
```

它注册 h264/h265 插件，实现在 `libvideoengine.so`。链接必须带 `-lvideoengine -lVE`。

**症状**：`VideoEngineCreate: unsupported format H264`，初始化失败。

### 7. `disp_dma_map` 需要的 fd 只能从 memops 索引 23 取

```c
static int picture_get_fd(...) {
    if (pic->nBufFd > 0) return pic->nBufFd;   // 实际恒为 0
    fn = ((fn_t *)(void *)memops)[23];          // get_fd_by_vir
    return fn(pic->pData0);
}
```

`VideoPicture.nBufFd` 恒为 0，**不能用**。索引 23 是 `get_fd_by_vir`，
32 位下偏移 92 字节。

**症状**：拿不到 fd，disp 送不进去；如果瞎试其他索引，可能调用到
`palloc`/`pfree` 之类的函数，**直接段错误或内存损坏**。

### 8. 工具链必须 musl

板上只有 `/lib/ld-musl-armhf.so.1`。
用 glibc 工具链编出来的 ELF 解释器是 `/lib/ld-linux-armhf.so.3`。

**症状**：板上一执行就是 `not found`，但 **`ldd` 显示一切正常**——
这是最容易误判的一条。用 `file` 看 interpreter 字段才准。

### 9. 头文件必须用 `tina_multimedia` 那一套

`SDK/package/allwinner/tina_multimedia/libcedarc/include`

SDK 里有**两套** cedarc 头文件，布局完全不同：

| | 旧版（liballwinner_tina） | 新版（tina_multimedia） |
|---|---|---|
| `VConfig` | 116 字节 | **196 字节** |
| `VideoPicture` | 144 字节 | **296 字节** |
| `memops` 偏移 | 112 | **136** |

板上 `libvdecoder.so` 是 `CedarC-v1.3.0`（含 `nVeFreq` 字段），属于**新版**。

**症状**：用旧版头文件 → 所有字段错位，
`InitializeVideoDecoder` 可能返回 0 但一帧都解不出来，或者直接段错误。

---

## 二、改动后怎么验证

**不要只看「程序没崩」就认为改对了。** 这个项目的坑特点是：错误配置下程序正常运行、
日志正常、调用全返回 0，只是画面不对或者没画面。

### 分级验证

按改动范围选对应级别，**不要跳级**。

#### L1：编译通过

```bash
make TC=<toolchain>/bin SDK=<Tina-Linux 路径>
file release/player
# 必须看到 interpreter /lib/ld-musl-armhf.so.1
```

#### L2：解码出帧

```bash
make deploy
adb shell "cd /opt && ./player /opt/media/1280x720.mp4 15"
```

看输出：
- `decoded` 和 `presented` 都应该在增长
- `fps` 应该接近 `clk_target_fps`（本例 25.0）
- 不该出现 `NO_FRAME_BUFFER` / `NO_BITSTREAM` 的持续刷屏

#### L3：画面内容正确

**这一步最关键，不能省。** 用 `dumppic` 导出裸帧，在 PC 上还原成图片看：

```bash
adb shell "/opt/dumppic /opt/media/1280x720.mp4 /tmp/f 30"
adb pull /tmp/f_1280x736_stride1280.yuv
```

然后用 Python 按 NV12 还原（U 在前、V 在后）：
- Y 均值应在 16~235 之间
- **方差 > 1000** 说明有真实纹理，不是黑屏/纯色
- 画面内容应该是正常照片，颜色正确

这一步能明确区分「解码问题」和「显示问题」，是最有效的排查手段。

#### L4：显示参数正确

```bash
adb shell "cat /sys/class/disp/disp/attr/sys"
```

期望看到（数值以 1280x720 为例）：
```
ch[0] lyr[0] z[20] fmt[76]  fb[1280, 736; 640, 368]  crop[0,0,1280,720]  frame[0,0,1920,1080]
ch[1] lyr[0] z[16]          ← 系统 fbdev 层，不能动
ch[1] lyr[1] z[30] fmt[0]   fb[210,111]  a[pixel 255]
```

检查点：
- `fmt[76]` = `0x4c` = `DISP_FORMAT_YUV420_SP_UVUV`（NV12）
- `fmt[0]` = `ARGB_8888`（OSD）
- `crop` 是 `1280,720` 而不是 `1280,736`
- `a[pixel]` 说明逐像素 alpha 生效
- `addr[]` 非零

> `/sys` 里的 `addr[]` 是最近一次 SET_CONFIG2 写入的配置值，
> **不会随每帧变化**，别拿它判断播放是否正常。判断播放看帧计数和 fd 轮转。

#### L5：长时稳定性（改时钟/循环/内存相关时必做）

```bash
adb shell "cd /opt && ./player /opt/media/1280x720.mp4 200"
```

- 200 秒应显示约 5000 帧（25fps × 200s）
- 循环点（本文件 170.84 秒）过后 PTS 应回退并继续播，不卡死
- 结束后 `MemAvailable` 应回到 60MB 以上（无内存泄漏）

---

## 三、代码结构约定

```
src/
├── player.c         主程序。三线程 + disp 送显 + OSD + 播放时钟
├── mp4parse.c/h     轻量 MP4 解封装。只支持 MP4 容器，不依赖 ffmpeg
├── avcc2annexb.c/h  AVCC → AnnexB 格式转换
├── osd.c/h          OSD 层。ARGB8888 + 逐像素 alpha + 位图字体
└── font.h           点阵字体数据，由 tools/genfont.py 生成，勿手改
tools/               诊断工具，每个都独立可跑
```

### 线程模型

三个线程，**唯一的共享状态是 `ctx_t` 里的 `state` 标志和统计计数器**：

| 线程 | 职责 | 关键调用 |
|---|---|---|
| `parser_thread` | 读 MP4 → 转格式 → 送码流 | `RequestVideoStreamBuffer` / `SubmitVideoStreamData` |
| `decoder_thread` | 循环解码 | `DecodeVideoStream` |
| main | 取帧 → 送显 → OSD → 控时钟 | `RequestPicture` / `DISP_LAYER_SET_CONFIG2` |

- 用 `get_state` / `set_state` 加 `pthread_mutex` 保护退出标志
- 统计计数器（`submitted`/`decoded`/`presented`）是 `volatile int`，容忍误差，不加锁
- **不要引入新的共享可变状态**。需要跨线程传递的东西，优先走 `ctx_t` 的原子标志

### 必须持两帧

```c
VideoPicture *held[2] = { NULL, NULL };
// 显示新帧后，只归还更早的那帧
if (held_n >= 2) {
    ReturnPicture(c.pdec, held[0]);
    held[0] = held[1];
    held[1] = pic;
}
```

disp 正在扫描当前帧的 dma-buf，如果立刻 `ReturnPicture` 还给解码器，
那块内存会被复用，导致**画面撕裂或显示脏数据**。

**不要改成「用完立刻归还」**，即使看起来更省内存。

### OSD 画布要小

OSD 画布只做 `210×111`（面板大小），**不要改成全屏**。

全屏 1920×1080 ARGB8888 需要 7.9MB，而板子 `MemTotal` 只有 108MB。
当前 210×111 只占 91KB。

### 内存显示用 `MemAvailable`

```c
else if (strcmp(key, "MemAvailable:") == 0) avail = val;
```

Linux 会把空闲内存用作页缓存，`MemFree` 常年只有 9MB 左右，
**这不代表内存不足**。`MemAvailable` 才是真正可用的量（通常 50~78MB）。

**不要改成显示 `MemFree`**，会让人误以为内存告急。

---

## 四、诊断工具

改代码遇到问题，先用工具定位，不要靠猜。

| 工具 | 什么时候用 |
|---|---|
| `dumppic` | **画面不对时第一个用**。导出裸帧，区分解码问题 / 显示问题 |
| `pixcheck` | 怀疑解码出黑屏/花屏时，看 Y 分量直方图 |
| `fdverify` | 改动了 memops 相关代码后，验证 fd 拿得对不对 |
| `dispclean` | disp 层状态混乱、有残留层时清理 |
| `osddump` | 改 OSD 排版后，离线渲染成 PPM 看效果，不用上板 |
| `tscheck` | 帧率不对时，先看 timescale 读对没有 |
| `ofcheck` | 怀疑头文件版本不匹配时，打印结构体大小与字段偏移 |

### `dispclean` 的一个注意事项

它**只清视频层和 OSD 层，保留 `ch1/lyr0`**（系统 fbdev 层）。

早前版本无差别遍历 ch0~3 / lyr0~3 全部关闭，把系统 UI 层也关了，
**屏幕直接黑掉且不会自动恢复**。恢复办法：

```bash
echo 0 > /sys/class/graphics/fb0/blank
```

如果你要改这个工具，**不要删掉 `UI_KEEP_LAYER` 的跳过逻辑**。

---

## 五、环境相关

### 交叉编译必须在 WSL / Linux 下

Windows 侧没有交叉编译器，板上没有 gcc/make。

```bash
export STAGING_DIR=/home/alex/t113/Tina-Linux/staging_dir/target
make TC=/home/alex/t113/Tina-Linux/prebuilt/gcc/linux-x86/arm/toolchain-sunxi-musl/toolchain/bin \
     SDK=/mnt/d/Linux/os/BingPi-M2/.workbuddy/vdec_probe/Tina-Linux
```

`STAGING_DIR` 不设会有大量 `STAGING_DIR not defined` 警告（不影响生成，但很吵）。

### 板子上缺常用命令

`head` / `tail` / `sort` / `wc` / `od` **都没有**。
写板上执行的 shell 命令时只能用 `grep` / `sed` / `awk` / `cat` / `ls`。

### 后台启动不可靠

`adb shell "nohup ... &"` 和 `setsid ... &` 都会在 adb 会话结束时被挂断。

**症状**：以为程序在跑，实际根本没起来，屏幕上留的是上一轮的最后一帧。
（曾被误判为「播放 5 秒就停」。）

需要在后台跑长任务时，用前台运行 + 大超时，或者确认 `ps` 里真的有进程。

### 内存本来就不宽裕

`MemTotal` 只有 108MB。加功能时注意内存开销，
new 出来的 dma-buf / ion 缓冲要确认能释放。

顺带一个数据：**所有用户进程 RSS 合计仅 2.4MB**，
想「清理内存」去杀进程收益极低，还会断调试连接或断网。

---

## 六、提交约定

### 提交信息

中文描述 + 「现象/原因/验证」结构。示例：

```
修复帧率慢 12.8 倍

原因：用了 mvhd.timescale(1000)，但 sample PTS 的单位是 mdhd.timescale(12800)。
现象：播放速度只有正常的 1/12.8。
验证：播放 200 秒，presented=5001 帧（理论 5000），fps 稳定 25.0。
```

### 提交前检查

```bash
# 不能有 token 之类的敏感信息
grep -rn "ghp_\|gho_\|password" --include="*.c" --include="*.md" .

# build/ 和 release/ 不能进仓库
git status --short | grep -E "build/|release/"
```

`.gitignore` 已排除 `build/`、`release/`、`lib/`、`sdk/`。
**`lib/` 里是 Allwinner 的预编译库，有版权问题，绝不能提交。**

### 推送

SSH 已配置好，直接 `git push origin main` 即可。详见 [`docs/git-setup.md`](docs/git-setup.md)。

---

## 七、常见误区

这些是我实际踩过的，写下来避免重复。

1. **「调用返回 0 就是成功」**——`InitializeVideoDecoder` 返回 0 只代表参数被接受，
   不代表能解出帧。必须验证 `presented` 在增长。

2. **「日志里有 FRAME_DECODED 就有画面」**——解码出帧和显示正确是两件事。
   必须导出裸帧看内容（L3 验证）。

3. **「`/sys` 里 addr[] 不变说明卡住了」**——不会，那只反映最近一次配置。
   判断播放状态看帧计数。

4. **「`total-vm` 大就是吃内存」**——看 RSS。`gopanel` 的 `total-vm` 有 572MB，
   但 `anon-rss` 只有 1.8MB。

5. **「`ldd` 正常就能跑」**——glibc/musl 的解释器路径不同，
   `ldd` 能解析依赖但 exec 会 `not found`。

6. **「日志格式串的参数个数 = 函数签名」**——日志里 `DecodeVideoStream` 只打印了
   4 个参数，但它实际是 5 参数。判断签名要查头文件或反汇编。

7. **「改头文件不用重编」**——已配 `-MMD -MP` 依赖跟踪，改 `.h` 会自动重编。
   如果发现没重编，检查 `build/*.d` 是否正常生成。

8. **「make 跑完 .o 还在」**——多级模式规则链里的 `.o` 会被 make 当中间文件删掉。
   本项目用 `.SECONDARY` 声明保留，**不要删掉那行**，否则增量编译失效。

---

## 八、如果想扩展

### 支持新格式

`InitializeVideoDecoder` 的 `eCodecFormat` 换成对应枚举即可（`VIDEO_CODEC_FORMAT_*`）。

板上 `/usr/lib/` 已有的插件（实测确认）：

| 视频 | 音频 |
|---|---|
| `libawh264` / `libawh265` / `libawmjpeg` | `libaw_aacdec` / `libaw_mp3dec` / `libaw_flacdec` |
| `libawmpeg2` / `libawmpeg4base` / `libawmpeg4dx` | `libaw_amrdec` / `libaw_oggdec` / `libaw_opusdec` |
|  | `libaw_alacdec` / `libaw_apedec` / `libaw_wavdec` |
|  | `libaw_dsddec` / `libaw_g729dec` / `libaw_siprdec` |
|  | `libaw_atrcdec` / `libaw_radec` |

`AddVDPlugin()` 会自动扫 `/usr/lib/libaw*.so`。
注意日志里会有很多 `Invalid plugin, CedarPluginVDInit not found` —— 那是音频插件，
它们不是视频解码插件，报这个警告是正常的。

需要自己解封装的话，MP4 之外的容器得另写 parser，`mp4parse.c` 可以当参考。

### 加音频

**目前完全没有音频**。要做的话需要：
- 音频解码器（`libaw_aacdec.so` 等）
- 音频输出通路（ALSA / `aplay`）
- 音视频同步（用音频时钟做主时钟，而不是现在的 PTS 自推）

最后一条是重点：现在的时钟是「PTS + 墙上时间」纯视频时钟，
加音频后应该改成「以音频播放位置为准」，否则会持续漂移。

### 支持缩放/裁剪显示

`disp_show()` 里 `screen_win` 控制目标区域，`crop` 控制源区域，
改这两组值就能实现缩放、letterbox、画面偏移。

注意 `screen_win` 超过屏幕尺寸会被驱动截断，`crop` 超出缓冲范围会显示脏数据。