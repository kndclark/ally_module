#!/usr/bin/env python3
# aura_ring_paths.py - why does PR #17 light nothing on the Ally rings?
#
# Our proven driver (ally_module) programs the rings with 0xb3/0xb5 on report
# 0x5a and sets the hardware brightness with "ba c5 c4 <0-3>" on 0x5a and 0x5d.
# PR #17 sends 0xb3/0xb5/0xb4 on report 0x5d and never touches the brightness
# level. This isolates the two differences, one step at a time, all volatile
# (no 0xb4 flash commit). Watch the rings and answer each prompt.
#
#   sudo python3 tests/aura_ring_paths.py
import fcntl, glob, os, time

def ioc(nr, ln): return (3 << 30) | (ln << 16) | (ord('H') << 8) | nr
SFEATURE = 0x06
SIZE = 64

def vendor_node():
    for p in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        ue = open(p + "/device/uevent").read()
        if "0B05:00001B4C" in ue.upper() and "input2" in ue:
            return "/dev/" + os.path.basename(p)
    raise SystemExit("vendor interface (input2) hidraw not found")

def send(fd, *b):
    pkt = bytearray(bytes(b) + bytes(SIZE - len(b)))
    fcntl.ioctl(fd, ioc(SFEATURE, SIZE), pkt)
    print(f"    sent {bytes(b).hex(' ')}")

def ask(q):
    a = input(f"    -> {q} [y/n/notes]: ").strip()
    print(f"    ANSWER: {q} => {a}")

def effect(fd, rid, r, g, b):
    # b3: zone 0, mode 0 (static), colour, speed 0, direction 0, pad 0, bg 0 0 0
    send(fd, rid, 0xb3, 0x00, 0x00, r, g, b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)
    send(fd, rid, 0xb5)          # latch (volatile); no 0xb4 flash commit here

fd = os.open(vendor_node(), os.O_RDWR)
print("step 1: hardware brightness to max on 0x5a and 0x5d (PR never sets this)")
send(fd, 0x5a, 0xba, 0xc5, 0xc4, 0x03)
send(fd, 0x5d, 0xba, 0xc5, 0xc4, 0x03)
time.sleep(1); ask("rings lit now (any colour)?")

print("step 2: static GREEN via report 0x5d (the PR's path)")
effect(fd, 0x5d, 0, 255, 0); time.sleep(1); ask("rings green?")

print("step 3: static BLUE via report 0x5a (our driver's path)")
effect(fd, 0x5a, 0, 0, 255); time.sleep(1); ask("rings blue?")

print("step 4: static RED via 0x5d again, now that brightness is up")
effect(fd, 0x5d, 255, 0, 0); time.sleep(1); ask("rings red?")

print("step 5: brightness back to 0 on both (dark, as your driver leaves them)")
send(fd, 0x5a, 0xba, 0xc5, 0xc4, 0x00)
send(fd, 0x5d, 0xba, 0xc5, 0xc4, 0x00)
time.sleep(1); ask("rings off?")
os.close(fd)
