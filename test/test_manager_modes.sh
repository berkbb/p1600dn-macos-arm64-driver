#!/bin/bash
set -eu

script_dir=$(cd "$(dirname "$0")" && pwd)
module_dir=$(cd "$script_dir/.." && pwd)
export P1600DN_SOURCE_ONLY=1
# shellcheck source=../p1600dn_manager.command
source "$module_dir/p1600dn_manager.command"
unset P1600DN_SOURCE_ONLY

fail() {
    echo "    [FAIL] $1" >&2
    exit 1
}

assert_file() {
    [ -f "$1" ] || fail "Missing expected file: $1"
}

assert_contains() {
    grep -Fq -- "$2" "$1" || fail "Expected '$2' in $1"
}

test_root=$(mktemp -d "/tmp/p1600dn-manager-modes.XXXXXX")
trap 'rm -rf "$test_root"' EXIT

workflow_log="$test_root/workflow.log"
touch "$workflow_log"

require_root() { return 0; }
check_sudo() { return 0; }
killall() { return 0; }
chown() { return 0; }
make() {
    printf 'make %s\n' "$*" >>"$workflow_log"
    return 0
}
# LaunchAgents go to a test directory and launchctl only records its calls.
LAUNCH_AGENTS_DIR="$test_root/system/Library/LaunchAgents"
LAUNCH_DAEMONS_DIR="$test_root/system/Library/LaunchDaemons"
CUPS_PPD_DIR="$test_root/system/etc/cups/ppd"
# Driver 5.0's paths, so the migration never touches the real system.
LEGACY_INSTALL_DIR="$test_root/system/Library/Printers/hp_p1606dn"
LEGACY_PPD="$test_root/system/Library/Printers/PPDs/Contents/Resources/HP-LaserJet-P1606dn-ARM.ppd"
OPENPRINTING_FILTER="$LEGACY_INSTALL_DIR/rastertohpp1606dn-openprinting"
OPENPRINTING_ENCODER="$LEGACY_INSTALL_DIR/foo2zjs-openprinting"
launchctl_log="$test_root/launchctl.log"
LAUNCHCTL="$test_root/launchctl"
printf '#!/bin/sh\necho "$*" >>"%s"\n' "$launchctl_log" >"$LAUNCHCTL"
chmod 755 "$LAUNCHCTL"
console_user_id() { echo 501; }
# Gutenprint lives under the test tree, and pkgutil lists $test_root/pkgs and
# records the rest of its calls.
GUTENPRINT_ROOT="$test_root/system"
pkgutil_log="$test_root/pkgutil.log"
PKGUTIL="$test_root/pkgutil"
printf '#!/bin/sh\nif [ "$1" = --pkgs ]; then cat "%s" 2>/dev/null; else echo "$*" >>"%s"; fi\n' "$test_root/pkgs" "$pkgutil_log" >"$PKGUTIL"
chmod 755 "$PKGUTIL"
: >"$pkgutil_log"

plant_gutenprint() {
    mkdir -p "$GUTENPRINT_ROOT/Library/Printers/Gutenprint.printerDriver/Contents" \
             "$GUTENPRINT_ROOT/Library/Receipts/Gutenprint.pkg" "$PPD_INSTALL_DIR"
    : >"$PPD_INSTALL_DIR/stp-hp-lj_1160.5.3.ppd.gz"
    echo "org.gutenprint.synthetic" >"$test_root/pkgs"
}

gutenprint_present() {
    [ -e "$GUTENPRINT_ROOT/Library/Printers/Gutenprint.printerDriver" ] ||
        [ -e "$GUTENPRINT_ROOT/Library/Receipts/Gutenprint.pkg" ] ||
        [ -e "$PPD_INSTALL_DIR/stp-hp-lj_1160.5.3.ppd.gz" ]
}

test_install_mode() {
    INSTALL_DIR="$test_root/system/Library/Printers/p1600dn"
    PPD_INSTALL_DIR="$test_root/system/Library/Printers/PPDs/Contents/Resources"
    INSTALLED_PPD="$PPD_INSTALL_DIR/P1600dn-Series.ppd"
    : >"$workflow_log"
    plant_gutenprint

    install_driver >"$test_root/install.out" 2>"$test_root/install.err" || fail "Install workflow failed"
    ! gutenprint_present || fail "The first install left Gutenprint behind"
    assert_contains "$pkgutil_log" "--forget org.gutenprint.synthetic"
    assert_contains "$test_root/install.out" "Gutenprint kaldırıldı"
    assert_file "$INSTALL_DIR/rastertop1600dn"
    assert_file "$INSTALL_DIR/p1600dn-status"
    assert_file "$INSTALL_DIR/commandtop1600dn"
    assert_file "$INSTALL_DIR/p1600dn-utility"
    assert_file "$INSTALL_DIR/P1600dn Series Utility.app/Contents/MacOS/P1600dnUtility"
    /usr/bin/codesign --verify --strict "$INSTALL_DIR/P1600dn Series Utility.app" || fail "Installed utility app signature broke"
    assert_file "$INSTALL_DIR/P1600dn Notifier.app/Contents/MacOS/P1600dnNotifier"
    /usr/bin/codesign --verify --strict "$INSTALL_DIR/P1600dn Notifier.app" || fail "Installed notifier app signature broke"
    assert_file "$LAUNCH_AGENTS_DIR/com.berkbb.p1600dn.notifier.plist"
    [ "$(stat -f '%Lp' "$LAUNCH_AGENTS_DIR/com.berkbb.p1600dn.notifier.plist")" = "644" ] || fail "Notifier LaunchAgent mode is not 644"
    assert_contains "$launchctl_log" "bootstrap gui/501 $LAUNCH_AGENTS_DIR/com.berkbb.p1600dn.notifier.plist"
    assert_file "$INSTALL_DIR/p1600dn-airprint"
    assert_file "$INSTALL_DIR/P1600dnPrinter.png"
    [ -x "$INSTALL_DIR/p1600dn-airprint" ] || fail "Installed AirPrint server is not executable"
    [ ! -e "$LAUNCH_DAEMONS_DIR/com.berkbb.p1600dn.airprint.plist" ] || fail "AirPrint must stay off until it is enabled"
    assert_file "$INSTALL_DIR/P1600dnPrinter.icns"
    assert_file "$INSTALLED_PPD"
    [ -x "$INSTALL_DIR/rastertop1600dn" ] || fail "Installed filter is not executable"
    [ -x "$INSTALL_DIR/p1600dn-status" ] || fail "Installed status tool is not executable"
    [ -x "$INSTALL_DIR/commandtop1600dn" ] || fail "Installed toner level filter is not executable"
    [ -x "$INSTALL_DIR/p1600dn-utility" ] || fail "Installed printer utility tool is not executable"
    [ "$(stat -f '%Lp' "$INSTALLED_PPD")" = "644" ] || fail "Installed PPD mode is not 644"
    assert_contains "$workflow_log" "make clean"
    assert_contains "$workflow_log" "make test"
    assert_contains "$test_root/install.out" "Sürücü dosyaları başarıyla kuruldu"
    # A reinstall or update is not a first install: Gutenprint stays.
    plant_gutenprint
    : >"$pkgutil_log"
    install_driver >"$test_root/reinstall.out" 2>"$test_root/reinstall.err" || fail "Reinstall workflow failed"
    gutenprint_present || fail "A reinstall removed Gutenprint"
    [ ! -s "$pkgutil_log" ] || fail "A reinstall touched Gutenprint's package receipts"
    echo "    [PASS] Mode 1: Build/Test/Install Workflow, Gutenprint Removed on First Install Only (isolated filesystem)"
}

