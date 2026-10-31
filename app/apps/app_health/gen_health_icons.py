#!/usr/bin/env python3
"""gen_health_icons.py — 健康 app 11 个指标的苹果风实心字形预渲染器。

88x88 A8 (纯 alpha) 字形, 4x 超采样 + LANCZOS 降采样 = 抗锯齿;
输出 C 数组 (lv_image_dsc_t, LV_COLOR_FORMAT_A8), 显示端配白色 recolor。
字形全部实心填充 (Apple Health 风), 部分带 cutout (火焰/水滴细节)。
"""
import math
from PIL import Image, ImageDraw

SZ = 88          # 目标尺寸
S = 4            # 超采样倍数
W = SZ * S       # 画布


def new_canvas(sz=SZ):
    img = Image.new('L', (sz * S, sz * S), 0)
    return img, ImageDraw.Draw(img)


def bezier(p0, p1, p2, p3, n=64):
    """三次贝塞尔采样点列"""
    pts = []
    for i in range(n + 1):
        t = i / n
        mt = 1 - t
        x = mt**3*p0[0] + 3*mt*mt*t*p1[0] + 3*mt*t*t*p2[0] + t**3*p3[0]
        y = mt**3*p0[1] + 3*mt*mt*t*p1[1] + 3*mt*t*t*p2[1] + t**3*p3[1]
        pts.append((x, y))
    return pts


def thick_polyline(d, pts, width, fill=255):
    """圆头粗折线 (PIL 无圆头 line, 端点/拐点补圆)"""
    d.line(pts, fill=fill, width=width, joint='curve')
    r = width / 2
    for (x, y) in (pts[0], pts[-1]):
        d.ellipse([x-r, y-r, x+r, y+r], fill=fill)


def sc(v):  # 坐标缩放到超采样画布
    return v * S


# ── 11 个字形 ────────────────────────────────────────────────────────────────

def g_steps():
    """步数: 经典脚印 (椭圆脚掌 + 弧排脚趾圆点), 两枚错位"""
    img, d = new_canvas()

    def foot(cx, cy, mirror=False):
        # 脚掌: 竖椭圆
        d.ellipse([sc(cx-8), sc(cy), sc(cx+8), sc(cy+26)], fill=255)
        # 脚趾: 4 个沿弧排布的圆点 (大→小)
        toes = [(-13, 1, 4.5), (-7, -4, 4.0), (0, -6, 3.6), (6, -5, 3.2)]
        if mirror:
            toes = [(13, 1, 4.5), (7, -4, 4.0), (0, -6, 3.6), (-6, -5, 3.2)]
        for tx, ty, tr in toes:
            d.ellipse([sc(cx+tx-tr), sc(cy+ty-tr), sc(cx+tx+tr), sc(cy+ty+tr)], fill=255)

    foot(30, 26)          # 左脚 (脚趾朝左)
    foot(58, 52, True)    # 右脚 (脚趾朝右)
    return img


def g_distance():
    """步行距离: 导航箭头 (Apple location.fill)"""
    img, d = new_canvas()
    pts = [(sc(44), sc(7)), (sc(79), sc(79)),
           (sc(44), sc(60)), (sc(9), sc(79))]
    d.polygon(pts, fill=255)
    return img


def g_heart():
    """心率/静息心率: 经典参数心形 (实心)"""
    img, d = new_canvas()
    pts = []
    for i in range(128):
        t = math.pi * 2 * i / 128
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2*t) - 2 * math.cos(3*t) - math.cos(4*t)
        pts.append((sc(44 + x * 2.35), sc(40 - y * 2.35)))
    d.polygon(pts, fill=255)
    return img


def g_hrv():
    """心率变异: ECG 波形 (粗圆头折线)"""
    img, d = new_canvas()
    pts = [(6, 46), (26, 46), (34, 34), (44, 64), (53, 24), (60, 46), (82, 46)]
    thick_polyline(d, [(sc(x), sc(y)) for x, y in pts], 8 * S)
    return img


