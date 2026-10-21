#!/usr/bin/env python3
# x20_raw_checks.py - two follow-ups to xbox_led_bisect.py, raw packets only.
#
# 1. PR brightness 0 turned the Xbox LED off and neither d1 09 ff nor
#    brightness 3 brought it back. Armoury Crate sets the Xbox LED's own
#    effect and level with 5a d1 21 02 / 5d d1 20 01; does that relight it?
# 2. The PR writes b3 to zones 1-4 and only the left ring takes the colour.
#    Armoury Crate writes zone 0. Does zone 0 reach both rings? In the PR's
#    two-colour breathe the right ring ran yellow primary / dark secondary,
#    never the palette, so this also tries a zone 0 breathe red / blue.
# No 0xb4, so nothing here is saved to flash. Ends by re-applying the PR's
# own effect through sysfs.
#
#   sudo python3 tests/x20_raw_checks.py
import fcntl, glob, os, sys, time
LED = "/sys/class/leds/asus::keyboard"
SF = 0xc0404806   # HIDIOCSFEATURE(64)
LOG = time.strftime("x20-raw-checks-%Y%m%d-%H%M%S.log")
def log(s):
    print(s)
    with open(LOG, "a") as f: f.write(s + "\n")
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
    a = input(f"    -> {q}\n       Xbox LED on/off, and the rings? ").strip()
    log(f"    ANSWER: {q} => {a}")

log(f"== {time.ctime()}  vendor interface {vendor_hidraw()}")
ask("baseline, nothing written yet")
send(0x5a, 0xd1, 0x21, 0x02, 0x00, 0x08)               # Xbox effect static
send(0x5d, 0xd1, 0x20, 0x01, 0x03, feature=False)      # Xbox level 3
time.sleep(1)
ask("Xbox LED's own effect + level (AC bytes)")
send(0x5a, 0xb3, 0x00, 0x00, 0x00, 0x00, 0xff)         # zone 0, static blue
send(0x5a, 0xb5)
time.sleep(1)
ask("b3 zone 0 static blue + b5 (AC form): which rings are blue?")
send(0x5a, 0xb3, 0x00, 0x01, 0xff, 0x00, 0x00, 0xeb, 0x00, 0x00,
     0x00, 0x00, 0xff)                                 # zone 0, breathe red/blue
send(0x5a, 0xb5)
time.sleep(6)
ask("b3 zone 0 breathe, primary red, secondary blue: each ring, each phase?")
with open(f"{LED}/effect", "w") as f: f.write("static")
log("    wrote effect = static")
time.sleep(1)
ask("PR static again (its own zones 1-4 writes)")
log(f"log saved to {LOG}")