test_install_failure_matrix() {
    local scenario
    for scenario in root clean test architecture; do
        (
            INSTALL_DIR="$test_root/install-failure-$scenario/driver"
            PPD_INSTALL_DIR="$test_root/install-failure-$scenario/ppds"
            INSTALLED_PPD="$PPD_INSTALL_DIR/P1600dn-Series.ppd"
            require_root() { [ "$scenario" != "root" ]; }
            make() {
                if [ "$scenario" = "clean" ] && [ "${1:-}" = "clean" ]; then return 1; fi
                if [ "$scenario" = "test" ] && [ "${1:-}" = "test" ]; then return 1; fi
                return 0
            }
            verify_arm64_binary() { [ "$scenario" != "architecture" ]; }
            if install_driver >"$test_root/install-failure-$scenario.out" 2>"$test_root/install-failure-$scenario.err"; then
                exit 1
            fi
            [ ! -e "$INSTALL_DIR/rastertop1600dn" ] || exit 1
            [ ! -e "$INSTALL_DIR/commandtop1600dn" ] || exit 1
            [ ! -e "$INSTALLED_PPD" ] || exit 1
        ) || fail "Install failure path did not stop safely: $scenario"
    done
    assert_contains "$test_root/install-failure-architecture.err" "arm64 mimarisinde değil"
    echo "    [PASS] Mode 1: Root/Build/Test/Architecture Failure Matrix"
}

test_airprint_mode() {
    enable_airprint >"$test_root/airprint-on.out" 2>"$test_root/airprint-on.err" || fail "AirPrint enable failed"
    assert_file "$LAUNCH_DAEMONS_DIR/com.berkbb.p1600dn.airprint.plist"
    [ "$(stat -f '%Lp' "$LAUNCH_DAEMONS_DIR/com.berkbb.p1600dn.airprint.plist")" = "644" ] || fail "AirPrint LaunchDaemon mode is not 644"
    plutil -lint "$LAUNCH_DAEMONS_DIR/com.berkbb.p1600dn.airprint.plist" >/dev/null || fail "AirPrint LaunchDaemon is not a valid plist"
    assert_contains "$LAUNCH_DAEMONS_DIR/com.berkbb.p1600dn.airprint.plist" "<string>nobody</string>"
    assert_contains "$launchctl_log" "bootstrap system $LAUNCH_DAEMONS_DIR/com.berkbb.p1600dn.airprint.plist"
    assert_contains "$test_root/airprint-on.out" "AirPrint açık"
    airprint_enabled || fail "AirPrint is not reported as enabled"
    # A reinstall restarts the running server with the new binary.
    install_shared_tools build >/dev/null 2>&1 || fail "Reinstall with AirPrint on failed"
    assert_contains "$launchctl_log" "kickstart -k system/com.berkbb.p1600dn.airprint"
    disable_airprint >"$test_root/airprint-off.out" 2>&1 || fail "AirPrint disable failed"
    [ ! -e "$LAUNCH_DAEMONS_DIR/com.berkbb.p1600dn.airprint.plist" ] || fail "AirPrint LaunchDaemon remained"
    assert_contains "$launchctl_log" "bootout system/com.berkbb.p1600dn.airprint"
    ! airprint_enabled || fail "AirPrint is still reported as enabled"
    (
        INSTALL_DIR="$test_root/no-driver"
        enable_airprint >/dev/null 2>"$test_root/airprint-missing.err"
    ) && fail "AirPrint was enabled without the driver"
    assert_contains "$test_root/airprint-missing.err" "Önce sürücüyü kurun"
    enable_airprint >/dev/null 2>&1 || fail "AirPrint re-enable failed"
    echo "    [PASS] Mode 9: AirPrint On/Off (LaunchDaemon as nobody, restart on reinstall, needs the driver)"
}

test_uninstall_mode() {
    ASSUME_YES=1
    uninstall_driver >"$test_root/uninstall.out" 2>"$test_root/uninstall.err" || fail "Uninstall workflow failed"
    [ ! -e "$INSTALL_DIR" ] || fail "Driver directory remained after uninstall"
    [ ! -e "$INSTALLED_PPD" ] || fail "PPD remained after uninstall"
    [ ! -e "$LAUNCH_AGENTS_DIR/com.berkbb.p1600dn.notifier.plist" ] || fail "Notifier LaunchAgent remained after uninstall"
    assert_contains "$launchctl_log" "bootout gui/501/com.berkbb.p1600dn.notifier"
    [ ! -e "$LAUNCH_DAEMONS_DIR/com.berkbb.p1600dn.airprint.plist" ] || fail "AirPrint LaunchDaemon remained after uninstall"
    ! gutenprint_present || fail "Uninstall left Gutenprint behind"
    assert_contains "$test_root/uninstall.out" "Gutenprint kaldırıldı"
    assert_contains "$test_root/uninstall.out" "sürücüsü ve PPD'si kaldırıldı"
    echo "    [PASS] Mode 2: Uninstall Workflow, Gutenprint Removed Without Asking (isolated filesystem)"
}

