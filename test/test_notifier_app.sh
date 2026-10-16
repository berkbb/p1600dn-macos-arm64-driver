#!/bin/bash
# Checks the built "P1600dn Notifier.app": arm64 for macOS 11+, signed with
# the hardened runtime, an LSUIElement agent with its bundle identifier, the
# same Notifier.strings keys in all 15 languages, and its LaunchAgent plist.
# In a logged-in GUI session (not as root) it then runs the agent against a
# stand-in cupsd (test/test_notify.c --serve) that scripts the printer: out
# of paper with the PPD's text, toner 40 -> 12 -> 4 -> 0 (the last through the
# ReportLevels job the agent sends after a finished job), manual duplex with
# the PPD's text, and a restart that must not repeat what was already shown. The
# notices go to a log file (P1600DN_NOTIFIER_LOG); Notification Center and
# its permission prompt are never used.
#
# "P1600dn Notifier.app" paketini denetler.
#
# Usage: test/test_notifier_app.sh <P1600dn Notifier.app>
set -euo pipefail
app="$1"
root="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "    [FAIL] $*" >&2; exit 1; }
languages=(en da de es fi fr it ko nl no pt sv zh_CN zh_TW tr ro)
info="$app/Contents/Info.plist"
plist_value() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null; }

binary="$app/Contents/MacOS/P1600dnNotifier"
[ -x "$binary" ] || fail "App executable missing"
/usr/bin/lipo "$binary" -verify_arch arm64 || fail "Not arm64"
[ "$(/usr/bin/lipo -archs "$binary")" = "arm64" ] || fail "Not arm64 only"
minimum=$(/usr/bin/otool -l "$binary" | awk '/LC_BUILD_VERSION/ {b=1} b && $1 == "minos" {print $2; exit}')
[ "$minimum" = "11.0" ] || fail "Minimum macOS is $minimum, not 11.0"
/usr/bin/codesign --verify --strict --deep "$app" || fail "Signature invalid"
signature=$(/usr/bin/codesign -dv "$app" 2>&1)
case "$signature" in *"flags=0x10002(adhoc,runtime)"*) ;; *) fail "Not signed ad hoc with the hardened runtime" ;; esac
case "$signature" in *"Identifier=com.berkbb.p1600dn.notifier"*) ;; *) fail "Signing identifier" ;; esac
/usr/bin/plutil -lint "$info" >/dev/null || fail "Invalid Info.plist"
[ "$(plist_value "$info" CFBundleIdentifier)" = "com.berkbb.p1600dn.notifier" ] || fail "Bundle identifier"
[ "$(plist_value "$info" CFBundleExecutable)" = "P1600dnNotifier" ] || fail "CFBundleExecutable"
[ "$(plist_value "$info" LSUIElement)" = "true" ] || fail "LSUIElement is not true"
[ "$(plist_value "$info" LSMinimumSystemVersion)" = "11.0" ] || fail "LSMinimumSystemVersion"
version=$(sed -n 's/^\*FileVersion: "\(.*\)"$/\1/p' "$root/ppd/P1600dn-Series.ppd")
[ -n "$version" ] && [ "$(plist_value "$info" CFBundleShortVersionString)" = "$version" ] || fail "Version is not the PPD's ($version)"
[ "$(plist_value "$info" CFBundleIconFile)" = "P1600dnApp" ] || fail "Icon name"
[ -f "$app/Contents/Resources/P1600dnApp.icns" ] || fail "Icon file missing"
echo "    [PASS] Notifier app: arm64 only, macOS 11.0+, hardened runtime signature, LSUIElement agent, Info.plist"

