#!/bin/bash
# Real macOS CUPS pipeline tests for rastertop1600dn: cgtexttopdf and
# cgpdftoraster rasterize generated text pages with the P1600dn PPD, the
# filter turns the raster into a ZjStream, and test_cups_pipeline checks the
# exact ZjStream framing, sheet order and every decoded JBIG pixel.
#
# Usage: test_cups_pipeline.sh [filter] [validator]
# P1600DN_PIPELINE_WORKDIR: directory the filters run in (coverage output).
set -eu

script_dir=$(cd "$(dirname "$0")" && pwd)
module_dir=$(cd "$script_dir/.." && pwd)
filter_binary="${1:-$module_dir/build/rastertop1600dn}"
validator_binary="${2:-$module_dir/build/test_cups_pipeline}"
ppd="$module_dir/ppd/P1600dn-Series.ppd"
readme="$module_dir/README.md"

fail() {
    echo "    [FAIL] $1" >&2
    exit 1
}

# sudo may drop /usr/sbin from PATH.
cupsfilter_binary=$(command -v cupsfilter 2>/dev/null || true)
if [ -z "$cupsfilter_binary" ] && [ -x /usr/sbin/cupsfilter ]; then
    cupsfilter_binary=/usr/sbin/cupsfilter
fi
if [ -z "$cupsfilter_binary" ]; then
    echo "    [SKIP] cupsfilter is unavailable; real macOS CUPS pipeline tests skipped" >&2
    exit 0
fi

[ -x "$filter_binary" ] || fail "Filter binary is unavailable: $filter_binary"
[ -x "$validator_binary" ] || fail "Pipeline validator is unavailable: $validator_binary"
[ -f "$ppd" ] || fail "P1600dn PPD is unavailable"
[ -f "$readme" ] || fail "README.md (pipeline text source) is unavailable"
filter_binary=$(cd "$(dirname "$filter_binary")" && pwd)/$(basename "$filter_binary")
validator_binary=$(cd "$(dirname "$validator_binary")" && pwd)/$(basename "$validator_binary")
pipeline_workdir="${P1600DN_PIPELINE_WORKDIR:-$module_dir}"
[ -d "$pipeline_workdir" ] || fail "Pipeline working directory is unavailable: $pipeline_workdir"
cd "$pipeline_workdir"

test_root=$(mktemp -d "${TMPDIR:-/tmp}/p1600dn-cups-pipeline.XXXXXX")
trap 'rm -rf "$test_root"' EXIT

# Text with exactly N form-feed separated pages of README.md lines (ASCII,
# short enough to fit even an A5 page without wrapping).
make_input() {
    awk -v pages="$1" '
        NR > pages * 18 { exit }
        (NR - 1) % 18 == 0 {
            if (NR > 1) printf "\f"
            printf "HP LaserJet P1600dn CUPS pipeline - page %d of %d\n\n", (NR - 1) / 18 + 1, pages
        }
        { line = $0; gsub(/[^ -~]/, "?", line); print substr(line, 1, 56) }
    ' "$readme" >"$2"
    [ "$(tr -cd '\f' <"$2" | wc -c | tr -d ' ')" -eq $(($1 - 1)) ] || fail "README.md is too short for $1 pages"
}

# Filter stderr: no errors and one "PAGE: n copies" line per raster page.
check_driver_log() {
    local log="$1" pages="$2" copies="$3" label="$4"
    if grep -Fq 'ERROR:' "$log"; then
        sed 's/^/        /' "$log" >&2
        fail "filter logged an error: $label"
    fi
    [ "$(grep -c '^PAGE:' "$log" || true)" -eq "$pages" ] || fail "expected $pages PAGE lines: $label"
    [ "$(grep -cE "^PAGE: [0-9]+ $copies\$" "$log" || true)" -eq "$pages" ] || \
        fail "PAGE lines do not report $copies copies: $label"
}

