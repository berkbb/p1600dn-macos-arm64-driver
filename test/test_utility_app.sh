#!/bin/bash
# Checks the built "P1600dn Series Utility.app": arm64 for macOS 11+, signed
# with the hardened runtime, 16 languages that cover every string key,
# and (in a logged-in GUI session, not as root) that it takes the printer
# queue from macOS's Open Application event ('prdt').
#
# "P1600dn Series Utility.app" paketini denetler.
set -euo pipefail
app="$1"
root="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "    [FAIL] $*" >&2; exit 1; }

binary="$app/Contents/MacOS/P1600dnUtility"
[ -x "$binary" ] || fail "App executable missing"
/usr/bin/lipo "$binary" -verify_arch arm64 || fail "Not arm64"
[ "$(/usr/bin/lipo -archs "$binary")" = "arm64" ] || fail "Not arm64 only"
minimum=$(/usr/bin/otool -l "$binary" | awk '/LC_BUILD_VERSION/ {b=1} b && $1 == "minos" {print $2; exit}')
[ "$minimum" = "11.0" ] || fail "Minimum macOS is $minimum, not 11.0"
/usr/bin/codesign --verify --strict --deep "$app" || fail "Signature invalid"
signature=$(/usr/bin/codesign -dv "$app" 2>&1)
case "$signature" in *"flags=0x10002(adhoc,runtime)"*) ;; *) fail "Not signed with the hardened runtime" ;; esac
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$app/Contents/Info.plist")" = "com.berkbb.p1600dn.utility" ] || fail "Bundle identifier"
[ "$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$app/Contents/Info.plist")" = "11.0" ] || fail "LSMinimumSystemVersion"
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIconFile' "$app/Contents/Info.plist")" = "P1600dnApp" ] || fail "Icon name"
[ -f "$app/Contents/Resources/P1600dnApp.icns" ] || fail "Icon file missing"
echo "    [PASS] Utility app: arm64 only, macOS 11.0+, hardened runtime signature, Info.plist"

count=0
for language in en da de es fi fr it ko nl no pt sv zh_CN zh_TW tr ro; do
    file="$app/Contents/Resources/$language.lproj/Utility.strings"
    [ -f "$file" ] || fail "Missing $language.lproj/Utility.strings"
    /usr/bin/plutil -lint "$file" >/dev/null || fail "Invalid $file"
    count=$((count + 1))
done
python="${PYTHON:-/opt/homebrew/bin/python3}"
[ -x "$python" ] || python=/usr/bin/python3
# Every language has English's keys, and English has every key the window
# asks for: the ownText("...") literals and the duration keys of both menus.
mismatch=$("$python" -I - "$app/Contents/Resources" "$root/utility/P1600dnUtility.swift" <<'PY'
import json, os, re, subprocess, sys
def keys(path):
    return set(json.loads(subprocess.run(["plutil", "-convert", "json", "-o", "-", path], capture_output=True, check=True).stdout))
base = keys(os.path.join(sys.argv[1], "en.lproj", "Utility.strings"))
for language in "da de es fi fr it ko nl no pt sv zh_CN zh_TW tr ro".split():
    other = keys(os.path.join(sys.argv[1], language + ".lproj", "Utility.strings"))
    if other != base:
        print(language, " ".join(sorted(base ^ other)))
used = set(re.findall(r'ownText\("([A-Za-z0-9]+)"\)', open(sys.argv[2], encoding="utf-8").read()))
used |= {"AfterMinutes%d" % m for m in (1, 5, 15, 30)} | {"AfterHours1", "AfterHours2", "Minutes30"}
used |= {"Hours%d" % h for h in (1, 2, 4, 8, 24)}
missing = sorted(used - base)
if missing:
    print("en", "missing", " ".join(missing))
PY
) || fail "Could not compare the Utility.strings keys"
[ -z "$mismatch" ] || fail "Utility.strings keys: $mismatch"
echo "    [PASS] Utility app localizations: $count languages, every key the window uses, the same keys in all"

# The header follows CUPS: a model probe (no window) against the stand-in
# cupsd of test/test_notify.c, which starts printing with the toner at 12%
# after the utility's first lookup.
probe_work=$(mktemp -d "${TMPDIR:-/tmp}/p1600dn-probe.XXXXXX")
cc -std=c11 -O1 -Wall -Wextra -Werror -I"$root/src" "$root/test/test_notify.c" "$root/src/p1600_notify.c" -lcups \
    -o "$probe_work/test_notify" || fail "Stand-in cupsd did not build"
