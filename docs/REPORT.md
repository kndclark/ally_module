# PR #17 Dynamic Lighting on the ROG Ally X and ROG Xbox Ally X - test report

Tested at PR head `fb149ab7` (6 commits, "leds: Add Dynamic Lighting class
and hid-asus Aura/Slash/AniMe support"): ROG Ally X on 2026-10-19, ROG Xbox
Ally X on 2026-10-20.

**Setup.** ROG Ally X (board RC72LA) and ROG Xbox Ally X (board RC74XA, MCU
firmware RC74XA.322). Both are USB `0b05:1b4c` with an MCU that answers the
0x5d probe. SteamOS kernel `7.2.7-valve2-1-neptune-72` on both. The PR's
`hid-asus.c`, `hid-ids.h` and `led-class-dynamic.c` built out of tree,
unmodified apart from a shim for the `led_classdev::led_dynamic` back-pointer
(a module cannot extend the kernel's struct, so the class keeps a cdev->ldev
table). The Xbox Ally X build also logged the discovery bytes (two `hid_info`
lines, nothing else changed). `hid-asus` alone bound to all six interfaces;
no Ally-specific driver loaded, same arrangement as linux-unstable master.
Repo with the build and the test scripts: `ally_dynlight`.

## Summary

The generic Aura keyboard path drives the joystick rings on both devices
with no Ally-specific code. Discovery, LampArray binding, breathe,
rainbow_cycle, rainbow_wave and pulse work on both; palette colours work
fully on the Ally X and on one ring of the Xbox Ally X.

On both devices:

1. **Brightness.** `asus::keyboard` advertises `max_brightness` 255 and the
   callback writes the value raw into the firmware's 0-3 level
   (`5a ba c5 c4 <n>`); nothing is pushed at probe. The node registers at
   255, and writing 255 is out of range (no effect). On the Ally X the
   hardware sat at 0, so the zone stayed dark until userspace wrote 3. The
   sibling `asus::kbd_backlight` already models the same level with max 3.
2. **Unsupported effects freeze the rings.** The 0x9e capability query is
   unanswered on both MCUs (probe fails with -61), so the fallback list
   advertises all 12 effects. star, rain, highlight, laser, ripple, comet
   and flash do nothing *and* the firmware latches: the running animation
   (rainbow, pulse) freezes until a supported mode is written.
3. **`enabled=false` does not turn the rings off.** The power packet it
   sends (`5d bd 01 ...`) changed nothing visible on either device. Both
   MCUs do obey feature `5a d1 09 01 00` (rings off) and `5a d1 09 01 ff`
   (back on), which would make a working `enabled`; on the Xbox Ally X the
   same flags also gate the Xbox-button LED and survive a power cycle.

On the Xbox Ally X only:

4. **The PR's colours reach the left ring only.** With discovery byte 9 =
   0x02 the PR writes `b3` to zones 1-4. On this MCU those are the left
   ring; zones 5-8 are the right ring, and zone 0 sets both. The right ring
   follows the PR's effect but keeps the colours it last had. Zone 0 sets
   both rings on the Ally X too, where zones 1-4 are the four half-rings
   and 5-8 change nothing, so writing zone 0 (or zones 1-8) is safe on
   both models.
5. **Brightness 0 turns the Xbox-button LED off, and the PR never turns it
   back on.** That LED has its own level packet, which the PR doesn't send.
6. **Direct mode is half resolution.** LampArray reports 8 lamps; the node
   exposes 4 entries, which the PR spreads across them.

On the Ally X only:

7. **Direct mode: a wrong first frame, not reproduced.** Twice in the
   first session, the first `direct_buffer` frame written after
   `effect=direct` showed the previous Aura colour instead; every later
   frame was right. Both misses followed an unsupported-effect freeze
   (item 2), but three deliberate freeze -> direct trials on a fresh boot
   applied the first frame correctly every time, as did four direct
   sessions on the Xbox Ally X. Probably nothing to fix.

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
       Registered Slash lighting LED: asus::slash           [bogus: report 0x5e exists here]
```

The probe is answered identically whether sent as interrupt OUT, SET_REPORT
OUTPUT or SET_REPORT FEATURE, with no delay needed before the GET (10/10 at
0 ms on all three paths). Effect packets on report 0x5d work; 0x5a is not
required on this device.

### Node as registered

```
zone_type keyboard   led_count 4   direct_buffer 12 bytes
effect_index: static breathe rainbow_cycle rainbow_wave star rain highlight laser ripple pulse comet flash direct
speed_range 0-2   direction_index left right   max_palette_entries 2
power_states_index boot awake sleep shutdown   brightness 255 / 255
```

The Xbox Ally X registers the same node.

### Effect walk (brightness 3)

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

### Direct mode (LampArray, 4 lamps)

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

Re-check on a fresh boot: three times, `rainbow_wave` -> `star` (freezes)
-> `direct` -> an all-red, all-green, then all-blue frame. The first frame
was right every time, and so was the second.

### Zones and palettes

Raw writes are feature `5a b3 <zone> 00 <rgb>` then `5a b5`, with no `b4`
(not saved), each on top of the last.

| write | result |
|---|---|
| raw zone 0, blue | both rings blue |
| raw zone 1, red | left stick, bottom half red |
| raw zone 2, red | left stick, top half red |
| raw zone 3, red | right stick, bottom half red |
| raw zone 4, red | right stick, top half red |
| raw zones 5, 6, 7, 8, white | no visible change |
| PR static, palette #ff0000 #0000ff | both rings red |
| PR breathe, same palette | both rings: red primary, blue secondary |
| PR breathe, palette #ff0000 | both rings breathe red, no dark phase |

Zone *k* is lamp *k*-1 of the LampArray order above, so the PR's zones 1-4
are the right set on this MCU. With a one-entry palette the PR sends black
as the second colour; the Xbox Ally X shows that as a long dark phase, this
MCU shows none.

## ROG Xbox Ally X (RC74XA)

### Probe data

```
5d 05 20 31 00 20                      -> 5d 05 20 31 00 1a 01 00 02 02 26 00 01 00 04 00 ...
                                          GET returns 32 bytes; byte 9 = 0x02, byte 13 = 0x00
                                          (same reply as captured under Armoury Crate on Windows)
dmesg: Bound ASUS LampArray interface (no hidraw)           [interface 0, page 0xff59]
       LampArray direct RGB backend (8 lamps, rid_base=0x00)
       Aura 0x9e capability probe failed: -61, using fallback effect list
       Registered dynamic lighting zones: kbd=1, lightbar=0
       Leaving Aura controller state unchanged
       Registered Slash lighting LED: asus::slash
```

Interface 0 carries LampArray report ids 1-6 on vendor page 0xff59
(Windows binds no driver to it); the PR binds it and direct mode works.

### Effect walk

| write | left ring | right ring |
|---|---|---|
| brightness 255, then 3 | dark | dark |
| static #ff0000 | red | orange-yellow |
| static #00ff00 | green | unchanged |
| breathe | breathes | breathes |
| rainbow_cycle | colour cycle | colour cycle |
| rainbow_wave | rainbow | rainbow |
| star, rain, highlight, laser, ripple | nothing; freezes | nothing; freezes |
| pulse | works, green | works, orange |
| comet, flash | nothing; freezes | nothing; freezes |
| enabled=false | no change | no change |

Before the first effect write the rings were still in the mode saved from
Windows (Reactive, dark until a button is pressed): the PR leaves the
controller state alone at probe, so brightness alone shows nothing here.
Reactive is mode 0x0e on this MCU; the PR's `AURA_EFFECT_REACTIVE` is 0x06.

With a one-entry palette, breathe ran about 6 s lit and 10 s dark. The PR
sends black as the second colour (`b3` bytes 10-12) in that case, and
Armoury Crate gives the same dark phase when its second colour is set to
black, so this is probably expected. Armoury Crate sends breathe with
`b3` byte 9 = 0x01 where the PR sends 0x00; a two-colour breathe with 0x00
still showed both colours, so that difference looks harmless.

### Zones: the right ring never gets the PR's colours

The PR writes zones 1-4 (`is_strix_4zone`). Raw writes below are feature
`5a b3 <zone> <mode> <rgb> ...` then `5a b5`, with no `b4` (not saved).

| write | left ring | right ring |
|---|---|---|
| PR static #ff0000 #0000ff, then breathe: first colour's phase | red | yellow |
| same, second colour's phase | blue | dark |
| raw zone 0, static blue | blue | blue |
| raw zone 0, breathe red / blue | red, then blue | red, then blue |
| PR static #00ff00 | green | unchanged (red) |
| raw zone 5, static blue | unchanged | bottom-left quarter blue |
| raw zone 6, then 7 | unchanged | one more quarter blue each |
| raw zone 8 | unchanged | all blue; bottom-right was last |

So zone 0 addresses both rings, zones 1-4 the left ring (it takes the PR's
colour as a whole; I didn't test 1-4 one at a time), and zones 5-8 the
right ring's quarters. The right ring runs the PR's effect with its own
colours: orange-yellow from Windows at first, red after the raw zone 0
write.

Writing zone 0 on this device would fix it. Armoury Crate sends zone 0 on
the Xbox Ally X, and the out-of-tree Ally driver we normally run sends
zone 0 on both models (they share the PID). Writing zones 1-8 would keep
per-zone control. To tell the two MCUs apart, the discovery replies
differ in bytes 8, 10 and 11, and the LampArray lamp count is 4 vs 8; I
don't know which byte is meant to carry the zone count.

One thing I can't explain: after a pass through `direct`, the right ring
went from raw-written blue (not saved) back to the red that the PR's last
`b4` had saved. My guess is that leaving LampArray reloads the saved state;
not tested.

### The Xbox-button LED

This device has a third light, the Xbox button. Armoury Crate sets its
level with interrupt OUT `5d d1 20 01 <0-3>` and its effect with feature
`5a d1 21 02 <effect> <speed>`. The PR sends neither, but its ring
brightness reaches the LED:

| write | Xbox LED | rings |
|---|---|---|
| PR brightness 0 (`5a ba c5 c4 00`) | off | off |
| PR brightness 3 | stays off | on |
| feature `5a d1 09 01 ff` (LED power flags) | stays off | on |
| interrupt OUT `5d d1 20 01 03` alone | on | on |

Brightness 0 turned the LED off in both runs, and brightness 3 never
restored it. So once userspace has set brightness 0, the Xbox LED stays
dark until a reboot under another driver. A fix would be to re-send
`5d d1 20 01 <level>` when brightness leaves 0, or to expose the button as
its own LED. Whether Armoury Crate's own ring brightness 0 does the same
on Windows is unknown: its slider sends only `ba c5 c4`, and nobody watched
the Xbox LED during that capture.

`5a d1 09 01 <flags>` gates the rings and the Xbox LED together: 00 turned
both off, ff turned them back on. The MCU keeps these flags across a power
cycle. With ff the Xbox LED was lit from power-on; with 00 it flashed once
and stayed off (one cold boot each). The PR never sends this packet. Its
own `enabled` packet did not relight anything with the flags at 00.

### Direct mode (LampArray, 8 lamps behind 4 entries)

All-red, green, blue and white frames showed correctly on both rings.
Single-entry frames:

| entry | lit |
|---|---|
| 0 | left ring, left side |
| 1 | left ring, right side |
| 2 | right ring, left side |
| 3 | right ring, right side |

`led_count` is fixed at `ROG_STRIX_4ZONE_KBD_LEDS` (4) and
`asus_lamparray_sample_rgb()` spreads the 4 entries over the 8 lamps, so
each entry lights about half a ring. Taking `led_count` from the LampArray
lamp count would give per-lamp control. The Ally X's first-frame miss
(item 7) did not show up in four direct sessions.

Once, brightness 0 written straight after leaving `direct` left the rings
lit. Repeating the same sequence (all-black frame, `effect=static`,
brightness 0) went dark both times, so I can't reproduce it.

## Also worth knowing

- `asus::slash` is registered on both devices because the vendor interface
  exposes report 0x5e; it controls nothing on either. Gating Slash on a
  successful Slash-specific probe would avoid it.
- Hot-unloading `hid-asus` crashed the Ally X's kernel once during the test;
  not attributed to the PR, pstore not yet read.
- A suspend that looked like a hang on the Ally X was not the PR: the
  suspend's filesystem sync waited 790 s, then 3196 s, on a microSD card
  behind a Steam download, and both suspends resumed normally.
- Once, right after those two long suspends, the Ally X's rings stayed dark
  for every write, the PR's and raw `b3` alike, with no driver warnings.
  That session ended before `d1 09 01 ff` was tried, and after a reboot
  everything worked. Not reproduced; cause unknown.