test_uninstall_cancel_and_root_failure() {
    local cancel_dir="$test_root/uninstall-cancel/driver"
    local cancel_ppd_dir="$test_root/uninstall-cancel/ppds"
    (
        INSTALL_DIR="$cancel_dir"
        PPD_INSTALL_DIR="$cancel_ppd_dir"
        INSTALLED_PPD="$cancel_ppd_dir/P1600dn-Series.ppd"
        mkdir -p "$INSTALL_DIR" "$PPD_INSTALL_DIR"
        : >"$INSTALL_DIR/rastertop1600dn"
        : >"$INSTALLED_PPD"
        ASSUME_YES=0
        uninstall_driver <<'EOF'
hayir
EOF
        [ -f "$INSTALL_DIR/rastertop1600dn" ] || exit 1
        [ -f "$INSTALLED_PPD" ] || exit 1
        require_root() { return 1; }
        if uninstall_driver >/dev/null 2>&1; then exit 1; fi
    ) >"$test_root/uninstall-cancel.out" 2>"$test_root/uninstall-cancel.err" || fail "Uninstall cancel/root matrix failed"
    assert_contains "$test_root/uninstall-cancel.out" "İşlem iptal edildi"
    echo "    [PASS] Mode 2: Cancellation and Root-Denial Safety Paths"
}

# Gutenprint cannot drive the P1600dn series. The purge moves P1600dn queues
# on it to the native PPD, keeps it while another printer uses it, and
# otherwise removes it without asking.
test_gutenprint_purge_mode() {
    (
        GUTENPRINT_ROOT="$test_root/purge/system"
        CUPS_PPD_DIR="$test_root/purge/cups-ppd"
        PPD_INSTALL_DIR="$test_root/purge/ppds"
        INSTALLED_PPD="$PPD_INSTALL_DIR/P1600dn-Series.ppd"
        mkdir -p "$CUPS_PPD_DIR" "$GUTENPRINT_ROOT/usr/libexec/cups/filter" "$GUTENPRINT_ROOT/usr/local/bin"
        plant_gutenprint
        : >"$INSTALLED_PPD"
        : >"$GUTENPRINT_ROOT/usr/libexec/cups/filter/rastertogutenprint.5.3"
        : >"$GUTENPRINT_ROOT/usr/local/bin/escputil"
        lpstat() {
            case "${2:-}" in
                Ofis) echo "device for Ofis: usb://HP/LaserJet%20Professional%20P1606dn?serial=SYNTH0001" ;;
                Epson) echo "device for Epson: usb://EPSON/XP-2100?serial=SYNTH0002" ;;
                HP_LaserJet_P1606dn) return 1 ;;  # known by its name alone
                *) echo "device for ${2:-}: socket://192.0.2.10:9100" ;;
            esac
        }
        for queue in Ofis HP_LaserJet_P1606dn Epson; do
            printf '*cupsFilter: "application/vnd.cups-raster 100 rastertogutenprint.5.3"\n' >"$CUPS_PPD_DIR/$queue.ppd"
        done
        printf '*cupsFilter: "application/vnd.cups-raster 0 /Library/Printers/p1600dn/rastertop1600dn"\n' \
            >"$CUPS_PPD_DIR/Native.ppd"
        : >"$workflow_log"
        : >"$pkgutil_log"

        # P1600dn queues (by device URI or name) move; the Epson keeps Gutenprint.
        purge_gutenprint >"$test_root/purge-kept.out" || exit 1
        grep -Fq "lpadmin <-p> <Ofis> <-E> <-P> <$INSTALLED_PPD>" "$workflow_log" || exit 1
        grep -Fq "lpadmin <-p> <HP_LaserJet_P1606dn> <-E> <-P> <$INSTALLED_PPD>" "$workflow_log" || exit 1
        if grep -Fq '<Epson>' "$workflow_log" || grep -Fq '<Native>' "$workflow_log"; then exit 1; fi
        grep -Fq "other printers use it: Epson" "$test_root/purge-kept.out" || exit 1
        gutenprint_present || exit 1
        [ ! -s "$pkgutil_log" ] || exit 1

        # Nobody else uses it: everything goes, with a single line of output.
        rm -f "$CUPS_PPD_DIR/Epson.ppd" "$CUPS_PPD_DIR/Ofis.ppd" "$CUPS_PPD_DIR/HP_LaserJet_P1606dn.ppd"
        : >"$workflow_log"
        purge_gutenprint >"$test_root/purge.out" || exit 1
        ! gutenprint_present || exit 1
        [ ! -e "$GUTENPRINT_ROOT/usr/libexec/cups/filter/rastertogutenprint.5.3" ] || exit 1
        [ ! -e "$GUTENPRINT_ROOT/usr/local/bin/escputil" ] || exit 1
        [ -e "$INSTALLED_PPD" ] || exit 1
        grep -Fq -- "--forget org.gutenprint.synthetic" "$pkgutil_log" || exit 1
        [ "$(wc -l <"$test_root/purge.out" | tr -d ' ')" -eq 1 ] || exit 1
        grep -Fq "Gutenprint kaldırıldı" "$test_root/purge.out" || exit 1
        [ ! -s "$workflow_log" ] || exit 1

        # Nothing left: no output and no error.
        : >"$test_root/pkgs"
        output=$(purge_gutenprint) || exit 1
        [ -z "$output" ] || exit 1

        # A path that cannot be removed is reported; the rest still goes.
        plant_gutenprint
        rm() {
            [ "${2:-}" != "$GUTENPRINT_ROOT/Library/Printers/Gutenprint.printerDriver" ] || return 1
            command rm "$@"
        }
        if purge_gutenprint >/dev/null; then exit 1; fi
        [ ! -e "$GUTENPRINT_ROOT/Library/Receipts/Gutenprint.pkg" ] || exit 1
        [ ! -e "$PPD_INSTALL_DIR/stp-hp-lj_1160.5.3.ppd.gz" ] || exit 1
    ) || fail "Gutenprint purge failed"
    echo "    [PASS] Gutenprint Purge (P1600dn queues moved, kept for other printers, single-line removal, failures reported)"
}

