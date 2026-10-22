#!/bin/bash
# ==============================================================================
# P1600dn Series - Native ARM Driver Manager (macOS), for HP LaserJet
# Professional P1606dn, P1607dn, P1608dn and P1609dn printers
# Central entry point for installation, maintenance, tests, and CUPS activation.
# ==============================================================================

set -u

MANAGER_SOURCE="${BASH_SOURCE[0]}"
if ! cd "$(dirname "$MANAGER_SOURCE")"; then
    echo "Sürücü dizinine geçilemedi." >&2
    exit 1
fi

DRIVER_DIR="$PWD"
MANAGER_PATH="$DRIVER_DIR/$(basename "$MANAGER_SOURCE")"
INSTALL_DIR="/Library/Printers/p1600dn"
PPD_INSTALL_DIR="/Library/Printers/PPDs/Contents/Resources"
PPD_FILE="$DRIVER_DIR/ppd/P1600dn-Series.ppd"
INSTALLED_PPD="$PPD_INSTALL_DIR/P1600dn-Series.ppd"
# Driver 5.0 and earlier installed under these names; installing moves their
# queues to this driver and removes them (retire_legacy_driver). The folder
# lies next to INSTALL_DIR, so tests that move INSTALL_DIR move it too.
LEGACY_INSTALL_DIR="${INSTALL_DIR%/*}/hp_p1606dn"
LEGACY_FILTER_NAME="rastertohpp1606dn"
LEGACY_PPD="$PPD_INSTALL_DIR/HP-LaserJet-P1606dn-ARM.ppd"
LEGACY_NOTIFIER_LABEL="com.berkbb.hpp1606dn.notifier"
LEGACY_AIRPRINT_LABEL="com.berkbb.hpp1606dn.airprint"
# Files of the OpenPrinting foo2zjs fallback that driver 3.1-3.2 could install
# in LEGACY_INSTALL_DIR and PPD_INSTALL_DIR. It encoded pages itself, past the
# page guard, so it was removed; these files are only cleaned up
# (retire_openprinting_fallback).
OPENPRINTING_FILTER="$LEGACY_INSTALL_DIR/rastertohpp1606dn-openprinting"
OPENPRINTING_ENCODER="$LEGACY_INSTALL_DIR/foo2zjs-openprinting"
OPENPRINTING_LICENSE="$LEGACY_INSTALL_DIR/OpenPrinting-foo2zjs-COPYING"
OPENPRINTING_INSTALLED_PPD="$PPD_INSTALL_DIR/HP-LaserJet-P1606dn-OpenPrinting.ppd"
CUPS_PPD_DIR="/etc/cups/ppd"
# Apple Silicon Macs run macOS 11 Big Sur or later; every binary targets it.
MACOS_MIN_VERSION="11.0"
# The printer utility app (the PPD's APPrinterUtilityPath).
UTILITY_APP_NAME="P1600dn Series Utility.app"
# Background agents in each user's session: printer notifications (always)
# and the AirPrint server (only when enabled).
NOTIFIER_APP_NAME="P1600dn Notifier.app"
NOTIFIER_LABEL="com.berkbb.p1600dn.notifier"
AIRPRINT_LABEL="com.berkbb.p1600dn.airprint"
LAUNCH_AGENTS_DIR="/Library/LaunchAgents"
LAUNCH_DAEMONS_DIR="/Library/LaunchDaemons"
LAUNCHCTL="/bin/launchctl"
# The apps are copied as root, which Launch Services does not notice: until
# they are registered, Finder shows them crossed out, Printers & Scanners
# cannot open the utility, and the notifier aborts in
# UNUserNotificationCenter. Tests replace it.
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
# Gutenprint has no P1600dn driver, and this driver replaces it: the first
# install offers to remove it, and every uninstall removes it once the
# uninstall is confirmed (purge_gutenprint). Tests move its paths under
# GUTENPRINT_ROOT and replace pkgutil.
GUTENPRINT_ROOT=""
PKGUTIL="/usr/sbin/pkgutil"

if [ -t 1 ]; then
    CYAN='\033[0;36m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    RED='\033[0;31m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    CYAN=''
    GREEN=''
    YELLOW=''
    RED=''
    BOLD=''
    NC=''
fi

print_color() {
    local color="$1"
    shift
    printf '%b%s%b\n' "$color" "$*" "$NC"
}

clear_screen() {
    if [ -t 1 ] && [ -n "${TERM:-}" ]; then
        clear 2>/dev/null || true
    fi
}

press_any_key() {
    printf '\n%bDevam etmek için [Enter] tuşuna basın... / Press [Enter] to continue...%b' "$YELLOW" "$NC"
    read -r _unused || true
}

check_sudo() {
    if [ "$EUID" -eq 0 ]; then
        return 0
    fi
    print_color "$YELLOW" "[!] Bu işlem yönetici (sudo) yetkisi gerektiriyor / Administrator (sudo) privileges required."
    if ! sudo -v; then
        print_color "$RED" "[-] Yönetici doğrulaması başarısız oldu / Administrator authorization failed."
        return 1
    fi
}

require_root() {
    local action="$1"
    if [ "$EUID" -ne 0 ]; then
        printf '[-] Bu işlem yönetici yetkisi gerektiriyor / Administrator privileges required:\n' >&2
        printf '    sudo "%s" %s\n' "$MANAGER_PATH" "$action" >&2
        return 1
    fi
}

run_as_root() {
    if [ "$EUID" -eq 0 ]; then
        "$MANAGER_PATH" "$@"
    elif check_sudo; then
        sudo "$MANAGER_PATH" "$@"
    else
        return 1
    fi
}

ensure_command_line_tools() {
    local resume_action="${1:---install}"

    if /usr/bin/xcode-select -p >/dev/null 2>&1 &&
       /usr/bin/xcrun --find clang >/dev/null 2>&1 &&
       /usr/bin/xcrun --find make >/dev/null 2>&1 &&
       /usr/bin/xcrun --find python3 >/dev/null 2>&1; then
        return 0
    fi

    print_color "$YELLOW" "[!] Xcode Command Line Tools kurulu değil. Apple yükleyicisi açılıyor... / Xcode CLT not installed. Launching Apple installer..."
    if /usr/bin/xcode-select --install; then
        echo "[i] Command Line Tools kurulumu başlatıldı / Command Line Tools installation initiated."
        echo "[i] Kurulum tamamlandıktan sonra bu komutu yeniden çalıştırın / After installation completes, re-run this command:"
        echo "    $MANAGER_PATH $resume_action"
    else
        print_color "$RED" "[-] Command Line Tools yükleyicisi başlatılamadı / Failed to launch Command Line Tools installer."
        echo "    Elle deneyin / Try manually: xcode-select --install"
    fi
    return 1
}

execute_privileged_action() {
    local action="$1"
    shift

    case "$action" in
        --install)
            ensure_command_line_tools "$action" || return 1 ;;
    esac

    if [ "$EUID" -ne 0 ]; then
        run_as_root "$action" "$@"
        return $?
    fi

    case "$action" in
        --install) install_driver ;;
        --uninstall) uninstall_driver ;;
        --enable-airprint) enable_airprint ;;
        --disable-airprint) disable_airprint ;;
        *) echo "Bilinmeyen yönetici işlemi: $action" >&2; return 2 ;;
    esac
}

# Asks a yes/no question; ASSUME_YES (--yes) answers yes. The second argument
# replaces the message shown for "no".
confirm_action() {
    local prompt="$1" declined="${2:-[i] İşlem iptal edildi / Operation cancelled.}"
    local answer

    if [ "${ASSUME_YES:-0}" -eq 1 ]; then
        return 0
    fi
    printf '%b%s [e/H - y/N]: %b' "$YELLOW" "$prompt" "$NC"
    if ! read -r answer; then
        return 1
    fi
    case "$answer" in
        e|E|evet|Evet|EVET|y|Y|yes|Yes|YES) return 0 ;;
        *) print_color "$YELLOW" "$declined"; return 1 ;;
    esac
}

is_safe_queue_name() {
    [ -n "$1" ] && [ "${#1}" -le 127 ] && [[ "$1" =~ ^[A-Za-z0-9._-]+$ ]]
}

is_safe_existing_queue_name() {
    [ -n "$1" ] && [ "${#1}" -le 127 ] && [[ "$1" != *[$'\r\n\t /#']* ]]
}

is_supported_device_uri() {
    if [ "${#1}" -gt 2048 ] || [[ "$1" == *[$'\r\n\t ']* ]]; then
        return 1
    fi
    case "$1" in
        usb://?*|ipp://?*|ipps://?*|dnssd://?*|socket://?*) return 0 ;;
        *) return 1 ;;
    esac
}

