#!/usr/bin/env python3
# aura_probe_race.py - does an immediate GET after the Aura probe return a
# stale buffer?
#
# PR #17 sends the 0x5d probe as interrupt OUT and reads the reply with a
# control GET_REPORT straight after. Interrupt OUT data is queued for the
# MCU's main loop, so the GET may still see the previous feature buffer. The
# driver's previous packet is the 0x5d "ASUS Tech.Inc." init signature, so a
# stale read starts 5d 41 ... and asus_aura_discover() returns -ENODEV.
#
# For each send path this parks the signature, sends the probe, then GETs
# after 0 / 1 / 5 / 20 / 50 ms, ten rounds each, and counts fresh replies.
#
#   sudo python3 tests/aura_probe_race.py
import fcntl, glob, os, time

def ioc(nr, ln): return (3 << 30) | (ln << 16) | (ord('H') << 8) | nr
GFEATURE, SFEATURE, SOUTPUT = 0x07, 0x06, 0x0B
SIZE = 64
SIG   = bytes([0x5D, 0x41, 0x53, 0x55, 0x53, 0x20, 0x54, 0x65, 0x63, 0x68, 0x2e, 0x49, 0x6e, 0x63, 0x2e, 0x00])
PROBE = bytes([0x5D, 0x05, 0x20, 0x31, 0x00, 0x20])
ROUNDS = 10
DELAYS_MS = (0, 1, 5, 20, 50)

def vendor_node():
    for p in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        ue = open(p + "/device/uevent").read()
        if "0B05:00001B4C" in ue.upper() and "input2" in ue:
            return "/dev/" + os.path.basename(p)
    raise SystemExit("vendor interface (input2) hidraw not found")

def pad(b): return b + bytes(SIZE - len(b))
def get(fd):
    buf = bytearray(SIZE); buf[0] = 0x5D
    fcntl.ioctl(fd, ioc(GFEATURE, SIZE), buf); return bytes(buf)
def set_feature(fd, b): fcntl.ioctl(fd, ioc(SFEATURE, SIZE), bytearray(pad(b)))
def send(fd, how, b):
    if how == "intr-OUT":    os.write(fd, pad(b))
    elif how == "SET_OUTPUT": fcntl.ioctl(fd, ioc(SOUTPUT, SIZE), bytearray(pad(b)))
    else:                     set_feature(fd, b)

dev = vendor_node()
fd = os.open(dev, os.O_RDWR)
print(f"vendor interface: {dev}")
print(f"{'path':11s} {'delay':>6s}  fresh/{ROUNDS}  sample reply")
for how in ("intr-OUT", "SET_OUTPUT", "SET_FEATURE"):
    for d in DELAYS_MS:
        fresh, sample = 0, b""
        for _ in range(ROUNDS):
            set_feature(fd, SIG); time.sleep(0.05)       # driver's init handshake state
            try:
                send(fd, how, PROBE)
            except OSError as e:
                sample = f"send failed: {e.strerror}".encode(); break
            if d: time.sleep(d / 1000)
            r = get(fd)
            if r[1:4] == bytes([0x05, 0x20, 0x31]): fresh += 1
            sample = r
        print(f"{how:11s} {d:>4d}ms  {fresh:>5}/{ROUNDS}     {sample[:12].hex(' ') if isinstance(sample, bytes) else sample}")
os.close(fd)
