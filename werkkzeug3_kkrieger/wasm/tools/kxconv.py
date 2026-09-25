#!/usr/bin/env python3
# Convert the player data of the .kkrieger beta (Breakpoint 2004) into the
# export dialect of data/kkrieger3383.kx, which is what the wasm player's
# loader and operator handlers understand.
#
#   python3 wasm/tools/kxconv.py kkrieger_beta.kx data/kkrieger_beta_conv.kx
#
# The input is the data block cut out of the unpacked beta executable (see
# wasm/tools/unpack_beta.py). What differs between the two dialects:
#
#  * header: the 2004 export has no BPM / length words and no root table.
#  * e0 (a 2004-only "world" object collecting scene inputs) feeding an f0
#    Viewport with a single size parameter; the later Viewport takes the scene
#    directly and carries camera/clear/fog parameters. e0 becomes Scene Add
#    (same operator index), f0 gets the later parameter block with flag 0x80,
#    a port extension meaning "camera from this scene's Camera op".
#  * Text: 2004 records carry text, font and 16 unused bytes; internal space
#    and line feed parameters did not exist yet.
#  * Monster: 2004 had one attack-event link and no walk style / weapon kind /
#    spawn switch / step spline; those are taken from the matching monsters of
#    the 3383 data (same radii and hit points). The attack events move into
#    the monster slots of the shot Events operator, where the later game
#    looks for them.
#  * KKrieger Para gained the environment argument.
#  * events gained start/end interval fields.
#  * the root is an IPP operator; the player wants Demo roots, so a Demo op
#    is appended and used for intro, menu and game alike.
#  * the credits page gets one more line: the port's author.
#
# Classes that only changed their float encodings, or only gained trailing
# parameters with nothing pushed after them (Rotate, PartSystem), are copied
# byte for byte: the loader decodes with the packing string stored in the
# file and zero-fills the rest. Where strings, splines or the output count
# follow the parameters (Viewport, Print, Monster), the block has to be the
# full current length or everything after it lands in the wrong slot.
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kxread
from kxread import OPC_GETINPUT, OPC_GETLINK, OPC_FLEXINPUT

src, dst = sys.argv[1], sys.argv[2]
raw = open(src, 'rb').read()
d = kxread.read(raw, beta=True)
assert d['end'] <= len(raw)
ops, classes = d['ops'], d['classes']
N = len(ops)

def w_short(v):
    assert 0 <= v <= 32767
    return bytes([v]) if v < 128 else bytes([(v & 127) | 128, v >> 7])
u32 = lambda v: struct.pack('<I', v & 0xffffffff)
f32 = lambda v: struct.pack('<f', v)
def f16(v):
    # the exporter's F16 writer is only needed for the event intervals
    if v == 0.0: return b'\x00'
    if v == 1.0: return b'\x80'
    raise ValueError(v)

# ---- class table --------------------------------------------------------
out_classes = []          # (id, conv, pack)
cls_map = {}              # beta class index -> output class index
for ci, (cid, conv, pack) in enumerate(classes):
    if cid == 0xe0:
        continue          # merged into Scene Add
    if cid == 0xf0:
        # all 24 parameters of the later Viewport: the output count is pushed
        # right after the file's parameters and the handler reads it at 24
        conv, pack = 0x08000118, 'bb' + 'c' * 22
    elif cid == 0x31:
        conv, pack = 0x468a0109, 'eeeecbbcc'
    elif cid == 0x10:
        conv |= 0x00800000
    elif cid == 0x61:
        # Print gained a radius parameter; the text pointer is pushed right
        # after the parameters, so a short block shifts it into "radius"
        conv, pack = 0x00011006, pack + 'f'
    elif cid == 0x11:
        conv, pack = 0xb410021d, 'fffffffffbiiififfbffffffffbib'
    cls_map[ci] = len(out_classes)
    out_classes.append((cid, conv, pack))
