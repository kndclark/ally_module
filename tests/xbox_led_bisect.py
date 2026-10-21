#!/usr/bin/env python3
# xbox_led_bisect.py - which PR #17 write turns the X20's Xbox LED off?
#
# After a cold boot with the d1 09 flags at ff the Xbox LED was on; after the
# smoke run and the colour map it was off. This repeats those writes one at a
# time from a lit LED. Whenever a step leaves it off, 5a d1 09 01 ff relights
# it (and the rings) before the next step, so each answer is about one write.
# The two-colour palette step also shows which ring reads which palette entry.
#
#   sudo python3 tests/xbox_led_bisect.py
import fcntl, glob, os, sys, time
LED = "/sys/class/leds/asus::keyboard"
SF = 0xc0404806   # HIDIOCSFEATURE(64)
LOG = time.strftime("xbox-led-bisect-%Y%m%d-%H%M%S.log")
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
    # interface 2: usage page ff31 with report ids 5a/5d/5e
    for d in glob.glob("/sys/bus/hid/devices/0003:0B05:1B4C.*"):
        desc = open(d + "/report_descriptor", "rb").read()
        if desc[:4] == bytes.fromhex("0631ff09") and b"\x85\x5d" in desc:
            return "/dev/" + os.listdir(d + "/hidraw")[0]
    sys.exit("vendor interface not found")
def relight():
    p = bytearray(64); p[:5] = (0x5a, 0xd1, 0x09, 0x01, 0xff)
    fd = os.open(vendor_hidraw(), os.O_RDWR)
    try: fcntl.ioctl(fd, SF, bytes(p))
    finally: os.close(fd)
    log("    feature: 5a d1 09 01 ff (relight)")
def ask(q):
    a = input(f"    -> {q}\n       Xbox LED on/off, and the rings? ").strip()
    log(f"    ANSWER: {q} => {a}")
    if a.lower().startswith("off"):
        relight(); time.sleep(1)
        b = input("       relit: Xbox LED on now? ").strip()
        log(f"    ANSWER: after relight => {b}")

n = int(open(f"{LED}/led_count").read())
log(f"== {time.ctime()}  vendor interface {vendor_hidraw()}  led_count {n}")
ask("baseline, nothing written yet")
w("enabled", "false"); time.sleep(1)          # PR: 5d bd 01 00 00 df 0f 00
ask("enabled=false (PR power packet, keyboard bits 00)")
w("enabled", "true"); time.sleep(1)           # PR: 5d bd 01 ff 00 df 0f 00
ask("enabled=true (keyboard bits ff)")
w("brightness", "3"); time.sleep(1)
ask("brightness 3")
w("brightness", "0"); time.sleep(1)
ask("brightness 0")
w("brightness", "3")
w("effect", "static"); w("effects_palette", "#ff0000"); time.sleep(1)
ask("static #ff0000 (b3 to zones 1-4, then b5 b4 b5)")
w("effects_palette", "#ff0000 #0000ff"); time.sleep(1)
ask("static, two colours #ff0000 #0000ff: which ring shows which colour?")
w("effect", "breathe"); time.sleep(4)
ask("breathe")
w("effect", "star"); time.sleep(4)
ask("star (unsupported, expect a freeze)")
w("effect", "direct"); frame((0, 0, 255) * n); time.sleep(1)
ask("direct, all-blue frame (LampArray)")
w("effect", "static"); time.sleep(1)
ask("static again (leaves direct, releases LampArray)")
log(f"done: effect static, brightness 3. log saved to {LOG}")
