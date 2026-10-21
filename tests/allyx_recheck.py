#!/usr/bin/env python3
# allyx_recheck.py - Ally X checks raised by the Xbox Ally X round.
#
# 1. On the X20 the PR's zones 1-4 are the left ring only; zones 5-8 are the
#    right ring and zone 0 is both. The report suggests writing zone 0 or
#    zones 1-8. Map zones 0-8 here: is zone 0 both rings, what does each of
#    1-4 light, and do 5-8 do anything at all?
# 2. The Ally X's two wrong first direct frames both followed a freeze from
#    an unsupported effect. On the X20 the first frame after a freeze was
#    right. Repeat freeze -> direct -> first frame three times.
# 3. Two-colour palette, static and breathe: which ring shows which colour,
#    per phase. And a one-colour breathe: how long lit, how long dark?
# 4. On the X20, 5a d1 09 01 00 turns the rings off and the MCU keeps it.
#    The PR never sends it. Does the Ally X honour it? Ends with ff.
# 0. The first run (after two long suspends) found the rings dark for every
#    write, PR and raw. If the PR's static green is dark again, send the
#    power flags ff at once: dark-then-lit means the flags were off.
# Raw b3 writes have no 0xb4 (not saved); the sysfs writes go through the
# PR, which saves to flash. ally_module rewrites zone 0 at probe.
#
#   sudo python3 tests/allyx_recheck.py
import fcntl, glob, os, sys, time
LED = "/sys/class/leds/asus::keyboard"
SF = 0xc0404806   # HIDIOCSFEATURE(64)
LOG = time.strftime("allyx-recheck-%Y%m%d-%H%M%S.log")
def log(s):
    print(s)
    with open(LOG, "a") as f: f.write(s + "\n")
def w(attr, val):
    with open(f"{LED}/{attr}", "w") as f: f.write(val)
    log(f"    wrote {attr} = {val}")
def frame(pixels):
    with open(f"{LED}/direct_buffer", "wb") as f: f.write(bytes(pixels))
    log(f"    wrote direct_buffer = {bytes(pixels).hex(' ')}")
def vendor_hidraw():
    # interface 2 (input2) carries the vendor reports 5a/5d/5e
    for p in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        ue = open(p + "/device/uevent").read()
        if "0B05:00001B4C" in ue.upper() and "input2" in ue:
            return "/dev/" + os.path.basename(p)
    sys.exit("vendor interface (input2) not found")
def send(*b):
    p = bytearray(64); p[:len(b)] = b
    fd = os.open(vendor_hidraw(), os.O_RDWR)
    try: fcntl.ioctl(fd, SF, bytes(p))
    finally: os.close(fd)
    log(f"    feature: {bytes(b).hex(' ')}")
    time.sleep(0.2)
def zone(z, r, g, b):
    send(0x5a, 0xb3, z, 0x00, r, g, b)                  # static, not saved
    send(0x5a, 0xb5)
    time.sleep(1)
def ask(q):
    a = input(f"    -> {q}\n       each ring (which part)? ").strip()
    log(f"    ANSWER: {q} => {a}")
    return a

n = int(open(f"{LED}/led_count").read())
log(f"== {time.ctime()}  vendor interface {vendor_hidraw()}  led_count {n}")
ask("before any write: are the rings lit, and what colour?")
w("brightness", "3"); w("enabled", "true")
w("effect", "static"); w("effects_palette", "#00ff00"); time.sleep(1)
if not ask("PR static green: both rings green? (y/n)").lower().startswith("y"):
    send(0x5a, 0xd1, 0x09, 0x01, 0xff); time.sleep(1)
    ask("rings were not green: after 5a d1 09 01 ff, green now?")

log("-- 1. zone map")
zone(0, 0x00, 0x00, 0xff)
ask("raw zone 0 blue: which rings turned blue?")
for z in (1, 2, 3, 4):
    zone(z, 0xff, 0x00, 0x00)
    ask(f"raw zone {z} red: which part turned red since the last step?")
for z in (5, 6, 7, 8):
    zone(z, 0xff, 0xff, 0xff)
    ask(f"raw zone {z} white: anything change since the last step?")

log("-- 2. first direct frame after a freeze")
for i, (rgb, name) in enumerate((((0xff, 0, 0), "red"), ((0, 0xff, 0), "green"),
                                 ((0, 0, 0xff), "blue")), 1):
    w("effect", "rainbow_wave"); time.sleep(3)
    w("effect", "star"); time.sleep(2)                 # unsupported: freezes
    w("effect", "direct"); time.sleep(0.5)
    frame(rgb * n); time.sleep(1)
    ask(f"trial {i}: 1st direct frame after a freeze ({name}): colour now?")
    frame(rgb * n); time.sleep(1)
    ask(f"trial {i}: 2nd frame ({name} again): colour now?")
w("effect", "static"); time.sleep(1)

log("-- 3. palettes")
w("effects_palette", "#ff0000 #0000ff"); time.sleep(1)
ask("static, palette #ff0000 #0000ff: which ring shows which colour?")
w("effect", "breathe"); time.sleep(8)
ask("breathe, same palette: each ring, each phase?")
w("effects_palette", "#ff0000"); time.sleep(8)
ask("breathe, palette #ff0000 only: about how many s lit, how many dark?")

log("-- 4. LED power flags")
w("effect", "static"); time.sleep(1)
send(0x5a, 0xd1, 0x09, 0x01, 0x00); time.sleep(1)
ask("5a d1 09 01 00: rings off?")
send(0x5a, 0xd1, 0x09, 0x01, 0xff); time.sleep(1)
ask("5a d1 09 01 ff: rings back?")

log(f"done: effect static #ff0000, brightness 3, flags ff. log saved to {LOG}")