# Runs the filter on a raster (file argument or stdin) and validates its
# ZjStream: filter_and_validate name raster pages copies page-copies transport
# options expectations...
filter_and_validate() {
    local name="$1" raster="$2" pages="$3" copies="$4" page_copies="$5" transport="$6" options="$7"
    shift 7
    local output="$test_root/$name.zjs" log="$test_root/$name-driver.log"
    if [ "$transport" = "stdin" ]; then
        PPD="$ppd" "$filter_binary" 1 test-user "$name" "$copies" "$options" \
            <"$raster" >"$output" 2>"$log" || fail "stdin filter path failed: $name"
    else
        PPD="$ppd" "$filter_binary" 1 test-user "$name" "$copies" "$options" "$raster" \
            </dev/null >"$output" 2>"$log" || fail "file filter path failed: $name"
    fi
    [ -s "$output" ] || fail "filter produced an empty ZjStream: $name"
    check_driver_log "$log" "$pages" "$page_copies" "$name"
    "$validator_binary" "$raster" "$output" mode=native "title=$name" "pages=$pages" "$@" </dev/null || \
        fail "ZjStream validation failed: $name"
    rm -f "$output"
}

echo "[*] Starting real macOS CUPS raster pipeline tests..."

# name | text pages | raster pages | trimmed pages | copies | transport | job options | expectations
#
# MediaType Paper, Light or Mid-Weight on Letter, Legal, A4 or 8.5x13 duplexes
# automatically; other media or sizes, or DupUnit=False, duplex manually.
# macOS cgpdftoraster appends an all-white back side to an odd duplex document
# (padded=1) and sends a one-page document simplex, so the filter's own blank
# partner and one-page manual paths are also run on the real raster trimmed
# to "trimmed pages" (0 = no trimmed run).
while IFS='|' read -r name text_pages raster_pages trimmed_pages copies transport options expect; do
    input="$test_root/$name.txt"
    raster="$test_root/$name.raster"
    read -r -a option_words <<<"$options"
    read -r -a expect_words <<<"$expect"
    cups_options=()
    trimmed_expect=()
    page_copies=1
    for option in "${option_words[@]}"; do cups_options+=(-o "$option"); done
    for word in "${expect_words[@]}"; do
        case "$word" in
            copies=*) page_copies="${word#copies=}" ;;
        esac
        [ "$word" = "padded=1" ] || trimmed_expect+=("$word")
    done

    make_input "$text_pages" "$input"
    "$cupsfilter_binary" -p "$ppd" -i text/plain -m application/vnd.cups-raster -n "$copies" \
        "${cups_options[@]}" "$input" </dev/null >"$raster" 2>"$test_root/$name-cups.log" || \
        fail "cupsfilter failed: $name"
    [ -s "$raster" ] || fail "cupsfilter produced an empty raster: $name"

    filter_and_validate "$name" "$raster" "$raster_pages" "$copies" "$page_copies" "$transport" "$options" \
        "${expect_words[@]}"
    if [ "$trimmed_pages" -gt 0 ]; then
        "$validator_binary" --trim "$raster" "$test_root/$name-trimmed.raster" "$trimmed_pages" </dev/null || \
            fail "could not trim the raster: $name"
        filter_and_validate "$name-trimmed" "$test_root/$name-trimmed.raster" "$trimmed_pages" "$copies" \
            "$page_copies" "$transport" "$options" ${trimmed_expect[@]+"${trimmed_expect[@]}"}
        rm -f "$test_root/$name-trimmed.raster"
    fi
    rm -f "$raster"