scene_add = next(i for i, c in enumerate(out_classes) if c[0] == 0xc1)
for ci, (cid, conv, pack) in enumerate(classes):
    if cid == 0xe0:
        cls_map[ci] = scene_add
demo_cls = len(out_classes)
out_classes.append((0x0d, 0xa5000000, ''))

def out_class(op): return cls_map[op['cls']]

# ---- parameter blocks ---------------------------------------------------
# The same monster types in data/kkrieger3383.kx (identical radii, hit
# points and timings), keyed by (notice radius, charge radius): walk style,
# weapon kind and spawn flag. The speed stays the 2004 value: the player
# restores the 2004 damping for beta data (kkriegergame.cpp, MonsterAI),
# and 3383's retuned speeds (a spider at 16) made them rush 8x too fast.
MONSTER_3383 = {
    (50.0, 50.0): (0, -1, 0, None),
    (32.0, 4.0):  (1, 5, 0, None),
    (32.0, 6.0):  (3, 7, 0, None),
}

def monster_extra(par):
    notice, charge = par[7], par[8]
    return MONSTER_3383.get((notice, charge), (0, -1, 0, None))

def f24(v):
    # inverse of kxread.R.f24 (the exporter's 24 bit float)
    if v == 0.0: return b'\x00'
    if v == 1.0: return b'\x01'
    if v == -1.0: return b'\xff'
    bits = struct.unpack('<I', struct.pack('<f', v))[0]
    bits = (bits + 0x80) & ~0xff            # round to the 24 bits the format keeps
    return bytes([(bits >> 23) & 0xff]) + struct.pack('<H', ((bits >> 16) & 0x8000) | ((bits >> 8) & 0x7fff))

def f24_len(b, p):
    return 1 if b[p] in (0, 1, 0xff) else 3

# appended to the credits page (menu: credits)
CREDITS_EXTRA = '\nPort By MasonDye\n'

walkstyles_used = {}
def params(op):
    cid = op['cid']
    if cid == 0xe0:
        return b''                          # Scene Add has no parameters
    if cid == 0xf0:
        size = op['par'][0]
        flags = 0x80 | 0x07                 # scene camera, game camera, clear colour+z
        return bytes([size, flags]) + b''.join([
            u32(0x00000000),                # clear colour (alpha 0: the text layer is merged by its alpha)
            f32(0), f32(0), f32(0),         # rot
            f32(0), f32(0), f32(0),         # pos
            f32(4096.0), f32(0.125),        # far, near clip
            f32(0), f32(0), f32(1), f32(1), # center, zoom
            u32(0xff000000), f32(4096.0), f32(16.0),    # fog colour, end, start
            f32(0), f32(0),                             # stereo eye distance, focal
            f32(0), f32(0), f32(0), f32(0)])            # sub-rectangle (unused when empty)
    if cid == 0x31:
        r = op['raw']
        text, font = op['strs']
        return r[:14] + f32(0.0) + f32(1.0) + text.encode('latin-1') + b'\0' + font.encode('latin-1') + b'\0' + u32(0)
    if cid == 0x61:
        text = op['strs'][0].encode('latin-1') + b'\0'
        assert op['raw'].endswith(text)
        if op['strs'][0].startswith('.KKRIEGER\nby\n'):          # the credits page
            # two more lines: glyphs a touch smaller and lines closer, so the
            # 15 lines take the height the 13 did (flags, size, space x, space y)
            flags, sx, sy, spx, spy = op['par']
            return (bytes([flags]) + f24(0.45) + f24(0.45) + f24(spx) + f24(-0.10) + b'\0'
                    + text[:-1] + CREDITS_EXTRA.encode('latin-1') + b'\0')
        return op['raw'][:-len(text)] + b'\0' + text     # radius 0 (F24 zero is one byte)
    if cid == 0x11:
        ws, wk, sp, speed = monster_extra(op['par'])
        # 2004 two-monster spider (same radii as the 3383 type 0) walks with style 2
        key = (op['par'][7], op['par'][8])
        walkstyles_used[key] = walkstyles_used.get(key, 0) + 1
        if key == (50.0, 50.0) and walkstyles_used[key] > 1:
            ws = 2
        raw = op['raw']
        if speed is not None:               # parameter 6, after six 24 bit floats
            p = 0
            for _ in range(6): p += f24_len(raw, p)
            raw = raw[:p] + f24(speed) + raw[p + f24_len(raw, p):]
        return raw + bytes([ws]) + struct.pack('<i', wk) + bytes([sp]) + struct.pack('<H', 0)
    return op['raw']