test_run_tests_mode() {
    : >"$workflow_log"
    run_driver_tests >"$test_root/run-tests.out" 2>"$test_root/run-tests.err" || fail "Run-tests workflow failed"
    assert_contains "$workflow_log" "make test"
    assert_contains "$test_root/run-tests.out" "Tüm otomatik testler geçti"
    echo "    [PASS] Mode 3: Run All Tests Workflow"
}

test_run_tests_failure_mode() {
    (
        ensure_build_ownership() { return 0; }
        make() { return 1; }
        if run_driver_tests >"$test_root/run-tests-failure.out" 2>"$test_root/run-tests-failure.err"; then exit 1; fi
    ) || fail "Run-tests workflow accepted a failed test suite"
    assert_contains "$test_root/run-tests-failure.out" "Testlerden en az biri başarısız oldu"
    echo "    [PASS] Mode 3: Failed Test Suite Propagation"
}

lpinfo() {
    echo "network socket://192.0.2.10:9100"
    echo "direct usb://HP/LaserJet%20Professional%20P1606dn"
}

lpstat() {
    case "${1:-}" in
        -p)
            if [ "${2:-}" = "-d" ]; then
                echo "printer Fakülte is idle. enabled since test"
                echo "system default destination: Fakülte"
            else
                echo "printer ${2:-Fakülte} is idle. enabled since test"
            fi
            return 0
            ;;
        -d) echo "system default destination: Fakülte"; return 0 ;;
        -v) echo "device for ${2:-Fakülte}: usb://HP/P1606dn"; return 0 ;;
        *) return 0 ;;
    esac
}

lpadmin() {
    {
        printf 'lpadmin'
        printf ' <%s>' "$@"
        printf '\n'
    } >>"$workflow_log"
    return 0
}

sudo() { "$@"; }
cupsenable() { printf 'cupsenable %s\n' "$*" >>"$workflow_log"; }
cupsaccept() { printf 'cupsaccept %s\n' "$*" >>"$workflow_log"; }

test_system_status_mode() {
    mkdir -p "$INSTALL_DIR" "$PPD_INSTALL_DIR"
    : >"$INSTALL_DIR/rastertop1600dn"
    : >"$INSTALL_DIR/p1600dn-status"
    : >"$INSTALLED_PPD"
    chmod 755 "$INSTALL_DIR/rastertop1600dn" "$INSTALL_DIR/p1600dn-status"
    show_system_status >"$test_root/system-status.out" 2>"$test_root/system-status.err"
    assert_contains "$test_root/system-status.out" "Raster filtresi kurulu"
    assert_contains "$test_root/system-status.out" "Durum aracı kurulu"
    assert_contains "$test_root/system-status.out" "PPD'si kurulu"
    assert_contains "$test_root/system-status.out" "system default destination"
    echo "    [PASS] Mode 4: Installation/Activity/CUPS Status Workflow"
}

test_system_status_missing_mode() {
    (
        INSTALL_DIR="$test_root/missing-system/driver"
        PPD_INSTALL_DIR="$test_root/missing-system/ppds"
        INSTALLED_PPD="$PPD_INSTALL_DIR/P1600dn-Series.ppd"
        lpstat() { return 1; }
        show_system_status
    ) >"$test_root/system-status-missing.out" 2>"$test_root/system-status-missing.err" || fail "Missing-installation status workflow failed"
    assert_contains "$test_root/system-status-missing.out" "Raster filtresi kurulu değil"
    assert_contains "$test_root/system-status-missing.out" "Durum aracı kurulu değil"
    assert_contains "$test_root/system-status-missing.out" "PPD'si kurulu değil"
    assert_contains "$test_root/system-status-missing.out" "CUPS kuyruğu bulunamadı"
    echo "    [PASS] Mode 4: Completely Missing Installation/CUPS Matrix"
}

test_printer_status_mode() {
    : >"$workflow_log"
    if query_printer_status "__p1600dn_missing_queue_mode_test__" >"$test_root/printer-status.out" 2>"$test_root/printer-status.err"; then
        fail "Printer status mode accepted a missing queue"
    fi
    assert_contains "$workflow_log" "make build/p1600dn-status"
    assert_contains "$test_root/printer-status.err" "CUPS kuyruğu bulunamadı"
    echo "    [PASS] Mode 5: Printer/Toner Status Failure Workflow"
}

test_printer_status_build_failure() {
    (
        make() { return 1; }
        if query_printer_status >"$test_root/printer-status-build.out" 2>"$test_root/printer-status-build.err"; then exit 1; fi
    ) || fail "Printer status workflow accepted a build failure"
    assert_contains "$test_root/printer-status-build.out" "Durum aracı derlenemedi"
    echo "    [PASS] Mode 5: Status-Tool Build Failure Propagation"
}

test_add_queue_mode() {
    : >"$workflow_log"
    configure_printer_queue >"$test_root/add-queue.out" 2>"$test_root/add-queue.err" <<'EOF'
HP_LaserJet_P1606dn
usb://HP/LaserJet%20Professional%20P1606dn
EOF
    assert_contains "$workflow_log" "lpadmin <-p> <HP_LaserJet_P1606dn> <-E> <-v> <usb://HP/LaserJet%20Professional%20P1606dn>"
    assert_contains "$test_root/add-queue.out" "Kuyruk oluşturuldu ve etkinleştirildi"
    echo "    [PASS] Mode 6: Add Queue/Activate Driver Workflow"
}