test_p1606dn_ppd() (
    set -eu

    fail() {
        echo "    [FAIL] $1" >&2
        exit 1
    }

    local ppd="$PPD_FILE" keyword expected count choice envelope medium duplex
    local paper_choices=(Letter Legal A4 Executive FanFoldGermanLegal A5 A6 B5 195x270mm 184x260mm 197x273mm Postcard DoublePostcard Env10 EnvMonarch EnvISOB5 EnvC5 EnvDL)
    local envelopes=(Env10 EnvMonarch EnvISOB5 EnvC5 EnvDL)
    [ -f "$ppd" ] || fail "Missing HP LaserJet Pro P1606dn PPD"
    grep -Fqx '*FileVersion: "6.0.1"' "$ppd" || fail "Wrong PPD version"
    # The PPD's version is the driver's: the diagnostic page, the PAPPL app
    # and both apps must carry the same one.
    grep -Fq '#define P1600_DRIVER_VERSION "6.0.1"' "$DRIVER_DIR/src/p1600_diagnostic.h" || fail "Diagnostic page version differs from the PPD"
    grep -Fqx '        "6.0.1",' "$DRIVER_DIR/src/pappl_p1600dn.c" || fail "PAPPL app version differs from the PPD"
    for plist in "$DRIVER_DIR/utility/Info.plist" "$DRIVER_DIR/notifier/Info.plist"; do
        [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist")" = "6.0.1" ] &&
            [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$plist")" = "6.0.1" ] ||
            fail "App version differs from the PPD: $plist"
    done
    grep -Fqx '*cupsVersion: "1.6"' "$ppd" || fail "Unsupported CUPS PPD extension version"
    # The PPD's languages, Turkish and Romanian included, all in the project's own translations.
    grep -Fqx '*cupsLanguages: "en da de es fi fr it ko nl no sv pt zh_CN zh_TW tr ro"' "$ppd" || fail "PPD languages are not the expected 16"
    grep -Fqx '*tr.Translation Resolution/Çözünürlük: ""' "$ppd" || fail "Turkish PPD localization is incomplete"
    grep -Fqx '*de.Translation Resolution/Auflösung: ""' "$ppd" || fail "German PPD localization is incomplete"
    grep -Fqx '*ro.Translation Resolution/Rezoluție: ""' "$ppd" || fail "Romanian PPD localization is incomplete"
    grep -q '^\*cupsIPPReason OutOfPaper/Out of Paper: "Out of Paper: Refill the paper tray' "$ppd" || fail "English status texts are not the driver's own"
    for keyword in da de es fi fr it ko nl no sv pt zh_CN zh_TW tr ro; do
        count=$(grep -c "^\\*$keyword\\." "$ppd")
        [ "$count" -eq 102 ] || fail "Language $keyword has $count of 102 translations"
    done
    if LC_ALL=C awk '/^\*/ {localized = ($0 ~ /^\*[a-z][a-z](_[A-Z][A-Z])?\./)} !localized && /[^ -~]/ {found=1} END {exit found ? 0 : 1}' "$ppd"; then
        fail "Non-ASCII text found outside a localized PPD keyword"
    fi
    if LC_ALL=C awk '/^[^*]/ || (/^\*[a-z][a-z](_[A-Z][A-Z])?\./ && !/"$/) {found=1} END {exit found ? 0 : 1}' "$ppd"; then
        fail "Localized PPD value spans more than one line"
    fi
    if LC_ALL=C awk 'length($0) > 255 {found=1} END {exit found ? 0 : 1}' "$ppd"; then
        fail "PPD line longer than 255 bytes"
    fi
    if grep -q '¯' "$ppd"; then fail "Mac Roman damage in the Danish text"; fi
    grep -Fqx '*cupsPJL pjlDensity: "@PJL SET DENSITY = %s%n"' "$ppd" || fail "The PJL density template is missing"
    grep -Fqx '*cupsFilter: "application/vnd.cups-raster 0 /Library/Printers/p1600dn/rastertop1600dn"' "$ppd" || fail "Wrong raster filter path"
    # Command filter: ReportLevels command jobs report the toner level.
    grep -Fqx '*cupsFilter: "application/vnd.cups-command 100 /Library/Printers/p1600dn/commandtop1600dn"' "$ppd" || fail "Wrong command filter path"
    grep -Fqx '*cupsCommands: "PrintSelfTestPage Clean"' "$ppd" || fail "Printer commands are not declared"
    grep -Fqx "*APPrinterUtilityPath: \"/Library/Printers/p1600dn/$UTILITY_APP_NAME\"" "$ppd" || fail "Printer utility path is missing"
    for keyword in PrinterReady CoverOpen TonerMissing ManualDuplex ManualFeed OutOfPaper OutputJam InputJam EngineFuser EngineScanner EngineComm Printingpage Complete; do
        grep -q "^\*cupsIPPReason $keyword/" "$ppd" || fail "Status text $keyword is missing"
        grep -q "^\*tr\.cupsIPPReason $keyword/" "$ppd" || fail "Turkish status text $keyword is missing"
    done
    grep -Fqx '*cupsModelNumber: 1606' "$ppd" || fail "Wrong model number"
    grep -Fqx '*ModelName: "HP LaserJet Professional P1600dn Series"' "$ppd" || fail "Wrong model name"
    # Raster contract: 600 dpi 8-bit gray screened by the filter, CUPS-made
    # collated copies and CUPS-rotated long-edge back sides.
    grep -Fqx '*cupsManualCopies: False' "$ppd" || fail "Wrong copies setting (cupsManualCopies must be False)"
    grep -Fqx '*cupsBackSide: "Rotated"' "$ppd" || fail "Back sides must be rotated by CUPS"
    grep -Fqx '*Resolution 600dpi/600 x 600 dpi: "<</HWResolution[600 600]/cupsBitsPerColor 8/cupsColorSpace 0/cupsCompression 0/cupsInteger0 1606>>setpagedevice"' "$ppd" || fail "600 dpi raster contract is wrong"
    grep -Fqx '*Resolution 1200x600dpi/FastRes 1200: "<</HWResolution[600 600]/cupsBitsPerColor 8/cupsColorSpace 0/cupsCompression 1/cupsInteger0 1606>>setpagedevice"' "$ppd" || fail "FastRes 1200 raster contract is wrong"
    grep -Fqx '*CustomPageSize True: "pop pop pop <</PageSize[5 -2 roll]/ImagingBBox null>>setpagedevice"' "$ppd" || fail "Custom page size is missing"

    for keyword in PageSize PageRegion ImageableArea PaperDimension; do
        count=$(awk -v prefix="*$keyword " 'index($0, prefix) == 1 {count++} END {print count + 0}' "$ppd")
        [ "$count" -eq 18 ] || fail "$keyword must declare all 18 paper sizes"
        for choice in "${paper_choices[@]}"; do
            grep -q "^\\*$keyword $choice[/:]" "$ppd" || fail "$keyword is missing $choice"
        done
    done

    while IFS=' ' read -r keyword expected; do
        count=$(awk -v prefix="*$keyword " 'index($0, prefix) == 1 {count++} END {print count + 0}' "$ppd")
        [ "$count" -eq "$expected" ] || fail "$keyword choice count is wrong (expected $expected, got $count)"
    done <<'EOF'
Resolution 2
Duplex 3
InputSlot 2
DupUnit 2
MediaType 16
pjlDensity 5
pjlJamRecovery 2
EconoMode 2
TersKose 2
EOF

    while IFS=' ' read -r medium expected; do
        grep -q "^\\*MediaType $medium/.*cupsMediaType $expected>>" "$ppd" || fail "MediaType $medium must send cupsMediaType $expected"
    done <<'EOF'
Paper 1
Light60-74g 258
Mid-Weight96-110g 282
Heavy111-130g 262
ExtraHeavy131-175g 283
TransparencyPaper 2
LabelsPaper 265
LetterheadPaper 513
EnvelopePaper 267
PreprintedPaper 514
PrepunchedPaper 515
ColorPaper 512
BondPaper 260
RecycledPaper 516
RoughPaper 263
VellumPaper 273
EOF

    for medium in TransparencyPaper LabelsPaper EnvelopePaper VellumPaper; do
        for duplex in DuplexNoTumble DuplexTumble; do
            grep -Fqx "*UIConstraints: *MediaType $medium *Duplex $duplex" "$ppd" || fail "$medium $duplex constraint is missing"
            grep -Fqx "*UIConstraints: *Duplex $duplex *MediaType $medium" "$ppd" || fail "$medium $duplex reverse constraint is missing"
        done
    done
    for envelope in "${envelopes[@]}"; do
        grep -Fqx "*UIConstraints: *PageSize $envelope *Duplex DuplexNoTumble" "$ppd" || fail "$envelope long-edge duplex constraint is missing"
        grep -Fqx "*UIConstraints: *PageSize $envelope *Duplex DuplexTumble" "$ppd" || fail "$envelope short-edge duplex constraint is missing"
    done

    grep -Fqx '*DefaultPageSize: A4' "$ppd" || fail "Wrong default page size"
    grep -Fqx '*DefaultPageRegion: A4' "$ppd" || fail "Wrong default page region"
    grep -Fqx '*DefaultResolution: 1200x600dpi' "$ppd" || fail "FastRes 1200 must be the default"
    # Auto (MediaPosition 7) is what foo2zjs and driver 3.1 sent to the P1606dn,
    # so it is the default here; Manual Feed sends MediaPosition 4.
    grep -Fqx '*DefaultInputSlot: Auto' "$ppd" || fail "Wrong default input slot"
    grep -Fqx '*InputSlot Auto/Automatically Select: "<</MediaPosition 7>>setpagedevice"' "$ppd" || fail "Auto must send MediaPosition 7"
    grep -Fqx '*InputSlot Manual/Manual Feed: "<</MediaPosition 4>>setpagedevice"' "$ppd" || fail "Manual Feed must send MediaPosition 4"
    grep -Fqx '*DefaultMediaType: Paper' "$ppd" || fail "Wrong default media type"
    grep -Fqx '*DefaultDuplex: None' "$ppd" || fail "Wrong default duplex mode"
    grep -Fqx '*DefaultDupUnit: True' "$ppd" || fail "Duplex unit must be installed by default"
    grep -Fqx '*DefaultEconoMode: False' "$ppd" || fail "Wrong default EconoMode"
    grep -Fqx '*DefaultpjlDensity: 3' "$ppd" || fail "Wrong default density"
    grep -Fqx '*DefaultpjlJamRecovery: False' "$ppd" || fail "Wrong default jam recovery"
    grep -Fqx '*DefaultTersKose: False' "$ppd" || fail "Wrong default rotation"

    [ -s "$DRIVER_DIR/assets/P1600dnPrinter.icns" ] || fail "macOS printer icon is missing or empty"
    [ -s "$DRIVER_DIR/assets/P1600dnPrinter.png" ] || fail "printer PNG preview is missing or empty"
    [ -s "$DRIVER_DIR/assets/P1600dnApp.icns" ] || fail "app icon is missing or empty"

    echo "    [PASS] HP LaserJet Pro P1606dn Complete PPD Option/Constraint Matrix"
    if command -v cupstestppd >/dev/null 2>&1; then
        cupstestppd -q -W all -I filters "$ppd" >/dev/null 2>&1 || fail "CUPS rejected $ppd"
        echo "    [PASS] Strict CUPS PPD Syntax Tests (-W all)"
    fi
)

