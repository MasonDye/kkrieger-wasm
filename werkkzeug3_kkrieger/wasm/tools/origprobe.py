#!/usr/bin/env python3
# Sample the game state of the original .kkrieger beta running under Wine:
# player life and position, and every monster's position, collider position,
# melee/ranged timers and state, every 50 ms after the game is started.
#
#   KK_BETA_DIR=<dir with pno0001.exe> KK_WINEPREFIX=<fresh prefix> #     python3 wasm/tools/origprobe.py <seconds> > /dev/null   # writes origprobe.json next to the exe
#
# The exe runs as our descendant (PR_SET_CHILD_SUBREAPER keeps the Wine
# process under us after its launcher exits), so /proc/<pid>/mem is readable
# with Yama ptrace_scope 1. Addresses are those of the 2004 player: game
# object pointer at 0x14a0998, player at game+0x14968 (life +0x14, collider
# position at game+0x14948), monster pointer array at game+0x34 (the count at
# +0x38 is only stable between frames: keep samples where it is the full
# count). Monster: matrix translation +0x30, collider +0x234, melee timer
# +0x1e8, ranged timer +0x1ec, state +0x1f0. It presses Return twice
# (intro -> menu -> game) with xdotool, so it needs an X/XWayland display.
# Set ShowCrashDialog=0 under HKCU\Software\Wine\WineDbg in the prefix: the
# beta occasionally crashes in the intro under Wine.
# from /proc/<pid>/mem (allowed for ancestors with Yama ptrace_scope=1).
import subprocess, os, time, struct, sys, glob, json
BETA = os.environ['KK_BETA_DIR']
PREFIX = os.environ['KK_WINEPREFIX']
SHOTS = os.environ.get('KK_SHOTS', BETA)
env = dict(os.environ, WINEPREFIX=PREFIX, WINEDEBUG='err+seh,fixme-all', WINEDLLOVERRIDES='mscoree,mshtml=')
subprocess.run(['wineserver', '-k'], env=env)
time.sleep(1)
import ctypes
ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)          # PR_SET_CHILD_SUBREAPER: orphaned Wine processes stay our descendants
p = subprocess.Popen(['wine', 'pno0001.exe'], cwd=BETA, env=env, stdout=open(BETA + '/origprobe.log', 'w'), stderr=subprocess.STDOUT)
duration = float(sys.argv[1]) if len(sys.argv) > 1 else 20
actions = sys.argv[2] if len(sys.argv) > 2 else ''

def game_pid():
    for d in glob.glob('/proc/[0-9]*'):
        try:
            if b'pno0001' in open(d + '/cmdline', 'rb').read():
                st = open(d + '/status').read()
                return int(d.split('/')[-1])
        except Exception: pass
    return None

def rd(f, a, n):
    f.seek(a); return f.read(n)
u32 = lambda f, a: struct.unpack('<I', rd(f, a, 4))[0]
i32 = lambda f, a: struct.unpack('<i', rd(f, a, 4))[0]
f3 = lambda f, a: struct.unpack('<3f', rd(f, a, 12))
ff = lambda f, a: struct.unpack('<f', rd(f, a, 4))[0]

def shot(name):
    subprocess.run(['import', '-window', win, SHOTS + '/' + name + '.png'])
win = None
for _ in range(60):
    time.sleep(1)
    r = subprocess.run(['xdotool', 'search', '--name', '^kk$'], capture_output=True, text=True)
    if r.stdout.strip(): win = r.stdout.split()[0]; break
pid = game_pid()
print('window', win, 'pid', pid, file=sys.stderr)
mem = open('/proc/%d/mem' % pid, 'rb', 0)
time.sleep(float(os.environ.get('KK_WAIT', '20')))   # precalc + intro running
shot('probe_before')
def key(k):
    subprocess.run(['xdotool', 'windowactivate', '--sync', win]); subprocess.run(['xdotool', 'windowfocus', '--sync', win])
    subprocess.run(['xdotool', 'mousemove', '--window', win, '512', '384'])
    subprocess.run(['xdotool', 'keydown', k]); time.sleep(0.12); subprocess.run(['xdotool', 'keyup', k])
key('Return'); time.sleep(3); shot('probe_menu'); key('Return'); time.sleep(0.5)
t0 = time.time()
out = []
while time.time() - t0 < duration:
    t = time.time() - t0
    try:
        gp = u32(mem, 0x14a0998)
        life = i32(mem, gp + 0x14968 + 0x14)
        arr, cnt = u32(mem, gp + 0x34), i32(mem, gp + 0x38)
        mons = []
        for i in range(min(cnt, 64)):
            m = u32(mem, arr + 4 * i)
            mons.append(dict(pos=f3(mem, m + 0x30), cpos=f3(mem, m + 0x234), mt=ff(mem, m + 0x1e8), rt=ff(mem, m + 0x1ec), st=i32(mem, m + 0x1f0)))
        out.append(dict(t=round(t, 3), life=life, cnt=cnt, gp=gp, ppos=f3(mem, gp + 0x14948), mons=mons))
    except Exception as e:
        out.append(dict(t=round(t, 3), err=str(e)))
    time.sleep(0.05)
shot('probe_end')
json.dump(out, open(BETA + '/origprobe.json', 'w'))
subprocess.run(['wineserver', '-k'], env=env)
print('samples', len(out), file=sys.stderr)
