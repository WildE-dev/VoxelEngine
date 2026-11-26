# Draws the application icon: a stone-brick block chiselled into a staircase of quarters
# (the engine's shaped blocks), seen isometric, in the game's own texture. Run from the
# repository's root (needs Pillow):  python tools/make_icon.py
# It writes Resources/Icon/icon.png (the window's icon) and src/app/VoxelEngine.ico (the
# executables', see src/app/VoxelEngine.rc).
import math, sys
from PIL import Image

tex = Image.open('Resources/Textures/Blocks/stone_brick.png').convert('RGBA')
T = tex.size[0]
tp = tex.load()
# Quarter heights [qx][qz], the front corner (+x, +z, nearest the eye) lowest.
H = [[1.0, 0.6875], [0.6875, 0.375]]
boxes = [((qx * 0.5, 0.0, qz * 0.5), (qx * 0.5 + 0.5, H[qx][qz], qz * 0.5 + 0.5)) for qx in range(2) for qz in range(2)]

s3 = math.sqrt(3)
d = (-1 / s3, -1 / s3, -1 / s3)
right = (1 / math.sqrt(2), 0.0, -1 / math.sqrt(2))
up = (right[1] * d[2] - right[2] * d[1], right[2] * d[0] - right[0] * d[2], right[0] * d[1] - right[1] * d[0])
centre = (0.5, 0.46, 0.5)
span = 1.80

def shade_at(sx, sy, N):
    o = [centre[i] - d[i] * 4 + sx * right[i] + sy * up[i] for i in range(3)]
    best, hitFace, hitBox = math.inf, 0, None
    for lo, hi in boxes:
        enter, leave, axis = -math.inf, math.inf, 0
        for i in range(3):
            t0 = (lo[i] - o[i]) / d[i]; t1 = (hi[i] - o[i]) / d[i]
            a, b = min(t0, t1), max(t0, t1)
            if a > enter: enter, axis = a, i
            leave = min(leave, b)
        if enter <= leave and 0 < enter < best: best, hitFace, hitBox = enter, axis, (lo, hi)
    if hitBox is None: return None
    x, y, z = (o[i] + best * d[i] for i in range(3))
    lo, hi = hitBox
    near = lambda a, l, h: min(a - l, h - a)
    if hitFace == 1:   u, v, shade, rim = x, z, 1.0, min(near(x, lo[0], hi[0]), near(z, lo[2], hi[2]))          # top
    elif hitFace == 0: u, v, shade, rim = 1 - z, 1 - y, 0.8, min(near(z, lo[2], hi[2]), near(y, lo[1], hi[1]))   # facing +x
    else:              u, v, shade, rim = x, 1 - y, 0.62, min(near(x, lo[0], hi[0]), near(y, lo[1], hi[1]))      # facing +z
    if N <= 32:
        # Small: flat faces (and at 16 a stronger light and dark) and a pixel-wide dark edge (the texture is mush at this size).
        c = (212, 212, 220, 255)
        edge = 1.0 if N <= 16 else (0.32 if rim < 0.75 * span / N else 1.0) # (at 16, the shading alone shows the steps)
    else:
        c = tp[min(T - 1, int(u % 1 * T)), min(T - 1, int(v % 1 * T))]
        edge = min(1.0, rim / (0.02 * 256 / N)) * 0.45 + 0.55   # each face's edges darkened, so the steps read
    if N <= 16: shade = {1.0: 0.95, 0.8: 0.66, 0.62: 0.42}[shade]
    k = shade * edge
    return (int(c[0] * k), int(c[1] * k), int(c[2] * k), 255)

def render(N, ss):
    M = N * ss
    img = Image.new('RGBA', (M, M), (0, 0, 0, 0))
    px = img.load()
    for j in range(M):
        sy = (0.5 - (j + 0.5) / M) * span
        for i in range(M):
            c = shade_at(((i + 0.5) / M - 0.5) * span, sy, N)
            if c: px[i, j] = c
    small = img.resize((N, N), Image.LANCZOS)
    if N <= 16:
        # Its outline darkened, so it stands out on a light background as on a dark one.
        q = small.load()
        solid = [[q[i, j][3] > 128 for j in range(N)] for i in range(N)]
        for i in range(N):
            for j in range(N):
                if not solid[i][j]: continue
                rim = any(not (0 <= i + a < N and 0 <= j + b < N and solid[i + a][j + b]) for a, b in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                if rim:
                    r, g, b2, al = q[i, j]
                    q[i, j] = (int(r * 0.45), int(g * 0.45), int(b2 * 0.45), al)
    return small

if __name__ == '__main__':
    render(256, 2).save('Resources/Icon/icon.png')
    sizes = [16, 24, 32, 48, 64, 128, 256]
    imgs = [render(n, 8 if n <= 48 else 2) for n in sizes]
    imgs[-1].save('src/app/VoxelEngine.ico', sizes=[(n, n) for n in sizes], append_images=imgs[:-1])
    if len(sys.argv) > 1:  # (and blown-up previews of the small sizes, into that folder)
        for n, im in zip(sizes, imgs):
            if n in (16, 32): im.resize((128, 128), Image.NEAREST).save(sys.argv[1] + '/icon%d.png' % n)