test_status_arguments() (
    set -eu
    local status_binary="${1:-./build/p1600dn-status}"
    local invalid_uri
    for invalid_uri in http://127.0.0.1/printer https://127.0.0.1/printer usb://HP/P1606dn socket://127.0.0.1:9100 ipp:// ipps://; do
        if "$status_binary" --uri "$invalid_uri" >/dev/null 2>&1; then
            echo "    [FAIL] Status utility accepted invalid URI: $invalid_uri" >&2
            exit 1
        fi
    done
    if "$status_binary" one two three >/dev/null 2>&1; then
        echo "    [FAIL] Status utility accepted invalid arguments" >&2
        exit 1
    fi
    if "$status_binary" --uri >/dev/null 2>&1; then
        echo "    [FAIL] Status utility accepted a missing --uri value" >&2
        exit 1
    fi
    if "$status_binary" __p1600dn_missing_queue_for_test__ >/dev/null 2>&1; then
        echo "    [FAIL] Status utility accepted a nonexistent queue" >&2
        exit 1
    fi
    echo "    [PASS] Status Utility Argument/URI/Queue Rejection Matrix"
)

# arm64 code for every Apple Silicon Mac (macOS 11+) with a valid signature.
verify_arm64_binary() {
    local binary="$1" minimum

    [ -f "$binary" ] || return 1
    /usr/bin/lipo "$binary" -verify_arch arm64 >/dev/null 2>&1 || return 1
    minimum=$(/usr/bin/otool -l "$binary" 2>/dev/null |
        awk '/LC_BUILD_VERSION/ {build = 1} build && $1 == "minos" {print $2; exit}')
    [ "$minimum" = "$MACOS_MIN_VERSION" ] || return 1
    /usr/bin/codesign --verify --strict "$binary" >/dev/null 2>&1
}

# Files both PPDs use: toner command filter and the printer utility (tool and app).
verify_shared_tools() {
    local build="$1"
    verify_arm64_binary "$build/commandtop1600dn" &&
        verify_arm64_binary "$build/p1600dn-utility" &&
        verify_arm64_binary "$build/$UTILITY_APP_NAME/Contents/MacOS/P1600dnUtility" &&
        verify_arm64_binary "$build/$NOTIFIER_APP_NAME/Contents/MacOS/P1600dnNotifier" &&
        verify_arm64_binary "$build/p1600dn-airprint"
}

# The logged-in user's GUI session (0 when nobody is logged in).
console_user_id() {
    stat -f %u /dev/console 2>/dev/null || echo 0
}

# Every user with a GUI session: the console user and the users behind fast
# user switching (each session runs its own loginwindow as that user).
gui_user_ids() {
    { console_user_id; ps -axo uid=,comm= 2>/dev/null | awk '$2 ~ /\/loginwindow$/ { print $1 }'; } |
        awk '$1 != 0 && !seen[$1]++ { print $1 }'
}

# (Re)starts a LaunchAgent in every GUI session, so an update also replaces
# the agent of a user behind fast user switching; users who log in later get
# it at login (RunAtLoad).
start_launch_agent() {
    local label="$1" uid
    for uid in $(gui_user_ids); do
        "$LAUNCHCTL" bootout "gui/$uid/$label" >/dev/null 2>&1 || true
        if ! "$LAUNCHCTL" bootstrap "gui/$uid" "$LAUNCH_AGENTS_DIR/$label.plist"; then
            echo "[i] $label bir sonraki oturum açılışında başlayacak / will start at the next login." >&2
        fi
    done
    return 0
}

stop_launch_agent() {
    local label="$1" uid
    for uid in $(gui_user_ids); do
        "$LAUNCHCTL" bootout "gui/$uid/$label" >/dev/null 2>&1 || true
    done
}

# Makes an installed app readable by every user: directories and the
# executable 755, other files 644. A bundle whose Info.plist only root can read
# shows a no-entry icon, cannot be opened, and makes the notifier abort in
# UNUserNotificationCenter. Modes are not part of the signature.
set_app_modes() {
    local app="$1"
    find "$app" -type d -exec chmod 755 {} + || return 1
    find "$app" -type f -exec chmod 644 {} + || return 1
    chmod 755 "$app/Contents/MacOS/"* || return 1
}

# Registers (-f) or unregisters (-u) an app with Launch Services, in the
# system's database and in that of every user with a GUI session (it is per
# user).
register_app() {
    local action="$1" app="$2" uid
    "$LSREGISTER" "$action" "$app" >/dev/null 2>&1 || true
    for uid in $(gui_user_ids); do
        "$LAUNCHCTL" asuser "$uid" /usr/bin/sudo -u "#$uid" "$LSREGISTER" "$action" "$app" >/dev/null 2>&1 || true
    done
}

install_launch_agent() {
    local source="$1" label="$2"
    mkdir -p "$LAUNCH_AGENTS_DIR" || return 1
    cp "$source" "$LAUNCH_AGENTS_DIR/$label.plist" || return 1
    chmod 644 "$LAUNCH_AGENTS_DIR/$label.plist" || return 1
    chown root:wheel "$LAUNCH_AGENTS_DIR/$label.plist" || return 1
}

remove_launch_agent() {
    local label="$1"
    stop_launch_agent "$label"
    rm -f "$LAUNCH_AGENTS_DIR/$label.plist"
}

airprint_enabled() {
    [ -f "$LAUNCH_DAEMONS_DIR/$AIRPRINT_LABEL.plist" ]
}

# Installs and (re)starts the AirPrint LaunchDaemon.
start_airprint_daemon() {
    mkdir -p "$LAUNCH_DAEMONS_DIR" || return 1
    cp "$DRIVER_DIR/airprint/$AIRPRINT_LABEL.plist" "$LAUNCH_DAEMONS_DIR/$AIRPRINT_LABEL.plist" || return 1
    chmod 644 "$LAUNCH_DAEMONS_DIR/$AIRPRINT_LABEL.plist" || return 1
    chown root:wheel "$LAUNCH_DAEMONS_DIR/$AIRPRINT_LABEL.plist" || return 1
    "$LAUNCHCTL" bootout "system/$AIRPRINT_LABEL" >/dev/null 2>&1 || true
    "$LAUNCHCTL" bootstrap system "$LAUNCH_DAEMONS_DIR/$AIRPRINT_LABEL.plist" || return 1
}

# AirPrint (iPhone, iPad) and IPP Everywhere for the P1600dn queues: a system
# service as the user nobody, only when the user turns it on. Its jobs go
# to the CUPS queue, so through the driver filter and its page guard.
enable_airprint() {
    local queue
    require_root "--enable-airprint" || return 1
    if [ ! -x "$INSTALL_DIR/p1600dn-airprint" ]; then
        echo "[-] Önce sürücüyü kurun (Menü: 1) / Install the driver first (menu 1)." >&2
        return 1
    fi
    start_airprint_daemon || return 1
    print_color "$GREEN" "[+] AirPrint açık / AirPrint enabled (TCP 8632)."
    echo "[i] iPhone/iPad: Paylaş → Yazdır → \"<yazıcı> @ $(scutil --get ComputerName 2>/dev/null || hostname)\""
    echo "[i] iPhone/iPad: Share → Print → \"<printer> @ $(scutil --get ComputerName 2>/dev/null || hostname)\""
    # macOS Printer Sharing (cupsctl _share_printers=1) would announce a shared
    # queue under the same name; while it is off, cupsd announces nothing.
    if cupsctl 2>/dev/null | grep -qx '_share_printers=1'; then
        while IFS= read -r queue; do
            [ -n "$queue" ] || continue
            if lpoptions -p "$queue" 2>/dev/null | grep -q 'printer-is-shared=true'; then
                print_color "$YELLOW" "[!] $queue macOS Yazıcı Paylaşımı ile de paylaşılıyor; iPhone'da iki kez görünür. Paylaşımı kapatın: lpadmin -p $queue -o printer-is-shared=false"
                print_color "$YELLOW" "[!] $queue is also shared by macOS Printer Sharing and appears twice on iPhone. Turn that off: lpadmin -p $queue -o printer-is-shared=false"
            fi
        done < <(p1600dn_queue_names)
    fi
    echo "[i] Güvenlik duvarı açıksa gelen bağlantılara izin verin / If the firewall is on, allow incoming connections for p1600dn-airprint."
}

