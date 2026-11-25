# T113 H.264 硬解播放器
#
# 交叉编译说明
# ============
# 板上是 musl libc（/lib/ld-musl-armhf.so.1），必须用 musl 工具链。
# 用 glibc 工具链编出来的 ELF 解释器是 /lib/ld-linux-armhf.so.3，
# 板上一执行就 "not found"（而 ldd 却显示正常，很容易误判）。
#
# 需要指定的两个路径：
#   TC   —— 交叉编译器前缀所在的 bin 目录
#   SDK  —— 解压后的 Tina SDK 根目录（取 cedarc 头文件用）
#
# 用法：
#   make TC=/path/to/toolchain/bin SDK=/path/to/Tina-Linux
#   make tools           # 顺便编诊断工具
#   make deploy          # 推送到板子 /opt
#   make clean           # 清掉 build/ 和 release/
#
# 目录约定：
#   build/    所有中间产物（.o / .d），按源码目录结构镜像
#   release/  最终可执行文件
#
# 头文件务必用 tina_multimedia 那一套（新版布局）。
# 详见 docs/排查记录.md 里的「坑 2」。

# ---------------------------------------------------------------------------
# 工具链
# ---------------------------------------------------------------------------

TOOLCHAIN ?= $(CURDIR)/toolchain/bin
SDK       ?= $(CURDIR)/sdk
CROSS     ?= arm-openwrt-linux-

# 允许用 TC=... 直接覆盖工具链 bin 目录（TOOLCHAIN 只是默认值）
# 注意：必须在 CC 定义之前处理，否则 CC 会先绑定到 TOOLCHAIN 的默认值。
ifdef TC
TOOLCHAIN := $(TC)
endif

CC    := $(TOOLCHAIN)/$(CROSS)gcc
STRIP := $(TOOLCHAIN)/$(CROSS)strip

# cedarc 头文件（新版 2025 布局，对应板上 CedarC v1.3.0 之后的 ABI）
CEDARC_INC := $(SDK)/package/allwinner/tina_multimedia/libcedarc/include
# 内核头（sunxi_display2.h 在这里的 video/ 下）
KERNEL_INC := $(SDK)/lichee/linux-5.4/include

# ---------------------------------------------------------------------------
# 编译选项
# ---------------------------------------------------------------------------

# -MMD -MP：生成头文件依赖，改 src/*.h 或 font.h 能正确触发重编。
# 不加的话改了 osd.h 里的结构体，引用它的 .o 不会重编，链接期才报错。
DEPFLAGS := -MMD -MP

CFLAGS  := -O2 -Wall -Wextra -std=gnu11 -D_GNU_SOURCE \
           -I$(CEDARC_INC) -I$(KERNEL_INC) -Isrc -I.

# 链接用的 .so 从板子上 pull 下来放在 lib/（不进仓库，见 .gitignore）。
# 用板上真实的库做链接，才能保证结构体布局、符号版本和运行期一致。
LIBDIR  := lib
LDFLAGS := -L$(LIBDIR) -Wl,-rpath,/usr/lib -Wl,--allow-shlib-undefined
# libvideoengine 提供 AddVDPlugin()，libVE 提供 VeInitialize()
# 少链接任何一个都会在运行期报 unsupported format
LDLIBS  := -lvdecoder -lvideoengine -lVE -lMemAdapter -lcdc_base \
           -lpthread -lm -ldl

# 需要从板上拉取的库
BOARD_LIBS := libvdecoder.so libvideoengine.so libVE.so libMemAdapter.so \
              libcdc_base.so libsbm.so libfbm.so libaftertreatment.so

# ---------------------------------------------------------------------------
# 目录
# ---------------------------------------------------------------------------

BUILDDIR := build
RELDIR   := release

# ---------------------------------------------------------------------------
# 目标与源文件
# ---------------------------------------------------------------------------

TARGET  := $(RELDIR)/player
SRCS    := src/player.c src/mp4parse.c src/avcc2annexb.c src/osd.c src/alsa_out.c
# 镜像源码结构：src/foo.c -> build/foo.o
OBJS    := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

# player 和大部分诊断工具共用这两个模块，只编一次
SHARED_OBJS := $(BUILDDIR)/mp4parse.o $(BUILDDIR)/avcc2annexb.o

# 诊断工具。这些工具共用 mp4parse/avcc2annexb 和同一组链接库，
# 统一按同一套依赖链接，省得每个工具单独维护依赖列表
# （多链进去的 .o 若没被引用，链接器会自动丢弃，无副作用）。
TOOL_NAMES := dispclean fdverify dumppic tscheck osddump
TOOL_SRCS  := $(addprefix tools/,$(addsuffix .c,$(TOOL_NAMES)))
TOOL_OBJS  := $(patsubst tools/%.c,$(BUILDDIR)/tools/%.o,$(TOOL_SRCS))
TOOL_BINS  := $(addprefix $(RELDIR)/tools/,$(TOOL_NAMES))
TOOL_DEPS  := $(TOOL_OBJS:.o=.d)