test_add_queue_rejection_matrix() {
    local case_name queue uri expected long_queue long_uri
    long_queue=$(printf 'q%.0s' {1..128})
    long_uri="usb://$(printf 'a%.0s' {1..2043})"
    while IFS='|' read -r case_name queue uri expected; do
        : >"$workflow_log"
        if configure_printer_queue >"$test_root/add-$case_name.out" 2>"$test_root/add-$case_name.err" <<EOF
$queue
$uri
EOF
        then
            fail "Queue creation accepted invalid case: $case_name"
        fi
        assert_contains "$test_root/add-$case_name.out" "$expected"
        [ ! -s "$workflow_log" ] || fail "Invalid queue case reached lpadmin: $case_name"
    done <<EOF
empty-name||usb://HP/P1606dn|Geçersiz kuyruk adı
space-name|bad queue|usb://HP/P1606dn|Geçersiz kuyruk adı
unicode-name|Türkçe|usb://HP/P1606dn|Geçersiz kuyruk adı
long-name|$long_queue|usb://HP/P1606dn|Geçersiz kuyruk adı
empty-uri|ValidQueue||Geçersiz veya desteklenmeyen URI
http-uri|ValidQueue|http://printer/ipp/print|Geçersiz veya desteklenmeyen URI
space-uri|ValidQueue|usb://bad uri|Geçersiz veya desteklenmeyen URI
long-uri|ValidQueue|$long_uri|Geçersiz veya desteklenmeyen URI
EOF

    (
        INSTALLED_PPD="$test_root/missing-add.ppd"
        if configure_printer_queue <<'EOF'
ValidQueue
usb://HP/P1606dn
EOF
        then exit 1; fi
    ) >"$test_root/add-missing-ppd.out" 2>"$test_root/add-missing-ppd.err" || fail "Missing-PPD queue rejection failed"
    assert_contains "$test_root/add-missing-ppd.out" "PPD'si kurulu değil"

    (
        check_sudo() { return 1; }
        if configure_printer_queue <<'EOF'
ValidQueue
usb://HP/P1606dn
EOF
        then exit 1; fi
    ) >"$test_root/add-sudo.out" 2>"$test_root/add-sudo.err" || fail "Queue creation accepted sudo failure"

    (
        lpadmin() { return 1; }
        if configure_printer_queue <<'EOF'
ValidQueue
usb://HP/P1606dn
EOF
        then exit 1; fi
    ) >"$test_root/add-lpadmin.out" 2>"$test_root/add-lpadmin.err" || fail "Queue creation accepted lpadmin failure"
    assert_contains "$test_root/add-lpadmin.out" "Kuyruk eklenemedi"
    echo "    [PASS] Mode 6: Name/URI/PPD/Sudo/lpadmin Rejection Matrix"
}

test_help_mode() {
    print_install_help >"$test_root/help.out"
    print_activation_help >>"$test_root/help.out"
    print_verification_help >>"$test_root/help.out"
    print_troubleshooting_help >>"$test_root/help.out"
    help_menu >>"$test_root/help.out" <<'EOF'
0
EOF
    assert_contains "$test_root/help.out" "DRIVER NASIL KURULUR?"
    assert_contains "$test_root/help.out" "DRIVER NASIL ETKİNLEŞTİRİLİR?"
    assert_contains "$test_root/help.out" "KURULUMU VE BASKIYI DOĞRULAMA"
    assert_contains "$test_root/help.out" "SORUN GİDERME"
    echo "    [PASS] Mode 7: Complete Help Center Workflow"
}

test_help_submenu_dispatch_matrix() {
    local choice expected
    while IFS='|' read -r choice expected; do
        (
            press_any_key() { return 0; }
            help_menu <<EOF
$choice
0
EOF
        ) >"$test_root/help-choice-$choice.out" 2>"$test_root/help-choice-$choice.err" || fail "Help submenu choice failed: $choice"
        assert_contains "$test_root/help-choice-$choice.out" "$expected"
    done <<'EOF'
1|DRIVER NASIL KURULUR?
2|DRIVER NASIL ETKİNLEŞTİRİLİR?
3|KURULUMU VE BASKIYI DOĞRULAMA
4|SORUN GİDERME
5|DRIVER NASIL KURULUR?
EOF
    (
        sleep() { return 0; }
        help_menu <<'EOF'
invalid
0
EOF
    ) >"$test_root/help-invalid.out" 2>"$test_root/help-invalid.err" || fail "Help submenu invalid-choice recovery failed"
    assert_contains "$test_root/help-invalid.out" "Geçersiz seçim"
    echo "    [PASS] Mode 7: Help Choices 1-5 + Invalid-Choice Recovery"
}

test_update_queue_mode() {
    : >"$workflow_log"
    ASSUME_YES=1
    update_printer_driver "Fakülte" >"$test_root/update-queue.out" 2>"$test_root/update-queue.err" || fail "Update queue workflow failed"
    assert_contains "$workflow_log" "lpadmin <-p> <Fakülte> <-E> <-P> <$INSTALLED_PPD> <-o> <Resolution=1200x600dpi> <-o> <Duplex=None>"
    if grep -Fq -- '<-v>' "$workflow_log"; then fail "Update queue workflow changed the device URI"; fi
    assert_contains "$workflow_log" "cupsenable Fakülte"
    assert_contains "$workflow_log" "cupsaccept Fakülte"
    assert_contains "$test_root/update-queue.out" "yerel P1600dn serisi sürücüsüne geçirildi"
    ASSUME_YES=0
    echo "    [PASS] Mode 8: In-Place Queue Driver Update Workflow"
}

