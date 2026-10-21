# PR #17 Dynamic Lighting on the ROG Ally X and ROG Xbox Ally X20 - test report

Tested PR head `fb149ab7` (top 6 commits, "leds: Add Dynamic Lighting class
and hid-asus Aura/Slash/AniMe support") on the ROG Ally X (2026-10-19, with
re-checks on 2026-10-21) and the ROG Xbox Ally X (2026-10-20).

**Setup**

- ROG Ally X (board RC72LA) and ROG Xbox Ally X (board RC74XA, MCU firmware
  RC74XA.322). Both are USB `0b05:1b4c`, and on both the MCU (the chip that
  runs the lights) answers the 0x5d probe.
- SteamOS kernel `7.2.7-valve2-1-neptune-72` on both.
- The PR's `hid-asus.c`, `hid-ids.h` and `led-class-dynamic.c`, built out of
  tree. One change: a module can't add the `led_classdev::led_dynamic`
  back-pointer to the kernel's struct, so my build keeps a cdev -> ldev
  table instead. Both builds also had four debug `hid_info` lines that log
  the discovery bytes. Nothing else changed.
- `hid-asus` alone bound all six interfaces, with no Ally-specific driver
  loaded, same as linux-unstable master.
- The build and test scripts are on a test branch of my ally_module
  repo: https://github.com/kndclark/ally_module/tree/DO_NOT_MERGE_ogc_pr17_test

## Summary

The generic Aura keyboard code drives the joystick rings on both devices,
with no Ally-specific code. Discovery, LampArray binding, breathe,
rainbow_cycle, rainbow_wave and pulse work on both. Palette colours work on
both rings of the Ally X, but only on the left ring of the Xbox Ally X.

Both devices:

1. **Brightness range is wrong.** `asus::keyboard` advertises
   `max_brightness` 255, but the callback writes the value straight into the
   firmware's 0-3 level (`5a ba c5 c4 <n>`). The node registers at 255,
   which is out of range: on the Ally X, writing 255 did nothing (on the
   Xbox Ally X, Reactive mode hid this). The probe sends no brightness,
   effect or colour (only the `5d bd` power-enable and zone-activate). On
   two boots the Ally X rings stayed dark until I wrote 3; on a third (the
   reboot after the wake described at the end), they came up solid red
   with nothing written. The sibling
   `asus::kbd_backlight` already models this level with max 3.
2. **Unsupported effects freeze the rings.** Neither MCU answers the 0x9e
   capability query (the probe fails with -61), so the PR falls back to
   advertising all 12 effects. star, rain, highlight, laser, ripple, comet
   and flash do nothing, and the running animation (rainbow, pulse) freezes
   until a supported effect is written.
3. **`enabled=false` doesn't turn the rings off.** Its power packet
   (`5d bd 01 ...`) changed nothing visible on either device. Both MCUs do
   obey feature report `5a d1 09 01 00` (rings off) and `5a d1 09 01 ff`
   (back on), which could back a working `enabled`. On the Xbox Ally X
   these flags also switch the Xbox-button LED, and they survive a power
   cycle. `enabled=true` re-sends the stored effect: on the Xbox Ally X it
   turned the left ring red.

Xbox Ally X only:

4. **The PR's colours only reach the left ring.** Discovery byte 9 is 0x02,
   so the PR writes `b3` to zones 1-4. On this MCU those are the left ring;
   zones 5-8 are the right ring, and zone 0 is both. The right ring follows
   the PR's effect but keeps its old colours. On the Ally X, zone 0 is also
   both rings (zones 1-4 are the four half-rings, and 5-8 do nothing), so
   writing zone 0, or zones 1-8, works on both models.
5. **Brightness 0 turns off the Xbox-button LED, and the PR never turns it
   back on.** That LED has its own level packet, which the PR doesn't send.
6. **Direct mode is half resolution.** LampArray reports 8 lamps, but the
   node exposes 4 entries and the PR spreads them across the 8.

Ally X only:

