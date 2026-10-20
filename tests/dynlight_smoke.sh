#!/bin/bash
# Walk PR #17's Dynamic Lighting nodes on the Ally and record what the rings do.
# Nothing here writes persistent state on purpose; power_states is only read.
#
#   tests/dynlight_smoke.sh            # interactive: asks after every change
#   tests/dynlight_smoke.sh --list     # read-only inventory
#
# Answers and sysfs reads land in dynlight-smoke-<stamp>.log next to the repo.
set -u
LOG="dynlight-smoke-$(date +%Y%m%d-%H%M%S).log"
MODE=${1:-interactive}
say() { echo "$*" | tee -a "$LOG"; }
rd()  { [ -r "$1" ] && tr '\n' ' ' < "$1" || echo "(absent)"; }
ask() {
    [ "$MODE" = "--list" ] && return
    read -r -p "    -> $1 [y/n/notes]: " ans
    say "    ANSWER: $1 => $ans"
}
wr() { # wr <path> <value>
    if echo -n "$2" | sudo tee "$1" >/dev/null 2>"$LOG.err"; then
        say "    wrote $(basename "$1") = $2"
    else
        say "    FAILED $(basename "$1") = $2 : $(cat "$LOG.err")"
    fi
}

say "== $(date)  kernel $(uname -r)  board $(cat /sys/class/dmi/id/board_name)"
say "== modules: $(lsmod | awk '$1 ~ /^(hid_asus|hid_asus_ally|led_class_dynamic)$/ {printf "%s ", $1}')"
say "== dmesg (asus / lamparray / dynamic):"
sudo dmesg | grep -iE 'asus|lamparray|dynamic' | tail -30 | sed 's/^/    /' | tee -a "$LOG"
say "== hidraw nodes bound to 0b05:1b4c (LampArray interface should have NO hidraw if claimed):"
for h in /sys/class/hidraw/hidraw*; do
    u=$(cat "$h/device/uevent" 2>/dev/null | grep -E 'HID_ID|HID_PHYS' | tr '\n' ' ')
    case "$u" in *0B05:00001B4C*) say "    $(basename "$h") $u";; esac
done
say "== LED class devices:"
ls -1 /sys/class/leds | sed 's/^/    /' | tee -a "$LOG"

for led in /sys/class/leds/asus::*; do
    [ -e "$led" ] || continue
    say ""
    say "==== $led"
    if [ ! -e "$led/effect_index" ]; then
        say "    (not a Dynamic Lighting node; attributes: $(ls "$led" | tr '\n' ' '))"
        for a in brightness max_brightness mode mode_index interval; do
            [ -e "$led/$a" ] && say "    $a: $(rd "$led/$a")"
        done
        continue
    fi
    for a in zone_type led_count effect effect_index speed speed_range direction direction_index \
             enabled enabled_index effects_palette max_palette_entries power_states power_states_index \
             brightness max_brightness; do
        say "    $a: $(rd "$led/$a")"
    done
    [ -e "$led/direct_buffer" ] && say "    direct_buffer: present ($(stat -c %s "$led/direct_buffer") bytes)"
    [ "$MODE" = "--list" ] && continue

    say "-- brightness: the PR writes this byte straight to the firmware (0-3), so 255 is out of range"
    wr "$led/brightness" 255
    sleep 1; ask "rings lit at brightness 255?"
    wr "$led/brightness" 3
    sleep 1; ask "rings lit at brightness 3?"

    say "-- enabling, static red"
    wr "$led/enabled" true
    wr "$led/effect" static
    wr "$led/effects_palette" "#ff0000"
    sleep 2; ask "rings solid red?"
    wr "$led/effects_palette" "#00ff00"
    sleep 2; ask "rings solid green?"

    for e in $(rd "$led/effect_index"); do
        [ "$e" = static ] && continue
        [ "$e" = direct ] && continue
        say "-- effect $e"
        wr "$led/effect" "$e"
        sleep 4; ask "rings show '$e'? (what do they do)"
    done

    if [ -e "$led/direct_buffer" ]; then
        n=$(rd "$led/led_count" | tr -d ' ')
        say "-- direct mode: $n LEDs, writing all-blue frame"
        wr "$led/effect" direct
        python3 - "$led/direct_buffer" "$n" <<'PY'
import sys
path, n = sys.argv[1], int(sys.argv[2])
open(path, 'wb').write(bytes([0, 0, 255]) * n)
PY
        sleep 2; ask "rings blue from direct_buffer?"
    fi

    say "-- enabled=0"
    wr "$led/enabled" false
    sleep 2; ask "rings off?"
    wr "$led/enabled" true
    wr "$led/effect" static
    wr "$led/brightness" 0
done
say ""
say "== log saved to $LOG"
