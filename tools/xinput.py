#!/usr/bin/env python3
"""Drives a running game on an X display with XTest (tests on Xvfb).
usage: xinput.py DISPLAY STEP...   steps:
  sleep:S        wait S seconds
  rel:DX,DY      move the mouse by DX,DY (in small steps: relative mode)
  slow:DX,DY     the same, one pixel at a time (no mouse acceleration)
  home           move far up-left (the game's cursor stops at the corner)
  click          left click
  wheel:N        turn the wheel N notches (negative: up)
  key:NAME       press and release a key (X keysym name: Return, Escape, a...)
"""
import ctypes, sys, time
x11 = ctypes.CDLL("libX11.so.6"); xt = ctypes.CDLL("libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XStringToKeysym.restype = ctypes.c_ulong
dpy = x11.XOpenDisplay(sys.argv[1].encode())
if not dpy: sys.exit("cannot open display " + sys.argv[1])
dpy = ctypes.c_void_p(dpy)
def flush(): x11.XFlush(dpy)
def rel(dx, dy):
    n = max(1, int(max(abs(dx), abs(dy)) / 20))
    for i in range(n):
        xt.XTestFakeRelativeMotionEvent(dpy, int(dx / n), int(dy / n), 0); flush(); time.sleep(0.02)
for step in sys.argv[2:]:
    k, _, v = step.partition(":")
    if k == "sleep": time.sleep(float(v))
    elif k == "rel": dx, dy = map(int, v.split(",")); rel(dx, dy)
    elif k == "slow":
        dx, dy = map(int, v.split(",")); n = max(abs(dx), abs(dy))
        for i in range(n):
            xt.XTestFakeRelativeMotionEvent(dpy, (dx > 0) - (dx < 0) if i < abs(dx) else 0, (dy > 0) - (dy < 0) if i < abs(dy) else 0, 0)
            flush(); time.sleep(0.01)
    elif k == "home":
        for i in range(30): rel(-200, -200)
    elif k == "click":
        xt.XTestFakeButtonEvent(dpy, 1, 1, 0); flush(); time.sleep(0.15)
        xt.XTestFakeButtonEvent(dpy, 1, 0, 0); flush()
    elif k == "wheel":
        b = 5 if int(v) > 0 else 4
        for i in range(abs(int(v))):
            xt.XTestFakeButtonEvent(dpy, b, 1, 0); flush(); time.sleep(0.05)
            xt.XTestFakeButtonEvent(dpy, b, 0, 0); flush(); time.sleep(0.3)
    elif k == "key":
        code = x11.XKeysymToKeycode(dpy, x11.XStringToKeysym(v.encode()))
        xt.XTestFakeKeyEvent(dpy, code, 1, 0); flush(); time.sleep(0.1)
        xt.XTestFakeKeyEvent(dpy, code, 0, 0); flush()
    time.sleep(0.1)