# 音频诊断工具。只依赖 alsa_out.c，不碰解码器，
# 所以单独列出来——这样即使 vdecoder 相关头文件缺失也能单独编译它。
AUDIO_TOOLS := audiotest
AUDIO_BINS  := $(addprefix $(RELDIR)/tools/,$(AUDIO_TOOLS))

# ---------------------------------------------------------------------------
# 规则
# ---------------------------------------------------------------------------

.PHONY: all tools clean deploy check-env pull-libs dirs

all: check-env $(TARGET)

# 输出目录。用 order-only 依赖（|）保证目录先建好，但目录时间戳变了不触发重编。
$(BUILDDIR) $(RELDIR) $(BUILDDIR)/tools $(RELDIR)/tools:
	@mkdir -p $@

# 阻止 make 把 .o 当成"中间文件"删掉。
#
# 默认情况下，由模式规则链产生（a.c -> a.o -> 可执行文件）的 .o 会被 make
# 视为临时中间产物，构建完成后自动删除。表现是 build/ 里只剩 .d 没有 .o，
# 每次 make 都得全量重编，增量编译完全失效。
# .SECONDARY 声明这些文件要保留，改了某个 .c 只重编它自己。
.SECONDARY: $(OBJS) $(TOOL_OBJS)

# 主程序
$(TARGET): $(OBJS) | $(RELDIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)
	@$(STRIP) $@ 2>/dev/null || true
	@printf -- '--- built ---\n'
	@file $@
	@ls -lh $@ | awk '{print "    size:", $$5}'

# 源码 -> 对象文件（镜像到 build/）
$(BUILDDIR)/%.o: src/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

# 工具源码 -> 对象文件（镜像到 build/tools/）
$(BUILDDIR)/tools/%.o: tools/%.c | $(BUILDDIR)/tools
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

# 诊断工具链接
$(RELDIR)/tools/%: $(BUILDDIR)/tools/%.o $(SHARED_OBJS) | $(RELDIR)/tools
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)
	@$(STRIP) $@ 2>/dev/null || true
	@echo "  built $@"

tools: check-env $(TOOL_BINS) $(AUDIO_BINS)
	@echo "--- $(words $(TOOL_BINS) $(AUDIO_BINS)) 个诊断工具已生成到 $(RELDIR)/tools/ ---"

# 音频工具只需 alsa_out.c，单独链接，不依赖厂商解码库
$(RELDIR)/tools/audiotest: $(BUILDDIR)/tools/audiotest.o $(BUILDDIR)/alsa_out.o | $(RELDIR)/tools
	$(CC) $(CFLAGS) -o $@ $^ -lm -lpthread
	@$(STRIP) $@ 2>/dev/null || true
	@echo "  built $@"

# 板上的音频通路测试脚本（纯 shell，不需要编译）
install-scripts:
	adb push tools/probe_audio.sh /tmp/probe_audio.sh
	adb shell "sed -i 's/\r$$//' /tmp/probe_audio.sh; chmod +x /tmp/probe_audio.sh"
	@echo "已部署到 /tmp/probe_audio.sh"

# 从板子上拉取链接用的库。
# 必须用板上真实的 .so：头文件和库版本对不上会导致结构体错位，
# 表现为 InitializeVideoDecoder 返回 0 但一帧都解不出来。
pull-libs:
	@mkdir -p $(LIBDIR)
	@for l in $(BOARD_LIBS); do \
	    echo "pull $$l"; \
	    adb pull /usr/lib/$$l $(LIBDIR)/$$l || echo "  (跳过 $$l)"; \
	done

check-env:
	@test -x "$(CC)" || { \
	    echo "错误：找不到编译器 $(CC)"; \
	    echo "请用 make TC=<toolchain>/bin SDK=<Tina-Linux 路径>"; \
	    exit 1; }
	@test -d "$(CEDARC_INC)" || { \
	    echo "错误：找不到 cedarc 头文件目录"; \
	    echo "  $(CEDARC_INC)"; \
	    echo "请用 make SDK=<Tina-Linux 路径>"; \
	    exit 1; }
	@test -d "$(LIBDIR)" || { \
	    echo "错误：找不到 $(LIBDIR)/ 目录（链接用的厂商库）"; \
	    echo "请先执行 make pull-libs 从板子上拉取"; \
	    exit 1; }

deploy: $(TARGET)
	adb push $(TARGET) /opt/player
	adb shell chmod +x /opt/player

clean:
	rm -rf $(BUILDDIR) $(RELDIR)
	@echo "已清理 $(BUILDDIR)/ 和 $(RELDIR)/"

# 头文件依赖。放在最后，首次构建时 .d 还不存在，用 -include 静默忽略。
-include $(DEPS) $(TOOL_DEPS)