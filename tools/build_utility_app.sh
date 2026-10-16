#!/bin/bash
# Builds "P1600dn Series Utility.app", the driver's native arm64 printer
# utility: the C core (src/p1600_utility.c and friends) and the SwiftUI window
# (utility/P1600dnUtility.swift) for macOS 11+, in 16 languages
# (utility/Resources, the project's own translations), signed ad hoc with the
# hardened runtime.
#
# "P1600dn Series Utility.app" uygulamasını derler (arm64, macOS 11+).
#
# Usage: tools/build_utility_app.sh <output.app> <object directory>
set -euo pipefail

app="$1"
objects="$2"
root="$(cd "$(dirname "$0")/.." && pwd)"
cc="${CC:-clang}"
swiftc="${SWIFTC:-swiftc}"
minimum="${MACOS_MIN:-11.0}"
c_flags=(-O3 -std=c11 -Wall -Wextra -Wpedantic -Werror -arch arm64 "-mmacosx-version-min=$minimum" -mcpu=apple-m1
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -I"$root/src")
core=(p1600_utility p1600_ews_proxy p1600_queues p1600_diagnostic p1600_net p1600_usb p1600_supplies)

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
    -import-objc-header "$root/utility/Bridging.h" -Xcc -I"$root/src" \
    "$root/utility/P1600dnUtility.swift" "${object_files[@]}" \
    -Xlinker -dead_strip -lcups -framework IOKit -framework CoreFoundation -framework CoreGraphics -framework CoreText \
    -o "$staging/Contents/MacOS/P1600dnUtility"

cp "$root/utility/Info.plist" "$staging/Contents/Info.plist"
printf 'APPL????' > "$staging/Contents/PkgInfo"
cp "$root/assets/P1600dnApp.icns" "$staging/Contents/Resources/P1600dnApp.icns"
for lproj in "$root"/utility/Resources/*.lproj; do
    language="$(basename "$lproj")"
    mkdir -p "$staging/Contents/Resources/$language"
    cp "$lproj"/*.strings "$staging/Contents/Resources/$language/"
done

/usr/bin/codesign --force --sign - --options runtime --identifier com.berkbb.p1600dn.utility "$staging" 2>/dev/null
/usr/bin/codesign --verify --strict "$staging"
rm -rf "$app"
mv "$staging" "$app"
echo "Built printer utility app: $app"
