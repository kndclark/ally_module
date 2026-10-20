#!/usr/bin/env python3
# direct_colour_map.py - what does the LampArray direct path do with each
# channel and each lamp on the Ally rings?
#
# The smoke run wrote an all-blue frame and the rings went yellow-green, so the
# channel order or lamp packing is off somewhere between direct_buffer (R,G,B
# per LED) and the LampArray MultiUpdate report. This writes pure frames and
# single-lamp frames and asks what the rings show.
#
#   sudo python3 tests/direct_colour_map.py
import os, time
LED = "/sys/class/leds/asus::keyboard"
def w(attr, val):
    with open(f"{LED}/{attr}", "w") as f: f.write(val)
def frame(pixels):
    with open(f"{LED}/direct_buffer", "wb") as f: f.write(bytes(pixels))
def ask(q):
    a = input(f"    -> {q} [colour / where]: ").strip(); print(f"    ANSWER: {q} => {a}")

n = int(open(f"{LED}/led_count").read())
w("brightness", "3"); w("enabled", "true"); w("effect", "direct")
print(f"led_count={n}; brightness 3, effect direct")
for name, px in (("RED", (255, 0, 0)), ("GREEN", (0, 255, 0)), ("BLUE", (0, 0, 255)), ("WHITE", (255, 255, 255))):
    frame(px * n); time.sleep(1); ask(f"all lamps {name}: what colour do the rings show?")
for i in range(n):
    px = [0, 0, 0] * n; px[i*3:i*3+3] = (255, 0, 0)
    frame(px); time.sleep(1); ask(f"only lamp {i} red: which ring / which part lit?")
frame([0, 0, 0] * n); w("effect", "static"); w("brightness", "0")
print("done: frame cleared, effect static, brightness 0")
