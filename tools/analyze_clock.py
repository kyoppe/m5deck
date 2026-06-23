#!/usr/bin/env python3
"""参考画像から目盛り・数字の黒ブロブを検出し、中心相対の座標/角度/半径を出力する。"""
import sys
import numpy as np
from PIL import Image
from scipy import ndimage

PATH = sys.argv[1]
img = Image.open(PATH).convert("L")
a = np.array(img)
H, W = a.shape

# 黒い部分(マーカー/針)を 1 とする
black = a < 100

# 連結成分
lbl, n = ndimage.label(black)
objs = ndimage.find_objects(lbl)
comps = []
for i, sl in enumerate(objs, start=1):
    ys, xs = sl
    mask = lbl[sl] == i
    area = int(mask.sum())
    cy, cx = ndimage.center_of_mass(mask)
    cy += ys.start
    cx += xs.start
    h = ys.stop - ys.start
    w = xs.stop - xs.start
    comps.append(dict(area=area, cx=cx, cy=cy, w=w, h=h,
                      x0=xs.start, y0=ys.start, x1=xs.stop, y1=ys.stop))

# 全黒画素の重心を仮の中心に（針が中心付近で交差するため近い）
ys_all, xs_all = np.where(black)
cx0, cy0 = xs_all.mean(), ys_all.mean()

# 針/ハブ＝大きい成分。マーカーは中くらいで周辺にある。
comps.sort(key=lambda c: -c["area"])
print(f"image {W}x{H}, naive center=({cx0:.0f},{cy0:.0f})")
print("== largest components (likely hands/hub) ==")
for c in comps[:4]:
    print(f"  area={c['area']:6d} c=({c['cx']:.0f},{c['cy']:.0f}) wh=({c['w']}x{c['h']})")

# マーカー候補：面積でフィルタ（針より小さく、ノイズより大きい）
markers = [c for c in comps if 60 < c["area"] < 4000]
# 中心から十分離れているもの
markers = [c for c in markers
           if (c["cx"] - cx0) ** 2 + (c["cy"] - cy0) ** 2 > 80 ** 2]

# 中心は 3/9 のy平均, 12/6 のx平均で推定したいが、まず角度で時刻に割り当て
# 中心を全マーカーの外接中心（x,y範囲の中点）で再推定
xs = [c["cx"] for c in markers]
ys = [c["cy"] for c in markers]
cx = (min(xs) + max(xs)) / 2
cy = (min(ys) + max(ys)) / 2
print(f"== marker-based center=({cx:.0f},{cy:.0f}), {len(markers)} markers ==")

res = []
for c in markers:
    dx = c["cx"] - cx
    dy = c["cy"] - cy
    ang = (np.degrees(np.arctan2(dx, -dy))) % 360  # 12時=0, 時計回り
    hour = int(round(ang / 30)) % 12
    if hour == 0:
        hour = 12
    res.append((hour, ang, dx, dy, c))

res.sort(key=lambda r: r[1])
print("hour |  angle |    dx    dy |  |r|  |  w x h  | area")
for hour, ang, dx, dy, c in res:
    r = (dx * dx + dy * dy) ** 0.5
    print(f" {hour:2d}  | {ang:6.1f} | {dx:6.1f} {dy:6.1f} | {r:5.0f} "
          f"| {c['w']:3d}x{c['h']:3d} | {c['area']}")

# 正規化（rx=3時のdx, ry=12時のdyを基準）。代表値を出す。
def pick(hr):
    cand = [r for r in res if r[0] == hr]
    return cand[0] if cand else None

print("\n== normalized (center-relative, divide by half-extent) ==")
half_w = (max(xs) - min(xs)) / 2
half_h = (max(ys) - min(ys)) / 2
print(f"half_w={half_w:.0f} half_h={half_h:.0f} ratio={half_w/half_h:.3f}")
for hour, ang, dx, dy, c in res:
    print(f" {hour:2d}: nx={dx/half_w:+.3f} ny={dy/half_h:+.3f}")