test_update_queue_rejection_matrix() {
    (
        INSTALLED_PPD="$test_root/missing-update.ppd"
        if update_printer_driver Fakülte; then exit 1; fi
    ) >"$test_root/update-missing-ppd.out" 2>"$test_root/update-missing-ppd.err" || fail "Update accepted a missing PPD"
    assert_contains "$test_root/update-missing-ppd.out" "Gerekli PPD kurulu değil"

    if update_printer_driver 'bad queue' >"$test_root/update-invalid.out" 2>"$test_root/update-invalid.err"; then
        fail "Update accepted an unsafe queue name"
    fi
    assert_contains "$test_root/update-invalid.out" "Geçersiz CUPS kuyruk adı"

    (
        lpstat() { return 1; }
        if update_printer_driver MissingQueue; then exit 1; fi
    ) >"$test_root/update-missing-queue.out" 2>"$test_root/update-missing-queue.err" || fail "Update accepted a missing queue"
    assert_contains "$test_root/update-missing-queue.out" "CUPS kuyruğu bulunamadı"

    : >"$workflow_log"
    ASSUME_YES=0
    update_printer_driver Fakülte >"$test_root/update-cancel.out" 2>"$test_root/update-cancel.err" <<'EOF'
n
EOF
    [ ! -s "$workflow_log" ] || fail "Cancelled update reached lpadmin"
    assert_contains "$test_root/update-cancel.out" "İşlem iptal edildi"

    (
        ASSUME_YES=1
        check_sudo() { return 1; }
        if update_printer_driver Fakülte; then exit 1; fi
    ) >"$test_root/update-sudo.out" 2>"$test_root/update-sudo.err" || fail "Update accepted sudo failure"

    (
        ASSUME_YES=1
        lpadmin() { return 1; }
        if update_printer_driver Fakülte; then exit 1; fi
    ) >"$test_root/update-lpadmin.out" 2>"$test_root/update-lpadmin.err" || fail "Update accepted lpadmin failure"
    assert_contains "$test_root/update-lpadmin.out" "driver'ı değiştirilemedi"
    echo "    [PASS] Mode 8: PPD/Name/Existence/Cancel/Sudo/lpadmin Rejection Matrix"
}

# Driver 3.1-3.2 could install the OpenPrinting foo2zjs fallback, which
# skipped the page guard. Installing or removing the driver retires it: its
# files go and the queues that used its PPD move to the native PPD.
test_openprinting_retirement_mode() {
    (
        CUPS_PPD_DIR="$test_root/retire/cups-ppd"
        INSTALL_DIR="$test_root/retire/driver"
        PPD_INSTALL_DIR="$test_root/retire/ppds"
        INSTALLED_PPD="$PPD_INSTALL_DIR/P1600dn-Series.ppd"
        OPENPRINTING_FILTER="$INSTALL_DIR/rastertohpp1606dn-openprinting"
        OPENPRINTING_ENCODER="$INSTALL_DIR/foo2zjs-openprinting"
        OPENPRINTING_INSTALLED_PPD="$PPD_INSTALL_DIR/HP-LaserJet-P1606dn-OpenPrinting.ppd"
        mkdir -p "$CUPS_PPD_DIR" "$INSTALL_DIR" "$PPD_INSTALL_DIR"
        : >"$INSTALLED_PPD"
        : >"$OPENPRINTING_FILTER"
        : >"$OPENPRINTING_ENCODER"
        : >"$OPENPRINTING_INSTALLED_PPD"
        printf '*cupsFilter: "application/vnd.cups-raster 0 /Library/Printers/hp_p1606dn/rastertohpp1606dn-openprinting"\n' \
            >"$CUPS_PPD_DIR/Fakülte.ppd"
        printf '*cupsFilter: "application/vnd.cups-raster 0 /Library/Printers/p1600dn/rastertop1600dn"\n' \
            >"$CUPS_PPD_DIR/Native.ppd"
        : >"$workflow_log"
        retire_openprinting_fallback
        [ ! -e "$OPENPRINTING_FILTER" ] && [ ! -e "$OPENPRINTING_ENCODER" ] && [ ! -e "$OPENPRINTING_INSTALLED_PPD" ] || exit 1
        grep -Fq "lpadmin <-p> <Fakülte> <-E> <-P> <$INSTALLED_PPD>" "$workflow_log" || exit 1
        if grep -Fq '<Native>' "$workflow_log"; then exit 1; fi
        # Nothing left to retire: no change, no error.
        : >"$workflow_log"
        rm -f "$CUPS_PPD_DIR/Fakülte.ppd"
        retire_openprinting_fallback
        [ ! -s "$workflow_log" ]
    ) >"$test_root/retire.out" 2>&1 || fail "OpenPrinting fallback retirement failed"
    assert_contains "$test_root/retire.out" "ana sürücüye geçirildi"
    echo "    [PASS] OpenPrinting Fallback Retirement (files removed, its queues moved to the native PPD)"
}