swiftc -target arm64-apple-macos11.0 -swift-version 5 -O -D UTILITY_RENDER -parse-as-library \
    -import-objc-header "$root/utility/Bridging.h" -Xcc -I"$root/src" \
    "$root/utility/P1600dnUtility.swift" "$root/test/utility_probe.swift" "$root/build/utility"/*.o \
    -lcups -framework IOKit -framework CoreFoundation -framework CoreGraphics -framework CoreText \
    -o "$probe_work/utility_probe" 2>"$probe_work/build.log" || { cat "$probe_work/build.log" >&2; fail "Probe did not build"; }
"$probe_work/test_notify" --serve "$probe_work/port" utility >"$probe_work/serve.out" 2>"$probe_work/serve.err" &
stand_in=$!
for _ in $(seq 1 100); do [ -s "$probe_work/port" ] && break; sleep 0.1; done
[ -s "$probe_work/port" ] || { kill "$stand_in" 2>/dev/null; fail "Stand-in cupsd did not start"; }
seen=$(CUPS_SERVER="127.0.0.1:$(cat "$probe_work/port")" "$probe_work/utility_probe" P1600DN_Notify_Test) || seen="probe failed"
kill "$stand_in" 2>/dev/null || true
wait "$stand_in" 2>/dev/null || true
[ "$seen" = "state=4 toner=12" ] ||
    fail "Utility header did not follow CUPS: $seen ($(cat "$probe_work/serve.out" "$probe_work/serve.err" | tr '\n' ' '))"
rm -rf "$probe_work"
echo "    [PASS] Utility header follows CUPS: printer state and toner level looked up again while it runs"

if [ "$(id -u)" -eq 0 ] || [ "$(launchctl managername 2>/dev/null)" != "Aqua" ]; then
    echo "    [SKIP] Utility app launch (needs a logged-in user's GUI session)"
    exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/p1600dn-launch.XXXXXX")
trap 'rm -rf "$work"' EXIT
swiftc -O -target arm64-apple-macos11.0 "$root/test/utility_launch.swift" -o "$work/utility_launch" 2>/dev/null || fail "Launch helper did not build"
queue="P1600DN_Test_Queue"
received=$("$work/utility_launch" "$app" "$work/report" "$queue") || fail "Launch: $received"
[ "$received" = "$queue" ] || fail "App received printer \"$received\" instead of \"$queue\""
echo "    [PASS] Utility app launched by macOS with the queue in 'prdt' (macOS's launch protocol for printer utilities)"
# The running utility gets the next printer the same way (Printers & Scanners,
# a notification for another queue).
rm -f "$work/report"
next="P1600DN_Other_Queue"
received=$("$work/utility_launch" "$app" "$work/report" "$queue" "$next") || fail "Second launch: $received"
[ "$received" = "$queue
$next" ] || fail "Running app received \"$received\" instead of \"$queue\" then \"$next\""
echo "    [PASS] Running utility gets the next printer from a later Open Application event"

# The window itself: drawn off screen with stand-in printers and captured by
# the app (ScreenCaptureKit lets an app capture its own windows from macOS
# 14.4 without the Screen Recording permission). Each variant must have the
# fixed width, a sane height and real content; the PNGs stay in build/.
if [ "$(/usr/bin/sw_vers -productVersion | awk -F. '{ printf "%d%03d", $1, $2 }')" -lt 14004 ]; then
    echo "    [SKIP] Utility window render (needs macOS 14.4)"
    exit 0
fi
renders="$root/build/utility-render"
rm -rf "$renders" && mkdir -p "$renders"
render_app="$work/Render.app"
/usr/bin/ditto "$app" "$render_app"
swiftc -target arm64-apple-macos14.4 -swift-version 5 -O -D UTILITY_RENDER -parse-as-library \
    -import-objc-header "$root/utility/Bridging.h" -Xcc -I"$root/src" \
    "$root/utility/P1600dnUtility.swift" "$root/test/utility_render.swift" "$root/build/utility"/*.o \
    -lcups -framework IOKit -framework CoreFoundation -framework CoreGraphics -framework CoreText \
    -o "$render_app/Contents/MacOS/utility_render" 2>"$work/render-build.log" || { cat "$work/render-build.log" >&2; fail "Render helper did not build"; }
variants=0
while read -r language layout appearance scene low high; do
    out="$renders/$language-$layout-$appearance-$scene.png"
    # System frameworks log sandbox notes for a bundle in a temporary folder;
    # they are shown only when the render fails.
    result=$("$render_app/Contents/MacOS/utility_render" "$out" "$layout" "$appearance" "$scene" -AppleLanguages "($language)" 2>"$work/render.err") ||
        { cat "$work/render.err" >&2; fail "Render $language $layout $appearance $scene: $result"; }
    size=${result%% *}
    width=${size%x*}
    height=${size#*x}
    colors=${result##*colors=}
    [ "$width" -eq 540 ] || fail "Render $language $layout $scene: width $width, not 540"
    [ "$height" -ge "$low" ] && [ "$height" -le "$high" ] || fail "Render $language $layout $scene: height $height outside $low-$high"
    [ "$colors" -ge 200 ] || fail "Render $language $layout $scene: only $colors colors (blank window?)"
    variants=$((variants + 1))
done <<'EOF'
tr grouped light printer 560 760
tr grouped dark printers 600 800
en grouped light error 560 760
de grouped dark printer 560 760
tr legacy light printer 540 740
en legacy dark printers 580 780
tr grouped light empty 240 380
zh_CN grouped light empty 240 380
EOF
echo "    [PASS] Utility window: $variants renders (grouped and macOS 11-12 layouts, light and dark, 1-2 printers, errors, no printer) in build/utility-render"
