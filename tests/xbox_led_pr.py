#!/usr/bin/env python3
# xbox_led_pr.py - does PR #17 touch the X20's Xbox-button LED at all?
#
# The LED came on and stayed on through a cold boot with PR #17 loaded,
# but the PR has no 0xd1 0x09 packet. ally_module sends 5a d1 09 01 ff
# (asusctl's Ally LED power flags) at probe, so the MCU may simply hold
# that. The PR's own power packet is 5d bd 01 <kbd> <bar> df 0f 00, sent
# at probe and on every enabled / power_states write. This flips each one
# and asks what the Xbox LED does. It ends with the flags at 00 so a cold
# boot can show whether the MCU keeps them; --restore sends ff again.
#
#   sudo python3 tests/xbox_led_pr.py
#   sudo python3 tests/xbox_led_pr.py --restore
import fcntl, glob, os, sys, time
LED = "/sys/class/leds/asus::keyboard"
SF = 0xc0404806   # HIDIOCSFEATURE(64)
LOG = time.strftime("xbox-led-pr-%Y%m%d-%H%M%S.log")
def log(s):
    print(s)
    with open(LOG, "a") as f: f.write(s + "\n")
def w(attr, val):
    with open(f"{LED}/{attr}", "w") as f: f.write(val)
    log(f"    wrote {attr} = {val}")
def vendor_hidraw():
    # interface 2: usage page ff31 with report ids 5a/5d/5e
    for d in glob.glob("/sys/bus/hid/devices/0003:0B05:1B4C.*"):
        desc = open(d + "/report_descriptor", "rb").read()
        if desc[:4] == bytes.fromhex("0631ff09") and b"\x85\x5d" in desc:
            return "/dev/" + os.listdir(d + "/hidraw")[0]
    sys.exit("vendor interface not found")
def feat(*b):
    p = bytearray(64); p[:len(b)] = b
    fd = os.open(vendor_hidraw(), os.O_RDWR)
    try: fcntl.ioctl(fd, SF, bytes(p))
    finally: os.close(fd)
    log(f"    feature: {bytes(b).hex(' ')}")
def ask(q):
    a = input(f"    -> {q} [on / off / other]: ").strip(); log(f"    ANSWER: {q} => {a}")

if sys.argv[1:] == ["--restore"]:
    feat(0x5a, 0xd1, 0x09, 0x01, 0xff); ask("after 5a d1 09 01 ff: Xbox LED?")
    sys.exit()

log(f"== {time.ctime()}  vendor interface {vendor_hidraw()}")
ask("baseline, nothing written yet: Xbox LED?")
w("enabled", "false"); time.sleep(1)          # PR: 5d bd 01 00 00 df 0f 00
ask("PR power packet, keyboard bits 00: Xbox LED?")
w("enabled", "true"); time.sleep(1)           # PR: 5d bd 01 ff 00 df 0f 00
ask("PR power packet, keyboard bits ff: Xbox LED?")
feat(0x5a, 0xd1, 0x09, 0x01, 0x00); time.sleep(1)
ask("LED power flags 5a d1 09 01 00: Xbox LED?")
w("enabled", "false"); w("enabled", "true"); time.sleep(1)
ask("PR power packet again, flags still 00: Xbox LED?")
log("done: flags left at 00. Power off, wait 15 s, power on, watch the LED.")
log(f"log saved to {LOG}")
