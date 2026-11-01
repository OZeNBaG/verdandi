#!/bin/sh
# smoke test on an emulator (.github/workflows/android.yml): install, start, walk the menu
# to the first script with touches on the keys, screenshot each step, collect the logs.
# outputs in ./smoke/
set -x
mkdir -p smoke
adb install -r Verdandi.apk
adb shell am start -n org.verdandi.app/.VerdandiActivity
sleep 45
adb exec-out screencap -p > smoke/1-boot.png

# screen size, landscape
SIZE=$(adb shell wm size | tail -1 | sed 's/.*: //')
A=${SIZE%x*}; B=${SIZE#*x}
if [ "$A" -gt "$B" ]; then W=$A; H=$B; else W=$B; H=$A; fi
# a control's centre in screen pixels (platform/macos/device_skin.c layout units)
pos() {
    awk -v W="$W" -v H="$H" -v ux="$1" -v uy="$2" 'BEGIN {
        s = W / 992; if (H / 662 < s) s = H / 662
        ox = (W - 960 * s) / 2; oy = (H - 630 * s) / 2
        printf "%d %d", ox + ux * s, oy + uy * s }'
}
tap() { adb shell input tap $(pos "$1" "$2"); sleep 1.5; }
K1="109 252"; K2="594 542"; K3="749 542"
# turn a knob: a drag to the right from its centre
turn() { set -- $(pos "$1" "$2") "$3"; adb shell input swipe "$1" "$2" $(( $1 + $3 )) "$2" 400; sleep 1; }

tap $K1                                  # menu
adb exec-out screencap -p > smoke/2-menu.png
tap $K3                                  # SELECT
adb exec-out screencap -p > smoke/3-select.png
tap $K3                                  # the first script
sleep 12
adb exec-out screencap -p > smoke/4-script.png
turn 672 420 300                         # E2
turn 827 420 300                         # E3
tap $K3
sleep 3
adb exec-out screencap -p > smoke/5-played.png

adb logcat -d > smoke/logcat.txt
adb shell cat /sdcard/Android/data/org.verdandi.app/files/norns.log > smoke/norns.log
adb shell ls -R /sdcard/Android/data/org.verdandi.app/files/dust > smoke/dust.txt
grep -E "FATAL|Abort|SIGSEGV|crash" smoke/logcat.txt | head -20
exit 0
