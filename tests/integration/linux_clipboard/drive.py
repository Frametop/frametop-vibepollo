# Drives vibepollo-clipboard (./vibepollo-clipboard) on a headless sway, checked with wl-copy and wl-paste. See run.sh.
import subprocess, sys, time, zlib, struct

def png(w, h, seed=0, level=0):
    raw = b''.join(b'\x00' + bytes(((x * 7 + y * 3 + seed) & 255) for x in range(w * 3)) for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, level)) + chunk(b'IEND', b''))

fails = 0
def check(ok, what):
    global fails
    print(('ok   ' if ok else 'FAIL ') + what, flush=True)
    fails += not ok

h = subprocess.Popen(['./vibepollo-clipboard'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
def line():
    out = b''
    while not out.endswith(b'\n'):
        c = h.stdout.read(1)
        if not c:
            return None
        out += c
    return out[:-1].decode()
def ask(req, body=b''):
    h.stdin.write(req.encode() + b'\n' + body)
    h.stdin.flush()
    l = line()
    if l and l.startswith('ok '):
        n = int(l[3:])
        return 'ok', h.stdout.read(n) if n else b''
    return l, b''
def paste(t=None):
    return subprocess.run(['wl-paste', '-n'] + (['-t', t] if t else []), capture_output=True, timeout=20).stdout
def types():
    return subprocess.run(['wl-paste', '-l'], capture_output=True, timeout=10).stdout.decode().split()
def copy(data, t=None):
    subprocess.run(['wl-copy'] + (['-t', t] if t else []), input=data, timeout=10, check=True)
    time.sleep(0.3)

check(line() == 'ready', 'ready')
check(ask('get text') == ('none', b''), 'empty clipboard: no text')
copy(b'hello from the pc')
check(ask('get text') == ('ok', b'hello from the pc'), 'reads text another app copied')
check(ask('get image') == ('none', b''), 'text only: no image')
check(ask('set text 15', b'from the frame\n') == ('ok', b''), 'sets text')
check(paste() == b'from the frame\n', 'another app pastes the same bytes')
check('text/plain;charset=utf-8' in types() and 'UTF8_STRING' in types(), 'offers the text types')
check(ask('get text') == ('ok', b'from the frame\n'), 'reads back its own text')
small = png(64, 48)
check(ask('set image %d' % len(small), small) == ('ok', b''), 'sets a PNG image')
check(paste('image/png') == small, 'another app pastes the same PNG')
check(types() == ['image/png'], 'offers image/png only')
check(ask('get text') == ('none', b''), 'its image: no text')
check(ask('get image') == ('ok', small), 'reads back its own image')
other = png(32, 32, 5)
copy(other, 'image/png')
check(ask('get image') == ('ok', other), 'reads an image another app copied')
check(ask('get text') == ('none', b''), 'their image: no text')
copy(b'not a png', 'image/png')
check(ask('get image') == ('none', b''), 'image/png that is no PNG: none')
big = png(2600, 2600)  # stored, not compressed: about 20 MB
print('big image', len(big), 'bytes', flush=True)
t = time.time()
check(ask('set image %d' % len(big), big) == ('ok', b''), 'sets a 20 MB image')
check(paste('image/png') == big, 'another app pastes all of it (%.1f s)' % (time.time() - t))
copy(big, 'image/png')
t = time.time()
check(ask('get image') == ('ok', big), 'reads a 20 MB image another app copied (%.1f s)' % (time.time() - t))
check(ask('set image 9', b'GIF89a...') == ("fail that isn't a PNG image", b''), 'refuses what is no PNG')
check(ask('get text') == ('none', b''), 'still in step after a refusal')
# Two pastes at once of its own big image: both get all of it.
ask('set image %d' % len(big), big)
a = subprocess.Popen(['wl-paste', '-n', '-t', 'image/png'], stdout=subprocess.PIPE)
b = subprocess.Popen(['wl-paste', '-n', '-t', 'image/png'], stdout=subprocess.PIPE)
check(a.communicate(timeout=30)[0] == big and b.communicate(timeout=30)[0] == big, 'two pastes at once both get it')
h.stdin.close()
check(h.wait(timeout=5) == 0, 'ends when its stdin closes')
time.sleep(0.3)
check(paste('image/png') == b'', 'and its copy leaves the clipboard with it')
print('FAILED %d' % fails if fails else 'ALL PASSED')
sys.exit(1 if fails else 0)
