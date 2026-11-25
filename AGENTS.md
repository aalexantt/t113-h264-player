# AGENTS.md

改代码前先读。这个项目**几乎每行都有原因**，很多写法看着奇怪，但都是实测撞出来的。
顺手「优化」很容易改坏，而坏了的症状通常是「程序正常跑、日志正常、但没画面」，极难查。

详细排查过程见 [`docs/排查记录.md`](docs/排查记录.md)。

平台：T113-s3 / armv7l / **musl libc** / 内核 5.4.61

---

## 不要改这 9 处

| 位置 | 必须是 | 改了会怎样 |
|---|---|---|
| `conf.bGpuBufValid` | `0` | 设 1 则解码器不分配帧缓冲，永远 `NO_FRAME_BUFFER`，日志刷 `Fbm module not create yet` |
| `DISP_CHANNEL` | `0` | 视频必须走 VI 通道。放 1（UI）会把 NV12 当 RGB 解 → **紫绿色乱码 + 左右重复** |
| `crop.height` | 真实高 `720` | 用缓冲高 736 会把 16 行黑边一起拉伸 → **画面纵向压扁** |
| `clock_start` 的 timescale | `media_timescale` | 用 `timescale`（mvhd=1000，mdhd=12800）→ 播放**慢 12.8 倍** |
| sample 送解码前 | 先转 AnnexB | 不转 → 永远 `NO_BITSTREAM`。转换缓冲按 `len + len/8 + 256` 留 |
| `AddVDPlugin()` | 最先调 | 不调 → `unsupported format H264`；链接要 `-lvideoengine -lVE` |
| 取 dma-buf fd | `memops[23]` | `nBufFd` 恒为 0 不能用。乱试索引可能调到 `palloc`/`pfree` → 崩溃 |
| 工具链 | musl | glibc 版板上一执行 `not found`，**但 `ldd` 显示正常**，极易误判 |
| 头文件 | `tina_multimedia/libcedarc` | 旧版布局差近一倍（`VConfig` 116 vs 196）→ 字段全错位 |

## 改完怎么验

**别只看「没崩」就收工。** 错误配置下程序照样跑、调用全返回 0。

1. 编译：`file release/player` 应显示 `interpreter /lib/ld-musl-armhf.so.1`
2. 出帧：`presented` 在增长，`fps` 接近 `clk_target_fps`
3. **看画面内容**（最关键）：`dumppic` 导出裸帧，PC 上还原成图片，Y 方差 >1000 才有真画面
4. 显示参数：`cat /sys/class/disp/disp/attr/sys`，应为 `fmt[76]`(NV12) / `fmt[0]`(ARGB) / `crop 1280,720`
5. 长时：跑 200 秒，跨过循环点不卡死，结束后内存回到 60MB+

第 3 步能区分「解码问题」还是「显示问题」，别跳过。

## 容易被误改的细节

- **持两帧再还**（`held[2]`）：disp 正在扫当前帧的 dma-buf，立刻 `ReturnPicture` 会撕裂。别改成用完就还。
- **OSD 画布只有 210×111**：全屏要 7.9MB，板子总共才 108MB。别改大。
- **内存显示用 `MemAvailable`**：`MemFree` 常年 9MB 是页缓存行为，不是内存不足。别改回 `MemFree`。
- **退出必须关 disp 层**：`disp_layer_disable()` 用 `DISP_LAYER_SET_CONFIG2` + `enable=0`（不是 `DISP_LAYER_DISABLE`）。不关的话退出后画面定格在最后一帧，下次启动会叠加花屏。**别动 `ch1/lyr0`**，那是系统 fbdev 层，关了黑屏。
- **信号处理只置标志**：`pthread_mutex_lock` 不是 async-signal-safe，在 handler 里加锁会死锁且无日志。只设 `volatile sig_atomic_t`，主循环自己检查。
- **必须清 fb0**：`fb_clear_alpha()` 不能去掉。u-boot 的 LOGO 留在 fb0 显存里，而 fb0 层是逐像素 alpha，LOGO 像素不透明会盖在视频上（表现为屏幕中央一个黑底方框、四周视频正常）。清成 BGRA 全 0（透明）即可。
- **视频必须走 ch0**：ch0 是 VI 通道，支持 YUV；ch1/2/3 是 UI 通道，只认 RGB。把 NV12 喂给 ch1 会得到紫绿乱码 + 左右重复画面（像素按 RGB 重排）。

## 不要被 err 计数误导

`/sys/class/disp/disp/attr/sys` 里的 `err` 字段在只有 ch0 有图层时每帧都会涨（约 60/秒），**这是 DE 的无害统计，不代表图层失败**。曾经据此误判"ch0 通道损坏"，实际视频显示完全正常。判断显示是否正常，只看屏幕实际画面。

## 环境

```bash
export STAGING_DIR=/home/alex/t113/Tina-Linux/staging_dir/target
make TC=.../toolchain/bin SDK=.../Tina-Linux
```

- 编译在 WSL 下做（Windows 没交叉编译器，板上没 gcc）
- 板上**没有** `head`/`tail`/`sort`/`wc`，只有 `grep`/`sed`/`awk`/`cat`
- `adb shell "nohup ... &"` 不可靠，会话一结束就被挂断，启动后必须 `ps` 确认
- `dispclean` 会保留 `ch1/lyr0`（系统 fbdev 层）。它被关掉会黑屏，恢复：`echo 0 > /sys/class/graphics/fb0/blank`

## 常见误区

- 调用返回 0 ≠ 成功。`InitializeVideoDecoder` 返 0 只代表参数被接受
- 有 `FRAME_DECODED` ≠ 有画面。解码成功和显示正确是两件事
- `/sys` 里 `addr[]` 不变 ≠ 卡住。那只反映最近一次配置，判断播放看帧计数
- `total-vm` 大 ≠ 吃内存。看 RSS
- `ldd` 正常 ≠ 能跑。看 `file` 的 interpreter 字段
- 日志参数个数 ≠ 函数签名。`DecodeVideoStream` 日志打 4 个参数，实际是 5 个
- make 跑完 `.o` 还在？多级规则链里的 `.o` 会被当中间文件删，靠 `.SECONDARY` 保留，别删那行

## 扩展

- **新格式**：改 `eCodecFormat` 即可，`AddVDPlugin()` 已注册 h264/h265/mjpeg/mpeg2/mpeg4
- **加音频**：目前完全没有音频。加上后主时钟要改成音频驱动，否则会持续漂移
- **缩放裁剪**：改 `disp_show()` 里的 `screen_win`（目标区域）和 `crop`（源区域）

## 提交

```bash
grep -rn "ghp_" .                                # 不能有 token
git status --short | grep -E "build/|release/"   # 应为空
git push origin main                             # SSH 已配好
```

`lib/` 是 Allwinner 预编译库，有版权，**绝不能提交**。
Git 配置细节见 [`docs/git-setup.md`](docs/git-setup.md)。