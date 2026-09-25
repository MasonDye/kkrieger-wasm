# Reader for exported werkkzeug3 player data (.kx), old layout (no flags word,
# 1-byte class ids), following KDoc::Init in werkkzeug3_kkrieger/kdoc.cpp.
import struct

OPC_GETDATA   = lambda c: c & 0xff
OPC_GETINPUT  = lambda c: (c >> 8) & 15
OPC_GETLINK   = lambda c: (c >> 12) & 15
OPC_GETSTRING = lambda c: (c >> 16) & 7
OPC_GETSPLINE = lambda c: (c >> 20) & 7
OPC_BLOB      = 0x00080000
OPC_FLEXINPUT = 0x80000000

class R:
    def __init__(s, b, p=0): s.b = b; s.p = p
    def u8(s): v = s.b[s.p]; s.p += 1; return v
    def u16(s): v = struct.unpack_from('<H', s.b, s.p)[0]; s.p += 2; return v
    def s16(s): v = struct.unpack_from('<h', s.b, s.p)[0]; s.p += 2; return v
    def u32(s): v = struct.unpack_from('<I', s.b, s.p)[0]; s.p += 4; return v
    def s32(s): v = struct.unpack_from('<i', s.b, s.p)[0]; s.p += 4; return v
    def short(s):
        v = s.u8()
        if v & 128: v = (v & 127) | (s.u8() << 7)
        return v
    def f16(s):
        v = s.u8()
        if v == 0: return 0.0
        if v == 0x80: return 1.0
        if v == 0x01: return 0.5
        if v == 0x81: return 0.25
        v = (v << 8) | s.u8()
        vd = ((v & 32768) << 16) | ((((v >> 10) & 31) + 128 - 16) << 23) | ((v & 1023) << 13)
        return struct.unpack('<f', struct.pack('<I', vd))[0]
    def f24(s):
        first = s.u8()
        if first == 0: return 0.0
        if first == 1: return 1.0
        if first == 0xff: return -1.0
        second = s.u16()
        full = (first << 23) | ((second & 32768) << 16) | ((second & 32767) << 8)
        return struct.unpack('<f', struct.pack('<I', full))[0]
    def x16(s): return s.s16() / 4096.0
    def cstr(s):
        e = s.b.index(b'\0', s.p); v = s.b[s.p:e].decode('latin-1'); s.p = e + 1; return v

def read(b, beta=False):
    r = R(b)
    first = struct.unpack_from('<I', b, 0)[0]
    old = bool(first & ~7)
    flags = 2 if old else r.u32()
    if not old and flags & 1: r.p += 32
    song = r.u32(); r.p += (song + 3) & ~3
    sample = 0
    if flags & 2:
        sample = r.u32(); r.p += (sample + 3) & ~3
    if beta:                       # Breakpoint 2004 export: no timing, no root table
        bpm = length = None
        nops = r.short(); nspl = r.short()
        roots = [nops - 1]
    else:
        bpm = r.u32(); length = r.u32()
        nops = r.short(); nspl = r.short()
        roots = [r.short() for _ in range(16)]
    classes = []
    while True:
        conv = struct.unpack_from('<I', b, r.p)[0]
        if conv == 0: r.p += 4; break
        r.p += 4
        cid = r.u8() if old else r.u16()
        pack = r.cstr()
        classes.append((cid, conv, pack))
    ops = []
    for i in range(nops):
        tb = r.u8()
        cid, conv, pack = classes[tb & 0x7f]
        if tb & 0x80: inputs = [i - 1]
        else:
            n = r.u8() if conv & OPC_FLEXINPUT else OPC_GETINPUT(conv)
            inputs = [i - 1 - r.short() for _ in range(n)]
        ops.append(dict(cls=tb & 0x7f, cid=cid, conv=conv, inputs=inputs, links=[], par=None, strs=[], spl=[]))
    for op in ops:
        for _ in range(OPC_GETLINK(op['conv'])):
            d = r.short(); op['links'].append(d - 1 if d else None)
    graph_end = r.p
    for ci, (cid, conv, pack) in enumerate(classes):
        for op in ops:
            if op['cls'] != ci: continue
            raw_start = r.p
            par = []
            for ch in pack:
                t = ch.lower()
                if t == 'g': par.append(round(r.f16(), 5))
                elif t == 'f': par.append(round(r.f24(), 5))
                elif t == 'e': par.append(round(r.x16(), 5))
                elif t == 'i': par.append(r.s32())
                elif t == 's': par.append(r.s16())
                elif t == 'b': par.append(r.u8())
                elif t == 'c': par.append('%08x' % r.u32())
                elif t == 'm': par.append('%06x' % (r.u32() & 0xffffff)); r.p -= 1
                else: par.append('?' + ch)
            op['par'] = par
            op['strs'] = [r.cstr() for _ in range(OPC_GETSTRING(conv))]
            if beta and cid == 0x31: r.p += 16     # 2004 Text op: 16 more bytes after text+font
            op['spl'] = [r.u16() for _ in range(OPC_GETSPLINE(conv))]
            if conv & OPC_BLOB: op['blob'] = r.u32()
            op['raw'] = b[raw_start:r.p]
    params_end = r.p
    CMD = [0,0,1,1,1,1,1,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,16,4,4,2,0,0,0,8]
    for op in ops:
        st = r.p
        while True:
            c = b[r.p]
            r.p += 2 if c >= 0x80 else 1 + (CMD[c] if c < len(CMD) else 0)
            if c == 1: break
        op['anim'] = b[st:r.p]
    anim_end = r.p
    events = []
    try:
        for _ in range(r.short()):
            ev = dict(op=r.short(), start=r.u32(), end=r.u32(), vel=r.f24(), mod=r.f24(), sel=r.u8(),
                      srt=[r.f24() for _ in range(9)], color=r.u32(), spline=r.short())
            if not beta:
                ev['si'] = r.f16(); ev['ei'] = r.f16()
            events.append(ev)
    except Exception as e:
        events.append(('error', str(e)))
    events_end = r.p
    splines = []
    spl_start = r.p
    try:
        for _ in range(nspl):
            j = r.u8(); chans = []
            for _ in range(j & 7):
                chans.append([(r.u8() / 255.0, r.f16()) for _ in range(r.short())])
            splines.append(dict(interp=j >> 3, chans=chans))
    except Exception as e:
        splines.append(('error', str(e)))
    splines_end = r.p
    spline_raw = b[spl_start:r.p]
    for op in ops:
        n = op.get('blob', 0)
        op['blobdata'] = b[r.p:r.p + n]; r.p += n
    return dict(old=old, song=song, sample=sample, bpm=bpm, length=length, nops=nops, nspl=nspl,
                roots=roots, classes=classes, ops=ops, graph_end=graph_end, params_end=params_end,
                anim_end=anim_end, events=events, events_end=events_end, splines=splines,
                splines_end=splines_end, spline_raw=spline_raw, end=r.p, size=len(b))