declared=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleLocalizations' "$info" | sed -n 's/^ *\([a-zA-Z_]*\)$/\1/p' | sort)
[ "$declared" = "$(printf '%s\n' "${languages[@]}" | sort)" ] || fail "CFBundleLocalizations: $declared"
keys() { /usr/bin/plutil -convert xml1 -o - "$1" | sed -n 's|.*<key>\(.*\)</key>.*|\1|p' | sort; }
english="$app/Contents/Resources/en.lproj/Notifier.strings"
[ -f "$english" ] || fail "Missing en.lproj/Notifier.strings"
english_keys=$(keys "$english")
[ "$(echo "$english_keys" | wc -l | tr -d ' ')" -ge 14 ] || fail "English table is incomplete"
for language in "${languages[@]}"; do
    file="$app/Contents/Resources/$language.lproj/Notifier.strings"
    [ -f "$file" ] || fail "Missing $language.lproj/Notifier.strings"
    /usr/bin/plutil -lint "$file" >/dev/null || fail "Invalid $file"
    [ "$(keys "$file")" = "$english_keys" ] || fail "$language.lproj/Notifier.strings has other keys than English"
    for key in Toner-low Toner-very-low Toner-replaced; do
        text=$(/usr/bin/plutil -extract "$key" raw -o - "$file")
        [ "$(printf '%s' "$text" | grep -o '%d' | wc -l | tr -d ' ')" = "1" ] || fail "$language $key needs one %d"
    done
done
echo "    [PASS] Notifier localizations: ${#languages[@]} languages with the same $(echo "$english_keys" | wc -l | tr -d ' ') keys"

agent="$root/notifier/com.berkbb.p1600dn.notifier.plist"
/usr/bin/plutil -lint "$agent" >/dev/null || fail "Invalid LaunchAgent plist"
[ "$(plist_value "$agent" Label)" = "com.berkbb.p1600dn.notifier" ] || fail "LaunchAgent label"
[ "$(plist_value "$agent" Program)" = "/Library/Printers/p1600dn/P1600dn Notifier.app/Contents/MacOS/P1600dnNotifier" ] ||
    fail "LaunchAgent program path"
[ "$(plist_value "$agent" RunAtLoad)" = "true" ] || fail "RunAtLoad"
[ "$(plist_value "$agent" KeepAlive:SuccessfulExit)" = "false" ] || fail "KeepAlive SuccessfulExit"
[ "$(plist_value "$agent" LimitLoadToSessionType)" = "Aqua" ] || fail "LimitLoadToSessionType"
[ "$(plist_value "$agent" ProcessType)" = "Background" ] || fail "ProcessType"
echo "    [PASS] LaunchAgent: valid plist, program in /Library/Printers/p1600dn, Aqua sessions, kept alive on failure"

if [ "$(id -u)" -eq 0 ] || [ "$(launchctl managername 2>/dev/null)" != "Aqua" ]; then
    echo "    [SKIP] Notifier run against a stand-in cupsd (needs a logged-in user's GUI session)"
    exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/p1600dn-notifier.XXXXXX")
pids=()
cleanup() {
    for pid in "${pids[@]+"${pids[@]}"}"; do kill "$pid" 2>/dev/null || true; done
    rm -rf "$work"
}
trap cleanup EXIT
"${CC:-clang}" -O2 -std=c11 -Wall -Wextra -Werror -arch arm64 -mmacosx-version-min=11.0 -I"$root/src" \
    "$root/test/test_notify.c" "$root/src/p1600_notify.c" -lcups -o "$work/test_notify" || fail "Stand-in did not build"
queue="P1600DN_Notify_Test"
title="Office P1606dn"

