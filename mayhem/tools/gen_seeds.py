#!/usr/bin/env python3
"""Generate the small, valid seed files under mayhem/<target>/testsuite/.

Run from the repo root: python3 mayhem/tools/gen_seeds.py
The outputs are committed; this script documents how they were made.
"""
import os
import struct
import zlib  # noqa: F401  (kept for parity with other generators)

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def put(target, name, data):
    d = os.path.join(ROOT, target, 'testsuite')
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), 'wb') as f:
        f.write(data)


# ---------------------------------------------------------------- images
def tga(w, h, rle=False, bpp=32, origin_top=False):
    desc = (8 if bpp == 32 else 0) | (0x20 if origin_top else 0)
    hdr = struct.pack('<BBBHHBHHHHBB', 0, 0, 10 if rle else 2, 0, 0, 0, 0, 0, w, h, bpp, desc)
    px = bytes((x * 40 + y * 7) & 0xff for y in range(h) for x in range(w * (bpp // 8)))
    if not rle:
        return hdr + px
    out = b''
    step = bpp // 8
    for i in range(0, len(px), step):
        out += b'\x00' + px[i:i + step]  # raw packets of one pixel
    return hdr + out


def bmp(w, h, bpp):
    pal = b''
    if bpp == 8:
        pal = b''.join(struct.pack('<BBBB', i, 255 - i, i // 2, 0) for i in range(256))
    rowbytes = (w * bpp // 8 + 3) & ~3
    data = b''
    for y in range(h):
        row = bytes((x * 31 + y * 17) & 0xff for x in range(w * bpp // 8))
        data += row + b'\x00' * (rowbytes - len(row))
    off = 54 + len(pal)
    hdr = b'BM' + struct.pack('<IIIIiiHHIIiiII', off + len(data), 0, off, 40, w, h, 1, bpp, 0,
                              len(data), 2835, 2835, 256 if bpp == 8 else 0, 0)
    return hdr + pal + data


def pcx(w, h):
    hdr = struct.pack('<BBBBHHHHHH', 0x0a, 5, 1, 8, 0, 0, w - 1, h - 1, 72, 72)
    hdr += b'\x00' * 48 + b'\x00' + b'\x01' + struct.pack('<HH', w, 1)
    hdr += b'\x00' * (128 - len(hdr))
    body = b''
    for y in range(h):
        for x in range(w):
            v = (x * 13 + y * 5) & 0xff
            if v >= 0xc0:
                body += b'\xc1'
            body += bytes([v])
    pal = b'\x0c' + bytes((i * 3 + c) & 0xff for i in range(256) for c in range(3))
    return hdr + body + pal


# ---------------------------------------------------------------- bsp
def bsp(with_patch):
    ents = b'{\n"classname" "worldspawn"\n"message" "fuzz"\n}\n' \
           b'{\n"classname" "info_player_deathmatch"\n"origin" "0 0 0"\n}\n\x00'
    shaders = struct.pack('<64sii', b'textures/common/caulk', 0, 1)  # CONTENTS_SOLID
    shaders += struct.pack('<64sii', b'textures/common/patch', 0, 1)
    # a 128-unit cube brush centred on the origin: 6 axial planes
    planes = b''
    for axis in range(3):
        for sign in (1, -1):
            n = [0.0, 0.0, 0.0]
            n[axis] = float(sign)
            planes += struct.pack('<4f', n[0], n[1], n[2], 64.0)
    nodes = struct.pack('<iii3i3i', 0, -1, -2, -512, -512, -512, 512, 512, 512)
    leafs = struct.pack('<ii3i3iiiii', 0, 0, -512, -512, -512, 512, 512, 512, 0, 1 if with_patch else 0, 0, 1)
    leafs += struct.pack('<ii3i3iiiii', 1, 1, -512, -512, -512, 512, 512, 512, 0, 0, 0, 1)
    leafsurfaces = struct.pack('<i', 0) if with_patch else b''
    leafbrushes = struct.pack('<i', 0)
    models = struct.pack('<6fiiii', -512, -512, -512, 512, 512, 512, 0, 1 if with_patch else 0, 0, 1)
    brushes = struct.pack('<iii', 0, 6, 0)
    brushsides = b''.join(struct.pack('<ii', p, 0) for p in range(6))
    drawverts = b''
    surfaces = b''
    if with_patch:
        for y in range(3):
            for x in range(3):
                z = 32.0 if (x == 1 and y == 1) else 0.0
                drawverts += struct.pack('<3f2f2f3f4B', x * 64.0 + 100, y * 64.0 + 100, z,
                                         0, 0, 0, 0, 0, 0, 1, 255, 255, 255, 255)
        surfaces = struct.pack('<iiiiiiiiiiii3f9fii', 1, -1, 2, 0, 9, 0, 0, -1, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 3)
    vis = struct.pack('<ii', 2, 1) + b'\x03\x03'
    lumps = [ents, shaders, planes, nodes, leafs, leafsurfaces, leafbrushes, models, brushes,
             brushsides, drawverts, b'', b'', surfaces, b'', b'', vis]
    off = 8 + 17 * 8
    hdr = b'IBSP' + struct.pack('<i', 47)
    body = b''
    for l in lumps:
        while (off + len(body)) % 4:
            body += b'\x00'
        hdr += struct.pack('<ii', off + len(body), len(l))
        body += l
    return hdr + body


# ---------------------------------------------------------------- scripts
MENU = b'''#include "ui/menudef.h"
#define WINDOW_X 16
#define W(x) ((x) * 2)
#if WINDOW_X > 8 && defined(W)
menuDef {
  name "fuzz"
  rect WINDOW_X 0 W(320) $evalint(240 / 2)
  itemDef { name "a" text "Hello \\"there\\"" textscale .25 forecolor 1 1 1 1 action { open main ; } }
}
#elif 0
#error never
#else
#undef W
#endif
'''


def main():
    put('image_tga', 'rgba32.tga', tga(4, 3))
    put('image_tga', 'rgb24_rle.tga', tga(3, 2, rle=True, bpp=24, origin_top=True))
    put('image_bmp', 'rgb24.bmp', bmp(4, 2, 24))
    put('image_bmp', 'pal8.bmp', bmp(4, 2, 8))
    put('image_pcx', 'rle8.pcx', pcx(4, 3))
    put('cm_load', 'box.bsp', bsp(False))
    put('cm_load', 'patch.bsp', bsp(True))
    put('botlib_precomp', 'menu.menu', MENU)
    # msg: byte0 = mode (0 = raw bitstream), then a key long and some ops
    put('msg_delta', 'raw.bin', b'\x00' + struct.pack('<i', 0x1234) + b'\x03hello\x00\x05line\n\x07' + b'\x00' * 16)
    # game_session: byte0 picks the gametype (0 objective, 1 stopwatch, 2 LMS, 3 map vote), then the
    # session/client00.json the server wrote at the previous map change for client 0
    guid = '0123456789ABCDEF0123456789ABCDEF'
    sess = ('{"guid":"%s","sessionTeam":1,"spectatorTime":0,"spectatorState":0,"spectatorClient":0,'
            '"playerType":0,"playerWeapon":3,"playerWeapon2":2,"latchPlayerType":0,"kills":3,"deaths":1,'
            '"muted":0,"ignoreClients1":0,"ignoreClients2":0,"enterTime":0,'
            '"multiview":{"mv1":0,"mv2":0},'
            '"wstats":{"weaponStats":[{"atts":5,"deaths":1,"headshots":1,"hits":3,"kills":1}],"rounds":1},'
            '"restart":{"skillpoints":[10,20,30,40,50,60,70],"medals":[0,1,0,0,0,0,0]}}' % guid).encode()
    put('game_session', 'mapvote.bin', b'\x03' + sess)
    put('game_session', 'objective.bin', b'\x00' + sess)
    put('game_session', 'lms.bin', b'\x02' + sess)
    put('msg_delta', 'oob.bin', b'\x02' + b'\xff\xff\xff\xffgetinfo xxx\n' + b'\x00' * 12)


if __name__ == '__main__':
    main()
