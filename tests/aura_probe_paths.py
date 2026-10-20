#!/usr/bin/env python3
# aura_probe_paths.py - does the Ally MCU answer the Aura probe on every path?
#
# PR #17's asus_aura_set_feature() sends 0x5d packets as interrupt OUT first,
# then raw OUTPUT, then FEATURE, and reads the answer back with GET_FEATURE.
# Our 2026-10-17 capture answered the probe when sent as a FEATURE report.
# If the MCU only answers on the FEATURE path, the PR's discovery sees the
# stale feature buffer, returns -ENODEV silently, and never registers
# asus::keyboard. This sends the same probe three ways and prints the reply.
#
#   sudo python3 tests/aura_probe_paths.py
import fcntl, glob, os, sys, time

def ioc(nr, ln): return (3 << 30) | (ln << 16) | (ord('H') << 8) | nr
GFEATURE, SFEATURE, SOUTPUT = 0x07, 0x06, 0x0B
SIZE = 64
PROBE = [0x5D, 0x05, 0x20, 0x31, 0x00, 0x20]
CAPS  = [0x5D, 0x9E, 0x01, 0x20]

def nodes():
    out = []
    for p in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        ue = open(p + "/device/uevent").read()
        if "0B05:00001B4C" not in ue.upper(): continue
        phys = [l.split("=",1)[1] for l in ue.splitlines() if l.startswith("HID_PHYS=")]
        out.append(("/dev/" + os.path.basename(p), phys[0] if phys else "?"))
    return out

def get(fd):
    b = bytearray(SIZE); b[0] = 0x5D
    fcntl.ioctl(fd, ioc(GFEATURE, SIZE), b); return bytes(b)

def clear(fd):
    # park a recognisable pattern in the feature buffer so a stale read shows
    pkt = bytes([0x5D, 0x41, 0x53, 0x55, 0x53]) + bytes(SIZE - 5)   # "ASUS" signature query
    fcntl.ioctl(fd, ioc(SFEATURE, SIZE), bytearray(pkt))

def send(fd, how, req):
    pkt = bytes(req) + bytes(SIZE - len(req))
    if how == "write()/intr-OUT":  return os.write(fd, pkt)
    if how == "SET_OUTPUT":        fcntl.ioctl(fd, ioc(SOUTPUT, SIZE), bytearray(pkt)); return SIZE
    if how == "SET_FEATURE":       fcntl.ioctl(fd, ioc(SFEATURE, SIZE), bytearray(pkt)); return SIZE

for dev, phys in nodes():
    try: fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
    except OSError as e: print(f"{dev} ({phys}): cannot open: {e}"); continue
    print(f"== {dev}  {phys}")
    for label, req in (("probe 05", PROBE), ("caps 9e", CAPS)):
        for how in ("write()/intr-OUT", "SET_OUTPUT", "SET_FEATURE"):
            try:
                clear(fd); time.sleep(0.05)
                n = send(fd, how, req); time.sleep(0.1)
                r = get(fd)
                answered = r[1:4] == bytes(req[1:4]) and any(r[4:20])  # firmware rewrites byte 5
                print(f"   {label} via {how:17s} sent={n:>3}  reply={r[:16].hex(' ')}  {'ANSWER' if answered else 'no answer'}")
            except OSError as e:
                print(f"   {label} via {how:17s} -> {e.strerror}")
    os.close(fd)