disable_airprint() {
    require_root "--disable-airprint" || return 1
    "$LAUNCHCTL" bootout "system/$AIRPRINT_LABEL" >/dev/null 2>&1 || true
    rm -f "$LAUNCH_DAEMONS_DIR/$AIRPRINT_LABEL.plist" || return 1
    print_color "$GREEN" "[+] AirPrint kapalı / AirPrint disabled."
}

# Names of the queues that use this driver (p1600dn-utility list: one
# "name[ *]<TAB>info<TAB>device-uri" line per queue).
p1600dn_queue_names() {
    [ -x "$INSTALL_DIR/p1600dn-utility" ] || return 0
    "$INSTALL_DIR/p1600dn-utility" --en list 2>/dev/null | awk -F '\t' 'NF >= 3 { sub(/ \*$/, "", $1); print $1 }'
}

airprint_menu() {
    if airprint_enabled; then
        print_color "$GREEN" "[+] AirPrint açık / AirPrint is on."
        if confirm_action "AirPrint kapatılsın mı? / Turn AirPrint off?"; then
            execute_privileged_action --disable-airprint || true
        fi
    else
        echo "[i] AirPrint kapalı / AirPrint is off."
        echo "    iPhone ve iPad'ler bu Mac üzerinden P1600dn serisi yazıcıya yazdırabilir; her sayfa sürücünün denetiminden geçer."
        echo "    iPhones and iPads can print to the P1600dn printer through this Mac; every page passes the driver's check."
        if confirm_action "AirPrint açılsın mı? / Turn AirPrint on?"; then
            execute_privileged_action --enable-airprint || true
        fi
    fi
}

# The signed binaries are replaced, never written over in place: macOS kills
# a running process whose signed file changes (and the next one started from
# it). A new file leaves running filters and the AirPrint server the old one.
replace_binary() {
    rm -f "$2" || return 1
    cp "$1" "$2"
}

install_shared_tools() {
    local build="$1"
    replace_binary "$build/commandtop1600dn" "$INSTALL_DIR/commandtop1600dn" || return 1
    replace_binary "$build/p1600dn-utility" "$INSTALL_DIR/p1600dn-utility" || return 1
    rm -rf "${INSTALL_DIR:?}/$UTILITY_APP_NAME" || return 1
    /usr/bin/ditto "$build/$UTILITY_APP_NAME" "$INSTALL_DIR/$UTILITY_APP_NAME" || return 1
    chmod 755 "$INSTALL_DIR/commandtop1600dn" "$INSTALL_DIR/p1600dn-utility" || return 1
    chown -R root:wheel "$INSTALL_DIR/commandtop1600dn" "$INSTALL_DIR/p1600dn-utility" \
          "$INSTALL_DIR/$UTILITY_APP_NAME" || return 1
    set_app_modes "$INSTALL_DIR/$UTILITY_APP_NAME" || return 1
    register_app -f "$INSTALL_DIR/$UTILITY_APP_NAME"
    # Printer notifications: the agent is replaced while stopped.
    stop_launch_agent "$NOTIFIER_LABEL"
    rm -rf "${INSTALL_DIR:?}/$NOTIFIER_APP_NAME" || return 1
    /usr/bin/ditto "$build/$NOTIFIER_APP_NAME" "$INSTALL_DIR/$NOTIFIER_APP_NAME" || return 1
    chown -R root:wheel "$INSTALL_DIR/$NOTIFIER_APP_NAME" || return 1
    set_app_modes "$INSTALL_DIR/$NOTIFIER_APP_NAME" || return 1
    register_app -f "$INSTALL_DIR/$NOTIFIER_APP_NAME"
    install_launch_agent "$DRIVER_DIR/notifier/$NOTIFIER_LABEL.plist" "$NOTIFIER_LABEL" || return 1
    start_launch_agent "$NOTIFIER_LABEL"
    # AirPrint server (started only when AirPrint is turned on) and its icon.
    replace_binary "$build/p1600dn-airprint" "$INSTALL_DIR/p1600dn-airprint" || return 1
    cp "$DRIVER_DIR/assets/P1600dnPrinter.png" "$INSTALL_DIR/P1600dnPrinter.png" || return 1
    chmod 755 "$INSTALL_DIR/p1600dn-airprint" || return 1
    chmod 644 "$INSTALL_DIR/P1600dnPrinter.png" || return 1
    chown root:wheel "$INSTALL_DIR/p1600dn-airprint" "$INSTALL_DIR/P1600dnPrinter.png" || return 1
    if airprint_enabled; then
        "$LAUNCHCTL" kickstart -k "system/$AIRPRINT_LABEL" >/dev/null 2>&1 || true
    fi
}

remove_shared_tools() {
    "$LAUNCHCTL" bootout "system/$AIRPRINT_LABEL" >/dev/null 2>&1 || true
    rm -f "$LAUNCH_DAEMONS_DIR/$AIRPRINT_LABEL.plist" "$INSTALL_DIR/p1600dn-airprint" "$INSTALL_DIR/P1600dnPrinter.png"
    remove_launch_agent "$NOTIFIER_LABEL"
    rm -f "$INSTALL_DIR/commandtop1600dn" "$INSTALL_DIR/p1600dn-utility" || return 1
    register_app -u "$INSTALL_DIR/$UTILITY_APP_NAME"
    register_app -u "$INSTALL_DIR/$NOTIFIER_APP_NAME"
    rm -rf "${INSTALL_DIR:?}/$UTILITY_APP_NAME" "${INSTALL_DIR:?}/$NOTIFIER_APP_NAME"
}

# install_driver runs make as root. build/ goes back to the user who ran sudo
# whether the install succeeds or stops halfway, so make, --test and --status
# keep working without sudo.
restore_build_ownership() {
    local sudo_group
    [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ] && [ -d build ] || return 0
    sudo_group=$(id -g "$SUDO_USER" 2>/dev/null || true)
    if [ -n "$sudo_group" ]; then
        chown -R "${SUDO_USER}:${sudo_group}" build 2>/dev/null || true
    else
        chown -R "$SUDO_USER" build 2>/dev/null || true
    fi
}

install_driver() {
    local status
    install_driver_steps
    status=$?
    restore_build_ownership
    return "$status"
}

install_driver_steps() {
    local first_install=0 remove_gutenprint=0
    require_root "--install" || return 1
    [ -e "$INSTALL_DIR/rastertop1600dn" ] || [ -e "$INSTALLED_PPD" ] ||
        [ -e "$LEGACY_INSTALL_DIR/$LEGACY_FILTER_NAME" ] || [ -e "$LEGACY_PPD" ] || first_install=1
    echo "=========================================================="
    echo " P1600dn Series - macOS ARM Kurucu / Installer"
    echo " (Desteklenenler / Supported: P1606dn, P1607dn, P1608dn, P1609dn)"
    echo "=========================================================="
    # Asked before the build: Gutenprint has no P1600dn driver. A reinstall or
    # update leaves it alone.
    if [ "$first_install" -eq 1 ] && gutenprint_found; then
        if confirm_action "Gutenprint kurulu; P1600dn için gerekmiyor. Kaldırılsın mı? / Gutenprint is installed and not needed for the P1600dn. Remove it?" \
                          "[i] Gutenprint bırakılıyor / Gutenprint is kept."; then
            remove_gutenprint=1
        fi
    fi
    echo "[1/4] Derleme ve tüm otomatik testler çalıştırılıyor / Building & running tests..."
    make clean || return 1
    make test || return 1
    if ! verify_arm64_binary build/rastertop1600dn ||
       ! verify_arm64_binary build/p1600dn-status ||
       ! verify_shared_tools build; then
        echo "[-] Derlenen ikililer arm64 mimarisinde değil / Binaries are not arm64 architecture." >&2
        return 1
    fi

    echo "[2/4] Sürücü, toner filtresi, yazıcı yardımcı programı, durum aracı ve simge yükleniyor / Installing driver, toner filter, printer utility, status tool, and icon..."
    mkdir -p "$INSTALL_DIR" || return 1
    # cupsd's filters (_lp), the notifier, the utility and AirPrint (nobody)
    # all read this folder, whatever umask sudo ran with.
    chmod 755 "$INSTALL_DIR" || return 1
    replace_binary build/rastertop1600dn "$INSTALL_DIR/rastertop1600dn" || return 1
    replace_binary build/p1600dn-status "$INSTALL_DIR/p1600dn-status" || return 1
    install_shared_tools build || return 1
    cp assets/P1600dnPrinter.icns "$INSTALL_DIR/P1600dnPrinter.icns" || return 1
    chmod 755 "$INSTALL_DIR/rastertop1600dn" "$INSTALL_DIR/p1600dn-status" || return 1
    chmod 644 "$INSTALL_DIR/P1600dnPrinter.icns" || return 1
    chown root:wheel "$INSTALL_DIR/rastertop1600dn" "$INSTALL_DIR/p1600dn-status" \
          "$INSTALL_DIR/P1600dnPrinter.icns" || return 1

    echo "[3/4] HP LaserJet Professional P1600dn Serisi PPD dosyası yükleniyor / Installing PPD..."
    mkdir -p "$PPD_INSTALL_DIR" || return 1
    cp "$PPD_FILE" "$INSTALLED_PPD" || return 1
    chmod 644 "$INSTALLED_PPD" || return 1
    chown root:wheel "$INSTALLED_PPD" || return 1

    retire_openprinting_fallback
    retire_legacy_driver
    if [ "$remove_gutenprint" -eq 1 ]; then
        purge_gutenprint || print_color "$YELLOW" "[!] Gutenprint tamamen kaldırılamadı / Gutenprint could not be removed completely."
    fi
    echo "[4/4] CUPS servisi güncelleniyor / Restarting CUPS daemon..."
    killall -HUP cupsd 2>/dev/null || true
    echo ""
    print_color "$GREEN" "[+] Sürücü dosyaları başarıyla kuruldu / Driver files installed successfully."
    echo "[i] Sonraki adım: Menüden [6] yeni kuyruk ekle, [8] mevcut kuyruğu P1600dn serisine geçir."
    echo "[i] Next step: Use menu [6] to add a new queue, or [8] to switch an existing queue."
}

