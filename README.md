# HP LaserJet Professional P1600dn Series — macOS Native Apple Silicon Driver

[![macOS](https://img.shields.io/badge/macOS-Apple%20Silicon%20(ARM64)-black.svg?logo=apple)](https://apple.com)
[![Architecture](https://img.shields.io/badge/Architecture-Native%20ARM64-blue.svg)]()
[![License](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](LICENSE)

Native Apple Silicon (`arm64`) CUPS raster driver exclusively for the **HP LaserJet Professional P1600dn Series** (P1606dn, P1607dn, P1608dn, P1609dn). It writes the printer's PJL/ZjStream print job — halftoning, PJL framing, ZjStream page items, JBIG chunking and duplex page order — in modern native ARM64 C, without Rosetta, Homebrew, or third-party runtime libraries. The job format was learned by static analysis of the printer maker's macOS driver, without running it, and is documented in `reference/zjstream/ZJS_STREAM_SPEC.md`.

License: GPL-3.0-or-later. See `LICENSE` and `NOTICE` for details.

> HP and LaserJet are trademarks of HP Inc. This project is an independent driver for these printers; it is not affiliated with, endorsed by or sponsored by HP Inc. Product names are used only to identify the printers it supports.

---

## Supported Models

| Printer Model | PPD | Official IEEE-1284 Device ID |
|---|---|---|
| HP LaserJet Professional P1606dn | `ppd/P1600dn-Series.ppd` | `MFG:Hewlett-Packard;MDL:HP LaserJet Professional P1606dn;...` |
| HP LaserJet Professional P1607dn | `ppd/P1600dn-Series.ppd` | `MFG:Hewlett-Packard;MDL:HP LaserJet Professional P1607dn;...` |
| HP LaserJet Professional P1608dn | `ppd/P1600dn-Series.ppd` | `MFG:Hewlett-Packard;MDL:HP LaserJet Professional P1608dn;...` |
| HP LaserJet Professional P1609dn | `ppd/P1600dn-Series.ppd` | `MFG:Hewlett-Packard;MDL:HP LaserJet Professional P1609dn;...` |

All models are automatically discovered via macOS Bonjour or USB plug-and-play using the IEEE-1284 Device ID directives declared in the PPD.

---

## Features

- **Pure Native ARM64**: Pure C11 implementation with no runtime dependencies beyond macOS `libcups` (plus the IOKit and CoreFoundation system frameworks for USB toner level queries). Built for every Apple Silicon Mac (macOS 11 Big Sur and later), tuned for the M1 baseline, hardened, and signed with the hardened runtime.
- **Uses Apple Silicon Properly**: Pages are JBIG-encoded in parallel on the performance cores and sent the moment they are ready (automatic-duplex sheets too), screening runs on NEON, and waiting for the printer is event driven — the job bytes are the same as with one thread and the scalar screen, about four times faster on multi-page jobs.
- **No Page Leaves Unchecked**: Every page is encoded into memory and proven before it is sent. Its bottom padding and right edge are forced white. Its geometry and framing are checked against what the printer expects, and its JBIG data is decoded and compared pixel by pixel. A page that is not exactly right is never sent. The bottom-of-page stripes came from exactly this kind of mismatch. The check always runs and cannot be turned off.
- **ZjStream Job Stream**: the PJL header and trailer, START_DOC/START_PAGE items, 12-line page geometry, one JBIG image per page in 64 KiB chunks, and JBIG coder output with the trailing stripe zeros kept, from the embedded ITU-T T.82 JBIG-KIT encoder.
- **Halftoning**: 600 × 600 dpi 8-bit gray from macOS is screened in FastRes 1200 (four-level 12 × 12) and 600 dpi (16 × 16), the job format's screen geometry, with the project's own screens: round dots that grow into crosses and meet in a checkerboard at 600 dpi; elliptical dots with soft edges of partial toner levels (about 18 partial pixels per 12 × 12 tile), chains in the midtones and round holes in a solid net in the dark tones in FastRes 1200. `tools/gen_screens.py` generates them with a tone curve of its own (a Bernstein polynomial with a few constants, chosen during development so that grays print at their target darkness, within 2% coverage).
- **Bilingual Scripts & Interface**: Interactive manager (`p1600dn_manager.command`), CLI, and PPD options provide full English and Turkish support.
- **Print Quality Modes**: FastRes 1200 (default) and 600 dpi, with the printer's 12 pt hardware printable margins.
- **Two-Sided Printing**: Automatic duplex (pages sent 2, 1, 4, 3, … with a blank partner for an odd last page) on Letter, Legal, A4 and 8.5 × 13 plain media; manual duplex with the printer's pause for other paper or when the duplex unit is marked absent.
- **Flexible Connectivity**: Supports direct USB connection as well as local network queues via the printer's built-in Ethernet port.
- **Comprehensive Media Support**: 18 paper sizes (A4, Letter, Legal, Executive, 8.5 × 13, A5, A6, JIS B5, three 16K sizes, postcards, five envelopes), custom sizes, and 16 media types. Every size gets a real paper code, including B5, the 16K sizes and custom sizes (0 is never sent).
- **Hardware Controls**: EconoMode, 1–5 print density, jam recovery, media source (Automatically Select by default, or Manual Feed) and type, printer-side copies, and 180° reverse-corner rotation.
- **Live Printer Status**: During a job the filter reads the printer's PJL USTATUS replies from the CUPS backchannel and reports the printer's states (out of paper, manual feed, manual duplex "turn the stack and press Go", jams, cover open, toner missing, engine errors) with the PPD's Turkish or English texts, plus page progress lines. The job stays open until the printer reports the last sheet out.
- **Printer Utility**: `P1600dn Series Utility.app` (SwiftUI, arm64) is the printer utility for this printer family: Sleep after and Turn off after (Energy Saving), Quiet Mode, Configuration/Demo/Supplies Status pages, the Cleaning Page, and the Printer Web Page (the printer's web pages, over USB through a local proxy). It sends the printer's PJL settings and page commands and is in 16 languages, all the project's own translations; information pages print in English in Romanian, which the printer does not have. Its window follows current macOS settings windows: the printer's state and toner level at the top, grouped sections, a Print button for each page, and settings that apply as soon as they change. It opens from **Printers & Scanners → Options & Supplies → Utility** and also configures network printers. `p1600dn-utility` does the same from Terminal.
- **16 Languages**: Every option, choice and status message in the PPD, the utility and the notifier in English, Danish, German, Spanish, Finnish, French, Italian, Korean, Dutch, Norwegian, Swedish, Portuguese, Simplified and Traditional Chinese, Turkish and Romanian, all in the project's own translations.
- **AirPrint (iPhone, iPad) and IPP Everywhere**: an optional AirPrint server makes every P1600dn queue printable from iPhone, iPad and IPP Everywhere clients through this Mac. It passes the official IPP Everywhere test suite (`ipptool ipp-everywhere.test`, 40 passed, 0 failed). Every job goes to the CUPS queue, so through the driver and its page check.
- **Printer Notifications**: a background agent (`P1600dn Notifier.app`) shows macOS notifications when the printer needs you:
  - out of paper, paper jam, cover open;
  - manual feed, and manual duplex ("turn the stack and press Go");
  - engine errors and an unreachable printer;
  - toner low (15 %), very low (5 %), empty, and a new cartridge.
  
  Where the printer reports a state, the text is the PPD's message for it. Each notice is shown once, not repeated after a restart. A click opens the printer utility for that printer.
- **Toner Level**: `commandtop1600dn` answers CUPS `ReportLevels` requests: USB printers are asked for the printer's LEDM consumables document over their vendor USB interface, network printers through Apple's `snmpInk`.
- **Status & Supply Monitoring**: Real-time CUPS/IPP printer state and device supply level querying (`--en` / `--tr`).
- **Gutenprint Removal**: Gutenprint has no P1600dn driver, and this driver replaces it. The first installation asks before it removes it (`--install --yes` does not ask), and every uninstallation removes it; P1600dn queues still on it are switched to this driver, and Gutenprint stays only while another printer prints through it.

---

## Apple Silicon

| | |
|---|---|
| Target | `arm64`, macOS 11.0+ (`-mmacosx-version-min=11.0`), `-mcpu=apple-m1` |
| Hardening | `-fstack-protector-strong`, `_FORTIFY_SOURCE=2`, ad-hoc signature with the hardened runtime (the form notarization needs) |
| Encoding | One JBIG encoder per performance core (at most four); pages leave in order as soon as they are encoded, while the next page is still being read |
| Screening | Own FastRes and 600 dpi screens on NEON (48 / 16 pixels per step), bit-identical to the scalar routine |
| Waiting | The status reader blocks in `poll()`, the job end on a condition variable (no polling timers), the reader thread at utility QoS |

`make bench` measures this Mac. On an M1 (8-page A4 jobs, ms per page):

| Content | FastRes before | FastRes now | 600 dpi before | 600 dpi now |
|---|---|---|---|---|
| Text | 51.5 | 14.2 | 39.5 | 12.5 |
| Photo (worst case) | 352.8 | 88.5 | 172.6 | 43.1 |
| Solid black | 37.9 | 11.1 | 30.0 | 10.2 |

The printer prints 25 pages per minute (2.4 s per page), so even a photo page is ready long before the printer needs it. Peak memory grows from about 14 MiB to at most about 77 MiB (four pages in flight). `P1600DN_ENCODER_THREADS=0` encodes on a single thread.

---

## Quick Start

```bash
make test
make test-all  # Sanitizers, static analysis, and code coverage checks
./p1600dn_manager.command --install
./p1600dn_manager.command --add-printer
```

---

## Build & Test

```bash
make clean
make test
make test-all
```

`make test` is the rigorous verification suite used during installation. `make test-all` extends it with AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), Clang static analysis, LLVM source code coverage reports, and Guard Malloc memory stress testing.

Test coverage highlights:
- Exact byte layout of the PJL header/trailer, START_DOC, START_PAGE items, BIH, 64 KiB BID chunks and END chunks;
- Screening checked against a reference written from the screening formulas of the job specification;
- Automatic and manual duplex page order, blank partner pages, the manual-duplex pause, copies and collation;
- All paper, media, source, density, jam recovery and copy count boundaries;
- Exact JBIG decode round-trip verification;
- The page guard (`make test-guard`): 770 page geometries pass, and every stripe-era defect is refused (JBIG height padded to 128 lines, VIDEO_Y 6817, odd RASTER_X, changed items, damaged data, BID framing);
- Strict PPD syntax checks (`cupstestppd -W all`);
- Corrupt raster rejection and subprocess fuzzing (256 deterministic cases);
- The printer status code table, USTATUS framing and the backchannel reader, including a filter run against a simulated printer (USTATUSOFF only after JOB END, unchanged job bytes);
- Toner levels: usb:// URI and device matching, HTTP/LEDM parsing, snmpInk plists, and `ReportLevels` command jobs against a stand-in `snmpInk`;
- Parallel encoding: identical job bytes for 0–4 encoder threads, page order, pages leaving before the next raster page arrives, NEON screening identical to the scalar screen for every phase and darkness value, ThreadSanitizer runs, and a wake-up count while waiting for the printer;
- Model restrictions and manager security matrices.

The real macOS `cgtexttopdf` -> `cgpdftoraster` pipeline is executed across simplex, reverse-corner, EconoMode, and duplex modes, decoding every emitted JBIG page to verify exact pixel fidelity against the source raster. Every PPD paper size is also printed black to the last line at FastRes 1200 and 600 dpi, which is the worst case for the bottom of the page.

---

## Installation

### Homebrew

```bash
brew tap berkbb/tap
brew install --cask berkbb/tap/p1600dn
```

Homebrew 7 treats taps outside Homebrew as untrusted: `brew install` takes the full name `berkbb/tap/p1600dn` once the tap is added. To find the cask with `brew search p1600dn` and use the short name `p1600dn`, trust the tap once with `brew trust berkbb/tap`.

The cask lives in the tap repository [berkbb/homebrew-tap](https://github.com/berkbb/homebrew-tap) (`Casks/p1600dn.rb`; its source is `packaging/homebrew/` here). It downloads this repository's latest release, checks its SHA-256, and runs `p1600dn_manager.command --install`, so the driver is built and tested on your Mac first. `brew uninstall --cask berkbb/tap/p1600dn` runs `--uninstall --yes`.

### Manager

The recommended method without Homebrew is double-clicking `p1600dn_manager.command` in Finder and selecting **Option 1**.

Command-line alternative:
```bash
./p1600dn_manager.command --install
```

Installation steps performed automatically:
1. Performs a clean build and executes all automated unit, protocol, and filter tests.
2. Installs `rastertop1600dn`, `commandtop1600dn`, `p1600dn-utility`, `p1600dn-status`, `P1600dn Series Utility.app`, and printer icons into `/Library/Printers/p1600dn/`.
3. Installs `P1600dn-Series.ppd` into `/Library/Printers/PPDs/Contents/Resources/`.
4. Reloads the system CUPS daemon (`cupsd`).

The manager automatically checks for Xcode Command Line Tools, launching `xcode-select --install` if necessary. It requests administrator (`sudo`) authorization only when needed.

### Installing on Another Mac

Get the sources on the other Mac with `git clone` or `git pull`, or copy the folder without `build/`, then run `--install` there. The driver and both apps are always built on the Mac they run on:

```bash
cd p1600dn-macos-arm64-driver && rm -rf build && ./p1600dn_manager.command --install
```

Do not copy `build/` or the installed apps from one Mac to another. The apps are signed ad hoc on the Mac that built them, and AirDrop, USB drives, cloud folders or zip archives can drop files from the bundle or add `._` files to it. That breaks the signature, so the copied app shows the following:

- **Double-click does nothing.** Gatekeeper rejects the app. It still starts from Terminal (`.../Contents/MacOS/P1600dnUtility`), which hides the problem.
- **No-entry icon.** The app icon shows a no-entry sign instead of the printer.
- **Untranslated keys.** The window shows keys such as `AppTitle`, `DiagnosticPage`, `SleepAfter` or `PrinterWebPage` instead of text.

The printer, its status and its toner level can still read correctly in that window, so the driver itself is fine. The fix is the clean install above, and then opening the app from `/Library/Printers/p1600dn/`, not from `build/`. To check a bundle:

```bash
codesign --verify --deep --strict -v "/Library/Printers/p1600dn/P1600dn Series Utility.app"
```

The driver and the apps are `arm64` only and do not run on Intel Macs (`uname -m` must print `arm64`). An existing queue keeps the PPD it was created with until it is switched with menu **[8]** or `--update-printer`.

---

## Adding a Printer Queue

After connecting the printer via USB or local Ethernet, choose **Option 6** from the manager menu:

```bash
./p1600dn_manager.command --add-printer
```

The tool prompts for the queue name and device URI, configuring the P1600dn PPD automatically. To discover available device URIs:

```bash
lpinfo -v
```

### USB Example:
```bash
sudo lpadmin -p HP_LaserJet_P1606dn -E \
  -v 'usb://HP/LaserJet%20Professional%20P1606dn' \
  -P /Library/Printers/PPDs/Contents/Resources/P1600dn-Series.ppd \
  -D 'HP LaserJet Pro P1606dn'
```

### Ethernet Example:
Typically `socket://PRINTER_IP:9100` or the printer's advertised `dnssd://...` URI.

In macOS GUI: Go to **System Settings > Printers & Scanners > Add Printer**, click "Use: Other...", and select the installed PPD file.

---

## Updating an Existing Queue's Driver

You can update an existing CUPS queue without deleting it, preserving its name and URI:

```bash
./p1600dn_manager.command --update-printer 'Faculty_Printer'
```

If you omit the queue argument, the tool interactively lists available queues. It updates the PPD while keeping your existing connection URI intact.

---

## Printer & Supply Status Query

```bash
/Library/Printers/p1600dn/p1600dn-status HP_LaserJet_P1606dn
# Or for English output explicitly:
/Library/Printers/p1600dn/p1600dn-status --en HP_LaserJet_P1606dn
```

If the device connection does not report supply levels, the utility avoids guessing and cleanly reports "unknown".

The toner level comes from the printer itself: when the notifier (after printing) or `lpadmin` sends the queue a `ReportLevels` command, `commandtop1600dn` asks the printer and stores the result as the queue's `marker-levels`, which the status utility then shows. USB printers answer only if they expose the LEDM interface (USB class `FF/02/10`). The PPD's `*cupsCommands` does not list `ReportLevels`: macOS would otherwise queue a toner query job again and again while **Options & Supplies** or the utility is open.

---

## Printer Utility

macOS opens **P1600dn Series Utility** from **System Settings → Printers & Scanners → (printer) → Options & Supplies → Utility → Open Printer Utility**, or open it from `/Library/Printers/p1600dn/`.

The window is laid out like System Settings on macOS 13 and later, and like the same grouped boxes on macOS 11 and 12:

- At the top: the printer's name, its connection (USB or network), its state (ready, printing, stopped) and its toner level as CUPS last received it from the driver. With several P1600dn printers, a menu picks one.
- Each information page has its own **Print** button.
- Settings take effect as soon as they are changed, and only a changed value is sent. While the printer has not answered, the settings stay disabled and **Try Again** reads them again.
- Without a P1600dn printer, the window explains how to add one and opens Printers & Scanners.
- If the app does not open, shows a no-entry icon, or shows keys such as `AppTitle` instead of text, it was copied from another Mac; see [Installing on Another Mac](#installing-on-another-mac).

It offers these controls:

| Control | What it does |
|---|---|
| Print Information Pages | Configuration, Demo or Supplies Status page (in the window's language), or the Cleaning Page (it asks first: load transparency or 70–90 g/m² paper in Tray 1) |
| Print Information Pages: Diagnostic Page | The driver's own two-sided check sheet, printed through the queue (see below) |
| Energy Saving: Sleep after | 1, 5, 15, 30 minutes, 1 or 2 hours |
| Energy Saving: Turn off after | Never, 30 minutes, 1, 2, 4, 8 or 24 hours |
| Quiet Mode | On or off |
| Printer Web Page | The printer's web pages (network: its address; USB: a local proxy for 3 idle minutes) |

The same from Terminal:

```bash
/Library/Printers/p1600dn/p1600dn-utility --en status
/Library/Printers/p1600dn/p1600dn-utility --queue HP_LaserJet_P1606dn sleep 15
/Library/Printers/p1600dn/p1600dn-utility quiet on
/Library/Printers/p1600dn/p1600dn-utility page configuration
/Library/Printers/p1600dn/p1600dn-utility page diagnostic
/Library/Printers/p1600dn/p1600dn-utility --media Letter --sides short page diagnostic en
/Library/Printers/p1600dn/p1600dn-utility --output ~/Desktop/diagnostic.pdf page diagnostic
```

**Diagnostic Page.** One sheet, front and back, for checking the driver on the real printer:
- **Bottom edge test:** a black bar is the last thing printed on each side. Below it there must be nothing down to the edge of the paper; the stripes this printer once printed would show here.
- **Print quality:** gray levels from 0 to 100 %, lines 1, 2, 4 and 8 printer dots wide, a fine-line star and text from 6 to 14 pt.
- **Edges and orientation:** corner marks on the printable area (12 pt from each edge), and arrows on both sides to check the 2-sided orientation (long or short edge).
- **Details and checklist:** the printer, queue, connection, paper, resolution, driver version and date, plus a checklist.

The page is in Turkish or English and uses the queue's default paper, 2-sided long-edge unless `--sides` says otherwise. It is printed as a normal job through the queue, so it takes the same path as every document, including the page check.

USB printers can be reached while no job is printing (the utility waits up to 10 seconds). The Configuration and Cleaning pages are also available as CUPS commands (`PrintSelfTestPage`, `Clean`) that queue behind print jobs.

---

## AirPrint (iPhone, iPad)

macOS Printer Sharing alone does not make this printer usable from iPhone and iPad. macOS cannot convert Apple's raster format (`image/urf`, what iOS sends) for driver-based queues, and CUPS does not support AirPrint for shared queues. The driver therefore brings its own AirPrint server, `p1600dn-airprint`. It is off until you turn it on:

```bash
./p1600dn_manager.command --enable-airprint     # or menu 9
./p1600dn_manager.command --disable-airprint
```

Once enabled, iPhone and iPad list the printer as **"<printer> @ <Mac name>"** under **Share → Print**. Android (Mopria) and other IPP Everywhere clients find it too.

- **What it is:** an IPP/2.0 printer per P1600dn queue, advertised over Bonjour (`_ipp._tcp`, `_universal`) on TCP port 8632.
- **Formats:** Apple raster (URF) and PWG raster are converted to PDF; PDF, JPEG and PNG are passed on unchanged.
- **Job options:** paper size and type, tray, 2-sided, copies, quality (600 dpi or FastRes 1200), page ranges and scaling become the queue's options.
- **Every page is checked:** jobs go to the CUPS queue, so every page passes the driver's page check before it reaches the printer. The server never talks to the printer itself.
- **How it runs:** a system service (`/Library/LaunchDaemons/com.berkbb.p1600dn.airprint.plist`) as the unprivileged user `nobody`, also when nobody is logged in. It waits for the first P1600dn queue and picks up added or removed queues within a minute. The log is `/Library/Logs/p1600dn-airprint.log`.
- **Limits:**
  - one document per job;
  - no TLS or passwords: anyone on the local network can print, as with an AirPrint printer;
  - if macOS Printer Sharing is also on for the queue, the printer appears twice (the manager warns);
  - with the macOS firewall on, allow incoming connections for `p1600dn-airprint`.

---

## Printer Notifications

The installer adds **P1600dn Notifier** (`/Library/Printers/p1600dn/P1600dn Notifier.app`) as a LaunchAgent (`/Library/LaunchAgents/com.berkbb.p1600dn.notifier.plist`) and starts it in the logged-in user's session. Other users get it when they next log in. macOS asks once for permission to show notifications.

It follows every P1600dn queue through CUPS and notifies:

| Situation | Notification |
|---|---|
| Out of paper, paper jam, cover or door open | The driver's message from the PPD in your language (e.g. *Out of Paper: Refill the paper tray, then press Go on the printer.*) |
| Manual feed, manual duplex | The driver's message (*Manual Duplex: For the second sides, move the printed sheets from the output bin to Tray 1, printed side down, then press Go on the printer.*) |
| Engine error | At once, with the driver's message |
| Printer unreachable | After 15 seconds, so the moment a job connects does not count |
| Toner 15 % or less, 5 % or less, empty | Once per level; the toner level is refreshed through the driver after printing (at most every 30 minutes) |
| New cartridge | When the level rises from 15 % or less to over 50 % |

A notice is shown once per occurrence, also across restarts and logins. Clicking it opens P1600dn Series Utility for that printer. The agent uses CUPS subscriptions: it checks every minute while the printer is idle and every 5 seconds while it prints.

---

## OpenPrinting

Earlier versions could also install the OpenPrinting `foo2zjs` encoder as a fallback. It encoded pages itself, so it skipped the page check that every page must pass. It has been removed.

Installing or uninstalling the driver cleans up an old fallback installation. Queues that still use its PPD are switched to the native driver; their names and connections stay the same.

The OpenPrinting PAPPL Printer Application adapter (`src/pappl_p1600dn.c`, `make pappl`, `./p1600dn_manager.command --build-pappl`) is an unfinished draft that cannot print: it registers no raster callbacks and, without libpappl, builds as a stub. It is kept for a future Linux Printer Application; when it is completed, its pages must go through the page check (`zjs_encode_page_checked()`) like every other path. `DISTRIBUTION_GUIDE.md` describes it.

A driver entry for the OpenPrinting database (foomatic-db) is drafted in `packaging/openprinting/`; the maintainers were asked in [foomatic-db#62](https://github.com/OpenPrinting/foomatic-db/issues/62) whether they want it.

---

## Uninstallation & System Cleanup

```bash
./p1600dn_manager.command --uninstall
# Or: ./p1600dn_manager.sh --uninstall
```
Removes the native driver binaries, the utility, the notifier and AirPrint, the PPD file, an old OpenPrinting fallback, legacy residue, and Gutenprint.

### Gutenprint

Gutenprint has no P1600dn driver. At best a network queue printed PCL through another model (HP LaserJet 1160), without the printer's duplex, paper codes, status or the page check. This driver replaces it, so it is removed:

- on the first installation, once you agree (`--install --yes` agrees; a reinstall or update leaves it alone);
- on every uninstallation.

Its package receipts are forgotten and its driver, filters, PPDs and tools are deleted. P1600dn queues that still use it are switched to this driver first, with their names and connections kept; a queue counts as a P1600dn queue when its name or device URI names a P1600dn model. While another printer prints through Gutenprint, Gutenprint stays and the manager names that queue.

---

## Project Structure

```text
p1600dn-macos-arm64-driver/
├── assets/                               Printer icon for the PPD and AirPrint (.icns, .png), app icon (.icns)
├── build/                                Output build artifacts (object files, binaries)
├── packaging/                            Homebrew cask (homebrew/) and OpenPrinting database drafts (openprinting/)
├── ppd/
│   └── P1600dn-Series.ppd       Bilingual P1600dn Series PPD
├── reference/zjstream/                   ZjStream job specification of the P1600dn series
├── src/
│   ├── commandtop1600dn.c              CUPS command filter: ReportLevels toner level (LEDM over USB, snmpInk)
│   ├── p1600_airprint.c / .h                AirPrint / IPP Everywhere server: IPP, Bonjour, CUPS submission
│   ├── p1600_diagnostic.c / .h              Diagnostic page (CoreGraphics PDF, printed through the queue)
│   ├── p1600_encoder_pool.c / .h            Parallel JBIG page encoding on the performance cores
│   ├── p1600_notify.c / .h                  Notifier core: CUPS subscriptions, status and toner notices
│   ├── p1600_raster_doc.c / .h              Apple raster (URF) and PWG raster to PDF for AirPrint jobs
│   ├── p1600dn_airprint.c              AirPrint server daemon (follows the queue list)
│   ├── p1600_utility.c / .h                 Printer utility: PJL settings, pages, INQUIRE, printer link
│   ├── p1600_ews_proxy.c / .h               Printer web pages over USB on the loopback address
│   ├── p1600_usb.c / p1600_net.c / p1600_queues.c USB interfaces (IOKit), AppSocket and Bonjour, P1600dn CUPS queues
│   ├── p1600dn_utility.c               p1600dn-utility command line tool
│   ├── p1600_status.c / p1600_status.h         PJL USTATUS backchannel reader, printer status codes and progress lines
│   ├── p1600_supplies.c / p1600_supplies.h     usb:// URIs, HTTP/LEDM and snmpInk parsing, marker attributes
│   ├── p1600_usb_ledm.c                     IOKit LEDM transport over the printer's vendor USB interface
│   ├── p1600dn_status.c                Bilingual CUPS/IPP printer status utility
│   ├── jbig.c / jbig.h                   Embedded ITU-T T.82 JBIG encoder
│   ├── jbig_ar.c / jbig_ar.h             Arithmetic coder
│   ├── p1600_raster.c / p1600_raster.h         CUPS raster validation, paper codes and page rendering
│   ├── p1600_screen.c / p1600_screen.h         Halftone screening, NEON and scalar (own tables from tools/gen_screens.py)
│   ├── pappl_p1600dn.c                 OpenPrinting PAPPL Printer Application adapter (unfinished draft)
│   ├── rastertop1600dn.c               Native CUPS raster filter entry point
│   ├── zjs.h                             ZjStream wire structures and protocol definitions
│   ├── zjs_encoder.c / zjs_encoder.h     PJL/ZjStream stream serializer
│   └── zjs_guard.c / zjs_guard.h         Page guard: every page proven before it is sent
├── test/                                 Comprehensive unit, protocol, filter, and pipeline test suite
├── tools/                                Screen table generator, icon drawing, utility and notifier app builds
├── airprint/                             AirPrint server LaunchDaemon (installed when AirPrint is turned on)
├── notifier/                             P1600dn Notifier agent (notifications), its LaunchAgent and 16 languages
├── utility/                              P1600dn Series Utility app (SwiftUI) and its Turkish, Romanian and own strings
├── CHANGELOG.md                          Project history and release notes
├── DEVELOPMENT.md                        Developer guide, test targets, and invariants
├── DISTRIBUTION_GUIDE.md                 Apple Notarization, Homebrew & OpenPrinting guide
├── GUTENPRINT_PORT_PLAN.md               Gutenprint compatibility and port analysis
├── p1600dn_manager.command           Bilingual interactive manager, installer, and queue tool (Finder)
├── p1600dn_manager.sh                Terminal shell script for manager, installer, and queue tool
├── LICENSE                               GPL-3.0-or-later license
├── Makefile                              Native ARM64 build and test harness
├── NOTICE                                Attribution and copyright notice
└── README.md                             User guide and documentation
```

---

## License

Copyright © 2026 berkbb. GPL-3.0-or-later. See `LICENSE` and `NOTICE` files for full licensing terms. The embedded JBIG-KIT encoder is licensed under GPL-2.0-or-later, which allows its use under GPL-3.0.

## Trademarks

HP and LaserJet are trademarks of HP Inc. This project is an independent driver for these printers; it is not affiliated with, endorsed by or sponsored by HP Inc. Product names are used only to identify the printers it supports.
