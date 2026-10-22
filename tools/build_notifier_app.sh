#!/bin/bash
# Builds "P1600dn Notifier.app", the background agent that shows the P1600dn
# driver's printer status (paper, jams, doors, manual feed and duplex, engine
# errors, toner) as macOS notifications: the C core (src/p1600_notify.c,
# src/p1600_queues.c) and the AppKit agent (notifier/P1600dnNotifier.swift) for
# macOS 11+, 15 localizations, signed ad hoc with the hardened runtime.
#
# "P1600dn Notifier.app" bildirim aracını derler (arm64, macOS 11+).
#
# Usage: tools/build_notifier_app.sh [<output.app> [<object directory>]]
#        (default: "build/P1600dn Notifier.app" and build/notifier)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
app="${1:-$root/build/P1600dn Notifier.app}"
objects="${2:-$root/build/notifier}"
cc="${CC:-clang}"
swiftc="${SWIFTC:-swiftc}"
minimum="${MACOS_MIN:-11.0}"
c_flags=(-O3 -std=c11 -Wall -Wextra -Wpedantic -Werror -arch arm64 "-mmacosx-version-min=$minimum" -mcpu=apple-m1
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -I"$root/src")
core=(p1600_notify p1600_queues)
languages=(en da de es fi fr it ko nl no pt sv zh_CN zh_TW tr ro)

mkdir -p "$objects"
object_files=()
for name in "${core[@]}"; do
    "$cc" "${c_flags[@]}" -c "$root/src/$name.c" -o "$objects/$name.o"
    object_files+=("$objects/$name.o")
done

staging="$app.partial"
rm -rf "$staging"
mkdir -p "$staging/Contents/MacOS" "$staging/Contents/Resources"
"$swiftc" -target "arm64-apple-macos$minimum" -swift-version 5 -O -parse-as-library \
    -import-objc-header "$root/notifier/Bridging.h" -Xcc -I"$root/src" \
    "$root/notifier/P1600dnNotifier.swift" "${object_files[@]}" \
    -Xlinker -dead_strip -lcups -framework AppKit -framework UserNotifications \
    -o "$staging/Contents/MacOS/P1600dnNotifier"

cp "$root/notifier/Info.plist" "$staging/Contents/Info.plist"
printf 'APPL????' > "$staging/Contents/PkgInfo"
cp "$root/assets/P1600dnApp.icns" "$staging/Contents/Resources/P1600dnApp.icns"
for language in "${languages[@]}"; do
    source="$root/notifier/Resources/$language.lproj/Notifier.strings"
    [ -f "$source" ] || { echo "Missing $source" >&2; exit 1; }
    mkdir -p "$staging/Contents/Resources/$language.lproj"
    cp "$source" "$staging/Contents/Resources/$language.lproj/Notifier.strings"
done

# Sources copied over USB or AirDrop can be mode 0700 and carry Finder info,
# quarantine or provenance attributes, which cp keeps: codesign refuses Finder
# info, and an installed bundle owned by root must be readable by every user.
/usr/bin/xattr -cr "$staging"
find "$staging" -type d -exec chmod 755 {} +
find "$staging" -type f -exec chmod 644 {} +
chmod 755 "$staging/Contents/MacOS/"*
/usr/bin/codesign --force --sign - --options runtime --identifier com.berkbb.p1600dn.notifier "$staging" 2>/dev/null
/usr/bin/codesign --verify --strict "$staging"
rm -rf "$app"
mv "$staging" "$app"
echo "Built notifier app: $app"