# Driver 5.0 used other names. Installing moves its queues to the new PPD,
# keeps AirPrint on when it was on, and removes its agents and files; a
# queue of another driver stays untouched.
test_legacy_migration_mode() {
    (
        CUPS_PPD_DIR="$test_root/legacy/cups-ppd"
        INSTALL_DIR="$test_root/legacy/driver"
        PPD_INSTALL_DIR="$test_root/legacy/ppds"
        INSTALLED_PPD="$PPD_INSTALL_DIR/P1600dn-Series.ppd"
        LEGACY_INSTALL_DIR="$test_root/legacy/hp_p1606dn"
        LEGACY_PPD="$PPD_INSTALL_DIR/HP-LaserJet-P1606dn-ARM.ppd"
        mkdir -p "$CUPS_PPD_DIR" "$INSTALL_DIR" "$PPD_INSTALL_DIR" "$LEGACY_INSTALL_DIR" \
                 "$LAUNCH_AGENTS_DIR" "$LAUNCH_DAEMONS_DIR"
        : >"$INSTALLED_PPD"
        : >"$LEGACY_PPD"
        : >"$LEGACY_INSTALL_DIR/$LEGACY_FILTER_NAME"
        printf '#!/bin/sh\n' >"$INSTALL_DIR/p1600dn-airprint"
        chmod 755 "$INSTALL_DIR/p1600dn-airprint"
        : >"$LAUNCH_AGENTS_DIR/$LEGACY_NOTIFIER_LABEL.plist"
        : >"$LAUNCH_DAEMONS_DIR/$LEGACY_AIRPRINT_LABEL.plist"
        printf '*cupsFilter: "application/vnd.cups-raster 0 /Library/Printers/hp_p1606dn/rastertohpp1606dn"\n' \
            >"$CUPS_PPD_DIR/Ofis Yazıcısı.ppd"
        printf '*cupsFilter: "application/vnd.cups-raster 0 /usr/libexec/cups/filter/rastertoother"\n' \
            >"$CUPS_PPD_DIR/Other.ppd"
        : >"$workflow_log"
        : >"$launchctl_log"
        retire_legacy_driver
        grep -Fq "lpadmin <-p> <Ofis Yazıcısı> <-E> <-P> <$INSTALLED_PPD>" "$workflow_log" || exit 1
        if grep -Fq '<Other>' "$workflow_log"; then exit 1; fi
        [ ! -e "$LEGACY_INSTALL_DIR" ] && [ ! -e "$LEGACY_PPD" ] || exit 1
        [ ! -e "$LAUNCH_AGENTS_DIR/$LEGACY_NOTIFIER_LABEL.plist" ] || exit 1
        [ ! -e "$LAUNCH_DAEMONS_DIR/$LEGACY_AIRPRINT_LABEL.plist" ] || exit 1
        grep -Fq "bootout system/$LEGACY_AIRPRINT_LABEL" "$launchctl_log" || exit 1
        grep -Fq "bootout gui/501/$LEGACY_NOTIFIER_LABEL" "$launchctl_log" || exit 1
        # AirPrint was on with driver 5.0, so the new server is on.
        [ -f "$LAUNCH_DAEMONS_DIR/$AIRPRINT_LABEL.plist" ] || exit 1
        grep -Fq "bootstrap system $LAUNCH_DAEMONS_DIR/$AIRPRINT_LABEL.plist" "$launchctl_log" || exit 1
        # Nothing left (cupsd has replaced the queue's PPD): no change, no error.
        printf '*cupsFilter: "application/vnd.cups-raster 0 /Library/Printers/p1600dn/rastertop1600dn"\n' \
            >"$CUPS_PPD_DIR/Ofis Yazıcısı.ppd"
        : >"$workflow_log"
        retire_legacy_driver
        [ ! -s "$workflow_log" ] || exit 1
        # Uninstall only removes the files and agents.
        mkdir -p "$LEGACY_INSTALL_DIR"
        : >"$LEGACY_PPD"
        retire_legacy_driver files
        [ ! -e "$LEGACY_INSTALL_DIR" ] && [ ! -e "$LEGACY_PPD" ] && [ ! -s "$workflow_log" ]
    ) >"$test_root/legacy.out" 2>&1 || fail "Driver 5.0 migration failed"
    assert_contains "$test_root/legacy.out" "6.0 sürücüsüne geçirildi"
    echo "    [PASS] Driver 5.0 Migration (queues moved to the new PPD, AirPrint kept on, old agents and files removed)"
}

test_exit_mode() {
    interactive_menu >"$test_root/interactive-exit.out" <<'EOF'
0
EOF
    assert_contains "$test_root/interactive-exit.out" "Çıkış yapılıyor"
    echo "    [PASS] Mode 0: Interactive Exit Workflow"
}

test_interactive_dispatch_matrix() {
    local mode expected input queue_name
    while IFS='|' read -r mode expected queue_name; do
        if [ "$mode" = "5" ]; then
            input=$(printf '5\n%s\n0\n' "$queue_name")
        else
            input=$(printf '%s\n0\n' "$mode")
        fi
        (
            dispatch_log="$test_root/menu-$mode.log"
            : >"$dispatch_log"
            press_any_key() { return 0; }
            confirm_action() { return 0; }
            execute_privileged_action() { printf 'privileged %s\n' "$*" >>"$dispatch_log"; }
            run_driver_tests() { echo tests >>"$dispatch_log"; }
            show_system_status() { echo system-status >>"$dispatch_log"; }
            query_printer_status() { printf 'printer-status <%s>\n' "${1:-}" >>"$dispatch_log"; }
            configure_printer_queue() { echo add-printer >>"$dispatch_log"; }
            help_menu() { echo help >>"$dispatch_log"; }
            update_printer_driver() { echo update-printer >>"$dispatch_log"; }
            airprint_menu() { echo airprint >>"$dispatch_log"; }
            interactive_menu <<<"$input" >"$test_root/menu-$mode.out" 2>"$test_root/menu-$mode.err"
            grep -Fq -- "$expected" "$dispatch_log"
        ) || fail "Interactive menu did not dispatch mode $mode to '$expected'"
    done <<'EOF'
1|privileged --install|
2|privileged --uninstall --yes|
3|tests|
4|system-status|
5|printer-status <Fakülte>|Fakülte
6|add-printer|
7|help|
8|update-printer|
9|airprint|
EOF
    (
        sleep() { return 0; }
        interactive_menu <<'EOF'
invalid
0
EOF
    ) >"$test_root/menu-invalid.out" 2>"$test_root/menu-invalid.err" || fail "Interactive menu invalid-choice recovery failed"
    assert_contains "$test_root/menu-invalid.out" "Geçersiz seçim"
    assert_contains "$test_root/menu-invalid.out" "Çıkış yapılıyor"
    (
        sleep() { return 0; }
        execute_privileged_action() { echo privileged >>"$test_root/menu-10.log"; }
        airprint_menu() { echo airprint >>"$test_root/menu-10.log"; }
        interactive_menu <<'EOF'
10
0
EOF
    ) >"$test_root/menu-10.out" 2>&1 || fail "Interactive menu did not recover from choice 10"
    assert_contains "$test_root/menu-10.out" "Geçersiz seçim"
    [ ! -e "$test_root/menu-10.log" ] || fail "Removed menu choice 10 still dispatched"
    echo "    [PASS] Modes 1-9: Exact Interactive Dispatch + Invalid-Choice Recovery"
}

