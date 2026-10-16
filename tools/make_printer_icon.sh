#!/bin/bash
# Draws the printer icon and the app icon (tools/draw_printer_icon.swift) and
# writes assets/P1600dnPrinter.png (1024 px) and assets/P1600dnPrinter.icns for
# the PPD and AirPrint, and assets/P1600dnApp.icns for the two apps.
#
# Yazıcı ve uygulama simgelerini çizer, assets/ altındaki PNG ile ICNS
# dosyalarını yazar.
#
# Usage: tools/make_printer_icon.sh
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/printer-icon.XXXXXX")"
trap 'rm -rf "$work"' EXIT

swift "$root/tools/draw_printer_icon.swift" "$work"
for pair in printer:P1600dnPrinter app:P1600dnApp; do
    name="${pair%%:*}"
    iconset="$work/$name.iconset"
    mkdir -p "$iconset"
    for size in 16 32 128 256 512; do
        cp "$work/$name-$size.png" "$iconset/icon_${size}x${size}.png"
        cp "$work/$name-$((size * 2)).png" "$iconset/icon_${size}x${size}@2x.png"
    done
    /usr/bin/iconutil --convert icns --output "$root/assets/${pair#*:}.icns" "$iconset"
done
cp "$work/printer-1024.png" "$root/assets/P1600dnPrinter.png"
echo "Wrote assets/P1600dnPrinter.png, assets/P1600dnPrinter.icns and assets/P1600dnApp.icns"