# Runs the agent against the stand-in's scenario until it has shown
# `count` notices; the log lands in $work/<scenario>.log.
run_agent() {
    local scenario="$1" language="$2" count="$3"
    rm -f "$work/port"
    "$work/test_notify" --serve "$work/port" "$scenario" >"$work/$scenario.serve" 2>"$work/$scenario.steps" &
    local server=$!
    pids+=("$server")
    for _ in $(seq 1 100); do [ -s "$work/port" ] && break; sleep 0.1; done
    [ -s "$work/port" ] || fail "Stand-in cupsd did not start"
    env P1600DN_CUPS_SERVER="127.0.0.1:$(cat "$work/port")" P1600DN_NOTIFIER_QUEUES="$queue" \
        P1600DN_NOTIFIER_LOG="$work/$scenario.log" P1600DN_NOTIFIER_EXIT_AFTER="$count" \
        P1600DN_NOTIFIER_LANG="$language" P1600DN_NOTIFIER_STATE="$work/state.plist" "$binary" &
    local agent=$!
    pids+=("$agent")
    # Ends a hanging run after 150 s.
    ( trap 'kill "$sleeper" 2>/dev/null; exit 0' TERM; sleep 150 & sleeper=$!; wait "$sleeper"
      kill "$agent" "$server" 2>/dev/null ) &
    local watchdog=$!
    pids+=("$watchdog")
    local agent_status=0 server_status=0
    wait "$agent" || agent_status=$?
    wait "$server" || server_status=$?
    kill "$watchdog" 2>/dev/null || true
    wait "$watchdog" 2>/dev/null || true
    [ "$agent_status" -eq 0 ] || fail "Agent ($scenario) exited with $agent_status; $(tr '\n' ';' <"$work/$scenario.steps")"
    [ "$server_status" -eq 0 ] || fail "Stand-in ($scenario): $(cat "$work/$scenario.serve") $(tr '\n' ';' <"$work/$scenario.steps")"
}

text() { /usr/bin/plutil -extract "$2" raw -o - "$app/Contents/Resources/$1.lproj/Notifier.strings"; }
expect_log() {
    local log="$1" expected="$2"
    if [ "$(cat "$log")" != "$expected" ]; then
        printf 'Expected:\n%s\nGot:\n%s\n' "$expected" "$(cat "$log")" >&2
        fail "Notices of $(basename "$log")"
    fi
    [ -z "$(sort "$log" | uniq -d)" ] || fail "Duplicate notices in $(basename "$log")"
}

run_agent main en 5
grep -q '^steps=ok cancel=ok reportlevels=ok jobs=1$' "$work/main.serve" || fail "Stand-in: $(cat "$work/main.serve")"
tab=$'\t'
expected="attention.media-empty$tab$queue$tab$title${tab}Out of Paper: Refill the paper tray, then press Go on the printer.
toner.low$tab$queue$tab$title$tab$(printf "$(text en Toner-low)" 12)
toner.very-low$tab$queue$tab$title$tab$(printf "$(text en Toner-very-low)" 4)
toner.empty$tab$queue$tab$title$tab$(text en Toner-empty)
attention.media-needed$tab$queue$tab$title${tab}Manual Duplex: For the second sides, move the printed sheets from the output bin to Tray 1, printed side down, then press Go on the printer."
expect_log "$work/main.log" "$expected"
[ "$(/usr/bin/plutil -extract "Queues.$queue.TonerBucket" raw -o - "$work/state.plist")" = "3" ] || fail "Toner bucket not remembered"
[ "$(/usr/bin/plutil -extract "Queues.$queue.Reasons" raw -o - "$work/state.plist")" = "media-needed" ] ||
    fail "Reported reasons not remembered"
echo "    [PASS] Agent run: out-of-paper text, toner low/very low/empty (ReportLevels after a job), manual duplex text, no duplicates"

# Restarted: the waiting stack and the empty cartridge were shown before.
# A backend connecting for 2 s stays quiet; an unreachable printer is shown
# after the settle time.
run_agent persist tr 2
grep -q '^steps=ok cancel=ok' "$work/persist.serve" || fail "Stand-in: $(cat "$work/persist.serve")"
expected="attention.media-jam$tab$queue$tab$title$tab$(text tr Reason-media-jam)
attention.connecting-to-device$tab$queue$tab$title${tab}The printer is unreachable at this time."
expect_log "$work/persist.log" "$expected"
echo "    [PASS] Agent restart: nothing repeated, jam in Turkish, brief connecting quiet, unreachable shown after settling"
