# PR #17 Dynamic Lighting on the ASUS ROG Ally X (RC72LA) - test report

Tested 2026-10-19 at PR head `fb149ab7` (6 commits, "leds: Add Dynamic
Lighting class and hid-asus Aura/Slash/AniMe support").

**Setup.** ROG Ally X, board RC72LA, USB `0b05:1b4c`, MCU answering the
0x5d probe. SteamOS kernel `7.2.7-valve2-1-neptune-72`. The PR's
`hid-asus.c`, `hid-ids.h` and `led-class-dynamic.c` built out of tree,
unmodified apart from a shim for the `led_classdev::led_dynamic` back-pointer
(a module cannot extend the kernel's struct, so the class keeps a cdev->ldev
table). `hid-asus` alone bound to all six interfaces; no Ally-specific driver
loaded, same arrangement as linux-unstable master. Repo with the build and
the test scripts: `ally_dynlight`.

## Summary

The generic Aura keyboard path drives the Ally's joystick rings with no
Ally-specific code. Discovery, LampArray binding, palette colours, breathe,
rainbow_cycle, rainbow_wave and pulse all work. Four problems, two of them
make the zone look dead out of the box:

1. **Brightness.** `asus::keyboard` advertises `max_brightness` 255 and the
   callback writes the value raw into the firmware's 0-3 level
   (`5a ba c5 c4 <n>`); nothing is pushed at probe. The node registers at
   255 with the hardware at 0, so the zone is dark until userspace writes 3,
   and writing 255 is out of range (no effect). The sibling
   `asus::kbd_backlight` already models the same level with max 3.
2. **Unsupported effects freeze the rings.** The 0x9e capability query is
   unanswered on this MCU (reply is an echo, byte 4 = 0), so the fallback
   list advertises all 12 effects. star, rain, highlight, laser, ripple,
   comet and flash do nothing *and* the firmware latches: the running
   animation (rainbow, pulse) freezes until a supported mode is written.
3. **`enabled=false` does not turn the rings off.** Only brightness 0 or a
   zeroed colour does on this MCU.
4. **Direct mode: one wrong frame, not isolated.** Twice, the first
   `direct_buffer` frame written after `effect=direct` showed the previous
   Aura colour instead; every later frame was right, and a clean
   static -> direct switch applied its first frame correctly. Both misses
   followed an unsupported-effect freeze (item 2) earlier in the same
   session, so this may be the latched firmware rather than the LampArray
   path. Channel and lamp order are correct (see "Direct mode").

## Probe data

```
5d 05 20 31 00 20                      -> 5d 05 20 31 00 1a 01 00 00 02 23 02 01 00 04 00 ...
                                          GET returns 32 bytes; byte 9 = 0x02 (read as 4-zone), byte 13 = 0x00
5d 9e 01 20 / 5d 9e 01 15              -> echoed back, no answer (byte 4 = 0)
dmesg: Bound ASUS LampArray interface (no hidraw)           [interface 0, page 0x59]
       LampArray direct RGB backend (4 lamps, rid_base=0x00)
       Aura 0x9e capability probe failed: -61, using fallback effect list
       Registered dynamic lighting zones: kbd=1, lightbar=0
       Registered Slash lighting LED: asus::slash           [bogus: report 0x5e exists here]
```

The probe is answered identically whether sent as interrupt OUT, SET_REPORT
OUTPUT or SET_REPORT FEATURE, with no delay needed before the GET (10/10 at
0 ms on all three paths). Effect packets on report 0x5d work; 0x5a is not
required on this device.

## Node as registered

```
zone_type keyboard   led_count 4   direct_buffer 12 bytes
effect_index: static breathe rainbow_cycle rainbow_wave star rain highlight laser ripple pulse comet flash direct
speed_range 0-2   direction_index left right   max_palette_entries 2
power_states_index boot awake sleep shutdown   brightness 255 / 255
```

## Effect walk (brightness 3)

| write | rings |
|---|---|
| brightness 255 | dark |
| brightness 3 | lit |
| static, palette #ff0000 / #00ff00 | solid red / solid green |
| breathe | works |
| rainbow_cycle | works |
| rainbow_wave | works |
| star, rain, highlight, laser, ripple | nothing; running animation freezes |
| pulse | works (this is the MCU's strobe, mode 0x0a) |
| comet, flash | nothing; freezes |
| enabled=false | no change |

Those five working effects are exactly the set the ring firmware supports
(static 0x00, breathing 0x01, cycle 0x02, rainbow 0x03, strobe 0x0a).

## Direct mode (LampArray, 4 lamps)

Channel order and lamp order are correct: pure green, blue and white frames
show as such, and single-lamp frames map

| lamp | position |
|---|---|
| 0 | left stick, bottom ring |
| 1 | left stick, top ring |
| 2 | right stick, bottom ring |
| 3 | right stick, top ring |

Three direct-mode sessions: in two, the first frame after `effect=direct`
showed the colour of the previous Aura effect (an all-red frame showed
green; an all-blue frame showed the yellow-green of a frozen `flash`), and
all later frames were correct. In the third, with a clean `static` (#0000ff)
immediately before, the first red frame applied at once. Leaving `direct`
(`effect=static`) hands the rings back to the Aura colour, visible as a
flash before brightness 0.

## Also worth knowing

- `asus::slash` is registered because the vendor interface exposes report
  0x5e; it controls nothing on this device. Gating Slash on a successful
  Slash-specific probe would avoid it.
- The Xbox Ally X (RC74XA) was not tested in this round. Its interface 0
  exposes the same LampArray report ids 1-6 but on vendor usage page 0xff59
  (Windows binds no driver to it), and its Reactive effect is mode 0x0e,
  not 0x06.
- Hot-unloading `hid-asus` crashed this kernel once during the test; not
  attributed to the PR, pstore not yet read.
