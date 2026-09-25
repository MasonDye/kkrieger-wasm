# Disassemble the 2004 code in the unpacked .kkrieger beta image (see
# unpack_beta.py; keep the image with its third argument). Operator handlers
# are labelled init_XX / exec_XX from the 2004 handler table, API calls from
# <image>.apis.
#
#   KK_BETA_IMAGE=image.bin python beta_dis.py init_f0        # one function
#   KK_BETA_IMAGE=image.bin python beta_dis.py 8151b3 200 x   # 200 lines, don't stop at ret
import struct, sys, os
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

IMAGE = os.environ.get('KK_BETA_IMAGE', 'image.bin')
IMG = open(IMAGE, 'rb').read()
BASE = 0x7c0000
TABLE = 0x83f328

names = {}
p = TABLE - BASE
while True:
    i, a, b = struct.unpack_from('<III', IMG, p)
    if i == 0: break
    names.setdefault(a, []).append('init_%02x' % i)
    names.setdefault(b, []).append('exec_%02x' % i)
    p += 12
try:
    for l in open(IMAGE + '.apis'):
        a, n = l.split()
        names.setdefault(int(a, 16), []).append(n)
except FileNotFoundError:
    pass

def handler(cls, kind):
    p = TABLE - BASE
    while True:
        i, a, b = struct.unpack_from('<III', IMG, p)
        if i == 0: return None
        if i == cls: return a if kind == 'init' else b
        p += 12

def rd32(va): return struct.unpack_from('<I', IMG, va - BASE)[0]
def rdf(va): return struct.unpack_from('<f', IMG, va - BASE)[0]

def dis(va, count=80, stop_ret=True):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    out = []
    for ins in md.disasm(IMG[va - BASE:va - BASE + count * 16], va):
        lab = names.get(ins.address)
        if lab: out.append('%s:' % ','.join(lab))
        note = ''
        for tok in ins.op_str.replace('[', ' ').replace(']', ' ').replace(',', ' ').split():
            if tok.startswith('0x'):
                v = int(tok, 16)
                if v in names: note += ' <' + ','.join(names[v]) + '>'
                elif BASE + 0x1000 <= v < BASE + len(IMG) - 4 and 'dword ptr' in ins.op_str and ('[' + tok + ']') in ins.op_str.replace(' ', ''):
                    f = rdf(v)
                    note += ' ; [%x]=%08x (%g)' % (v, rd32(v), f)
        out.append('  %x: %-7s %s%s' % (ins.address, ins.mnemonic, ins.op_str, note))
        count -= 1
        if count <= 0 or (stop_ret and ins.mnemonic == 'ret'): break
    return '\n'.join(out)

if __name__ == '__main__':
    arg = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 80
    if arg.startswith(('init_', 'exec_')):
        va = handler(int(arg[5:], 16), arg[:4])
    else:
        va = int(arg, 16)
    print(dis(va, n, stop_ret=len(sys.argv) <= 3))
