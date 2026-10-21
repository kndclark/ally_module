#!/usr/bin/env python3
# x20_followup.py - three loose ends left by x20_raw_checks.py.
#
# 1. After the zone 0 red/blue writes, the PR's static (zones 1-4) left both
#    rings red; before them the right ring was yellow. Guess: the right ring
#    keeps the colours of some zone the PR never writes, and the mode is
#    global. A new PR colour (green) should then reach the left ring only.
#    Zones 5-8 are then tried one at a time to find the right ring's zones.
# 2. Armoury Crate's Xbox LED effect + level packets relit the Xbox LED after
#    the PR's brightness 0, sent together. Which one does it?
# 3. In the colour map, brightness 0 straight after effect=static left the
#    rings lit. Repeat that sequence, then send brightness 0 again.
# The raw b3 writes have no 0xb4; the sysfs writes go through the PR, which
# saves to flash. ally_module rewrites zone 0 and the Xbox LED at probe.
#
#   sudo python3 tests/x20_followup.py
import fcntl, glob, os, sys, time
LED = "/sys/class/leds/asus::keyboard"
SF = 0xc0404806   # HIDIOCSFEATURE(64)
LOG = time.strftime("x20-followup-%Y%m%d-%H%M%S.log")
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
def send(*b, feature=True):
    p = bytearray(64); p[:len(b)] = b
    fd = os.open(vendor_hidraw(), os.O_RDWR)
    try:
        if feature: fcntl.ioctl(fd, SF, bytes(p))
        else: os.write(fd, bytes(p))
    finally: os.close(fd)
    log(f"    {'feature' if feature else 'int-out'}: {bytes(b).hex(' ')}")
    time.sleep(0.2)
def ask(q):
    a = input(f"    -> {q}\n       Xbox LED on/off, and each ring? ").strip()
    log(f"    ANSWER: {q} => {a}")

n = int(open(f"{LED}/led_count").read())
log(f"== {time.ctime()}  vendor interface {vendor_hidraw()}  led_count {n}")
ask("baseline, nothing written yet")

log("-- 1. where the right ring's colours come from")
w("effects_palette", "#00ff00"); time.sleep(1)
ask("PR static green (zones 1-4): left green? right still red?")
for z in (5, 6, 7, 8):
    send(0x5a, 0xb3, z, 0x00, 0x00, 0x00, 0xff)        # zone z, static blue
    send(0x5a, 0xb5)
    time.sleep(1)
    ask(f"b3 zone {z} static blue + b5: what changed since the last step?")

log("-- 2. which packet relights the Xbox LED")
w("brightness", "0"); time.sleep(1)
ask("PR brightness 0 (expect Xbox off, rings off)")
w("brightness", "3"); time.sleep(1)
ask("PR brightness 3 (expect rings back, Xbox still off)")
send(0x5d, 0xd1, 0x20, 0x01, 0x03, feature=False)      # Xbox level 3 only
time.sleep(1)
ask("Xbox level 3 only (int-out 5d d1 20 01 03)")
send(0x5a, 0xd1, 0x21, 0x02, 0x00, 0x08)               # Xbox effect static
time.sleep(1)
ask("then Xbox effect static (feature 5a d1 21 02 00 08)")

log("-- 3. brightness 0 straight after leaving direct")
w("effect", "direct"); frame([0, 0, 0] * n); time.sleep(1)
w("effect", "static"); w("brightness", "0")            # colour map's ending
time.sleep(1)
ask("effect static then brightness 0 at once: rings dark?")
w("brightness", "0"); time.sleep(1)
ask("brightness 0 again, a second later: rings dark?")

w("brightness", "3"); w("effects_palette", "#ff0000")
send(0x5d, 0xd1, 0x20, 0x01, 0x03, feature=False)
send(0x5a, 0xd1, 0x21, 0x02, 0x00, 0x08)
time.sleep(1)
ask("end: brightness 3, PR static red, Xbox level + effect re-sent")
log(f"log saved to {LOG}")
