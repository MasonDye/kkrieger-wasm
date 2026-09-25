#!/usr/bin/env python3
# Unpack the .kkrieger beta executable (Breakpoint 2004, pno0001.exe from
# kkrieger-beta.zip on scene.org) and cut out its player data.
#
#   python3 -m venv /tmp/kkvenv && /tmp/kkvenv/bin/pip install unicorn
#   /tmp/kkvenv/bin/python wasm/tools/unpack_beta.py pno0001.exe data/kkrieger_beta.kx [image.bin]
#
# The executable is kkrunchy-packed (one section, decompressor stub at the
# entry point). Nothing is run natively: the stub runs inside Unicorn with
# LoadLibraryA / GetProcAddress faked, until the unpacked program makes its
# first real API call. At that point the whole image is in place:
#   * the player data (same export format as the 2004 werkkzeug, older than
#     data/kkrieger3383.kx) sits in the data section,
#   * the operator handler table and all 2004 code are there too, which is
#     what wasm/tools/beta_dis.py disassembles.
# Pass a third argument to keep the image (and <image>.apis, the IAT names).
import struct, sys, os
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX, UC_X86_REG_EIP

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kxread

exe = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
keep = sys.argv[3] if len(sys.argv) > 3 else None

pe = struct.unpack_from('<I', exe, 0x3c)[0]
nsec = struct.unpack_from('<H', exe, pe + 6)[0]
optsz = struct.unpack_from('<H', exe, pe + 20)[0]
opt = pe + 24
entry = struct.unpack_from('<I', exe, opt + 16)[0]
base = struct.unpack_from('<I', exe, opt + 28)[0]
imgsize = struct.unpack_from('<I', exe, opt + 56)[0]
hdrsize = struct.unpack_from('<I', exe, opt + 60)[0]
imp_rva = struct.unpack_from('<I', exe, opt + 96 + 8)[0]

PAGE = 0x1000
align = lambda v: (v + PAGE - 1) & ~(PAGE - 1)
mu = Uc(UC_ARCH_X86, UC_MODE_32)
mu.mem_map(base, align(imgsize))
mu.mem_write(base, exe[:min(hdrsize, len(exe))])
sec = opt + optsz
for i in range(nsec):
    vs, va, rs, ra = struct.unpack_from('<IIII', exe, sec + 8)
    mu.mem_write(base + va, exe[ra:ra + rs])
    sec += 40

TRAP, TRAPSIZE = 0x100000, 0x10000             # fake API entry points (ret)
mu.mem_map(TRAP, TRAPSIZE)
mu.mem_write(TRAP, b'\xc3' * TRAPSIZE)
names = {}

def rd_str(addr):
    return bytes(mu.mem_read(addr, 256)).split(b'\0')[0].decode('latin-1')

d = imp_rva                                     # fill the IAT like the loader
while True:
    oft, ts, fc, name, ft = struct.unpack('<IIIII', bytes(mu.mem_read(base + d, 20)))
    if name == 0: break
    dll, thunk, k = rd_str(base + name), oft or ft, 0
    while True:
        e = struct.unpack('<I', bytes(mu.mem_read(base + thunk + 4 * k, 4)))[0]
        if e == 0: break
        addr = TRAP + 16 * len(names)
        names[addr] = dll + '!' + rd_str(base + e + 2)
        mu.mem_write(base + ft + 4 * k, struct.pack('<I', addr))
        k += 1
    d += 20

STACK = 0x200000
mu.mem_map(STACK, 0x100000)
esp = STACK + 0x100000 - 0x100
mu.mem_write(esp, struct.pack('<I', TRAP + TRAPSIZE - 16))
mu.reg_write(UC_X86_REG_ESP, esp)

modules, state = {}, {}
def stdcall_return(uc, nargs, value):
    sp = uc.reg_read(UC_X86_REG_ESP)
    ret = struct.unpack('<I', bytes(uc.mem_read(sp, 4)))[0]
    uc.reg_write(UC_X86_REG_EAX, value)
    uc.reg_write(UC_X86_REG_ESP, sp + 4 + 4 * nargs)
    uc.reg_write(UC_X86_REG_EIP, ret)

def on_trap(uc, addr, size, user):
    name = names.get(addr, 'exit')
    sp = uc.reg_read(UC_X86_REG_ESP)
    a0, a1 = struct.unpack('<II', bytes(uc.mem_read(sp + 4, 8)))
    if name.endswith('LoadLibraryA'):
        h = 0x10000000 + 0x100000 * len(modules)
        modules[h] = rd_str(a0)
        return stdcall_return(uc, 1, h)
    if name.endswith('GetProcAddress'):
        a = TRAP + 16 * len(names)
        names[a] = modules.get(a0, '?') + '!' + (rd_str(a1) if a1 >= 0x10000 else '#%d' % a1)
        return stdcall_return(uc, 2, a)
    state['stop'] = name
    uc.emu_stop()
mu.hook_add(UC_HOOK_CODE, on_trap, begin=TRAP, end=TRAP + TRAPSIZE - 1)

try:
    mu.emu_start(base + entry, 0)
except UcError as e:
    sys.exit('emulation failed: %s' % e)
print('unpacked; the program starts with %s' % state.get('stop'))

img = bytes(mu.mem_read(base, align(imgsize)))
if keep:
    open(keep, 'wb').write(img)
    with open(keep + '.apis', 'w') as f:
        for a, n in sorted(names.items()):
            pat, i = struct.pack('<I', a), img.find(struct.pack('<I', a))
            while i >= 0:
                f.write('%x %s\n' % (base + i, n.split('!')[1]))
                i = img.find(pat, i + 4)

# the export starts with the song size followed by the V2M tune, whose first
# words are fixed (timediv 96, max time, 1 global parameter block)
sig = struct.pack('<III', 0x60, 0x4e00, 1)
i = img.find(sig)
while i >= 0:
    start = i - 4
    try:
        k = kxread.read(img[start:start + 0x40000], beta=True)
        if k['nops'] > 1000:
            break
    except Exception:
        pass
    i = img.find(sig, i + 1)
else:
    sys.exit('player data not found')
data = img[start:start + k['end']]
open(out, 'wb').write(data)
print('%s: %d bytes at image offset 0x%x, %d operators, %d classes' % (out, len(data), start, k['nops'], len(k['classes'])))
