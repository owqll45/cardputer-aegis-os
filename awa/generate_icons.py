
# 图标工厂 v2: 全部用绘图原语重画, 40x40 RGB565
import math

def color_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

COLORS = {
    ' ': 0x0000, # 背景黑
    'K': 0x0000, # 纯黑(图元用)
    'W': 0xFFFF, # 白
    'R': 0xF800, # 红
    'G': 0x07E0, # 绿
    'B': 0x001F, # 蓝
    'Y': 0xFFE0, # 黄
    'C': 0x07FF, # 青
    'M': 0xF81F, # 品红
    'O': 0xFC00, # 橙
    '.': 0x3186, # 深灰
    'd': 0x4208, # 更暗灰
    'S': 0xC618, # 银
    'P': 0xF81F, # 品红
    'g': 0x0540, # 深绿
    'w': 0xE79C, # 木色
}

def new_img():
    return [[' '] * 40 for _ in range(40)]

def px(img, x, y, c):
    if 0 <= x < 40 and 0 <= y < 40:
        img[y][x] = c

def rect(img, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px(img, x, y, c)

def recto(img, x0, y0, x1, y1, c):
    for x in range(x0, x1 + 1):
        px(img, x, y0, c); px(img, x, y1, c)
    for y in range(y0, y1 + 1):
        px(img, x0, y, c); px(img, x1, y, c)

def disc(img, cx, cy, r, c):
    for dy in range(-r, r + 1):
        half = int((r * r - dy * dy) ** 0.5)
        for dx in range(-half, half + 1):
            px(img, cx + dx, cy + dy, c)

def ring(img, cx, cy, r, c):
    for a in range(360):
        x = int(round(cx + r * math.cos(math.radians(a))))
        y = int(round(cy + r * math.sin(math.radians(a))))
        px(img, x, y, c)

def line(img, x0, y0, x1, y1, c):
    dx = abs(x1 - x0); sx = 1 if x0 < x1 else -1
    dy = -abs(y1 - y0); sy = 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        px(img, x0, y0, c)
        if x0 == x1 and y0 == y1: break
        e2 = 2 * err
        if e2 >= dy: err += dy; x0 += sx
        if e2 <= dx: err += dx; y0 += sy

def tri(img, x0, y0, x1, y1, x2, y2, c):
    # 扫描线填三角
    ys = sorted([(y0, x0), (y1, x1), (y2, x2)])
    (ya, xa), (yb, xb), (yc, xc) = ys
    for y in range(ya, yc + 1):
        def ix(y, ya, xa, yb, xb):
            if yb == ya: return xa
            return xa + (y - ya) * (xb - xa) // (yb - ya)
        xl = ix(y, ya, xa, yc, xc)
        xr = ix(y, ya, xa, yb, xb) if y <= yb else ix(y, yb, xb, yc, xc)
        if xl > xr: xl, xr = xr, xl
        for x in range(xl, xr + 1): px(img, x, y, c)

def bitmap(img, rows, ox, oy, scale, cmap):
    for ry, row in enumerate(rows):
        for rx, ch in enumerate(row):
            if ch in cmap:
                for sy in range(scale):
                    for sx in range(scale):
                        px(img, ox + 0 if False else 0, 0, ' ')  # noop guard
    # 上面占位不用, 直接画:
    for ry, row in enumerate(rows):
        for rx, ch in enumerate(row):
            if ch in cmap:
                rect(img, ox + rx * scale, oy + ry * scale,
                     ox + (rx + 1) * scale - 1, oy + (ry + 1) * scale - 1, cmap[ch])

def to_array(img):
    data = []
    for y in range(40):
        for x in range(40):
            data.append(COLORS.get(img[y][x], 0x0000))
    return data

icons = {}

# ---------------- Gomoku 五子棋: 木盘+网格+黑五连+白二 ----------------
im = new_img()
rect(im, 2, 2, 37, 37, 'w')
recto(im, 2, 2, 37, 37, 'K')
for i in range(5):  # 网格 6x6
    p = 5 + i * 6
    line(im, 5, p, 35, p, 'K')
    line(im, p, 5, p, 35, 'K')
for i in range(5):  # 黑五连(对角) 带高光
    cx = 8 + i * 6; cy = 8 + i * 6
    disc(im, cx, cy, 2, 'K')
    px(im, cx - 1, cy - 1, 'd')
disc(im, 14, 8, 2, 'W'); disc(im, 20, 14, 2, 'W')  # 两白子
icons['gomoku'] = to_array(im)

# ---------------- Reversi 黑白棋: 绿盘+黑白对角子 ----------------
im = new_img()
rect(im, 2, 2, 37, 37, 'g')
recto(im, 2, 2, 37, 37, 'K')
for i in range(4):
    p = 6 + i * 9
    line(im, 4, p, 36, p, 'K')
    line(im, p, 4, p, 36, 'K')
for cx, cy, c in [(10, 10, 'K'), (28, 10, 'W'), (10, 28, 'W'), (28, 28, 'K')]:
    disc(im, cx, cy, 4, 'S' if c == 'W' else 'd')
    disc(im, cx, cy, 3, c)
disc(im, 19, 19, 4, 'K'); disc(im, 19, 19, 3, 'W')  # 中央翻转中的子
icons['reversi'] = to_array(im)

# ---------------- Checkers 西洋跳棋 ----------------
im = new_img()
for r in range(9):
    for c in range(9):
        col = '.' if (r + c) % 2 == 0 else 'S'
        rect(im, 2 + c * 4, 2 + r * 4, 2 + (c + 1) * 4 - 1, 2 + (r + 1) * 4 - 1, col)
recto(im, 2, 2, 37, 38, 'K')
for cx, cy, c in [(16, 24, 'R'), (24, 8, 'W'), (32, 32, 'R')]:
    disc(im, cx, cy, 3, 'K')
    disc(im, cx, cy, 2, c)
icons['checkers'] = to_array(im)

# ---------------- Connect4 四子棋(保留原版设计) ----------------
im = new_img()
rect(im, 2, 2, 37, 38, 'B')
recto(im, 2, 2, 37, 38, 'K')
def c4hole(im, cx, cy, ch):
    disc(im, cx, cy, 2, ch)
for ry in range(5):
    for rx in range(6):
        c4hole(im, 7 + rx * 5, 8 + ry * 6, ' ')
for rx, ry, ch in [(1, 4, 'R'), (2, 4, 'Y'), (3, 4, 'R'), (4, 4, 'Y'),
                   (2, 3, 'Y'), (3, 3, 'R'), (3, 2, 'R')]:
    c4hole(im, 7 + rx * 5, 8 + ry * 6, ch)
icons['connect4'] = to_array(im)

# ---------------- UNO(保留) ----------------
im = new_img()
rect(im, 8, 2, 31, 37, 'W')
recto(im, 8, 2, 31, 37, 'R')
for cx, cy, ch in [(13, 9, 'R'), (21, 9, 'Y'), (13, 17, 'G'), (21, 17, 'B')]:
    rect(im, cx, cy, cx + 6, cy + 6, ch)
rect(im, 13, 27, 26, 28, 'd')
icons['uno'] = to_array(im)

# ---------------- Tank(保留微调) ----------------
im = new_img()
rect(im, 18, 3, 21, 15, 'Y')          # 炮管
rect(im, 9, 13, 30, 26, 'Y')          # 车体
rect(im, 15, 16, 24, 22, 'O')         # 炮塔舱
rect(im, 6, 27, 32, 32, '.')          # 履带
for wx in [9, 15, 21, 27]:
    disc(im, wx + 2, 29, 2, 'K')
icons['tank'] = to_array(im)

# ---------------- Pong: 双拍+中线+大球 ----------------
im = new_img()
for y in range(3, 38, 6):
    rect(im, 19, y, 20, y + 3, 'd')   # 中线虚线
rect(im, 4, 12, 6, 25, 'W')           # 左拍
rect(im, 33, 14, 35, 31, 'W')         # 右拍
rect(im, 21, 7, 24, 10, 'Y')          # 球(醒目黄)
icons['pong'] = to_array(im)

# ---------------- Snake 贪吃蛇: 粗蛇身+头+苹果 ----------------
im = new_img()
body = [(6, 30), (6, 26), (6, 22), (10, 22), (14, 22), (18, 22), (18, 18), (18, 14), (22, 14), (26, 14), (30, 14)]
for i in range(len(body) - 1):
    x0, y0 = body[i]; x1, y1 = body[i + 1]
    for k in range(3):
        line(im, x0, y0 + k - 1, x1, y1 + k - 1, 'G')
hx, hy = body[-1]
rect(im, hx - 1, hy - 2, hx + 5, hy + 2, 'G')   # 头
px(im, hx + 3, hy - 1, 'W'); px(im, hx + 4, hy - 1, 'K')  # 眼
line(im, hx + 6, hy, hx + 8, hy, 'R')           # 信子
disc(im, 10, 8, 3, 'R')                          # 苹果
line(im, 10, 5, 12, 3, 'G')                      # 苹果叶
icons['snake'] = to_array(im)

# ---------------- Tetris 俄罗斯方块: 堆叠+下落 ----------------
im = new_img()
def blk(im, x, y, c):
    rect(im, x, y, x + 5, y + 5, c)
    recto(im, x, y, x + 5, y + 5, 'K')
    px(im, x + 1, y + 1, 'W')
# 底排: I O O T T T S S
for i, c in enumerate(['C', 'Y', 'Y', 'M', 'M', 'M', 'G']):
    blk(im, 2 + i * 6, 32, c)
# 中排
blk(im, 2, 26, 'C'); blk(im, 14, 26, 'R'); blk(im, 20, 26, 'M'); blk(im, 26, 26, 'G'); blk(im, 32, 26, 'O')
blk(im, 2, 20, 'C'); blk(im, 20, 20, 'R'); blk(im, 32, 20, 'O')
blk(im, 2, 14, 'C')
# 下落的 L
blk(im, 10, 4, 'O'); blk(im, 10, 10, 'O'); blk(im, 16, 10, 'O')
icons['tetris'] = to_array(im)

# ---------------- Breakout 打砖块 ----------------
im = new_img()
for i, c in enumerate(['R', 'O', 'Y', 'G', 'B']):
    for bx in range(5):
        rect(im, 2 + bx * 8, 3 + i * 4, 8 + bx * 8, 5 + i * 4, c)
disc(im, 26, 24, 2, 'W')              # 球
rect(im, 12, 34, 28, 36, 'S')         # 挡板
icons['breakout'] = to_array(im)

# ---------------- Invaders(保留) ----------------
im = new_img()
inv = [
    "        G     G        ",
    "       GGGGGGGGG       ",
    "      GG GGGGG GG      ",
    "     GGGGGGGGGGGGG     ",
    "     G GGGGGGGGG G     ",
    "     G G       G G     ",
]
for ry, row in enumerate(inv):
    for rx, ch in enumerate(row):
        if ch == 'G':
            rect(im, rx + 8, 12 + ry * 3, rx + 8, 12 + ry * 3 + 2, 'G')
icons['invaders'] = to_array(im)

# ---------------- Flappy 小鸟: 身体+翅+眼+喙+管道 ----------------
im = new_img()
rect(im, 28, 0, 36, 12, 'G')          # 上管
rect(im, 26, 10, 38, 14, 'G')         # 上管沿
rect(im, 28, 30, 36, 39, 'G')         # 下管
rect(im, 26, 26, 38, 30, 'G')         # 下管沿
disc(im, 14, 20, 7, 'Y')              # 身体
disc(im, 10, 22, 4, 'O')              # 翅膀
disc(im, 17, 17, 3, 'W')              # 眼白
disc(im, 18, 17, 1, 'K')              # 瞳孔
tri(im, 21, 20, 21, 24, 26, 22, 'O')  # 喙
icons['flappy'] = to_array(im)

# ---------------- Minesweeper 扫雷: 黑雷+引信+火花 ----------------
im = new_img()
rect(im, 2, 26, 37, 37, '.')          # 地面区
for gx in range(2, 38, 6):
    line(im, gx, 26, gx, 37, 'd')
disc(im, 20, 20, 9, 'K')              # 雷体
disc(im, 16, 16, 2, 'd')              # 高光
for a in range(0, 360, 45):           # 刺
    x1 = int(20 + 12 * math.cos(math.radians(a)))
    y1 = int(20 + 12 * math.sin(math.radians(a)))
    line(im, 20, 20, x1, y1, 'K')
line(im, 24, 8, 30, 3, 'S')           # 引信
px(im, 31, 2, 'Y'); px(im, 32, 2, 'R'); px(im, 31, 3, 'O'); px(im, 30, 1, 'Y')  # 火花
icons['minesweeper'] = to_array(im)

# ---------------- Dice 骰子: 五点 ----------------
im = new_img()
rect(im, 5, 5, 34, 34, 'W')
recto(im, 5, 5, 34, 34, 'K')
recto(im, 6, 6, 33, 33, 'S')
for cx, cy in [(13, 13), (26, 13), (20, 20), (13, 26), (26, 26)]:
    disc(im, cx, cy, 3, 'R')
icons['dice'] = to_array(im)

# ---------------- Coin 金币: 金盘+$ ----------------
im = new_img()
disc(im, 20, 20, 15, 'Y')
ring(im, 20, 20, 15, 'O')
ring(im, 20, 20, 12, 'O')
# $ 符号
rect(im, 19, 9, 21, 30, 'K')          # 中竖
rect(im, 14, 11, 26, 13, 'K')         # 上横
rect(im, 14, 18, 26, 20, 'K')         # 中横
rect(im, 14, 26, 26, 28, 'K')         # 下横
rect(im, 14, 11, 16, 20, 'K')         # 左上竖
rect(im, 24, 18, 26, 28, 'K')         # 右下竖
icons['coin'] = to_array(im)

# ---------------- Dino 小恐龙(Chrome 风) ----------------
im = new_img()
dino = [
    "      GGGGGGG  ",
    "     GGGGGGGGG ",
    "     GG GGGGGG ",
    "     GGGGGGGGG ",
    "     GGGG      ",
    "     GGGGGGGG  ",
    "G    GGGG      ",
    "GG  GGGGGG  G  ",
    "GGGGGGGGGG GG  ",
    "GGGGGGGGGGG    ",
    " GGGGGGGGG     ",
    "  GGGGGGG      ",
    "   GG  GG      ",
    "   G   G       ",
    "   GG  GG      ",
]
for ry, row in enumerate(dino):
    for rx, ch in enumerate(row):
        if ch == 'G':
            rect(im, 4 + rx * 2, 2 + ry * 2, 5 + rx * 2, 3 + ry * 2, 'G')
px(im, 12, 5, 'K')  # 眼睛(眼洞)
line(im, 0, 34, 39, 34, '.')  # 地
icons['dino'] = to_array(im)

# ---------------- Cube3D 立方体(等轴测) ----------------
im = new_img()
# 顶面(白): (20,4) (34,12) (20,20) (6,12)
tri(im, 20, 4, 34, 12, 20, 20, 'W')
tri(im, 20, 4, 20, 20, 6, 12, 'W')
# 左面(青): (6,12) (20,20) (20,34) (6,26)
tri(im, 6, 12, 20, 20, 20, 34, 'C')
tri(im, 6, 12, 20, 34, 6, 26, 'C')
# 右面(品红): (34,12) (34,26) (20,34) (20,20)
tri(im, 34, 12, 34, 26, 20, 34, 'M')
tri(im, 34, 12, 20, 34, 20, 20, 'M')
line(im, 20, 4, 34, 12, 'K'); line(im, 34, 12, 34, 26, 'K')
line(im, 34, 26, 20, 34, 'K'); line(im, 20, 34, 6, 26, 'K')
line(im, 6, 26, 6, 12, 'K'); line(im, 6, 12, 20, 4, 'K')
line(im, 20, 20, 20, 34, 'K'); line(im, 6, 12, 20, 20, 'K')
line(im, 34, 12, 20, 20, 'K')
icons['cube3d'] = to_array(im)

# ---------------- Timer 闹钟 ----------------
im = new_img()
disc(im, 14, 8, 3, 'O')   # 左铃
disc(im, 26, 8, 3, 'O')   # 右铃
disc(im, 20, 21, 13, 'O')
disc(im, 20, 20, 11, 'W')
for a in range(0, 360, 90):
    x1 = int(20 + 10 * math.cos(math.radians(a)))
    y1 = int(20 + 10 * math.sin(math.radians(a)))
    px(im, x1, y1, 'K')
line(im, 20, 20, 20, 12, 'K')   # 分针
line(im, 20, 20, 26, 20, 'R')   # 时针
disc(im, 20, 20, 1, 'K')
line(im, 12, 33, 9, 37, 'S'); line(im, 28, 33, 31, 37, 'S')  # 脚
icons['timer'] = to_array(im)

# ---------------- Stopwatch 秒表 ----------------
im = new_img()
rect(im, 17, 2, 23, 5, 'S')   # 表冠
rect(im, 18, 5, 22, 8, 'S')
disc(im, 20, 22, 13, 'B')
disc(im, 20, 22, 11, 'W')
for a in range(0, 360, 30):
    x1 = int(20 + 9 * math.cos(math.radians(a)))
    y1 = int(22 + 9 * math.sin(math.radians(a)))
    px(im, x1, y1, 'd')
line(im, 20, 22, 20, 13, 'R')   # 大针
disc(im, 20, 22, 2, 'K')
icons['stopwatch'] = to_array(im)

# ---------------- Flashlight 手电 ----------------
im = new_img()
tri(im, 30, 6, 38, 2, 38, 14, 'Y')    # 光束上
tri(im, 30, 18, 38, 12, 38, 26, 'Y')  # 光束下
rect(im, 22, 8, 30, 16, 'G')          # 灯头
rect(im, 4, 10, 22, 14, 'S')          # 筒身
rect(im, 10, 8, 14, 10, 'd')          # 开关
icons['flashlight'] = to_array(im)

# ---------------- SysInfo 系统信息: 监视器+柱状图 ----------------
im = new_img()
rect(im, 3, 4, 36, 28, 'B')           # 屏幕框
rect(im, 5, 6, 34, 26, 'K')           # 屏
rect(im, 8, 16, 12, 24, 'G')          # CPU 柱
rect(im, 15, 11, 19, 24, 'Y')         # MEM
rect(im, 22, 19, 26, 24, 'R')         # TEMP
rect(im, 29, 8, 33, 24, 'C')          # BAT
line(im, 6, 25, 34, 25, 'S')          # 基线
rect(im, 16, 29, 24, 31, 'B')         # 支架
rect(im, 11, 32, 29, 34, 'B')
icons['sysinfo'] = to_array(im)

# ---------------- Matrix 矩阵雨 ----------------
im = new_img()
import random
random.seed(42)
for x in range(2, 38, 3):
    head = random.randint(2, 30)
    for k in range(random.randint(4, 10)):
        y = head - k
        if 0 <= y < 40:
            im[y][x] = 'W' if k == 0 else ('G' if k < 3 else 'g')
icons['matrix'] = to_array(im)

# ---------------- Drum 鼓+鼓槌 ----------------
im = new_img()
line(im, 6, 2, 16, 12, 'Y')           # 左槌
line(im, 34, 2, 24, 12, 'Y')          # 右槌
disc(im, 5, 2, 2, 'O'); disc(im, 35, 2, 2, 'O')  # 槌头
rect(im, 8, 14, 32, 34, 'R')          # 鼓身
disc(im, 20, 14, 12, 'W')             # 鼓面
rect(im, 8, 14, 32, 18, 'W')          # 补平鼓面上缘
ring(im, 20, 16, 11, 'd')
for x in range(10, 33, 5):
    px(im, x, 24, 'S'); px(im, x, 30, 'S')  # 鼓钉
icons['drum'] = to_array(im)

# ---------------- Piano 钢琴 ----------------
im = new_img()
for i in range(8):
    rect(im, 2 + i * 5, 6, 5 + i * 5, 34, 'W')
    line(im, 5 + i * 5, 6, 5 + i * 5, 34, 'K')
for i in [0, 1, 3, 4, 5]:
    rect(im, 4 + i * 5, 6, 6 + i * 5, 21, 'K')
icons['piano'] = to_array(im)

# ---------------- Sketch 画板+铅笔 ----------------
im = new_img()
rect(im, 4, 6, 30, 36, 'W')           # 纸
recto(im, 4, 6, 30, 36, 'K')
line(im, 8, 12, 26, 12, 'd')
line(im, 8, 17, 22, 17, 'd')
disc(im, 14, 25, 4, 'R')              # 画的太阳
line(im, 8, 32, 20, 26, 'B')          # 画的山
line(im, 20, 26, 26, 32, 'B')
line(im, 26, 10, 36, 20, 'Y')         # 铅笔杆
line(im, 27, 9, 37, 19, 'Y')
tri(im, 36, 20, 33, 22, 38, 23, 'K')  # 笔尖
icons['sketch'] = to_array(im)

# ---------------- Ruler 尺子 ----------------
im = new_img()
rect(im, 2, 14, 37, 26, 'Y')
recto(im, 2, 14, 37, 26, 'K')
for i, x in enumerate(range(4, 37, 4)):
    h = 8 if i % 2 == 0 else 5
    line(im, x, 14, x, 14 + h, 'K')
icons['ruler'] = to_array(im)

# ---------------- Breathing 呼吸: 同心圆 ----------------
im = new_img()
ring(im, 20, 20, 15, 'g')
ring(im, 20, 20, 11, 'C')
ring(im, 20, 20, 7, 'W')
disc(im, 20, 20, 3, 'W')
icons['breathing'] = to_array(im)

# ---------------- MusicBox 音符 ----------------
im = new_img()
line(im, 14, 6, 14, 26, 'M')          # 左符杆
line(im, 28, 4, 28, 16, 'M')          # 右符杆
rect(im, 14, 4, 28, 8, 'M')           # 符梁
disc(im, 11, 30, 4, 'M')              # 左符头
disc(im, 25, 22, 4, 'M')              # 右符头
icons['musicbox'] = to_array(im)

# ---------------- Water Level 水滴+水位罐 ----------------
im = new_img()
rect(im, 26, 8, 36, 36, 'S')          # 罐
rect(im, 27, 22, 35, 35, 'C')         # 水
line(im, 27, 22, 35, 22, 'W')         # 水面
for y in (14, 20, 26, 32):
    line(im, 24, y, 26, y, 'W')       # 刻度
# 水滴
tri(im, 10, 4, 3, 18, 17, 18, 'C')
disc(im, 10, 18, 7, 'C')
disc(im, 8, 16, 2, 'W')               # 高光
icons['water'] = to_array(im)

# ---------------- Calc 科学计算器(保留微调) ----------------
im = new_img()
rect(im, 8, 2, 31, 37, 'S')
recto(im, 8, 2, 31, 37, 'K')
rect(im, 11, 5, 28, 13, 'C')          # 屏
recto(im, 11, 5, 28, 13, 'K')
line(im, 13, 10, 20, 10, 'K')         # 屏上算式
px(im, 25, 10, 'R')
for ry in range(3):
    for rx in range(3):
        rect(im, 11 + rx * 6, 17 + ry * 6, 15 + rx * 6, 20 + ry * 6, 'B')
        recto(im, 11 + rx * 6, 17 + ry * 6, 15 + rx * 6, 20 + ry * 6, 'K')
rect(im, 11, 35, 15, 36, 'R')         # = 键点缀
icons['calc'] = to_array(im)

# ---------------- FuncCalc 函数计算器: 坐标系+正弦曲线+描点 ----------------
im = new_img()
rect(im, 2, 2, 37, 37, 'K')            # 深色面板
recto(im, 2, 2, 37, 37, 'S')           # 银边框
for gx in range(15, 36, 7):            # 淡网格点
    for gy in range(8, 33, 7):
        px(im, gx, gy, '.')
line(im, 8, 5, 8, 34, 'W')             # y轴
line(im, 5, 19, 36, 19, 'W')           # x轴
tri(im, 8, 3, 6, 7, 10, 7, 'W')        # y轴箭头
tri(im, 38, 19, 34, 17, 34, 21, 'W')   # x轴箭头
for ty in (12, 26):                    # y轴刻度
    px(im, 7, ty, 'W')
prev = None                            # 绿色正弦曲线
for x in range(9, 37):
    y = int(round(19 - 8 * math.sin((x - 9) * math.pi * 2 / 14)))
    if prev is not None:
        line(im, x - 1, prev, x, y, 'G')
    prev = y
disc(im, 23, 11, 2, 'R')               # 描点(峰)
px(im, 22, 10, 'W')                    # 描点高光
icons['funccalc'] = to_array(im)

# ---------------- 文件夹: 游戏手柄(保留) ----------------
im = new_img()
rect(im, 6, 13, 33, 27, 'S')
disc(im, 8, 20, 7, 'S'); disc(im, 31, 20, 7, 'S')
rect(im, 10, 19, 15, 21, 'K')         # 十字
rect(im, 11, 17, 13, 23, 'K')
for cx, cy, ch in [(24, 17, 'R'), (28, 20, 'G'), (24, 23, 'Y'), (20, 20, 'B')]:
    disc(im, cx, cy, 1, ch)
icons['folder_games'] = to_array(im)

# ---------------- 文件夹: 工具扳手(保留) ----------------
im = new_img()
for t in range(0, 19):
    rect(im, 14 + t, 28 - t, 16 + t, 30 - t, 'S')
ring(im, 12, 12, 7, 'S')
disc(im, 12, 12, 5, 'S')
tri(im, 12, 12, 20, 2, 22, 6, ' ')
disc(im, 17, 6, 4, ' ')
icons['folder_tools'] = to_array(im)

# ---------------- 文件夹: 系统齿轮(保留) ----------------
im = new_img()
for k in range(8):
    ang = k * math.pi / 4
    for d in range(11, 16):
        x = int(round(20 + d * math.cos(ang)))
        y = int(round(20 + d * math.sin(ang)))
        rect(im, x - 2, y - 2, x + 2, y + 2, 'S')
disc(im, 20, 20, 11, 'S')
disc(im, 20, 20, 4, ' ')
icons['folder_system'] = to_array(im)

# ---------------- Generic 占位(保留) ----------------
im = new_img()
rect(im, 5, 5, 34, 34, '.')
recto(im, 5, 5, 34, 34, 'd')
px(im, 18, 14, 'W'); px(im, 19, 14, 'W'); px(im, 20, 14, 'W')
px(im, 19, 17, 'W'); px(im, 19, 18, 'W')
px(im, 19, 21, 'W')  # ?
icons['generic'] = to_array(im)

# Output
header_content = "#pragma once\n#include <cstdint>\n\n"
source_content = '#include "generated_icons.h"\n\n'

for name in icons:
    var_name = f"icon_{name}_generated"
    header_content += f"extern const uint16_t {var_name}[1600];\n"
    source_content += f"const uint16_t {var_name}[1600] = {{\n"
    data = icons[name]
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        source_content += "    " + ", ".join([f"0x{x:04x}" for x in chunk]) + ",\n"
    source_content += "};\n\n"

with open(r"c:\Users\awa\Documents\trae_projects\zd\awa\main\apps\utils\icons\generated_icons.h", "w") as f:
    f.write(header_content)
with open(r"c:\Users\awa\Documents\trae_projects\zd\awa\main\apps\utils\icons\generated_icons.cpp", "w") as f:
    f.write(source_content)

print("All icons generated:", len(icons))
