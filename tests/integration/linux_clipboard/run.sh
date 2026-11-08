#!/bin/bash
# vibepollo-clipboard against a real compositor (headless sway) in a throwaway container, so
# the desktop's own clipboard stays untouched. From the repo (wlr-protocols checked out):
#   docker run --rm -v "$PWD":/src:ro archlinux/archlinux:base-devel /src/tests/integration/linux_clipboard/run.sh
set -e
pacman -Sy --noconfirm --needed sway wl-clipboard wayland python >/tmp/pacman.log 2>&1
mkdir -p /tmp/clip && cd /tmp/clip
xml=/src/third-party/wlr-protocols/unstable/wlr-data-control-unstable-v1.xml
wayland-scanner client-header $xml wlr-data-control-unstable-v1.h
wayland-scanner private-code $xml wlr-data-control-unstable-v1.c
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -I. -I/src/packaging/linux -o vibepollo-clipboard \
  /src/packaging/linux/vibepollo-clipboard.c wlr-data-control-unstable-v1.c -lwayland-client
cp /usr/bin/sway /tmp/sway  # a copy drops the file capability the container may not grant
export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p -m 700 $XDG_RUNTIME_DIR
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman /tmp/sway -c /dev/null >/tmp/sway.log 2>&1 &
for i in $(seq 100); do ls $XDG_RUNTIME_DIR/wayland-? >/dev/null 2>&1 && break; sleep 0.1; done
ls $XDG_RUNTIME_DIR/wayland-? >/dev/null 2>&1 || { cat /tmp/sway.log; exit 1; }
export WAYLAND_DISPLAY=$(basename $(ls $XDG_RUNTIME_DIR/wayland-? | head -1))
python3 /src/tests/integration/linux_clipboard/drive.py