done <<'EOF'
simplex-a4-default|2|2|0|1|file|PageSize=A4|paper=9 source=7 media=1 bpp=2 density=3
simplex-reverse-econo-manual-feed|2|2|0|1|stdin|PageSize=A4 TersKose=True EconoMode=True pjlDensity=5 pjlJamRecovery=True InputSlot=Manual MediaType=LabelsPaper|rotate=1 econo=1 density=5 jam=1 source=4 media=265
simplex-600dpi|2|2|0|1|file|PageSize=A4 Resolution=600dpi pjlDensity=1|bpp=1 density=1
duplex-long-auto-odd|3|4|3|1|stdin|PageSize=A4 Duplex=DuplexNoTumble MediaType=Paper|duplex=long sheets=auto padded=1
duplex-short-auto-light|2|2|0|1|file|PageSize=A4 Duplex=DuplexTumble MediaType=Light60-74g|duplex=short sheets=auto media=258
duplex-long-auto-letter-midweight|4|4|0|1|stdin|PageSize=Letter Duplex=DuplexNoTumble MediaType=Mid-Weight96-110g TersKose=True|duplex=long sheets=auto paper=1 media=282 rotate=1
duplex-manual-heavy-odd|3|4|3|1|file|PageSize=A4 Duplex=DuplexNoTumble MediaType=Heavy111-130g|duplex=long sheets=manual media=262 padded=1
duplex-manual-heavy-even|4|4|0|1|stdin|PageSize=A4 Duplex=DuplexTumble MediaType=Heavy111-130g TersKose=True|duplex=short sheets=manual media=262 rotate=1
duplex-manual-no-dupunit|2|2|0|1|file|PageSize=A4 Duplex=DuplexNoTumble MediaType=Paper DupUnit=False|duplex=long sheets=manual
duplex-manual-a5|2|2|1|1|stdin|PageSize=A5 Duplex=DuplexNoTumble MediaType=Paper|duplex=long sheets=manual paper=11
duplex-single-page-document|1|1|0|1|file|PageSize=A4 Duplex=DuplexNoTumble|duplex=none
copies-uncollated|2|2|0|2|file|PageSize=A4|copies=2
copies-collated|2|4|0|2|stdin|PageSize=A4 collate=true|copies=1
letter-simplex|2|2|0|1|file|PageSize=Letter InputSlot=Auto|paper=1 source=7
EOF

# A one-page PDF of `width` x `height` points, black to every edge: the
# worst case for the bottom of the page, where the P1606dn once printed
# stripes. ASCII only, so ${#body} counts bytes for the xref offsets.
make_black_pdf() {
    local width="$1" height="$2" out="$3" content body offsets=() i
    content="0 g 0 0 $width $height re f"
    local objects=(
        "<< /Type /Catalog /Pages 2 0 R >>"
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 $width $height] /Contents 4 0 R >>"
        "<< /Length ${#content} >>"$'\n'"stream"$'\n'"$content"$'\n'"endstream"
    )
    body="%PDF-1.4"$'\n'
    for i in "${!objects[@]}"; do
        offsets+=("${#body}")
        body+="$((i + 1)) 0 obj"$'\n'"${objects[$i]}"$'\n'"endobj"$'\n'
    done
    local xref=${#body}
    body+="xref"$'\n'"0 5"$'\n'"0000000000 65535 f "$'\n'
    for i in "${offsets[@]}"; do body+="$(printf '%010d 00000 n ' "$i")"$'\n'; done
    body+="trailer"$'\n'"<< /Size 5 /Root 1 0 R >>"$'\n'"startxref"$'\n'"$xref"$'\n'"%%EOF"$'\n'
    printf '%s' "$body" >"$out"
}