# Driver 3.1-3.2 could install the OpenPrinting foo2zjs fallback, which
# skipped the page guard. Queues that still use its PPD move to the native
# PPD (when installed), and its files are removed.
retire_openprinting_fallback() {
    local ppd queue
    for ppd in "$CUPS_PPD_DIR"/*.ppd; do
        [ -f "$ppd" ] || continue
        grep -Fq 'rastertohpp1606dn-openprinting' "$ppd" 2>/dev/null || continue
        queue=$(basename "$ppd" .ppd)
        if [ -f "$INSTALLED_PPD" ] && lpadmin -p "$queue" -E -P "$INSTALLED_PPD"; then
            echo "[i] '$queue' OpenPrinting yedeğinden ana sürücüye geçirildi / moved from the OpenPrinting fallback to the native driver."
        fi
    done
    rm -f "$OPENPRINTING_FILTER" "$OPENPRINTING_ENCODER" "$OPENPRINTING_LICENSE" "$OPENPRINTING_INSTALLED_PPD"
}

# Driver 5.0 and earlier used other names (LEGACY_*). Its queues move to this
# driver's PPD (lpadmin keeps their option defaults), AirPrint stays on when it
# was on, and its agents and files are removed. With "files", only the files
# and agents go (uninstall).
retire_legacy_driver() {
    local ppd queue airprint_was_on=0
    [ -f "$LAUNCH_DAEMONS_DIR/$LEGACY_AIRPRINT_LABEL.plist" ] && airprint_was_on=1
    "$LAUNCHCTL" bootout "system/$LEGACY_AIRPRINT_LABEL" >/dev/null 2>&1 || true
    rm -f "$LAUNCH_DAEMONS_DIR/$LEGACY_AIRPRINT_LABEL.plist"
    remove_launch_agent "$LEGACY_NOTIFIER_LABEL"
    if [ "${1:-}" != files ]; then
        for ppd in "$CUPS_PPD_DIR"/*.ppd; do
            [ -f "$ppd" ] || continue
            grep -Fq "/$LEGACY_FILTER_NAME\"" "$ppd" 2>/dev/null || continue
            queue=$(basename "$ppd" .ppd)
            if [ -f "$INSTALLED_PPD" ] && lpadmin -p "$queue" -E -P "$INSTALLED_PPD"; then
                echo "[i] '$queue' 6.0 sürücüsüne geçirildi / moved to driver 6.0."
            fi
        done
        if [ "$airprint_was_on" -eq 1 ] && [ -x "$INSTALL_DIR/p1600dn-airprint" ]; then
            start_airprint_daemon || print_color "$YELLOW" "[!] AirPrint yeniden açılamadı / AirPrint could not be turned on again."
        fi
    fi
    rm -rf "${LEGACY_INSTALL_DIR:?}" "$LEGACY_PPD"
}

uninstall_driver() {
    require_root "--uninstall" || return 1
    confirm_action "HP LaserJet Professional P1600dn serisi sürücüsü, Gutenprint ve eski kalıntılar kaldırılsın mı? / Remove the driver, Gutenprint and legacy residue?" || return 0
    echo "[*] HP LaserJet Professional P1600dn Serisi Native ARM sürücüsü ve eski kalıntılar kaldırılıyor / Removing driver..."
    rm -f "$INSTALL_DIR/rastertop1600dn" "$INSTALL_DIR/p1600dn-status" "$INSTALL_DIR/P1600dnPrinter.icns" || return 1
    remove_shared_tools || return 1
    retire_openprinting_fallback
    retire_legacy_driver files
    rmdir "$INSTALL_DIR" 2>/dev/null || true
    rm -f "$INSTALLED_PPD" || return 1
    # Eski arm64 modül kalıntılarını da temizle (INSTALL_DIR'in yanında).
    # / Older arm64 module residue, next to INSTALL_DIR.
    rm -rf "${INSTALL_DIR%/*}/hp1010dn" 2>/dev/null || true
    rm -f "$PPD_INSTALL_DIR"/HP-LaserJet-10*.ppd 2>/dev/null || true
    purge_gutenprint || print_color "$YELLOW" "[!] Gutenprint tamamen kaldırılamadı / Gutenprint could not be removed completely."
    killall -HUP cupsd 2>/dev/null || true
    print_color "$GREEN" "[+] HP LaserJet Professional P1600dn serisi sürücüsü ve PPD'si kaldırıldı / Driver and PPD removed."
    echo "[i] Mevcut CUPS kuyrukları otomatik silinmedi. Gerekirse Sistem Ayarları'ndan kaldırın."
    echo "[i] Existing CUPS queues were not deleted automatically. Remove them from System Settings if needed."
}

# Gutenprint's package receipts known to pkgutil.
gutenprint_packages() {
    "$PKGUTIL" --pkgs 2>/dev/null | grep -Ei 'gutenprint|gimp-print' || true
}

# Gutenprint's files that exist: driver, tools, CUPS filter, PPDs, receipts.
gutenprint_files() {
    local path
    local paths=(
        /Library/Printers/gutenprint /Library/Printers/Gutenprint /Library/Printers/Gutenprint.printerDriver
        /Library/PreferencePanes/Gutenprint.prefPane /usr/local/share/gutenprint /usr/local/lib/gutenprint
        /usr/local/include/gutenprint /usr/local/bin/escputil /usr/local/bin/cups-calibrate
    )
    for path in "${paths[@]/#/$GUTENPRINT_ROOT}" "$GUTENPRINT_ROOT"/usr/libexec/cups/filter/rastertogutenprint*; do
        if [ -e "$path" ] || [ -L "$path" ]; then printf '%s\n' "$path"; fi
    done
    {
        [ ! -d "$PPD_INSTALL_DIR" ] || find "$PPD_INSTALL_DIR" -maxdepth 2 \( -iname '*gutenprint*' -o -name 'stp-*.ppd*' \)
        [ ! -d "$GUTENPRINT_ROOT/Library/Receipts" ] || find "$GUTENPRINT_ROOT/Library/Receipts" -maxdepth 1 \( -iname '*gutenprint*' -o -iname '*gimp-print*' \)
    } 2>/dev/null
    return 0
}

gutenprint_found() {
    [ -n "$(gutenprint_packages)$(gutenprint_files)" ]
}

# Gutenprint has no P1600dn driver: at best a network queue printed PCL
# through another model (HP LaserJet 1160), without the printer's duplex,
# paper codes, status or the page guard. The first install removes it when the
# user agrees, every confirmed uninstall removes it. P1600dn queues that still
# use it move to the native PPD first. While another printer prints through
# Gutenprint, it stays, so that printer keeps working.
purge_gutenprint() {
    local ppd queue device pkg path found=0 failed=0 users=()

    for ppd in "$CUPS_PPD_DIR"/*.ppd; do
        [ -f "$ppd" ] || continue
        grep -Fq 'rastertogutenprint' "$ppd" 2>/dev/null || continue
        queue=$(basename "$ppd" .ppd)
        device=$(lpstat -v "$queue" 2>/dev/null || true)
        if [[ "$queue $device" =~ [Pp]160[0-9][Dd][Nn] ]]; then
            if [ -f "$INSTALLED_PPD" ] && lpadmin -p "$queue" -E -P "$INSTALLED_PPD"; then
                echo "[i] '$queue' Gutenprint'ten P1600dn serisi sürücüsüne geçirildi / moved from Gutenprint to the P1600dn series driver."
            fi
        else
            users+=("$queue")
        fi
    done
    if [ "${#users[@]}" -gt 0 ]; then
        echo "[i] Gutenprint bırakıldı; başka yazıcılar kullanıyor / Gutenprint kept; other printers use it: ${users[*]}"
        return 0
    fi

    while IFS= read -r pkg; do
        [ -n "$pkg" ] || continue
        found=1
        "$PKGUTIL" --forget "$pkg" >/dev/null 2>&1 || true
    done < <(gutenprint_packages)
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        found=1
        rm -rf "$path" || failed=1
    done < <(gutenprint_files)
    [ "$failed" -eq 0 ] || return 1
    [ "$found" -eq 0 ] || echo "[i] Gutenprint kaldırıldı / Gutenprint removed."
}

ensure_build_ownership() {
    if [ -d build ] && [ ! -w build ]; then
        print_color "$YELLOW" "[!] build dizini sahipliği düzeltiliyor."
        check_sudo || return 1
        sudo chown -R "$(id -u):$(id -g)" build || return 1
    fi
}

run_driver_tests() {
    ensure_build_ownership || return 1
    print_color "$CYAN" "[*] Sürücü, PPD ve yönetici testleri çalıştırılıyor / Running driver, PPD, and manager tests..."
    if make test; then print_color "$GREEN" "[+] Tüm otomatik testler geçti / All automatic tests passed."
    else print_color "$RED" "[-] Testlerden en az biri başarısız oldu / At least one test failed."; return 1
    fi
}

show_system_status() {
    print_color "$CYAN" "[*] Sürücü kurulum durumu / Driver installation status"
    if [ -x "$INSTALL_DIR/rastertop1600dn" ]; then print_color "$GREEN" "[+] Raster filtresi kurulu / Raster filter installed: $INSTALL_DIR/rastertop1600dn"
    else print_color "$YELLOW" "[i] Raster filtresi kurulu değil / Raster filter is not installed. (Menü / Menu: 1)"
    fi
    if [ -x "$INSTALL_DIR/p1600dn-status" ]; then print_color "$GREEN" "[+] Durum aracı kurulu / Status utility installed: $INSTALL_DIR/p1600dn-status"
    else echo "[i] Durum aracı kurulu değil / Status utility is not installed."
    fi
    if [ -d "$INSTALL_DIR/$UTILITY_APP_NAME" ]; then print_color "$GREEN" "[+] Yazıcı yardımcı programı kurulu / Printer utility installed: $INSTALL_DIR/$UTILITY_APP_NAME"
    else echo "[i] Yazıcı yardımcı programı kurulu değil / Printer utility is not installed."
    fi
    if [ -d "$INSTALL_DIR/$NOTIFIER_APP_NAME" ] && [ -f "$LAUNCH_AGENTS_DIR/$NOTIFIER_LABEL.plist" ]; then print_color "$GREEN" "[+] Yazıcı bildirimleri açık / Printer notifications on: $INSTALL_DIR/$NOTIFIER_APP_NAME"
    else echo "[i] Yazıcı bildirimleri kurulu değil / Printer notifications are not installed."
    fi
    if airprint_enabled; then print_color "$GREEN" "[+] AirPrint açık / AirPrint on: $INSTALL_DIR/p1600dn-airprint (TCP 8632)"
    else echo "[i] AirPrint kapalı / AirPrint is off. (Menü / Menu: 9)"
    fi
    if [ -x "$INSTALL_DIR/commandtop1600dn" ]; then print_color "$GREEN" "[+] Toner seviyesi filtresi kurulu / Toner level filter installed: $INSTALL_DIR/commandtop1600dn"
    else echo "[i] Toner seviyesi filtresi kurulu değil / Toner level filter is not installed."
    fi
    if [ -f "$INSTALLED_PPD" ]; then print_color "$GREEN" "[+] HP LaserJet Professional P1600dn Serisi PPD'si kurulu / PPD is installed."
    else print_color "$YELLOW" "[i] HP LaserJet Professional P1600dn Serisi PPD'si kurulu değil / PPD is not installed."
    fi
    echo ""
    print_color "$CYAN" "[*] CUPS kuyrukları / CUPS queues"
    lpstat -p -d 2>&1 || echo "[i] CUPS kuyruğu bulunamadı veya bilgi alınamadı / No CUPS queues found."
    echo ""
    echo "Driver ancak bir yazıcı kuyruğu doğru PPD ile oluşturulduğunda aktiftir."
    echo "Driver is active only when a printer queue is created with the matching PPD."
    echo "Kuyruk yoksa menüden seçenek 6'yı kullanın / If no queue exists, use option 6 from menu."
}

query_printer_status() {
    local queue_name="${1:-}"
    if ! make build/p1600dn-status; then print_color "$RED" "[-] Durum aracı derlenemedi / Failed to build status utility."; return 1; fi
    if [ -n "$queue_name" ]; then ./build/p1600dn-status "$queue_name"; else ./build/p1600dn-status; fi
}

configure_printer_queue() {
    local queue_name device_uri admin_command
    print_color "$CYAN" "[*] HP LaserJet Professional P1600dn Serisi CUPS kuyruğu ekleme ve driver'ı etkinleştirme / Add Queue & Activate Driver"
    echo "Yazıcıyı açıp bağlayın. CUPS tarafından algılanan destekli cihazlar / Detected supported devices:"
    lpinfo -v 2>/dev/null | grep -E ' (usb|ipp|ipps|dnssd|socket)://' || echo "  [i] Otomatik algılanan cihaz yok; URI'yi elle girebilirsiniz / No devices auto-detected; enter URI manually."
    echo "USB: usb://HP/LaserJet%20Professional%20P1606dn | IPP: ipp://IP/ipp/print | Ağ/Network: socket://IP:9100"
    printf 'Kuyruk adı / Queue name (örnek HP_LaserJet_P1606dn veya HP_LaserJet_P1600dn): '; read -r queue_name || return 1
    if ! is_safe_queue_name "$queue_name"; then print_color "$RED" "[-] Geçersiz kuyruk adı / Invalid queue name."; return 1; fi
    printf "CUPS cihaz URI'si / Device URI: "; read -r device_uri || return 1
    if ! is_supported_device_uri "$device_uri"; then print_color "$RED" "[-] Geçersiz veya desteklenmeyen URI / Invalid or unsupported URI."; return 1; fi
    if [ ! -f "$INSTALLED_PPD" ]; then print_color "$RED" "[-] P1600dn serisi PPD'si kurulu değil. Önce seçenek 1'i kullanın / PPD not installed. Run option 1 first."; return 1; fi
    # lpadmin -p would change an existing queue: its device URI and PPD.
    if lpstat -p "$queue_name" >/dev/null 2>&1; then
        print_color "$RED" "[-] '$queue_name' adlı kuyruk zaten var ve değiştirilmedi. Sürücüsünü değiştirmek için [8]'i kullanın veya başka bir ad girin / Queue '$queue_name' already exists and was not changed. Use [8] to switch its driver, or choose another name."
        return 1
    fi
    check_sudo || return 1
    if [ "$EUID" -eq 0 ]; then admin_command=(lpadmin); else admin_command=(sudo lpadmin); fi
    if "${admin_command[@]}" -p "$queue_name" -E -v "$device_uri" -P "$INSTALLED_PPD" -D "HP LaserJet Professional P1600dn Series"; then
        print_color "$GREEN" "[+] Kuyruk oluşturuldu ve etkinleştirildi: $queue_name / Queue created and activated"
        lpstat -p "$queue_name" 2>/dev/null || true
        echo "Test baskısı / Test print: lp -d '$queue_name' /tam/yol/test.pdf"
        echo "Seçenekler / Options: lpoptions -p '$queue_name' -l"
    else
        print_color "$RED" "[-] Kuyruk eklenemedi. URI'yi 'lpinfo -v' ile doğrulayın / Failed to add queue. Verify URI with 'lpinfo -v'."
        return 1
    fi
}

update_printer_driver() {
    update_queue_driver "${1:-}" "$INSTALLED_PPD" \
        "P1600dn-Series.ppd" "yerel P1600dn serisi"
}

update_queue_driver() {
    local queue_name="$1" target_ppd="$2" ppd_name="$3" driver_label="$4"
    local current_device admin_command

    print_color "$CYAN" "[*] Mevcut CUPS kuyruğunu $driver_label sürücüsüne geçirme / Switch queue to $driver_label"
    if [ ! -f "$target_ppd" ]; then
        print_color "$RED" "[-] Gerekli PPD kurulu değil: $target_ppd / Required PPD is not installed"
        return 1
    fi

    if [ -z "$queue_name" ]; then
        echo "Mevcut CUPS kuyrukları / Existing CUPS queues:"
        lpstat -p 2>&1 || true
        printf "Driver'ı değiştirilecek kuyruk adı / Target queue name (örnek / e.g. Fakülte): "
        read -r queue_name || return 1
    fi

    if ! is_safe_existing_queue_name "$queue_name"; then
        print_color "$RED" "[-] Geçersiz CUPS kuyruk adı. 'lpstat -p' çıktısındaki adı aynen kullanın / Invalid queue name."
        return 1
    fi
    if ! lpstat -p "$queue_name" >/dev/null 2>&1; then
        print_color "$RED" "[-] CUPS kuyruğu bulunamadı: $queue_name / CUPS queue not found"
        echo "    Kullanılabilir kuyruklar / Available queues: lpstat -p"
        return 1
    fi

    current_device=$(lpstat -v "$queue_name" 2>/dev/null || true)
    echo "Kuyruk / Queue: $queue_name"
    [ -z "$current_device" ] || echo "$current_device"
    echo "Yeni driver / New driver: $ppd_name"
    echo "[i] Kuyruk adı ve mevcut USB/ağ cihaz URI'si korunacak / Queue name and device URI will be preserved."
    confirm_action "'$queue_name' kuyruğu $driver_label sürücüsüne geçirilsin mi? / Switch '$queue_name' to $driver_label?" || return 0

    check_sudo || return 1
    if [ "$EUID" -eq 0 ]; then admin_command=(lpadmin); else admin_command=(sudo lpadmin); fi
    if "${admin_command[@]}" -p "$queue_name" -E -P "$target_ppd" \
         -o Resolution=1200x600dpi -o Duplex=None; then
        cupsenable "$queue_name" 2>/dev/null || true
        cupsaccept "$queue_name" 2>/dev/null || true
        print_color "$GREEN" "[+] '$queue_name' kuyruğu $driver_label sürücüsüne geçirildi."
        echo "Bağlantı URI'si değiştirilmedi. Seçenekleri doğrulamak için / Device URI unchanged. To verify options:"
        echo "    lpoptions -p '$queue_name' -l"
    else
        print_color "$RED" "[-] Mevcut kuyruğun driver'ı değiştirilemedi / Failed to change driver for queue."
        return 1
    fi
}

print_install_help() {
    cat <<EOF

DRIVER NASIL KURULUR? / HOW TO INSTALL THE DRIVER
-------------------------------------------------
1. Bu dosyayı çift tıklayıp ana menüden [1] seçin veya Terminalde:
   Double-click this file and choose [1] from the main menu, or in Terminal:
     cd "$DRIVER_DIR"
     ./p1600dn_manager.command --install
2. Komut, Xcode Command Line Tools yoksa Apple yükleyicisini açar; kurulum bitince aynı komutu yeniden çalıştırın.
   If Xcode Command Line Tools are missing, the command launches Apple's installer; re-run when finished.
3. Komut gerektiğinde sudo parolasını kendisi ister.
   The script prompts for administrator (sudo) password when root privileges are required.
4. Kurulum HP LaserJet Professional P1600dn Serisi (P1606dn, P1607dn, P1608dn, P1609dn) PPD'sini doğrular, tüm testleri çalıştırır ve filtreyi sisteme yükler.
   Installation verifies P1600dn series PPD, runs all automated tests, and installs the filter to /Library/Printers/p1600dn/.
5. İlk kurulum, Gutenprint kuruluysa onu kaldırmayı sorar (başka bir yazıcı kullanıyorsa bırakılır); --yes sormadan kaldırır.
   The first install offers to remove Gutenprint when it is installed (kept while another printer uses it); --yes removes it without asking.
Kurulum tek başına kuyruk oluşturmaz; aşağıdaki etkinleştirme adımını da uygulayın.
Installation does not create a print queue by itself; follow the activation steps below.

EOF
}

print_activation_help() {
    cat <<EOF

DRIVER NASIL ETKİNLEŞTİRİLİR? / HOW TO ACTIVATE THE DRIVER
-----------------------------------------------------------
1. Yazıcıyı açın ve USB veya dahili Ethernet ile bağlayın.
   Power on the printer and connect via USB or local Ethernet network.
2. Cihaz URI'lerini görün / Discover device URIs: lpinfo -v
3. Ana menüden [6] seçip kuyruk adı ile URI'yi girin. Model otomatik olarak HP LaserJet Professional P1600dn Serisi'dir.
   Select [6] from main menu and enter queue name + URI. The driver model is configured automatically.
4. Kontrol / Verify: lpstat -p -d
5. Seçenekler / Options: lpoptions -p KUYRUK_ADI -l
6. Test baskısı / Test print: lp -d KUYRUK_ADI /tam/yol/test.pdf

Grafik yöntem / GUI method:
  Sistem Ayarları > Yazıcılar ve Tarayıcılar > Yazıcı Ekle > Kullan: Diğer... > P1600dn-Series.ppd dosyasını seçin.
  System Settings > Printers & Scanners > Add Printer > Use: Other... > select P1600dn-Series.ppd

Mevcut bir kuyruk zaten varsa silmeniz gerekmez. Ana menüden [8] seçin;
kuyruk adı ve USB/ağ URI'si korunurken yalnız driver P1600dn serisi olarak değiştirilir.
If an existing queue already exists, you do not need to delete it. Choose [8] from main menu;
queue name and URI are preserved while updating only the driver to P1600dn series.
EOF
}

print_verification_help() {
    cat <<EOF

KURULUMU VE BASKIYI DOĞRULAMA / VERIFYING INSTALLATION & PRINTING
-----------------------------------------------------------------
- Ana menü [4] / Main menu [4]: filtre, PPD ve CUPS kuyruğu durumu / filter, PPD, and CUPS status.
- Ana menü [3] / Main menu [3]: bütün otomatik testler / run complete automated test suite.
- Ana menü [5] / Main menu [5]: CUPS/IPP yazıcı ve toner durumu / printer & toner status.
- Kuyruğu aç / Enable queue: cupsenable KUYRUK_ADI
- İş kabulünü aç / Accept jobs: cupsaccept KUYRUK_ADI
- Dağıtım ve Entegrasyon Rehberi / Distribution Guide: DISTRIBUTION_GUIDE.md (Apple PKG, Homebrew Tap, OpenPrinting & PAPPL)
EOF
}

build_pappl_target() {
    print_color "$CYAN" "[*] OpenPrinting PAPPL Printer Application derleniyor... / Building PAPPL Application..."
    make pappl
}


print_troubleshooting_help() {
    cat <<EOF

SORUN GİDERME / TROUBLESHOOTING
--------------------------------
- PPD yok / Missing PPD: ana menü [1] ile kurun / install via main menu [1].
- URI yok / Missing URI: bağlantıyı kontrol edip 'lpinfo -v' çalıştırın / check connection and run 'lpinfo -v'.
- Baskı yok / No print: HP LaserJet Professional P1600dn Serisi PPD'sini ve URI'yi kontrol edin / verify PPD & URI.
- Duran kuyruk / Stopped queue: cupsenable KUYRUK_ADI; cupsaccept KUYRUK_ADI
EOF
}

help_menu() {
    local help_choice
    while true; do
        clear_screen
        print_color "$CYAN" "  Yardım / Help Center"
        print_color "$CYAN" "  ────────────────────────────────────────────────────────"
        echo "  [1] Kurulum / Installation"
        echo "  [2] Etkinleştirme & Kuyruk Ekleme / Activation & Add Queue"
        echo "  [3] Doğrulama / Verification"
        echo "  [4] Sorun Giderme / Troubleshooting"
        echo "  [5] Tümü / All Topics"
        echo "  [0] Geri / Back"
        print_color "$CYAN" "  ────────────────────────────────────────────────────────"
        printf 'Seçim / Choice [0-5]: '; read -r help_choice || return 0
        case "$help_choice" in
            1) print_install_help; press_any_key ;; 2) print_activation_help; press_any_key ;;
            3) print_verification_help; press_any_key ;; 4) print_troubleshooting_help; press_any_key ;;
            5) print_install_help; print_activation_help; print_verification_help; print_troubleshooting_help; press_any_key ;;
            0) return 0 ;; *) print_color "$RED" "[!] Geçersiz seçim / Invalid choice."; sleep 1 ;;
        esac
    done
}

print_cli_help() {
    cat <<EOF
P1600dn Series (Native ARM64) Driver Manager

Kullanım / Usage:
  ./p1600dn_manager.command                  Etkileşimli ana menü / Interactive main menu
  ./p1600dn_manager.command --help           Yardım / Help
  ./p1600dn_manager.command --install [--yes]
                                                 Sudo iste, derle, test et ve kur; ilk kurulum Gutenprint'i kaldırmayı sorar
                                                 Build, test & install; the first install offers to remove Gutenprint
  ./p1600dn_manager.command --build-pappl    PAPPL Printer Application uygulamasını derle / Build the PAPPL app
  ./p1600dn_manager.command --uninstall [--yes]
                                                 Sürücüyü ve Gutenprint'i kaldır / Remove the driver and Gutenprint
  ./p1600dn_manager.command --enable-airprint
                                                 iPhone/iPad için AirPrint'i aç / Turn on AirPrint (iPhone, iPad)
  ./p1600dn_manager.command --disable-airprint
                                                 AirPrint'i kapat / Turn off AirPrint
  ./p1600dn_manager.command --test           Tüm testleri çalıştır / Run all automated tests
  ./p1600dn_manager.command --status [KUYRUK]
                                                 Yazıcı ve toner durumunu sorgula / Query printer/toner status
  ./p1600dn_manager.command --add-printer    Yazıcı kuyruğu ekle / Add new printer queue
  ./p1600dn_manager.command --update-printer [KUYRUK]
                                                 Kuyruğun sürücüsünü güncelle / Update queue driver

Geliştirici & Test / Developer & Test: --test-ppds, --test-status [STATUS_BINARY], --self-test, --build-pappl
EOF
    print_install_help
    print_activation_help
}

manager_self_test() {
    local script value boundary_value manager_code
    local scripts=("$MANAGER_PATH" "$DRIVER_DIR/p1600dn_manager.sh" "$DRIVER_DIR/test/test_ppds.sh" "$DRIVER_DIR/test/test_status.sh" "$DRIVER_DIR/test/test_manager_modes.sh" "$DRIVER_DIR/test/test_cups_pipeline.sh")
    for script in "${scripts[@]}"; do
        [ -x "$script" ] || { echo "    [FAIL] Script is not executable: $script" >&2; return 1; }
        bash -n "$script" || return 1
    done
    cmp -s "$DRIVER_DIR/p1600dn_manager.command" "$DRIVER_DIR/p1600dn_manager.sh" || return 1
    grep -Fq -- '--test-ppds "$@"' "$DRIVER_DIR/test/test_ppds.sh" || return 1
    grep -Fq -- '--test-status "$@"' "$DRIVER_DIR/test/test_status.sh" || return 1
    # The manager's code without these grep lines, each of which contains the
    # text it looks for.
    manager_code=$(grep -vF -- 'grep -Fq' "$MANAGER_PATH")
    grep -Fq -- 'if [ "${P1600DN_SOURCE_ONLY:-0}" != "1" ]; then' <<<"$manager_code" || return 1
    grep -Fq -- 'if /usr/bin/xcode-select --install; then' <<<"$manager_code" || return 1
    grep -Fq -- 'ensure_command_line_tools "$action" || return 1' <<<"$manager_code" || return 1
    if ! verify_arm64_binary "$DRIVER_DIR/build/rastertop1600dn" ||
       ! verify_arm64_binary "$DRIVER_DIR/build/p1600dn-status" ||
       ! verify_shared_tools "$DRIVER_DIR/build"; then
        echo "    [FAIL] Installer ARM64 architecture validation failed" >&2
        return 1
    fi
    if verify_arm64_binary "$DRIVER_DIR/build/nonexistent-binary"; then
        echo "    [FAIL] Installer architecture validation accepted a missing binary" >&2
        return 1
    fi
    local unsigned_copy
    unsigned_copy=$(mktemp "${TMPDIR:-/tmp}/p1600dn-unsigned.XXXXXX") || return 1
    cp "$DRIVER_DIR/build/rastertop1600dn" "$unsigned_copy" &&
        /usr/bin/codesign --remove-signature "$unsigned_copy" 2>/dev/null
    if verify_arm64_binary "$unsigned_copy"; then
        rm -f "$unsigned_copy"
        echo "    [FAIL] Installer architecture validation accepted an unsigned binary" >&2
        return 1
    fi
    rm -f "$unsigned_copy"
    echo "    [PASS] Installer ARM64 Architecture Validation Test (arm64, macOS $MACOS_MIN_VERSION+, signed)"
    for value in HP_LaserJet_P1606dn printer-1 printer.1 printer_1 A; do
        is_safe_queue_name "$value" || { echo "    [FAIL] Safe new queue name rejected: $value" >&2; return 1; }
    done
    for value in '' 'bad queue' 'bad/queue' 'bad#queue' $'bad\nqueue' 'Türkçe'; do
        if is_safe_queue_name "$value"; then echo "    [FAIL] Unsafe new queue name accepted" >&2; return 1; fi
    done
    boundary_value=$(printf 'q%.0s' {1..127})
    is_safe_queue_name "$boundary_value" || { echo "    [FAIL] 127-character queue name rejected" >&2; return 1; }
    boundary_value="${boundary_value}q"
    if is_safe_queue_name "$boundary_value"; then echo "    [FAIL] 128-character queue name accepted" >&2; return 1; fi
    for value in Fakülte HP_LaserJet_P1606dn printer-1; do
        is_safe_existing_queue_name "$value" || { echo "    [FAIL] Safe existing queue name rejected: $value" >&2; return 1; }
    done
    for value in '' 'geçersiz/kuyruk' 'bad queue' $'bad\tqueue' $'bad\nqueue'; do
        if is_safe_existing_queue_name "$value"; then echo "    [FAIL] Unsafe existing queue name accepted" >&2; return 1; fi
    done
    for value in usb://HP/P1606dn ipp://printer/ipp/print ipps://printer/ipp/print dnssd://printer socket://192.0.2.1:9100; do
        is_supported_device_uri "$value" || { echo "    [FAIL] Supported device URI rejected: $value" >&2; return 1; }
    done
    for value in '' usb:// ipp:// ipps:// dnssd:// socket:// file:///tmp/printer http://printer 'usb://bad uri' $'usb://bad\nuri'; do
        if is_supported_device_uri "$value"; then echo "    [FAIL] Unsafe device URI accepted" >&2; return 1; fi
    done
    boundary_value="usb://$(printf 'a%.0s' {1..2042})"
    is_supported_device_uri "$boundary_value" || { echo "    [FAIL] 2048-character device URI rejected" >&2; return 1; }
    boundary_value="${boundary_value}a"
    if is_supported_device_uri "$boundary_value"; then echo "    [FAIL] 2049-character device URI accepted" >&2; return 1; fi
    "$MANAGER_PATH" --help >/dev/null || return 1
    for value in --help --install --build-pappl --test --test-ppds \
                 --self-test --add-printer --enable-airprint --disable-airprint; do
        if "$MANAGER_PATH" "$value" unexpected-extra-argument >/dev/null 2>&1; then
            echo "    [FAIL] Manager accepted extra argument for $value" >&2
            return 1
        fi
    done
    if "$MANAGER_PATH" --invalid-action-for-test >/dev/null 2>&1; then echo "    [FAIL] Manager accepted an invalid action" >&2; return 1; fi
    echo "    [PASS] Manager CLI/Wrapper/Queue/URI Security Matrix"
}

interactive_menu() {
    local choice queue_name
    while true; do
        clear_screen
        printf '%b  P1600dn Serisi — Sürücü Yöneticisi / Driver Manager%b\n' "$BOLD" "$NC"
        print_color "$CYAN" "  ──────────────────────────────────────────────────────────────────────────"
        printf '  %b[1]%b  %bKur / Install%b  (Derle + Test et / Build + Test)\n'                  "$BOLD" "$NC" "$GREEN"  "$NC"
        printf '  %b[2]%b  %bKaldır / Uninstall%b  (Native ARM64, Gutenprint & Eski Kalıntılar / Legacy Residue)\n' "$BOLD" "$NC" "$RED" "$NC"
        printf '  %b[3]%b  Testleri Çalıştır / Run Tests\n'                                     "$BOLD" "$NC"
        printf '  %b[4]%b  Sistem Durumu / System Status\n'                                     "$BOLD" "$NC"
        printf '  %b[5]%b  Yazıcı & Toner Durumu / Printer & Toner Status\n'                    "$BOLD" "$NC"
        printf '  %b[6]%b  Yazıcı Kuyruğu Ekle / Add Printer Queue (P1600dn Serisi)\n'          "$BOLD" "$NC"
        printf '  %b[7]%b  %bYardım / Help Center%b\n'                                          "$BOLD" "$NC" "$CYAN"   "$NC"
        printf '  %b[8]%b  Kuyruğun Sürücüsünü Değiştir / Switch Queue Driver\n'               "$BOLD" "$NC"
        printf '  %b[9]%b  %bAirPrint (iPhone/iPad) Aç-Kapat / On-Off%b\n'                      "$BOLD" "$NC" "$GREEN"  "$NC"
        printf '  %b[0]%b  Çıkış / Exit\n'                                                      "$BOLD" "$NC"
        print_color "$CYAN" "  ──────────────────────────────────────────────────────────────────────────"
        printf 'Seçim / Choice [0-9]: '
        if ! read -r choice; then echo ""; return 0; fi
        case "$choice" in
            1) echo ""; execute_privileged_action --install || true; press_any_key ;;
            2) echo ""; if confirm_action "Sürücü dosyaları ve Gutenprint kaldırılsın mı? / Remove driver files and Gutenprint?"; then execute_privileged_action --uninstall --yes || true; fi; press_any_key ;;
            3) echo ""; run_driver_tests || true; press_any_key ;;
            4) echo ""; show_system_status; press_any_key ;;
            5) echo ""; printf 'Kuyruk adı (tümü için boş / blank for all): '; read -r queue_name || queue_name=""; query_printer_status "$queue_name" || true; press_any_key ;;
            6) echo ""; configure_printer_queue || true; press_any_key ;;
            7) help_menu ;; 0) print_color "$GREEN" "[+] Çıkış yapılıyor / Exiting."; return 0 ;;
            8) echo ""; update_printer_driver || true; press_any_key ;;
            9) echo ""; airprint_menu; press_any_key ;;
            *) print_color "$RED" "[!] Geçersiz seçim / Invalid choice. (0-9)"; sleep 1 ;;
        esac
    done
}

main() {
    local action
    ASSUME_YES=0

    if [ "$#" -eq 0 ]; then
        interactive_menu
        return $?
    fi
    action="$1"
    case "$action" in
        --interactive) [ "$#" -eq 1 ] || return 2; interactive_menu ;;
        --help|-h) [ "$#" -eq 1 ] || return 2; print_cli_help ;;
        --install)
            if [ "${2:-}" = "--yes" ] && [ "$#" -eq 2 ]; then ASSUME_YES=1; elif [ "$#" -ne 1 ]; then return 2; fi
            if [ "$ASSUME_YES" -eq 1 ]; then execute_privileged_action --install --yes; else execute_privileged_action --install; fi ;;
        --build-pappl) [ "$#" -eq 1 ] || return 2; build_pappl_target ;;
        --uninstall)
            if [ "${2:-}" = "--yes" ] && [ "$#" -eq 2 ]; then ASSUME_YES=1; elif [ "$#" -ne 1 ]; then return 2; fi
            if [ "$ASSUME_YES" -eq 1 ]; then execute_privileged_action --uninstall --yes; else execute_privileged_action --uninstall; fi ;;
        --enable-airprint) [ "$#" -eq 1 ] || return 2; execute_privileged_action --enable-airprint ;;
        --disable-airprint) [ "$#" -eq 1 ] || return 2; execute_privileged_action --disable-airprint ;;
        --test-ppds) [ "$#" -eq 1 ] || return 2; test_p1606dn_ppd ;;
        --test-status) [ "$#" -le 2 ] || return 2; test_status_arguments "${2:-./build/p1600dn-status}" ;;
        --self-test) [ "$#" -eq 1 ] || return 2; manager_self_test ;;
        --test) [ "$#" -eq 1 ] || return 2; run_driver_tests ;;
        --status) [ "$#" -le 2 ] || return 2; query_printer_status "${2:-}" ;;
        --add-printer) [ "$#" -eq 1 ] || return 2; configure_printer_queue ;;
        --update-printer) [ "$#" -le 2 ] || return 2; update_printer_driver "${2:-}" ;;
        *) echo "Bilinmeyen işlem: $action" >&2; echo "Yardım: $MANAGER_PATH --help" >&2; return 2 ;;
    esac
}

if [ "${P1600DN_SOURCE_ONLY:-0}" != "1" ]; then
    main "$@"
fi
