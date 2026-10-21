# ally_dynlight: OGC PR #17 Dynamic Lighting on the ROG Ally, out of tree

A test build of [OpenGamingCollective/linux-unstable PR #17](https://github.com/OpenGamingCollective/linux-unstable/pull/17)
(Dynamic Lighting LED class + hid-asus Aura / LampArray / Slash / AniMe) for the
SteamOS 7.2.x kernel on the ROG Ally X and ROG Xbox Ally X. It exists to answer
one question from NeroReflex: does the generic Aura keyboard path in that PR
drive the Ally's joystick rings with no Ally-specific code?

Forked from [ally_module](https://github.com/kndclark/ally_module) for its build
scaffolding. The ally_module driver (Nero's series plus the X20 work) is
**not** part of this build, and the PR is tested the way linux-unstable
itself would run it: hid-asus alone, hid-asus-ally blacklisted.

It is published as the `DO_NOT_MERGE_ogc_pr17_test` branch of
ally_module, a standalone test record that is not meant to be merged
into any ally_module branch.

## What is in here

| File | Origin |
|---|---|
| `hid-asus.c`, `hid-ids.h` | PR #17 head `fb149ab7`, byte-identical |
| `led-class-dynamic.c` | PR #17, plus one marked shim (see below) |
| `include/linux/led-dynamic-lighting.h` | PR #17, plus the matching shim |
| `dynlight_compat.h` | two defines the 7.2.7 headers lack (`LED_DYNAMIC_LIGHTING`, `cmp_int`) |
| `docs/` | the PR's sysfs ABI documents, for attribute names |
| `tests/dynlight_smoke.sh` | on-device walk-through, logs what the rings do |
| `tests/lamparray_probe.py`, `tests/lamparray_coexist.py`, `tests/aura_caps_probe.py` | earlier probes of the same hardware paths |

**The shim.** Upstream adds a `led_dynamic` back-pointer to `struct led_classdev`
in `include/linux/leds.h`. An out-of-tree module cannot grow the kernel's
struct, so `led-class-dynamic.c` keeps a small `cdev -> ldev` table and
`lcdev_to_dldev()` looks it up. Behaviour is unchanged; every site is marked
`OUT-OF-TREE SHIM`. The PR's KUnit test and Kconfig are not carried.

## Expected behaviour, predicted from captures before any run

- Both devices answer the PR's Aura probe (`5d 05 20 31 00 20`), so an
  `asus::keyboard` Dynamic Lighting node should appear on the vendor interface.
  The reply's byte 9 is `0x02`, which the PR reads as a 4-zone keyboard.
- The PR also registers `asus::slash` whenever report `0x5e` exists; the Ally's
  vendor interface has it, so expect a Slash LED that controls nothing.
- Xbox Ally X: USB interface 0 carries LampArray report ids 1-6 on vendor page
  `0xff59`, which the PR's detector accepts, so expect "LampArray direct RGB
  backend" in dmesg and interface 0 losing its hidraw node.
- Ally X: interface 0 is a real LampArray (page `0x59`) but has no report id 4,
  so the PR falls back to Aura `0xbc` for direct mode.
- "highlight" sends mode `0x06`; the X20's Reactive effect is `0x0e`.

## Use

```
./install.sh            # build on the device, pre-flight symbols, install, reload
tests/dynlight_smoke.sh # interactive walk-through; answers go to a log file
./uninstall.sh          # put the ally_module hid-asus.ko.zst back
```

`install.sh` keeps the displaced `hid-asus.ko.zst` as `*.bak-ally_module` and
`uninstall.sh` restores it. Modules go to `/lib/modules/$(uname -r)/updates/ally/`.

Local compile check without the device: extract the matching SteamOS headers
package (`linux-neptune-72-headers-7.2.7.valve1-1`) and run
`make KDIR=<extracted>/usr/lib/modules/<kver>/build`. GCC 15 rejects the
`__counted_by_ptr` attribute those GCC-16-built headers use; undefining
`CONFIG_CC_HAS_COUNTED_BY_PTR` in the extracted `include/generated/autoconf.h`
is enough for a smoke build.