# Every paper size of the PPD, black to the last line, at FastRes 1200 and
# 600 dpi, through the real cgpdftoraster: each real page geometry must pass
# the filter's page guard (no ERROR) and the validator (every decoded pixel,
# white VIDEO_Y padding), and name its paper. macOS truncates PageSize (B5 is
# 515 x 728, 16K 195x270 is 552 x 765), where an exact size match finds no
# paper code.
paper_sizes=0
while IFS=' ' read -r size paper; do
    dimensions=$(awk -v size="$size" 'index($0, "*PaperDimension " size "/") == 1 {
        split($0, quoted, "\""); print quoted[2]; exit }' "$ppd")
    [ -n "$dimensions" ] || fail "PPD has no PaperDimension for $size"
    pdf="$test_root/black-$size.pdf"
    make_black_pdf ${dimensions% *} ${dimensions#* } "$pdf"
    for resolution in 1200x600dpi 600dpi; do
        bpp=2
        [ "$resolution" = "600dpi" ] && bpp=1
        name="black-$size-$resolution"
        raster="$test_root/$name.raster"
        "$cupsfilter_binary" -p "$ppd" -i application/pdf -m application/vnd.cups-raster \
            -o "PageSize=$size" -o "Resolution=$resolution" "$pdf" </dev/null >"$raster" \
            2>"$test_root/$name-cups.log" || fail "cupsfilter failed: $name"
        [ -s "$raster" ] || fail "cupsfilter produced an empty raster: $name"
        filter_and_validate "$name" "$raster" 1 1 1 file "PageSize=$size Resolution=$resolution" \
            "paper=$paper" "bpp=$bpp"
        rm -f "$raster"
        paper_sizes=$((paper_sizes + 1))
    done
done <<'EOF'
Letter 1
Legal 5
A4 9
Executive 7
FanFoldGermanLegal 41
A5 11
A6 70
B5 13
195x270mm 264
184x260mm 263
197x273mm 257
Postcard 43
DoublePostcard 69
Env10 20
EnvMonarch 37
EnvISOB5 34
EnvC5 28
EnvDL 27
EOF
[ "$paper_sizes" -eq "$(($(grep -c '^\*PageSize ' "$ppd") * 2))" ] || fail "not every PPD paper size was printed"
echo "    [PASS] All $((paper_sizes / 2)) PPD paper sizes, black to the last line, at FastRes 1200 and 600 dpi"

# The complete PPD-selected chain: cupsfilter must pick cgtexttopdf ->
# cgpdftoraster -> this filter from the PPD (which also hands the filter the
# PPD environment variable), and the job must match the raster cgpdftoraster
# produces for the same PPD and options.
# run_chain name ppd text-pages raster-pages [option...] -- [expectation...]
run_chain() {
    local name="$1" chain_ppd="$2" text_pages="$3" raster_pages="$4"
    shift 4
    local input="$test_root/$name.txt" raster="$test_root/$name.raster" output="$test_root/$name.zjs"
    local log="$test_root/$name.log"
    local option_words=() expect_words=() word
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do option_words+=(-o "$1"); shift; done
    [ $# -gt 0 ] && shift
    for word in "$@"; do expect_words+=("$word"); done

    make_input "$text_pages" "$input"
    "$cupsfilter_binary" -p "$chain_ppd" -i text/plain -m application/vnd.cups-raster \
        ${option_words[@]+"${option_words[@]}"} "$input" >"$raster" 2>"$test_root/$name-raster.log" || \
        fail "chain reference raster failed: $name"
    "$cupsfilter_binary" -e -m printer/p1600dn -p "$chain_ppd" -i text/plain -t "$name" \
        ${option_words[@]+"${option_words[@]}"} "$input" >"$output" 2>"$log" || \
        fail "complete PPD-driven CUPS pipeline failed: $name"
    [ -s "$output" ] || fail "complete PPD-driven CUPS pipeline produced no output: $name"
    check_driver_log "$log" "$raster_pages" 1 "$name"
    "$validator_binary" "$raster" "$output" mode=native "title=$name" "pages=$raster_pages" \
        ${expect_words[@]+"${expect_words[@]}"} || fail "complete PPD-driven CUPS output failed validation: $name"
    rm -f "$raster" "$output"
}

# cupsfilter refuses PPD lines over 255 bytes, and the PPD names the filter
# by its path: a driver folder with a long path names it through a short
# link (in mktemp's folder, root's and 0700 when the tests run as root).
ppd_filter_path() {
    if [ "$(printf '%s' "$1" | wc -c)" -le 150 ]; then
        printf '%s\n' "$1"
    else
        ln -s "$1" "$2" && printf '%s\n' "$2"
    fi
}

# The project PPD with `filter` as its raster filter, and the chain CUPS
# resolves for it (cgtexttopdf -> cgpdftoraster -> the filter).
write_pipeline_ppd() {
    awk -v filter="$1" '
        index($0, "*cupsFilter:") == 1 {
            print "*cupsFilter: \"application/vnd.cups-raster 0 " filter "\""
            next
        }
        { print }
    ' "$ppd" >"$2"
}
check_chain() {
    local chain_ppd="$1" filter="$2" filters="$test_root/$3.filters"
    [ -f "$test_root/full-pipeline.txt" ] || make_input 1 "$test_root/full-pipeline.txt"
    if ! "$cupsfilter_binary" --list-filters -e -m printer/p1600dn -p "$chain_ppd" \
            -i text/plain "$test_root/full-pipeline.txt" >"$filters" 2>"$test_root/$3-list.log"; then
        sed 's/^/        /' "$test_root/$3-list.log" >&2
        fail "CUPS could not resolve the complete PPD filter chain ($3)"
    fi
    [ "$(sed -n '1p' "$filters")" = "cgtexttopdf" ] || fail "full chain did not start with cgtexttopdf ($3)"
    [ "$(sed -n '2p' "$filters")" = "cgpdftoraster" ] || fail "full chain omitted cgpdftoraster ($3)"
    [ "$(sed -n '3p' "$filters")" = "$filter" ] || fail "full chain omitted the P1600dn filter ($3)"
    [ "$(wc -l <"$filters" | tr -d ' ')" -eq 3 ] || fail "full chain contains unexpected filters ($3)"
}

pipeline_ppd="$test_root/full-pipeline.ppd"
pipeline_filter=$(ppd_filter_path "$filter_binary" "$test_root/rastertop1600dn") || fail "could not link the filter"
write_pipeline_ppd "$pipeline_filter" "$pipeline_ppd"
check_chain "$pipeline_ppd" "$pipeline_filter" full-pipeline
echo "    [PASS] PPD resolves cgtexttopdf -> cgpdftoraster -> $(basename "$filter_binary")"

# A driver folder whose path is too long for a PPD line.
long_dir="$test_root/$(printf 'd%.0s' {1..60})/$(printf 'e%.0s' {1..60})/$(printf 'f%.0s' {1..60})"
mkdir -p "$long_dir" && ln -s "$filter_binary" "$long_dir/rastertop1600dn" || fail "could not prepare a long path"
long_filter=$(ppd_filter_path "$long_dir/rastertop1600dn" "$test_root/rastertop1600dn-long") ||
    fail "could not link the filter of a long path"
write_pipeline_ppd "$long_filter" "$test_root/long-path.ppd"
check_chain "$test_root/long-path.ppd" "$long_filter" long-path
echo "    [PASS] A $(printf '%s' "$long_dir/rastertop1600dn" | wc -c | tr -d ' ')-byte filter path, too long for a PPD line, still resolves"

run_chain chain-default "$pipeline_ppd" 2 2 \
    -- paper=9 source=7 media=1 bpp=2 density=3
run_chain chain-options "$pipeline_ppd" 3 4 \
    PageSize=A4 Duplex=DuplexNoTumble pjlDensity=4 EconoMode=True pjlJamRecovery=True InputSlot=Auto \
    -- duplex=long sheets=auto padded=1 density=4 econo=1 jam=1 source=7

# An absent duplex unit is a queue (PPD) setting, not a job option.
no_dupunit_ppd="$test_root/full-pipeline-no-dupunit.ppd"
sed 's/^\*DefaultDupUnit: True$/*DefaultDupUnit: False/' "$pipeline_ppd" >"$no_dupunit_ppd"
grep -Fqx '*DefaultDupUnit: False' "$no_dupunit_ppd" || fail "could not prepare the DupUnit=False PPD"
run_chain chain-no-dupunit "$no_dupunit_ppd" 2 2 Duplex=DuplexNoTumble \
    -- duplex=long sheets=manual

echo "[+] All real macOS CUPS pipeline modes passed."
