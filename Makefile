# T113 H.264 硬解播放器
#
# 交叉编译说明
# ============
# 板上是 musl libc（/lib/ld-musl-armhf.so.1），必须用 musl 工具链。
# 用 glibc 工具链编出来的 ELF 解释器是 /lib/ld-linux-armhf.so.3，
# 板上一执行就 "not found"（而 ldd 却显示正常，很容易误判）。
#
# 需要指定的两个路径：
#   TOOLCHAIN  —— 交叉编译器前缀所在的 bin 目录
#   SDK        —— 解压后的 Tina SDK 根目录（取 cedarc 头文件用）
#
# 用法：
#   make SDK=/path/to/Tina-Linux TC=/path/to/toolchain/bin
#   make deploy          # 推送到板子 /opt
#
# 头文件务必用 tina_multimedia 那一套（新版布局）。
# 详见 docs/排查记录.md 里的「坑 2」。

TOOLCHAIN ?= $(CURDIR)/toolchain/bin
SDK       ?= $(CURDIR)/sdk
CROSS     ?= arm-openwrt-linux-

# 允许用 TC=... 直接覆盖工具链 bin 目录（TOOLCHAIN 只是默认值）
# 注意：必须在 CC 定义之前处理，否则 CC 会先绑定到 TOOLCHAIN 的默认值。
ifdef TC
TOOLCHAIN := $(TC)
endif

CC      := $(TOOLCHAIN)/$(CROSS)gcc
STRIP   := $(TOOLCHAIN)/$(CROSS)strip

# cedarc 头文件（新版 2025 布局，对应板上 CedarC v1.3.0 之后的 ABI）
CEDARC_INC := $(SDK)/package/allwinner/tina_multimedia/libcedarc/include
# 内核头（sunxi_display2.h 在这里的 video/ 下）
KERNEL_INC := $(SDK)/lichee/linux-5.4/include

CFLAGS  := -O2 -Wall -Wextra -std=gnu11 -D_GNU_SOURCE \
           -I$(CEDARC_INC) -I$(KERNEL_INC) -I.
# 链接用的 .so 从板子上 pull 下来放在 lib/（不进仓库，见 .gitignore）。
# 用板上真实的库做链接，才能保证结构体布局、符号版本和运行期一致。
LIBDIR  := lib
LDFLAGS := -L$(LIBDIR) -Wl,-rpath,/usr/lib -Wl,--allow-shlib-undefined
# libvideoengine 提供 AddVDPlugin()，libVE 提供 VeInitialize()
# 少链接任何一个都会在运行期报 unsupported format
LDLIBS  := -lvdecoder -lvideoengine -lVE -lMemAdapter -lcdc_base -lpthread -lm -ldl

# 需要从板上拉取的库
BOARD_LIBS := libvdecoder.so libvideoengine.so libVE.so libMemAdapter.so \
              libcdc_base.so libsbm.so libfbm.so libaftertreatment.so

TARGET  := player
SRCS    := src/player.c src/mp4parse.c src/avcc2annexb.c src/osd.c
OBJS    := $(SRCS:.c=.o)

.PHONY: all clean deploy tools check-env pull-libs

all: check-env $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)
	@echo "--- built ---"
	@file $@

# 从板子上拉取链接用的库。
# 必须用板上真实的 .so：头文件和库版本对不上会导致结构体错位，
# 表现为 InitializeVideoDecoder 返回 0 但一帧都解不出来。
pull-libs:
	@mkdir -p $(LIBDIR)
	@for l in $(BOARD_LIBS); do \
	    echo "pull $$l"; \
	    adb pull /usr/lib/$$l $(LIBDIR)/$$l || echo "  (跳过 $$l)"; \
	done

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 排查用的诊断工具：都直接调 vdecoder，与 player 共用解码参数
tools: check-env
	@for t in dispclean fdverify dumppic tscheck osddump; do \
	    echo "building $$t"; \
	    $(CC) $(CFLAGS) -o tools/$$t tools/$$t.c src/mp4parse.c \
	        src/avcc2annexb.c $(LDFLAGS) $(LDLIBS) || exit 1; \
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

deploy: $(TARGET)
	adb push $(TARGET) /opt/$(TARGET)
	adb shell chmod +x /opt/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS) tools/dispclean tools/fdverify tools/dumppic \
	      tools/tscheck tools/osddump