7. **Direct mode: a frame written right after `effect=direct` is usually
   lost.** On a clean boot, 5 of 6 frames written with no wait were lost:
   the rings kept the effect from before. Both frames written after a
   0.5 s wait were right. Over all my sessions, 9 of 13 no-wait frames
   were lost and 0 of 8 after a 0.5 s wait (if the wait made no
   difference, a split this lopsided would happen about 1 time in 200;
   Fisher's exact test, p = 0.005). Writing the same frame again fixed
   all 7 misses where I tried it. On the Xbox Ally X I tried one no-wait
   frame, and it was right.

Both devices also register a bogus `asus::slash` (see the last section).

## ROG Ally X (RC72LA)

### Probe data

```
5d 05 20 31 00 20                      -> 5d 05 20 31 00 1a 01 00 00 02 23 02 01 00 04 00 ...
                                          GET returns 32 bytes; byte 9 = 0x02 (read as 4-zone), byte 13 = 0x00
5d 9e 01 20 / 5d 9e 01 15              -> echoed back, no answer (byte 4 = 0)
dmesg: Bound ASUS LampArray interface (no hidraw)           [interface 0, page 0x59]
       LampArray direct RGB backend (4 lamps, rid_base=0x00)
       Aura 0x9e capability probe failed: -61, using fallback effect list
       Registered dynamic lighting zones: kbd=1, lightbar=0
       Leaving Aura controller state unchanged
       Registered Slash lighting LED: asus::slash           [bogus: report 0x5e exists here]
```

The probe gets the same answer whether sent as interrupt OUT, SET_REPORT
OUTPUT or SET_REPORT FEATURE, and no delay is needed before the GET (10/10
at 0 ms on all three). Effect packets work on report 0x5d; 0x5a isn't
needed on this device.

### Node as registered

```
zone_type keyboard   led_count 4   direct_buffer 12 bytes
effect_index: static breathe rainbow_cycle rainbow_wave star rain highlight laser ripple pulse comet flash direct
speed_range 0-2   direction_index left right   max_palette_entries 2
power_states_index boot awake sleep shutdown   brightness 255 / 255
```

The Xbox Ally X registers the same node.

### Effect walk (brightness 3)

- brightness 255: dark
- brightness 3: lit
- static, palette #ff0000 / #00ff00: solid red / solid green
- breathe, rainbow_cycle, rainbow_wave: work
- pulse: works (it's the MCU's strobe, mode 0x0a)
- star, rain, highlight, laser, ripple, comet, flash: nothing, and the
  running animation freezes
- enabled=false: no change

Of the 12 advertised effects, these five work: static 0x00, breathing 0x01,
cycle 0x02, rainbow 0x03 and strobe 0x0a. I don't know the Ally X's full
mode list. The Xbox Ally X also has Reactive (0x0e).

### Direct mode (LampArray, 4 lamps)

Colour order and lamp order are right: all-green, all-blue and all-white
frames show correctly, and single-lamp frames light:

- lamp 0: left stick, bottom half
- lamp 1: left stick, top half
- lamp 2: right stick, bottom half
- lamp 3: right stick, top half

The first frame after `effect=direct` (item 7). Every trial switched to
another effect first, so `effect=direct` had to take LampArray control
back.

Clean boot, 8 trials (`tests/direct_nowait.py`):

- Static green, brightness 3, `enabled=true`, `effect=direct`, then an
  all-red frame at once: right once, lost twice (the rings stayed green).
- Static yellow, pulse, comet, flash (pulse freezes), `effect=direct`, then
  an all-blue frame at once: lost 3 times out of 3 (the rings stayed the
  frozen pulse's yellow).
- The same two lead-ins with a 0.5 s wait before the frame: right both
  times.
- Every lost frame showed when I wrote it a second time.

Earlier sessions:

- Lost: `enabled=true`, `effect=direct`, then an all-red frame at once.
  It showed green, the static colour `enabled=true` had just re-sent.
- Lost: pulse, comet, flash, `effect=direct`, then an all-blue frame at
  once. It showed the frozen pulse's yellow-green.
- Right: static #0000ff, `effect=direct`, 0.5 s wait, all-red frame.
- Right, on a fresh boot, three times: `rainbow_wave` -> `star` (freezes)
  -> `direct` -> 0.5 s wait -> all-red, all-green, then all-blue frame.
  The first and second frames were right every time.
- Right after the wake described at the end (static didn't light then),
  the same script: 2 of the 5 no-wait frames I could read were lost (the
  rings kept the previous frame); both 0.5 s frames were right.

Where it may come from (from reading the code, not measured): switching
to a firmware effect releases LampArray (`lamparray_controlled = false`),
so the next push runs `asus_lamparray_take_control_unlocked()` (Aura zone
handoff, then AutonomousMode on, then off) and sends the LampArray update
straight after. `effect=direct` does this itself when a cached frame
exists, so my frame followed it within milliseconds. If the MCU needs a
moment after that handoff, a short delay there or one re-send of the frame
would cover it. I haven't timed how long it needs.

Leaving `direct` (`effect=static`) hands the rings back to the Aura colour,
which shows as a flash before brightness 0.

### Zones and palettes

Raw writes are feature `5a b3 <zone> 00 <rgb>` then `5a b5`, with no `b4`
(so not saved), each on top of the last.

- raw zone 0, blue: both rings blue
- raw zone 1, red: left stick, bottom half red
- raw zone 2, red: left stick, top half red
- raw zone 3, red: right stick, bottom half red
- raw zone 4, red: right stick, top half red
- raw zones 5, 6, 7, 8, white: no visible change
- PR static, palette #ff0000 #0000ff: both rings red
- PR breathe, same palette: both rings breathe red, with blue as the
  background colour
- PR breathe, palette #ff0000: both rings breathe red, no dark phase

Zone *k* is lamp *k*-1 above, so the PR's zones 1-4 are the right set on
this MCU. With a one-entry palette the PR sends black as the second colour.
The Xbox Ally X shows that as a long dark phase; this MCU shows none.

## ROG Xbox Ally X (RC74XA)

### Probe data

```
5d 05 20 31 00 20                      -> 5d 05 20 31 00 1a 01 00 02 02 26 00 01 00 04 00 ...
                                          GET returns 32 bytes; byte 9 = 0x02, byte 13 = 0x00
                                          (same reply my own probe got on Windows)
dmesg: Bound ASUS LampArray interface (no hidraw)           [interface 0, page 0xff59]
       LampArray direct RGB backend (8 lamps, rid_base=0x00)
       Aura 0x9e capability probe failed: -61, using fallback effect list
       Registered dynamic lighting zones: kbd=1, lightbar=0
       Leaving Aura controller state unchanged
       Registered Slash lighting LED: asus::slash
```

Interface 0 carries LampArray report ids 1-6 on vendor page 0xff59. Windows
binds no driver to it; the PR binds it, and direct mode works.

### Effect walk

- brightness 255, then 3: both rings dark
- static #ff0000: left red, right orange-yellow
- static #00ff00: left green, right unchanged
- breathe, rainbow_cycle, rainbow_wave: work on both rings
- pulse: works; left green, right orange
- star, rain, highlight, laser, ripple, comet, flash: nothing, and both
  rings freeze
- enabled=false: no change

Before the first effect write, the rings were still in the mode saved from
Windows: Reactive, which stays dark until the sticks or buttons are used.
The PR doesn't send an effect at probe, so brightness alone shows nothing
here. Reactive is mode 0x0e on this MCU; the PR's `AURA_EFFECT_REACTIVE` is
0x06.

With a one-entry palette, breathe ran about 6 s lit and 10 s dark, because
the PR sends black as the second colour (`b3` bytes 10-12). Armoury Crate
with a black second colour also has a dark phase (about 5 s at High speed),
so this is probably expected. Armoury Crate sends breathe with `b3` byte 9
= 0x01 where the PR sends 0x00; a two-colour breathe with 0x00 still showed
both colours, so that looks harmless.

### Zones: the right ring never gets the PR's colours

The PR writes zones 1-4 (`is_strix_4zone`). Raw writes below are feature
`5a b3 <zone> <mode> <rgb> ...` then `5a b5`, with no `b4` (not saved).

- PR static #ff0000 #0000ff, then breathe, first colour: left red, right
  yellow
- same, second colour: left blue, right dark
- raw zone 0, static blue: both rings blue
- raw zone 0, breathe red / blue: both rings alternate red and blue (I
  didn't see which came first)
- PR static #00ff00: left green, right unchanged (red)
- raw zone 5, static blue: right ring, bottom-left quarter blue
- raw zones 6, then 7: one more right-ring quarter blue each
- raw zone 8: whole right ring blue; bottom-right was last
- the left ring didn't change during the zone 5-8 writes

Zones 1-4 take the PR's colour as one ring (I didn't test them one at a
time). The right ring runs the PR's effect with its own colours:
orange-yellow from Windows at first, then red after the raw zone 0 write.

Writing zone 0 on this device would fix it. Armoury Crate sends zone 0 on
the Xbox Ally X, and the out-of-tree Ally driver I normally run sends zone 0
on both models (they share the PID). Writing zones 1-8 would keep per-zone
control. To tell the two MCUs apart: their discovery replies differ in
bytes 8, 10 and 11, and the LampArray lamp count is 4 vs 8. I don't know
which byte is meant to carry the zone count.

One thing I can't explain: the right ring went from raw-written blue (not
saved) back to the red that the PR's last `b4` had saved. The steps in
between included `direct`, brightness 0 and a PR static write. I didn't
isolate which one caused it.

### The Xbox-button LED

This device has a third light, the Xbox button. Armoury Crate sets its
level with interrupt OUT `5d d1 20 01 <0-3>` and its effect with feature
`5a d1 21 02 <effect> <speed>`. The PR sends neither, but its ring
brightness reaches the LED:

- PR brightness 0 (`5a ba c5 c4 00`): Xbox LED off, rings off
- PR brightness 3: Xbox LED stays off, rings on
- feature `5a d1 09 01 ff` (LED power flags): Xbox LED stays off, rings on
- interrupt OUT `5d d1 20 01 03` alone: Xbox LED on, rings on

Brightness 0 turned the LED off in both runs, and brightness 3 never
brought it back. So once userspace sets brightness 0, the Xbox LED stays
dark until the next power cycle, or until something sends
`5d d1 20 01 <level>`. A fix would be to re-send that packet when
brightness leaves 0, or to expose the button as its own LED. I don't know
whether Armoury Crate's ring brightness 0 does the same on Windows: its
slider sends only `ba c5 c4`, and I didn't watch the Xbox LED during that
capture.

`5a d1 09 01 <flags>` switches the rings and the Xbox LED together: 00
turned both off, and ff turned them back on (but after brightness 0, ff
relit neither). The MCU keeps these flags across a power cycle: with ff the
Xbox LED was lit from power-on, and with 00 it flashed once and stayed off
(one cold boot each). The PR never sends this packet. Its own `enabled`
packet did not relight the Xbox LED with the flags at 00.

### Direct mode (LampArray, 8 lamps behind 4 entries)

All-red, green, blue and white frames showed correctly on both rings.
Single-entry frames light:

- entry 0: left ring, left side
- entry 1: left ring, right side
- entry 2: right ring, left side
- entry 3: right ring, right side

`led_count` is fixed at `ROG_STRIX_4ZONE_KBD_LEDS` (4), and
`asus_lamparray_sample_rgb()` spreads the 4 entries over the 8 lamps, so
each entry lights about half a ring (one time it looked nearer
three-quarters). Taking `led_count` from the LampArray lamp count would give
per-lamp control. Here, a first frame written with no wait after
`effect=direct` was right (see item 7).

Once, brightness 0 written straight after leaving `direct` left the rings
lit. A similar sequence (`direct`, an all-black frame, `effect=static`,
brightness 0, no `enabled` toggle) went dark and stayed dark on a second
brightness 0, so I couldn't reproduce it.

## Also worth knowing

- `asus::slash` is registered on both devices because the vendor interface
  has report 0x5e. Neither device has a Slash display, and I didn't write to
  the node. Gating Slash on a Slash-specific probe would avoid it.
- Hot-unloading `hid-asus` crashed the Ally X's kernel once during testing.
  Cause unknown; I haven't read the saved crash log (pstore) yet.
- With the PR loaded, sleep on the Ally X looked like a hang: the screen
  stopped halfway through dimming and froze. The two suspends I traced were
  not the PR. Before sleeping, the kernel flushes pending writes to disk,
  and a Steam download was writing to the microSD card. The flush took
  790 s, then 3,196 s, and both suspends then finished and woke normally.
- Twice, right after a wake from sleep, the Ally X stopped showing the
  PR's effects:
  - After those two long suspends, the rings stayed dark for every write,
    the PR's and raw `b3` alike, with no driver warnings. That session
    ended before I tried `d1 09 01 ff`.
  - On 2026-10-21, after a 9-hour sleep with nothing written on that boot,
    the rings woke solid red. On wake the kernel reset the MCU's USB
    device (`usb 1-2: reset full-speed USB device`). From the code,
    `asus_aura_restore()` returns early when nothing has been set, so
    nothing was sent, not even the zone-activate the probe sends. After
    that, static green at brightness 3 didn't show, not even after
    `5a d1 09 01 ff`, but direct frames did. hid-asus logged no warnings.
  - Both times a reboot fixed it: brightness 3 then static green turned
    both rings green at once.

  I haven't tested whether re-sending the zone-activate after a USB reset
  fixes it.
