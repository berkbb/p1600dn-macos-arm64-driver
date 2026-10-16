#!/bin/bash
# The diagnostic page through the real macOS chain: p1600dn-utility writes
# the PDF, cgpdftoraster rasterizes it with the P1600dn PPD, the driver
# filter (with its page guard) turns it into a ZjStream job, and the
# pipeline validator checks every decoded pixel. The rasterized front page
# must end in the bottom edge test's black bar with nothing below it.
#
# Teşhis sayfası gerçek macOS zincirinde: alt kenar bandının altı boş olmalı.
#
# Usage: test_diagnostic.sh <p1600dn-utility> <filter> <validator> <test_diagnostic>
set -eu

script_dir=$(cd "$(dirname "$0")" && pwd)
module_dir=$(cd "$script_dir/.." && pwd)
tool="$1"
filter="$2"
validator="$3"
checker="$4"
ppd="$module_dir/ppd/P1600dn-Series.ppd"

fail() {
    echo "    [FAIL] $1" >&2
    exit 1
}

cupsfilter_binary=$(command -v cupsfilter 2>/dev/null || true)
[ -n "$cupsfilter_binary" ] || cupsfilter_binary=/usr/sbin/cupsfilter
if [ ! -x "$cupsfilter_binary" ]; then
    echo "    [SKIP] cupsfilter is unavailable; diagnostic page pipeline test skipped" >&2
    exit 0
fi
for binary in "$tool" "$filter" "$validator" "$checker"; do
    [ -x "$binary" ] || fail "Missing binary: $binary"
done

test_root=$(mktemp -d "${TMPDIR:-/tmp}/p1600dn-diagnostic.XXXXXX")
trap 'rm -rf "$test_root"' EXIT

echo "[*] Starting diagnostic page pipeline tests..."
# paper | sides | language | validator expectations
while IFS='|' read -r paper sides language expect; do
    name="diagnostic-$paper-$sides-$language"
    pdf="$test_root/$name.pdf"
    raster="$test_root/$name.raster"
    output="$test_root/$name.zjs"
    log="$test_root/$name.log"
    read -r -a expect_words <<<"$expect"
    case "$sides" in
        one) cups_sides=one-sided ;;
        long) cups_sides=two-sided-long-edge ;;
        short) cups_sides=two-sided-short-edge ;;
    esac

    "$tool" --media "$paper" --sides "$sides" --output "$pdf" page diagnostic "$language" >/dev/null ||
        fail "p1600dn-utility could not write the diagnostic page: $name"
    "$cupsfilter_binary" -p "$ppd" -i application/pdf -m application/vnd.cups-raster \
        -o "PageSize=$paper" -o "sides=$cups_sides" "$pdf" </dev/null >"$raster" 2>"$test_root/$name-cups.log" ||
        fail "cupsfilter failed: $name"
    "$checker" --check-raster "$raster" >/dev/null || fail "bottom edge test is not clean: $name"

    PPD="$ppd" "$filter" 1 test-user "$name" 1 "PageSize=$paper sides=$cups_sides" "$raster" \
        </dev/null >"$output" 2>"$log" || fail "driver filter failed: $name"
    if grep -Fq 'ERROR:' "$log"; then
        sed 's/^/        /' "$log" >&2
        fail "driver filter (page guard) reported an error: $name"
    fi
    [ "$(grep -c '^PAGE:' "$log" || true)" -eq 2 ] || fail "expected 2 pages: $name"
    "$validator" "$raster" "$output" mode=native "title=$name" pages=2 "${expect_words[@]}" </dev/null >/dev/null ||
        fail "ZjStream validation failed: $name"
    echo "    [PASS] $name: 2 pages through cgpdftoraster, page guard and validator; nothing below the bottom bar"
    rm -f "$pdf" "$raster" "$output"
done <<'EOF'
A4|long|tr|duplex=long sheets=auto paper=9
Letter|short|en|duplex=short sheets=auto paper=1
A5|one|tr|paper=11
A6|long|en|duplex=long sheets=manual paper=70
Env10|one|tr|paper=20
FanFoldGermanLegal|long|en|duplex=long sheets=auto paper=41
EOF
echo "[+] Diagnostic page pipeline tests passed."
