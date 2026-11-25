"""
genfont.py — 从系统字体生成 1bpp 位图字体表 (font.h)

为什么自己生成位图字体：板子上没有 freetype，也不想为了几个数字
引入一个渲染库。直接生成一个固定单元格的点阵表，运行时按位拷贝即可。

输出格式：
  - 覆盖 ASCII 32~126
  - 每行按字节对齐（bits_per_row = (FONT_W + 7) / 8）
  - 高位在左，MSB first
  - 字形数据放在 font_map[]，索引 = ch - FONT_FIRST

用法：python genfont.py
"""

from PIL import Image, ImageDraw, ImageFont
import os

FONT_PATH = r"C:\Windows\Fonts\consola.ttf"
FONT_SIZE = 15          # 像素高度，屏幕上会被视频放大 1.5x（720p→1080p）
FIRST, LAST = 32, 126
OUT = "font.h"

font = ImageFont.truetype(FONT_PATH, FONT_SIZE)

# 量出所有字符的最大宽度，用作固定单元格宽度
maxw = 0
for code in range(FIRST, LAST + 1):
    ch = chr(code)
    bbox = font.getbbox(ch)
    w = bbox[2] - bbox[0]
    maxw = max(maxw, w)

CELL_W = maxw
CELL_H = FONT_SIZE + 2      # 留点余量给下伸部
BPR = (CELL_W + 7) // 8     # bytes per row
CHAR_BYTES = BPR * CELL_H

print(f"字体 {FONT_PATH} 大小={FONT_SIZE} 单元格={CELL_W}x{CELL_H} "
      f"每行{BPR}字节 每字符{CHAR_BYTES}字节")

glyphs = []
for code in range(FIRST, LAST + 1):
    ch = chr(code)
    img = Image.new("L", (CELL_W, CELL_H), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), ch, font=font, fill=255)
    # 二值化：>128 视为亮点
    px = img.load()
    buf = bytearray(CHAR_BYTES)
    for y in range(CELL_H):
        for x in range(CELL_W):
            if px[x, y] > 128:
                buf[y * BPR + (x >> 3)] |= (0x80 >> (x & 7))
    glyphs.append(bytes(buf))

with open(OUT, "w", encoding="utf-8") as f:
    f.write("/* 自动生成，勿手改。生成器：genfont.py */\n")
    f.write("/* 源字体 %s，大小 %d，单元格 %dx%d，每行 %d 字节 */\n"
            % (os.path.basename(FONT_PATH), FONT_SIZE, CELL_W, CELL_H, BPR))
    f.write("#ifndef OSD_FONT_H\n#define OSD_FONT_H\n\n")
    f.write("#define FONT_FIRST   %d\n" % FIRST)
    f.write("#define FONT_LAST    %d\n" % LAST)
    f.write("#define FONT_W       %d\n" % CELL_W)
    f.write("#define FONT_H       %d\n" % CELL_H)
    f.write("#define FONT_BPR     %d   /* bytes per row */\n" % BPR)
    f.write("#define FONT_CHAR_SZ %d\n\n" % CHAR_BYTES)
    f.write("static const unsigned char font_map[%d][%d] = {\n"
            % (LAST - FIRST + 1, CHAR_BYTES))
    for i, g in enumerate(glyphs):
        vals = ",".join("0x%02x" % b for b in g)
        f.write("  {%s},  /* '%s' */\n" % (vals, chr(FIRST + i).replace("\\", "\\\\")))
    f.write("};\n\n#endif\n")

print(f"已写 {OUT}，共 {len(glyphs)} 个字符，"
      f"数据量 {len(glyphs) * CHAR_BYTES} 字节")
print(f"渲染 '0' 的样子：")
img = Image.new("L", (CELL_W, CELL_H), 0)
ImageDraw.Draw(img).text((0, 0), "0", font=font, fill=255)
px = img.load()
for y in range(CELL_H):
    print("  " + "".join("#" if px[x, y] > 128 else "." for x in range(CELL_W)))