#!/bin/bash
# End-to-end tests for p1600dn-airprint in test mode (--spool-dir): the IPP
# Everywhere suite of ipptool, AirPrint checks with real AirPrint documents
# (Apple raster made by macOS cgpdftoraster, PWG raster, JPEG, PDF with
# options), then every spooled document through the real driver chain
# (cupsfilter with the P1600dn PPD -> rastertop1600dn and its page guard),
# the HTTP pages, Bonjour on the local-only interface and a clean SIGTERM.
# No CUPS queue is used and nothing is advertised on the network.
#
# Usage: test_airprint.sh <daemon> <driver-filter> <ppd>
#   P1600DN_AIRPRINT_TEST   test_airprint (writes the PWG sample; default:
#                             next to the daemon)
#   P1600DN_AIRPRINT_QUICK  1: only the IPP Everywhere suite (ThreadSanitizer runs)
set -eu

fail() {
    echo "    [FAIL] $1" >&2
    exit 1
}

[ $# -eq 3 ] || { echo "usage: $0 <daemon> <driver-filter> <ppd>" >&2; exit 2; }
script_dir=$(cd "$(dirname "$0")" && pwd)
module_dir=$(cd "$script_dir/.." && pwd)
absolute() { (cd "$(dirname "$1")" && echo "$(pwd)/$(basename "$1")"); }
[ -x "$1" ] || fail "daemon is not executable: $1"
[ -x "$2" ] || fail "driver filter is not executable: $2"
[ -f "$3" ] || fail "PPD is missing: $3"
daemon=$(absolute "$1")
filter=$(absolute "$2")
ppd=$(absolute "$3")
helper="${P1600DN_AIRPRINT_TEST:-$(dirname "$daemon")/test_airprint}"
quick="${P1600DN_AIRPRINT_QUICK:-0}"
icon="$module_dir/assets/P1600dnPrinter.png"
samples=/usr/share/cups/ipptool

# sudo may drop /usr/sbin from PATH.
tool() {
    command -v "$1" 2>/dev/null || { [ -x "/usr/sbin/$1" ] && echo "/usr/sbin/$1"; } || true
}
ipptool_binary=$(tool ipptool)
cupsfilter_binary=$(tool cupsfilter)
dnssd_binary=$(tool dns-sd)
curl_binary=$(tool curl)
[ -n "$ipptool_binary" ] && [ -f "$samples/ipp-everywhere.test" ] || {
    echo "    [SKIP] ipptool or its IPP Everywhere tests are unavailable" >&2
    exit 0
}
[ -n "$cupsfilter_binary" ] || fail "cupsfilter is unavailable"

test_root=$(mktemp -d "${TMPDIR:-/tmp}/p1600dn-airprint.XXXXXX")
pids=()
cleanup() {
    local pid
    for pid in ${pids[@]+"${pids[@]}"}; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# Sanitizer reports in a daemon log are failures.
check_log() {
    if grep -Eq 'ERROR: AddressSanitizer|runtime error:|WARNING: ThreadSanitizer|ERROR: LeakSanitizer' "$1"; then
        sed 's/^/        /' "$1" | tail -60 >&2
        fail "sanitizer report in $(basename "$1")"
    fi
}

# start_daemon log args... : starts the daemon on a free loopback port and
# waits for its readiness line; sets daemon_pid and port.
start_daemon() {
    local log="$1"
    shift
    "$daemon" --listen-loopback --port 0 --icon "$icon" "$@" >"$log" 2>&1 &
    daemon_pid=$!
    pids+=("$daemon_pid")
    port=""
    for _ in $(seq 1 200); do
        port=$(sed -n 's/^airprint: listening on port \([0-9][0-9]*\)$/\1/p' "$log" | head -1)
        [ -n "$port" ] && break
        kill -0 "$daemon_pid" 2>/dev/null || break
        sleep 0.05
    done
    if [ -z "$port" ]; then
        sed 's/^/        /' "$log" >&2
        fail "the daemon did not get ready"
    fi
}

# stop_daemon pid log: SIGTERM must end it cleanly.
stop_daemon() {
    local pid="$1" log="$2" status=0
    kill -TERM "$pid"
    wait "$pid" || status=$?
    if [ "$status" -ne 0 ]; then
        sed 's/^/        /' "$log" | tail -30 >&2
        fail "daemon exit status $status after SIGTERM"
    fi
    grep -q '^airprint: stopped$' "$log" || fail "daemon did not stop cleanly"
    check_log "$log"
}

echo "[*] AirPrint end-to-end tests ($(basename "$daemon"))"

# 1. The project PPD with the given driver filter and without the command filter.
test_ppd="$test_root/test.ppd"
awk -v filter="$filter" '
    index($0, "*cupsFilter:") == 1 {
        if (index($0, "application/vnd.cups-command")) next
        print "*cupsFilter: \"application/vnd.cups-raster 0 " filter "\""
        next
    }
    { print }
' "$ppd" >"$test_ppd"
[ "$(grep -c '^\*cupsFilter:' "$test_ppd")" -eq 1 ] || fail "could not prepare the test PPD"
grep -Fqx "*cupsFilter: \"application/vnd.cups-raster 0 $filter\"" "$test_ppd" || fail "test PPD lacks the filter"

# 2. The daemon in test mode.
spool="$test_root/spool"
mkdir "$spool"
log="$test_root/daemon.log"
start_daemon "$log" --no-dnssd --queue TestQ --make-model "HP LaserJet Professional P1606dn" --spool-dir "$spool"
main_pid=$daemon_pid
uri="ipp://127.0.0.1:$port/ipp/print/TestQ"
echo "    daemon on port $port (pid $main_pid)"

# 3. ipptool's IPP Everywhere suite (it includes ipp-2.0.test and ipp-1.1.test).
# The PWG raster sample files are not installed on macOS: ipptool stops at the
# first of them, after every other test.
everywhere="$test_root/ipp-everywhere.log"
status=0
(cd "$test_root" && "$ipptool_binary" -V 2.0 -tf "$samples/testfile.pdf" "$uri" "$samples/ipp-everywhere.test") \
    >"$everywhere" 2>&1 || status=$?
passed=$(grep -c '\[PASS\]' "$everywhere" || true)
failed=$(grep -c '\[FAIL\]' "$everywhere" || true)
skipped=$(grep -c '\[SKIP\]' "$everywhere" || true)
if [ "$failed" -ne 0 ] || [ "$passed" -lt 30 ]; then
    sed 's/^/        /' "$everywhere" >&2
    fail "ipp-everywhere.test: $passed passed, $failed failed"
fi
if [ "$status" -ne 0 ] && ! grep -q 'pwg-raster-samples-.*cannot be read' "$everywhere"; then
    sed 's/^/        /' "$everywhere" >&2
    fail "ipptool exit status $status"
fi
grep -q 'PWG 5100.14 section 5.1/5.2 - Required Operations and Attributes.*\[PASS\]' "$everywhere" ||
    fail "the IPP Everywhere attribute test did not pass"
echo "    [PASS] ipp-everywhere.test: $passed passed, $failed failed, $skipped skipped (optional operations)"
if grep -q 'pwg-raster-samples-.*cannot be read' "$everywhere"; then
    echo "           PWG raster sample prints skipped: $samples/pwg-raster-samples-* are not installed"
fi

if [ "$quick" = "1" ]; then
    stop_daemon "$main_pid" "$log"
    echo "[+] AirPrint IPP Everywhere run passed."
    exit 0
fi

# 4. AirPrint documents: Apple raster and PWG raster from macOS's own
# cgpdftoraster, a PWG raster written by libcups, JPEG, and PDF with options.
# ipptool reads the gzip-compressed samples transparently; cupsfilter's
# filters need them plain.
plain() {
    if [ "$(head -c 2 "$1" | od -An -tx1 | tr -d ' \n')" = "1f8b" ]; then
        gzip -dc <"$1" >"$2"
    else
        cp "$1" "$2"
    fi
}
plain "$samples/onepage-a4.pdf" "$test_root/onepage-a4.pdf"
plain "$samples/testfile.pdf" "$test_root/testfile.pdf"
plain "$samples/testfile.jpg" "$test_root/testfile.jpg"
"$cupsfilter_binary" -p "$test_ppd" -m image/urf -i application/pdf "$test_root/onepage-a4.pdf" \
    >"$test_root/apple.urf" 2>"$test_root/urf.log" || fail "cupsfilter could not make Apple raster"
[ "$(head -c 7 "$test_root/apple.urf")" = "UNIRAST" ] || fail "cupsfilter did not write Apple raster"
"$cupsfilter_binary" -p "$test_ppd" -m image/pwg-raster -i application/pdf "$test_root/onepage-a4.pdf" \
    >"$test_root/apple.pwg" 2>"$test_root/pwg.log" || fail "cupsfilter could not make PWG raster"
[ "$(head -c 4 "$test_root/apple.pwg")" = "RaS2" ] || fail "cupsfilter did not write PWG raster"
pwg_rgb="$test_root/apple.pwg"
if [ -x "$helper" ]; then
    pwg_rgb="$test_root/libcups-srgb.pwg"
    "$helper" --write-pwg "$pwg_rgb" srgb_8 300 2 || fail "test_airprint could not write the PWG sample"
else
    echo "    [NOTE] $helper is missing; the sRGB PWG raster sample is skipped"
fi

cat >"$test_root/airprint.test" <<'EOF'
# AirPrint checks for p1600dn-airprint.
{
    NAME "AirPrint printer attributes"
    OPERATION Get-Printer-Attributes
    GROUP operation-attributes-tag
    ATTR charset attributes-charset utf-8
    ATTR naturalLanguage attributes-natural-language en
    ATTR uri printer-uri $uri
    ATTR name requesting-user-name $user
    ATTR keyword requested-attributes all,media-col-database
    STATUS successful-ok
    EXPECT urf-supported OF-TYPE keyword IN-GROUP printer-attributes-tag WITH-VALUE "V1.4"
    EXPECT urf-supported WITH-VALUE "W8"
    EXPECT urf-supported WITH-VALUE "SRGB24"
    EXPECT urf-supported WITH-VALUE "RS300-600"
    EXPECT urf-supported WITH-VALUE "DM1"
    EXPECT urf-supported WITH-VALUE "PQ4-5"
    EXPECT document-format-supported OF-TYPE mimeMediaType WITH-VALUE "image/urf"
    EXPECT document-format-supported WITH-VALUE "application/pdf"
    EXPECT document-format-supported WITH-VALUE "image/pwg-raster"
    EXPECT document-format-supported WITH-VALUE "image/png"
    EXPECT pwg-raster-document-sheet-back OF-TYPE keyword WITH-VALUE "normal"
    EXPECT pwg-raster-document-type-supported WITH-VALUE "sgray_8"
    EXPECT pwg-raster-document-type-supported WITH-VALUE "srgb_8"
    EXPECT media-col-database OF-TYPE collection COUNT 18
    EXPECT media-supported WITH-VALUE "jpn_oufuku_148x200mm"
    EXPECT media-source-supported WITH-VALUE "manual"
    EXPECT color-supported WITH-VALUE false
    EXPECT print-quality-supported WITH-VALUE 5
    EXPECT sides-supported WITH-VALUE "two-sided-short-edge"
    EXPECT printer-make-and-model WITH-VALUE "HP LaserJet Professional P1606dn"
    EXPECT printer-uuid OF-TYPE uri WITH-VALUE "/^urn:uuid:[0-9a-f-]{36}$/"
}
{
    NAME "Print-Job Apple raster (image/urf from cgpdftoraster)"
    OPERATION Print-Job
    GROUP operation-attributes-tag
    ATTR charset attributes-charset utf-8
    ATTR naturalLanguage attributes-natural-language en
    ATTR uri printer-uri $uri
    ATTR name requesting-user-name $user
    ATTR name job-name "airprint-urf"
    ATTR mimeMediaType document-format image/urf
    GROUP job-attributes-tag
    ATTR enum print-quality 4
    FILE $urf
    STATUS successful-ok
    EXPECT job-id OF-TYPE integer WITH-VALUE >0
}
{
    NAME "Wait for the URF job"
    OPERATION Get-Job-Attributes
    GROUP operation-attributes-tag
    ATTR charset attributes-charset utf-8
    ATTR naturalLanguage attributes-natural-language en
    ATTR uri printer-uri $uri
    ATTR integer job-id $job-id
    ATTR name requesting-user-name $user
    STATUS successful-ok
    EXPECT job-state WITH-VALUE 9 REPEAT-NO-MATCH REPEAT-LIMIT 60
}
{
    NAME "Print-Job PWG raster (cgpdftoraster)"
    OPERATION Print-Job
    GROUP operation-attributes-tag
    ATTR charset attributes-charset utf-8
    ATTR naturalLanguage attributes-natural-language en
    ATTR uri printer-uri $uri
    ATTR name requesting-user-name $user
    ATTR name job-name "airprint-pwg"
    ATTR mimeMediaType document-format image/pwg-raster
    FILE $pwg
    STATUS successful-ok
}
{
    NAME "Wait for the PWG raster job"
    OPERATION Get-Job-Attributes
    GROUP operation-attributes-tag
    ATTR charset attributes-charset utf-8
    ATTR naturalLanguage attributes-natural-language en
    ATTR uri printer-uri $uri
    ATTR integer job-id $job-id
    ATTR name requesting-user-name $user
    STATUS successful-ok
    EXPECT job-state WITH-VALUE 9 REPEAT-NO-MATCH REPEAT-LIMIT 60
}
{
    NAME "Print-Job PWG raster (sRGB, deflate, octet-stream)"
    OPERATION Print-Job
    GROUP operation-attributes-tag
    ATTR charset attributes-charset utf-8
    ATTR naturalLanguage attributes-natural-language en
    ATTR uri printer-uri $uri
    ATTR name requesting-user-name $user
    ATTR name job-name "airprint-pwg-srgb"
    ATTR mimeMediaType document-format application/octet-stream
    ATTR keyword compression deflate
    GROUP job-attributes-tag
    ATTR keyword sides two-sided-short-edge
    COMPRESSION deflate
    FILE $pwg_rgb
    STATUS successful-ok
}
{
    NAME "Wait for the sRGB PWG raster job"
    OPERATION Get-Job-Attributes
    GROUP operation-attributes-tag
    ATTR charset attributes-charset utf-8
    ATTR naturalLanguage attributes-natural-language en
    ATTR uri printer-uri $uri
    ATTR integer job-id $job-id
    ATTR name requesting-user-name $user
    STATUS successful-ok
    EXPECT job-state WITH-VALUE 9 REPEAT-NO-MATCH REPEAT-LIMIT 60
}
{
    NAME "Print-Job JPEG"
    OPERATION Print-Job
    GROUP operation-attributes-tag
    ATTR charset attributes-charset utf-8
    ATTR naturalLanguage attributes-natural-language en
    ATTR uri printer-uri $uri
    ATTR name requesting-user-name $user
    ATTR name job-name "airprint-jpeg"
    ATTR mimeMediaType document-format image/jpeg
    GROUP job-attributes-tag
    ATTR keyword media na_letter_8.5x11in
    ATTR keyword print-scaling fit
    FILE $jpeg
    STATUS successful-ok
}
{
    NAME "Wait for the JPEG job"
    OPERATION Get-Job-Attributes
    GROUP operation-attributes-tag
    ATTR charset attributes-charset utf-8
    ATTR naturalLanguage attributes-natural-language en
    ATTR uri printer-uri $uri
    ATTR integer job-id $job-id
    ATTR name requesting-user-name $user
    STATUS successful-ok
    EXPECT job-state WITH-VALUE 9 REPEAT-NO-MATCH REPEAT-LIMIT 60
}
{
    NAME "Print-Job PDF, two-sided-long-edge, A4, 2 copies"
    OPERATION Print-Job
    GROUP operation-attributes-tag
    ATTR charset attributes-charset utf-8
    ATTR naturalLanguage attributes-natural-language en
    ATTR uri printer-uri $uri
    ATTR name requesting-user-name $user
    ATTR name job-name "airprint-pdf-duplex"
    ATTR mimeMediaType document-format application/pdf
    GROUP job-attributes-tag
    ATTR keyword sides two-sided-long-edge
    ATTR keyword media iso_a4_210x297mm
    ATTR integer copies 2
    FILE $pdf
    STATUS successful-ok
}
{
    NAME "Wait for the PDF job"
    OPERATION Get-Job-Attributes
    GROUP operation-attributes-tag
    ATTR charset attributes-charset utf-8
    ATTR naturalLanguage attributes-natural-language en
    ATTR uri printer-uri $uri
    ATTR integer job-id $job-id
    ATTR name requesting-user-name $user
    STATUS successful-ok
    EXPECT job-state WITH-VALUE 9 REPEAT-NO-MATCH REPEAT-LIMIT 60
}
EOF
airprint_log="$test_root/airprint-ipptool.log"
if ! (cd "$test_root" && "$ipptool_binary" -V 2.0 -t -d "urf=$test_root/apple.urf" -d "pwg=$test_root/apple.pwg" \
        -d "pwg_rgb=$pwg_rgb" -d "jpeg=$test_root/testfile.jpg" -d "pdf=$test_root/testfile.pdf" "$uri" \
        "$test_root/airprint.test") >"$airprint_log" 2>&1; then
    sed 's/^/        /' "$airprint_log" >&2
    fail "AirPrint ipptool checks"
fi
grep -q '\[FAIL\]' "$airprint_log" && fail "AirPrint ipptool checks"
echo "    [PASS] AirPrint: urf-supported, formats, media-col-database;"
echo "           URF, PWG (sGray, sRGB deflate), JPEG and PDF jobs"

# The options each AirPrint job got.
job_with() {
    grep -l "^title=$1\$" "$spool"/job-*.options | head -1 | sed 's/\.options$//'
}
expect_option() {
    grep -Fqx "$2" "$1.options" || { sed 's/^/        /' "$1.options" >&2; fail "$(basename "$1") lacks $2"; }
}
urf_job=$(job_with airprint-urf)
pwg_job=$(job_with airprint-pwg)
rgb_job=$(job_with airprint-pwg-srgb)
jpeg_job=$(job_with airprint-jpeg)
pdf_job=$(job_with airprint-pdf-duplex)
for job in "$urf_job" "$pwg_job" "$rgb_job" "$jpeg_job" "$pdf_job"; do
    [ -n "$job" ] || fail "an AirPrint job was not spooled"
done
expect_option "$urf_job" "document-format=application/pdf"
expect_option "$urf_job" "PageSize=A4"
expect_option "$urf_job" "Resolution=600dpi"
expect_option "$pwg_job" "document-format=application/pdf"
expect_option "$pwg_job" "PageSize=A4"
expect_option "$rgb_job" "sides=two-sided-short-edge"
expect_option "$jpeg_job" "document-format=image/jpeg"
expect_option "$jpeg_job" "PageSize=Letter"
expect_option "$jpeg_job" "fit-to-page=true"
expect_option "$pdf_job" "document-format=application/pdf"
expect_option "$pdf_job" "sides=two-sided-long-edge"
expect_option "$pdf_job" "PageSize=A4"
expect_option "$pdf_job" "copies=2"
for document in "$urf_job.pdf" "$pwg_job.pdf" "$rgb_job.pdf" "$jpeg_job.jpg" "$pdf_job.pdf"; do
    [ -s "$document" ] || fail "$(basename "$document") is missing"
done
cmp -s "$pdf_job.pdf" "$test_root/testfile.pdf" || fail "the PDF did not pass through unchanged"
cmp -s "$jpeg_job.jpg" "$test_root/testfile.jpg" || fail "the JPEG did not pass through unchanged"
echo "    [PASS] Spooled: URF/PWG as PDF with PageSize, PDF/JPEG unchanged, CUPS options (sides, copies, PageSize, ...)"

# 5. Every spooled document through the real driver chain and its page guard.
documents=0
for options in "$spool"/job-*.options; do
    base=${options%.options}
    format=$(sed -n 's/^document-format=//p' "$options")
    case "$format" in
        application/pdf) document="$base.pdf" ;;
        image/jpeg) document="$base.jpg" ;;
        image/png) document="$base.png" ;;
        *) fail "unexpected format $format in $(basename "$options")" ;;
    esac
    [ -s "$document" ] || fail "$(basename "$document") is missing"
    cups_options=()
    while IFS= read -r line; do
        case "$line" in
            document-format=* | title=* | "") ;;
            *) cups_options+=(-o "$line") ;;
        esac
    done <"$options"
    name=$(basename "$base")
    if ! "$cupsfilter_binary" -e -m printer/p1600dn -p "$test_ppd" -i "$format" \
            ${cups_options[@]+"${cups_options[@]}"} "$document" >"$test_root/$name.zjs" \
            2>"$test_root/$name-driver.log"; then
        sed 's/^/        /' "$test_root/$name-driver.log" | tail -20 >&2
        fail "the driver chain failed for $name ($format)"
    fi
    if grep -q 'ERROR:' "$test_root/$name-driver.log"; then
        grep 'ERROR:' "$test_root/$name-driver.log" | sed 's/^/        /' >&2
        fail "the driver chain logged an error for $name"
    fi
    grep -q '^PAGE:' "$test_root/$name-driver.log" || fail "the driver sent no page for $name"
    [ -s "$test_root/$name.zjs" ] || fail "the driver wrote nothing for $name"
    rm -f "$test_root/$name.zjs"
    documents=$((documents + 1))