# ---- write --------------------------------------------------------------
o = bytearray()
o += u32(d['song']); o += raw[4:4 + d['song']]; o += b'\0' * ((-d['song']) % 4)
p = 4 + ((d['song'] + 3) & ~3)
o += u32(d['sample']); o += raw[p + 4:p + 4 + d['sample']]; o += b'\0' * ((-d['sample']) % 4)
o += u32(0x590000)                          # 89 BPM, same tune as kkrieger3383
o += u32(0x4000000)                         # 1024 beats
o += w_short(N + 1) + w_short(d['nspl'])
roots = [N, N, N] + [N + 1] * 13
for r in roots: o += w_short(r)
for cid, conv, pack in out_classes:
    o += u32(conv) + bytes([cid]) + pack.encode() + b'\0'
o += u32(0)

graph_ops = ops + [dict(cls=None, cid=0x0d, inputs=[N - 1], links=[], anim=b'\x01')]
for i, op in enumerate(graph_ops):
    ci = demo_cls if op['cid'] == 0x0d else out_class(op)
    conv = out_classes[ci][1]
    o += bytes([ci])
    if conv & OPC_FLEXINPUT:
        o += bytes([len(op['inputs'])])
    else:
        assert len(op['inputs']) == OPC_GETINPUT(conv), (i, hex(op['cid']), op['inputs'], hex(conv))
    for j in op['inputs']:
        o += w_short(i - 1 - j)
# 2004 monsters carried their shot event in a link and fired it themselves;
# the later game fires WeaponShot[weapon kind], filled by the Events operator
# for shots (parameter 0). Its monster slots are empty in the beta.
monster_shots = {}
for op in ops:
    if op['cid'] == 0x11 and op['links'] and op['links'][0] is not None:
        wk = monster_extra(op['par'])[1]
        if wk >= 0:
            monster_shots[wk] = op['links'][0]

for i, op in enumerate(graph_ops):
    ci = demo_cls if op['cid'] == 0x0d else out_class(op)
    conv = out_classes[ci][1]
    links = op['links'][:OPC_GETLINK(conv)] if op['cid'] != 0x11 else []
    if op['cid'] == 0x12 and op['par'][0] == 0:
        links = list(links)
        for wk, ev in monster_shots.items():
            assert links[wk] is None, (wk, links)
            links[wk] = ev
    links += [None] * (OPC_GETLINK(conv) - len(links))
    for l in links:
        o += w_short(0 if l is None else l + 1)
for ci, (cid, conv, pack) in enumerate(out_classes):
    for op in ops:
        if op['cid'] != 0x0d and out_class(op) == ci:
            o += params(op)
for op in graph_ops:
    o += op['anim']
o += w_short(len(d['events']))
rr = kxread.R(raw, d['anim_end'])
rr.short()
for ev in d['events']:
    st = rr.p
    rr.short(); rr.u32(); rr.u32(); rr.f24(); rr.f24(); rr.u8()
    for _ in range(9): rr.f24()
    rr.u32(); rr.short()
    o += raw[st:rr.p] + f16(0.0) + f16(1.0)
o += d['spline_raw']
open(dst, 'wb').write(o)

# self check: the converted file must parse in the 3383 dialect
c = kxread.read(bytes(o))
assert c['end'] == len(o), (c['end'], len(o))
assert c['nops'] == N + 1
print('%s: %d ops (%d + demo root), %d classes, %d events, %d bytes' % (dst, N, N, len(out_classes), len(c['events']), len(o)))