test_cli_dispatch_matrix() {
    (
        dispatch_log="$test_root/cli.log"
        : >"$dispatch_log"
        interactive_menu() { echo interactive >>"$dispatch_log"; }
        print_cli_help() { echo help >>"$dispatch_log"; }
        execute_privileged_action() { printf 'privileged %s\n' "$*" >>"$dispatch_log"; }
        test_p1606dn_ppd() { echo test-ppd >>"$dispatch_log"; }
        test_status_arguments() { printf 'test-status <%s>\n' "${1:-}" >>"$dispatch_log"; }
        manager_self_test() { echo self-test >>"$dispatch_log"; }
        run_driver_tests() { echo test >>"$dispatch_log"; }
        query_printer_status() { printf 'status <%s>\n' "${1:-}" >>"$dispatch_log"; }
        configure_printer_queue() { echo add-printer >>"$dispatch_log"; }
        update_printer_driver() { printf 'update-printer <%s>\n' "${1:-}" >>"$dispatch_log"; }
        build_pappl_target() { echo build-pappl >>"$dispatch_log"; }
        main
        main --interactive
        main --help
        main -h
        main --install
        main --build-pappl
        main --uninstall
        main --uninstall --yes
        main --test-ppds
        main --test-status
        main --test-status synthetic-status
        main --self-test
        main --test
        main --status
        main --status Fakülte
        main --add-printer
        main --update-printer
        main --update-printer Fakülte
        main --enable-airprint
        main --disable-airprint
        for expected in interactive help 'privileged --install' build-pappl 'privileged --uninstall' \
            'privileged --uninstall --yes' test-ppd 'test-status <./build/p1600dn-status>' \
            'test-status <synthetic-status>' self-test test 'status <>' 'status <Fakülte>' add-printer \
            'update-printer <>' 'update-printer <Fakülte>' 'privileged --enable-airprint' \
            'privileged --disable-airprint'; do
            grep -Fq -- "$expected" "$dispatch_log" || exit 1
        done
        if grep -Fq -- 'gutenprint' "$dispatch_log"; then exit 1; fi
        set +e
        main --invalid >/dev/null 2>&1; invalid_status=$?
        main --build-pappl one two >/dev/null 2>&1; pappl_arity=$?
        main --status one two >/dev/null 2>&1; status_arity=$?
        main --update-printer one two >/dev/null 2>&1; update_arity=$?
        main --build-openprinting >/dev/null 2>&1; removed_openprinting=$?
        main --uninstall-gutenprint >/dev/null 2>&1; removed_gutenprint=$?
        main --test-status one two >/dev/null 2>&1; test_status_arity=$?
        main --enable-airprint now >/dev/null 2>&1; airprint_arity=$?
        set -e
        [ "$airprint_arity" -eq 2 ] || exit 1
        [ "$invalid_status" -eq 2 ] || exit 1
        [ "$pappl_arity" -eq 2 ] || exit 1
        [ "$status_arity" -eq 2 ] || exit 1
        [ "$update_arity" -eq 2 ] || exit 1
        [ "$removed_openprinting" -eq 2 ] || exit 1
        [ "$removed_gutenprint" -eq 2 ] || exit 1
        [ "$test_status_arity" -eq 2 ] || exit 1
    ) || fail "CLI action/arity dispatch matrix failed"
    echo "    [PASS] Every CLI Action, Alias, Optional Argument, and Arity Boundary"
}

test_wrapper_scripts() {
    cmp -s "$module_dir/p1600dn_manager.command" "$module_dir/p1600dn_manager.sh" || fail "p1600dn_manager.sh is not an identical copy of p1600dn_manager.command"
    [ -x "$module_dir/p1600dn_manager.sh" ] || fail "p1600dn_manager.sh is not executable"
    "$module_dir/p1600dn_manager.sh" --help >/dev/null || fail "p1600dn_manager.sh --help failed"

    local wrapper_root="$test_root/wrappers"
    local wrapper_log="$test_root/wrappers.log"
    mkdir -p "$wrapper_root/test"
    cp "$module_dir/test/test_ppds.sh" "$module_dir/test/test_status.sh" "$wrapper_root/test/"
    printf '%s\n' '#!/bin/bash' '{ printf "CALL"; for arg in "$@"; do printf " <%s>" "$arg"; done; printf "\\n"; } >>"$P1600DN_WRAPPER_LOG"' 'if [ "${P1600DN_WRAPPER_FAIL_ACTION:-}" = "${1:-}" ]; then exit 37; fi' >"$wrapper_root/p1600dn_manager.command"
    chmod 755 "$wrapper_root/p1600dn_manager.command" "$wrapper_root/test"/*.sh
    : >"$wrapper_log"
    export P1600DN_WRAPPER_LOG="$wrapper_log"
    "$wrapper_root/test/test_ppds.sh" ppd-extra
    "$wrapper_root/test/test_status.sh" status-binary
    assert_contains "$wrapper_log" "CALL <--test-ppds> <ppd-extra>"
    assert_contains "$wrapper_log" "CALL <--test-status> <status-binary>"
    [ "$(wc -l <"$wrapper_log" | tr -d ' ')" -eq 2 ] || fail "A wrapper dispatched an unexpected number of manager calls"
    while IFS='|' read -r wrapper action; do
        export P1600DN_WRAPPER_FAIL_ACTION="$action"
        set +e
        "$wrapper_root/$wrapper" "$action" >/dev/null 2>&1
        wrapper_status=$?
        set -e
        [ "$wrapper_status" -eq 37 ] || fail "$wrapper did not propagate manager exit status 37"
    done <<'EOF'
test/test_ppds.sh|--test-ppds
test/test_status.sh|--test-status
EOF
    unset P1600DN_WRAPPER_FAIL_ACTION
    unset P1600DN_WRAPPER_LOG
    echo "    [PASS] All Compatibility .sh Wrappers + Exact Arguments/Exit Status"
}

echo "[*] Starting isolated manager mode and wrapper tests..."
test_install_mode
test_airprint_mode
test_install_failure_matrix
test_uninstall_mode
test_uninstall_cancel_and_root_failure
test_gutenprint_purge_mode
test_run_tests_mode
test_run_tests_failure_mode
test_system_status_mode
test_system_status_missing_mode
test_printer_status_mode
test_printer_status_build_failure
test_add_queue_mode
test_add_queue_rejection_matrix
test_help_mode
test_help_submenu_dispatch_matrix
test_update_queue_mode
test_update_queue_rejection_matrix
test_openprinting_retirement_mode
test_legacy_migration_mode
test_exit_mode
test_interactive_dispatch_matrix
test_cli_dispatch_matrix
test_wrapper_scripts
echo "[+] All manager modes and compatibility wrappers passed."