def g_oxygen():
    """血氧: 水滴 (圆 + 两条贝塞尔切线到下尖)"""
    img, d = new_canvas()
    cx, cy, r = 44, 36, 25
    d.ellipse([sc(cx-r), sc(cy-r), sc(cx+r), sc(cy+r)], fill=255)
    # 左右切点到短尖端 (44,78) 的贝塞尔 (更胖的水滴)
    left = bezier((sc(cx-r), sc(cy)), (sc(cx-r), sc(cy+14)), (sc(cx-8), sc(cy+26)), (sc(cx), sc(78)))
    right = bezier((sc(cx), sc(78)), (sc(cx+8), sc(cy+26)), (sc(cx+r), sc(cy+14)), (sc(cx+r), sc(cy)))
    poly = left + right
    d.polygon(poly, fill=255)
    return img


def g_breath():
    """呼吸速率: 三条风纹弧 (不交叉, 长短错落, Apple wind 风)"""
    img, d = new_canvas()
    arcs = [
        (10, 78, 24, 7),   # (x0, x1, y, r) 上: 长弧
        (10, 66, 44, 7),   # 中: 中弧
        (10, 52, 64, 7),   # 下: 短弧
    ]
    for x0, x1, y, r in arcs:
        cx = x0 + r
        pts = []
        for i in range(49):
            a = math.pi * i / 48  # 180°→0° (上半圆)
            x = cx + (x1 - x0 - 2*r) / 2 + (r) * math.cos(a * 0)  # placeholder
        # 用椭圆弧: 圆心在弦中点上方, 从左端画到右端 (上凸)
        midx = (x0 + x1) / 2
        rr = (x1 - x0) / 2
        pts = []
        for i in range(49):
            a = math.pi - math.pi * i / 48
            x = midx - rr * math.cos(a)
            y2 = y - 9 * math.sin(a)
            pts.append((sc(x), sc(y2)))
        thick_polyline(d, pts, 6 * S)
    return img


def g_flame():
    """活动能量: 火焰 (外焰贝塞尔 + 内焰 cutout)"""
    img, d = new_canvas()
    tip = (sc(44), sc(5))
    right = bezier(tip, (sc(58), sc(22)), (sc(74), sc(60)), (sc(44), sc(80)))
    left = bezier((sc(44), sc(80)), (sc(14), sc(60)), (sc(30), sc(22)), tip)
    d.polygon(left + right, fill=255)
    # 内焰 cutout (alpha=0 → 透出底色)
    itip = (sc(44), sc(40))
    ir = bezier(itip, (sc(58), sc(50)), (sc(56), sc(70)), (sc(44), sc(72)))
    il = bezier((sc(44), sc(72)), (sc(32), sc(70)), (sc(30), sc(50)), itip)
    # 内焰底部补圆 (更圆润)
    d.ellipse([sc(34), sc(56), sc(54), sc(72)], fill=0)
    d.polygon(il + ir, fill=0)
    return img


def g_exercise():
    """锻炼时间: 圆环 + 双指针 (表盘)"""
    img, d = new_canvas()
    cx, cy, r, w = 44, 44, 33, 8
    d.arc([sc(cx-r), sc(cy-r), sc(cx+r), sc(cy+r)], 0, 360, fill=255, width=w * S)
    # 指针: 向上 + 右下
    thick_polyline(d, [(sc(cx), sc(cy)), (sc(cx), sc(cy-17))], 7 * S)
    thick_polyline(d, [(sc(cx), sc(cy)), (sc(cx+13), sc(cy+9))], 7 * S)
    return img


def g_stairs():
    """爬楼层: 阶梯剪影"""
    img, d = new_canvas()
    pts = [(12, 82), (12, 64), (31, 64), (31, 46), (50, 46),
           (50, 28), (69, 28), (69, 10), (78, 10), (78, 82)]
    d.polygon([(sc(x), sc(y)) for x, y in pts], fill=255)
    return img