done
[ "$documents" -ge 15 ] || fail "only $documents documents were spooled"
echo "    [PASS] All $documents spooled documents printed through cupsfilter -> rastertop1600dn"
echo "           (PAGE lines, no ERROR from the page guard)"

# 6. HTTP pages.
if [ -n "$curl_binary" ]; then
    "$curl_binary" -fsS "http://127.0.0.1:$port/" | grep -q 'TestQ' || fail "status page"
    "$curl_binary" -fsS -o "$test_root/icon.png" "http://127.0.0.1:$port/icon.png" || fail "icon"
    cmp -s "$test_root/icon.png" "$icon" || fail "the icon differs from $icon"
    "$curl_binary" -fsS -g "http://[::1]:$port/" | grep -q 'TestQ' || fail "status page over IPv6 (::1)"
    code=$("$curl_binary" -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/missing")
    [ "$code" = "404" ] || fail "missing page answered $code"
    code=$("$curl_binary" -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/ipp/print/TestQ")
    [ "$code" = "405" ] || fail "GET of the printer answered $code"
    echo "    [PASS] HTTP: status page (IPv4 and IPv6 loopback), /icon.png, 404, 405"
fi
stop_daemon "$main_pid" "$log"
echo "    [PASS] SIGTERM: graceful shutdown, exit status 0"

# 7. Bonjour on the local-only interface (never on the network).
if [ -z "$dnssd_binary" ] || ! "$dnssd_binary" -lo -t 1 -B _airprint-probe._tcp local. >/dev/null 2>&1; then
    echo "    [SKIP] dns-sd -lo is not supported here; Bonjour test skipped"
else
    queue="AirTest$$"
    bonjour_spool="$test_root/bonjour"
    mkdir "$bonjour_spool"
    bonjour_log="$test_root/bonjour.log"
    start_daemon "$bonjour_log" --local-only --queue "$queue" --make-model "HP LaserJet Professional P1606dn" \
        --spool-dir "$bonjour_spool"
    bonjour_pid=$daemon_pid
    service=""
    for _ in $(seq 1 100); do
        service=$(sed -n 's/^airprint: Bonjour: "\(.*\)" (_ipp\._tcp.*/\1/p' "$bonjour_log" | head -1)
        [ -n "$service" ] && break
        sleep 0.1
    done
    [ -n "$service" ] || { sed 's/^/        /' "$bonjour_log" >&2; fail "the service was not registered"; }
    "$dnssd_binary" -lo -t 3 -B _ipp._tcp,_universal local. >"$test_root/browse.log" 2>&1 || true
    if ! grep -Fq "$service" "$test_root/browse.log"; then
        cat "$test_root/browse.log" >&2
        fail "browse did not find \"$service\""
    fi
    "$dnssd_binary" -lo -t 3 -L "$service" _ipp._tcp local. >"$test_root/resolve.log" 2>&1 || true
    for key in "URF=V1.4,W8,SRGB24,RS300-600,CP1,DM1,IS1,MT1-2-3-4-5-8,OB10,PQ4-5" "pdl=application/pdf" \
            "rp=ipp/print/$queue" "kind=document" "Duplex=T" "Color=F" "UUID="; do
        grep -Fq "$key" "$test_root/resolve.log" || { cat "$test_root/resolve.log" >&2; fail "TXT lacks $key"; }
    done
    if ! grep -Eq "can be reached at .*:$port " "$test_root/resolve.log"; then
        cat "$test_root/resolve.log" >&2
        fail "the service does not resolve to port $port"
    fi
    stop_daemon "$bonjour_pid" "$bonjour_log"
    echo "    [PASS] Bonjour (local-only): \"$service\" browsed as _ipp._tcp,_universal,"
    echo "           resolved with its URF/pdl TXT keys"
fi

# 8. Serving every P1600dn queue with none set up (CUPS unreachable here), the
# daemon waits for one instead of exiting, and still stops cleanly.
wait_log="$test_root/wait.log"
CUPS_SERVER=/nonexistent/p1600dn-cupsd.sock "$daemon" --listen-loopback --port 0 --no-dnssd >"$wait_log" 2>&1 &
wait_pid=$!
pids+=("$wait_pid")
for _ in $(seq 1 100); do
    grep -q 'waiting for a P1600dn series CUPS queue' "$wait_log" && break
    sleep 0.05
done
grep -q 'waiting for a P1600dn series CUPS queue' "$wait_log" || { cat "$wait_log" >&2; fail "the daemon did not wait for a queue"; }
sleep 0.5
kill -0 "$wait_pid" 2>/dev/null || { cat "$wait_log" >&2; fail "the daemon exited without a queue"; }
stop_daemon "$wait_pid" "$wait_log"
echo "    [PASS] No P1600dn queue: the daemon waits for one (no exit, no launchd restart loop)"

check_log "$log"
echo "[+] All AirPrint end-to-end tests passed."
