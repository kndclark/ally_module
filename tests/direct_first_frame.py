#!/usr/bin/env python3
# direct_first_frame.py - is the first direct_buffer frame after entering
# direct mode lost?
#
# In direct_colour_map.py an all-red first frame showed the previous Aura
# colour (green); every later frame was right. This parks a known Aura colour
# (blue), enters direct mode, writes red once, asks, writes red again, asks.
#
#   sudo python3 tests/direct_first_frame.py
import time
LED = "/sys/class/leds/asus::keyboard"
def w(attr, val):
    with open(f"{LED}/{attr}", "w") as f: f.write(val)
def frame(px):
    with open(f"{LED}/direct_buffer", "wb") as f: f.write(bytes(px))
def ask(q):
    a = input(f"    -> {q}: ").strip(); print(f"    ANSWER: {q} => {a}")
n = int(open(f"{LED}/led_count").read())
w("brightness", "3"); w("enabled", "true")
w("effect", "static"); w("effects_palette", "#0000ff"); time.sleep(1)
ask("Aura static: rings blue?")
w("effect", "direct"); time.sleep(0.5)
frame((255, 0, 0) * n); time.sleep(1); ask("1st direct frame (red): colour now?")
frame((255, 0, 0) * n); time.sleep(1); ask("2nd direct frame (red again): colour now?")
w("effect", "static"); time.sleep(1); ask("back to static: colour now?")
w("brightness", "0"); print("done, brightness 0")