def g_temp():
    """手腕温度: 温度计 (圆头管 + 球, 实心)"""
    img, d = new_canvas()
    cx = 44
    d.rounded_rectangle([sc(cx-8), sc(12), sc(cx+8), sc(58)], radius=8 * S, fill=255)
    d.ellipse([sc(cx-15), sc(52), sc(cx+15), sc(82)], fill=255)
    # 管内镂空槽 (读作液柱管)
    d.rounded_rectangle([sc(cx-3), sc(18), sc(cx+3), sc(56)], radius=3 * S, fill=0)
    return img


def g_tile_health():
    """工具页健康 tile: 实心心形 + ECG 镂空穿过 (Apple 健康 App 图标意象, 96x96)"""
    img, d = new_canvas(96)
    pts = []
    for i in range(128):
        t = math.pi * 2 * i / 128
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2*t) - 2 * math.cos(3*t) - math.cos(4*t)
        pts.append((sc(48 + x * 2.5), sc(47 - y * 2.85)))   # x 压扁 y 拉高 = 圆润两瓣
    d.polygon(pts, fill=255)
    # ECG 镂空 (P-QRS-T 完整节奏, 圆头粗线, 不出心形边界)
    ecg = [(16, 48), (28, 48), (34, 42), (40, 48),   # P 波
           (50, 48), (56, 33), (63, 63), (68, 45),   # QRS
           (74, 48), (80, 48)]                       # T 波
    thick_polyline(d, [(sc(x), sc(y)) for x, y in ecg], 8 * S, fill=0)
    return img


GLYPHS = {
    'hl_steps': g_steps,
    'hl_distance': g_distance,
    'hl_heart': g_heart,
    'hl_hrv': g_hrv,
    'hl_oxygen': g_oxygen,
    'hl_breath': g_breath,
    'hl_flame': g_flame,
    'hl_exercise': g_exercise,
    'hl_stairs': g_stairs,
    'hl_temp': g_temp,
    'hl_tile_health': g_tile_health,
}

OUT_C = '/Users/leenzhou/Projects/M5Tab5/M5Tab5-UserDemo-yrobot-cockpit/app/apps/app_health/health_icons.c'
OUT_PNG = '/tmp/hl_icons_preview.png'

rows = []
names = list(GLYPHS.keys())
preview = Image.new('L', (SZ * len(names), SZ), 0)
for idx, name in enumerate(names):
    img0 = GLYPHS[name]()
    sz = img0.width // S
    img = img0.resize((sz, sz), Image.LANCZOS)
    if sz == SZ:
        preview.paste(img, (idx * SZ, 0))
    data = img.tobytes()
    assert len(data) == sz * sz
    rows.append((name, data, sz))

preview.save(OUT_PNG)

with open(OUT_C, 'w') as f:
    f.write("""/*
 * health_icons.c — 健康 app 指标图标 (88x88 A8 抗锯齿实心字形).
 * 由 gen_health_icons.py 预渲染 (PIL 4x 超采样 + LANCZOS), 勿手改;
 * 显示端 lv_image + 白色 recolor (Apple Health 同款观感).
 */
#include <lvgl.h>

""")
    for name, data, sz in rows:
        f.write(f"static const uint8_t {name}_map[{sz * sz}] = {{\n")
        for i in range(0, len(data), 16):
            chunk = ','.join(f'0x{b:02X}' for b in data[i:i+16])
            f.write(f"  {chunk},\n")
        f.write("};\n\n")
        f.write(f"const lv_image_dsc_t {name} = {{\n")
        f.write(f"  .header.cf = LV_COLOR_FORMAT_A8,\n")
        f.write(f"  .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
        f.write(f"  .header.w = {sz},\n  .header.h = {sz},\n")
        f.write(f"  .data_size = {sz * sz},\n  .data = {name}_map,\n}};\n\n")

print(f"wrote {OUT_C}, preview {OUT_PNG}, glyphs: {len(rows)}")
