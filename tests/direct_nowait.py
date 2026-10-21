#!/usr/bin/env python3
# direct_nowait.py - report item 7 retry on the Ally X: is the first
# direct_buffer frame wrong when written with no wait after effect=direct?
#
# Both misses wrote the frame at once after effect=direct; all four hits
# waited 0.5 s. Two lead-ins, each copied from one miss:
#   A  static green, then brightness 3, enabled=true, effect=direct, red frame
#      (direct_colour_map.py:21; that miss showed green)
#   B  pulse yellow, comet, flash (freezes), effect=direct, blue frame
#      (dynlight_smoke.sh:82-88; that miss showed the frozen pulse)
# Six no-wait trials and two 0.5 s controls, interleaved. The log records the
# measured gap between the effect=direct write and the frame write; the
# expected colour goes to the log only, not the prompt.
#
#   sudo python3 tests/direct_nowait.py
import fcntl, glob, os, sys, time
LED = "/sys/class/leds/asus::keyboard"
SF = 0xc0404806   # HIDIOCSFEATURE(64)
LOG = time.strftime("direct-nowait-%Y%m%d-%H%M%S.log")
def logf(s):
    with open(LOG, "a") as f: f.write(s + "\n")
def log(s):
    print(s)
    logf(s)
def w(attr, val):
    with open(f"{LED}/{attr}", "w") as f: f.write(val)
    t = time.monotonic()
    log(f"    wrote {attr} = {val}")
    return t
def frame(pixels):
    t = time.monotonic()
    with open(f"{LED}/direct_buffer", "wb") as f: f.write(bytes(pixels))
    log(f"    wrote direct_buffer = {bytes(pixels).hex(' ')}")
    return t
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
def ask(q):
    a = input(f"    -> {q}\n       ").strip()
    log(f"    ANSWER: {q} => {a}")
    return a

def trial(i, lead, wait):
    log(f"-- trial {i}: lead-in {lead}, wait {wait} s")
    if lead == "A":
        w("effect", "static"); w("effects_palette", "#00ff00"); time.sleep(1)
        w("brightness", "3"); w("enabled", "true")
        rgb, name = (0xff, 0x00, 0x00), "red"
    else:
        w("effect", "static"); w("effects_palette", "#ffff00")
        w("effect", "pulse"); time.sleep(3)
        w("effect", "comet"); w("effect", "flash"); time.sleep(2)
        rgb, name = (0x00, 0x00, 0xff), "blue"
    t0 = w("effect", "direct")
    time.sleep(wait)
    gap = (frame(rgb * n) - t0) * 1000
    logf(f"    gap {gap:.1f} ms; expected {name}")
    time.sleep(1)
    if name not in ask(f"trial {i}: colour now?").lower():
        frame(rgb * n); time.sleep(1)
        ask(f"trial {i}: same frame written again: colour now?")

n = int(open(f"{LED}/led_count").read())
log(f"== {time.ctime()}  vendor interface {vendor_hidraw()}  led_count {n}")
ask("before any write: are the rings lit, and what colour?")
w("brightness", "3"); w("enabled", "true")
w("effect", "static"); w("effects_palette", "#00ff00"); time.sleep(1)
if not ask("PR static green: both rings green? (y/n)").lower().startswith("y"):
    send(0x5a, 0xd1, 0x09, 0x01, 0xff); time.sleep(1)
    ask("rings were not green: after 5a d1 09 01 ff, green now?")

for i, (lead, wait) in enumerate((("A", 0), ("B", 0), ("A", 0.5), ("B", 0.5),
                                  ("A", 0), ("B", 0), ("A", 0), ("B", 0)), 1):
    trial(i, lead, wait)
w("effect", "static"); w("effects_palette", "#00ff00")
log(f"done: effect static #00ff00, brightness 3. log saved to {LOG}")
