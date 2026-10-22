# HP LaserJet Professional P1600dn Series — Driver Development Guide

This project implements a native Apple Silicon (`arm64`) CUPS raster filter exclusively for the **HP LaserJet Professional P1600dn Series** (P1606dn, P1607dn, P1608dn, P1609dn). It writes the printer's PJL/ZjStream job — halftoning of 600 dpi gray with the project's own screens, ZjStream page items and paper/media codes, one ITU-T T.82 JBIG image per page, and the duplex page order — with zero external runtime dependencies on macOS. The job format is specified in `reference/zjstream/ZJS_STREAM_SPEC.md`.

---

## Prerequisites

- macOS 12 or later running natively on Apple Silicon (`arm64`).
- Xcode Command Line Tools (`clang`, `make`, `python3` for `make test-screens`).
- macOS SDK CUPS headers and system `libcups`.

---

## Build and Test Commands

```bash
make
make test
make test-all
make pappl
make status
make manager
make clean
```

The build enforces strict C11 compliance (`-Wall -Wextra -Werror -pedantic`), compiles with `-O3 -mcpu=apple-m1`, targets `-arch arm64` for macOS 11.0 and later (`MACOS_MIN`), hardens with `-fstack-protector-strong -D_FORTIFY_SOURCE=2`, dead-strips, signs every shipped binary ad hoc with the hardened runtime (`codesign -o runtime`), and dynamically links only against the system `libcups` (the toner level command filter also uses the IOKit and CoreFoundation system frameworks). The manager refuses to install a binary that is not arm64, not built for macOS 11.0, or not validly signed.

- `make test`: Performs a clean native ARM64 build and executes the comprehensive unit, protocol, JBIG pixel round-trip, CUPS filter framing, complete mode matrix, printer status reader, toner level command filter, strict PPD syntax, status utility, real macOS CUPS pipeline, and manager security test suites.
- `make test-all`: Extends `make test` with AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), Guard Malloc edge memory stress testing, Clang static analysis, and LLVM source code coverage reporting.
- `make pappl`: Compiles the standalone OpenPrinting PAPPL Printer Application binary (`build/p1600dn-app`).

### Individual Test Targets

```bash
make test-unit          # Screening vs. reference formulas, paper codes, raster geometry, rendering
make test-protocol      # Exact PJL/ZjStream bytes, BID blocking, JBIG round trips, rotation
make test-guard         # Page guard: 770 geometries pass, every stripe-era defect is refused
make test-filter        # CUPS raster CLI parsing, validation, and error propagation
make test-filter-modes  # Simplex/duplex/econo/density/paper mode matrix tests
make test-cups-pipeline # Real cgtexttopdf -> cgpdftoraster -> driver pipeline validation
make test-encoder-pool  # Parallel encoding: pool order/bytes, 0-4 threads give identical jobs, streaming
make test-transport     # USB interface lookup, AppSocket with timeouts, Bonjour names, device URIs
make test-utility       # Utility PJL bytes, languages, INQUIRE, stand-in printer, CLI, web page proxy
make test-diagnostic    # Diagnostic page PDFs (18 papers, 2 languages, 3 sides), CLI, real chain with bottom check
make test-notify        # Notifier decisions and IPP (subscriptions, ReportLevels) against a stand-in cupsd
make test-notifier-app  # Notifier bundle, LaunchAgent, live agent runs against the stand-in cupsd
make test-airprint      # AirPrint: unit tests, ipptool IPP Everywhere suite, URF/PWG/JPEG/PDF jobs through the driver
make test-utility-app   # App bundle: arm64, macOS 11, hardened runtime, 16 languages, launch with 'prdt'
make utility-app        # Build "P1600dn Series Utility.app" (tools/build_utility_app.sh)
make test-ustatus       # Printer status codes, USTATUS framing, backchannel reader, filter vs. simulated printer
make test-supplies      # Toner level: URIs, USB matching, HTTP/LEDM, snmpInk plists, ReportLevels jobs
make test-ppd           # Strict cupstestppd -W all and PPD option constraint checks
make test-status        # CUPS/IPP status utility output and localization tests
make test-manager       # Manager interactive menu, CLI actions, and wrapper tests
make test-sanitize      # AddressSanitizer + UndefinedBehaviorSanitizer, then ThreadSanitizer for the threads
make bench              # Pages per second on this Mac (timing only)
make test-stress        # Guard Malloc boundary protection and memory stress tests
make analyze            # Clang static code analysis
make coverage           # LLVM code coverage analysis and HTML report generation
```

---

## Automated Test Coverage

The automated test suite verifies:

1. **Halftoning**: FastRes 1200 and 600 dpi screening compared line by line with a reference written from the specification's screening formulas, plus screen-table invariants.
2. **ZjStream Serialization**: Exact PJL header/trailer bytes, 28-byte START_DOC, START_PAGE item order, BIH fields, 64 KiB BID chunks with pad-to-4 final chunk, empty END_JBIG/END_PAGE, PAUSE chunk.
3. **Page Geometry**: `VIDEO_Y = RASTER_Y = JBIG Yd` rounded up to 12 lines of white, `RASTER_X = VIDEO_BPP * VIDEO_X`, DMPAPER code by page dimensions.
4. **End-to-End CUPS Raster Conversion**: 600 dpi 8-bit gray (current PPD) and legacy 1200 × 600 1-bit (PPD 3.x queues) rasters rendered with zero pixel loss.
5. **Duplex**: Automatic order (2, 1, 4, 3, …, blank partner), manual order with PAUSE, DupUnit and media/paper rules, CUPS-rotated back sides.
6. **Input Validation & Error Handling**: Immediate rejection of truncated/corrupt rasters and unsupported raster formats.
7. **PPD Integrity**: Zero errors and zero warnings under strict `cupstestppd -W all`, the full option set with English base names and Turkish localizations.
8. **Status Utility**: Live IPP supply and printer state queries, supporting both `--en` and `--tr` output.
9. **Native Architecture**: Verification via `lipo` that all binaries are pure Apple Silicon (`arm64`).
10. **Real macOS CUPS Pipeline**: Full execution of `cgtexttopdf` -> `cgpdftoraster` -> `rastertop1600dn`, decoding each emitted JBIG page and asserting byte-for-byte identity with the rendered raster.
11. **Deterministic Fuzz Matrix**: 256 isolated corrupt-raster subprocess runs verifying zero crashes and clean error exits.
12. **Manager Conformance**: Isolated execution of menu modes (0–9, AirPrint on/off included), help options (1–5), CLI flags, the retirement of an old OpenPrinting fallback, the Gutenprint removal (first install only, every uninstall, P1600dn queues switched, kept while another printer uses it, removal failures reported), and compatibility wrappers. Checks inside `( … ) || fail` subshells end in `|| exit 1`, because bash ignores `set -e` there.
13. **Printer Status (readback thread)**: All 19 entries of the status code table with their STATE/INFO lines in Turkish and English, PPD text lookup by language, the 256-line progress rule, USTATUS framing (form feeds, NUL resets, NUL 0xAC blocks, oversized messages), reader thread end conditions (JOB END, CANCELED, backchannel EOF, quiet and idle limits), and the filter against a simulated printer on fd 3: `@PJL USTATUSOFF` only after `USTATUS JOB END`, job bytes identical with and without a backchannel.
14. **Toner Level (LEDM and snmpInk)**: usb:// URI parsing and USB device matching, HTTP responses (Content-Length, chunked, split reads, malformed input, limits), the LEDM consumables document, snmpInk plists with MaxCapacity scaling, and `commandtop1600dn` command jobs against `test/fake_snmpink.sh`. The USB transfer itself needs a printer and is not covered by the automated suite.
15. **Parallel Encoding & NEON**: The encoder pool returns pages in order with exactly `zjs_write_page()`'s bytes for every worker count and depth; the filter writes identical jobs with 0, 1, 2 and 4 encoder threads; pages and automatic-duplex sheets leave while the next raster page has not arrived; the NEON screen equals the scalar screen for every darkness value at every phase, row and line tail; the encoder pool, writer thread and status reader run clean under ThreadSanitizer; and waiting for the printer causes at most a handful of wake-ups (macOS `TASK_POWER_INFO`).
16. **Printer Utility**: the exact bytes and lengths for every Sleep/Auto Off value, Quiet Mode, the three information pages and the cleaning page; page languages, with the correct PJL language for English, Finnish, Korean and Traditional Chinese; INQUIRE replies (split, after other output, unknown, malformed); a stand-in printer on the loopback AppSocket port checks the session order and bytes for reading and writing; `p1600dn-utility` end to end; the web page proxy against a stand-in LEDM interface (chunked pages, form posts, 502, idle end); the app bundle (arm64, macOS 11, hardened runtime, the project's string table complete in all 16 languages with the same keys) and its launch by macOS with the queue in `'prdt'`. The window is drawn off screen with stand-in printers (`test/utility_render.swift`, ScreenCaptureKit, macOS 14.4+) in eight variants (grouped and macOS 11–12 layouts, light and dark, one or two printers, no answer, no printer) and each render must have the fixed width, a plausible height and real content; the PNGs stay in `build/utility-render`.
17. **PPD Languages**: 15 languages besides English (all the project's own translations) for all 102 localized entries, no line over 255 bytes, `cupstestppd -W all` clean.
18. **Page Guard**: 770 page geometries (every width residue in bytes and 128-bit JBIG lines, every height residue in 12-line VIDEO_Y groups and 128-line stripes, 600 dpi and FastRes, simplex and automatic duplex, full A4/Letter/Legal/A5 pages) pass with their padding forced white and decode to the page. Every stripe-era defect is refused with its reason: JBIG Yd padded to 128 lines, VIDEO_Y 6817, VIDEO_Y more than 11 lines past the content, odd RASTER_X, a changed VIDEO_Y item, a missing DMDUPLEX, JBIG parameters other than the job format's, damaged data, a one-pixel difference, a short BID before the last, BID padding that is unaligned, too long or non-zero, a cut page and trailing bytes. The encoder pool reports a refused page and keeps the pages after it. The real macOS pipeline prints all 18 PPD paper sizes black to the last line at both resolutions.
19. **Diagnostic Page**: 108 PDFs (every PPD paper, Turkish and English, 1-sided, long and short edge), each two pages of exactly the paper's size; paper lookup by PPD name, PWG name and rounded size; the CLI (`--output`, `--media`, `--sides`, errors); and six paper/sides cases through `cgpdftoraster`, the filter (page guard) and the pixel validator, where the rasterized front page must end in the black bar with nothing below it outside the corner marks.
20. **Printer Notifications**: `p1600_notify_compare()` for every reason of interest with every suffix, with and without the PPD's message; all toner transitions and bucket edges; priming; new cartridge; memory across restarts. The IPP calls run against a stand-in cupsd: Create-Printer-Subscriptions (ippget, 3 events), Get-Notifications with sequence numbers, repeats and cupsd's notify-get-interval, renewal, cancel, not-found, refused subscriptions, the exact 27-byte ReportLevels command as `application/vnd.cups-command`, 4 threads, and cupsd going down and coming back. The app bundle (arm64, macOS 11, hardened runtime, LSUIElement, 15 languages with the same keys, LaunchAgent) runs live against the stand-in: the PPD's out-of-paper and manual-duplex texts, toner 40 → 12 → 4 → 0, no duplicates, nothing repeated after a restart, a brief `connecting-to-device` stays quiet.
21. **AirPrint / IPP Everywhere**: unit tests (URF and PWG decoding including libcups' own Apple raster writer, the PDF writer, all 18 media against the PPD, IPP→CUPS options, TXT record, UUID, HTTP/IPP edge cases, busy and connection limits, real-mode submission against a stand-in cupsd); `ipptool -V 2.0 ipp-everywhere.test` (40 passed, 0 failed; the PWG sample prints skip because macOS lacks the files); URF made by `cgpdftoraster`, PWG raster (sGray, sRGB deflate), JPEG and PDF jobs whose 17 spooled documents all print through `cupsfilter` and the driver filter with no `ERROR:`; HTTP over IPv4/IPv6, SIGTERM, waiting without a queue, and local-only Bonjour browse/resolve with URF/pdl TXT keys. ASan/UBSan run the whole e2e, TSan the ipptool part.
22. **Robustness checks**: a canceled job (SIGTERM) closed at a page boundary; queue PPD defaults for density, jam recovery, EconoMode and TersKose; Bonjour-discovered and driverless queues left out and an unreachable cupsd reported as -1; AirPrint limits (8 connections per address, 10 s header, decoded-size and PDF limits, uploads cut off or stalled, uploads under way counted against the queue limit, Bonjour retried); the EWS proxy refusing other sites' requests, ending bodiless replies and dropping late replies; the notifier canceling an unfinished ReportLevels job; the utility header following CUPS (`test/utility_probe.swift`) and a running utility switching printers; manager installs under umask 077, binaries replaced instead of overwritten, every system path isolated, and each CLI action and alias dispatched alone. UBSan errors stop `make test-sanitize` (`-fno-sanitize-recover=undefined`).

---

## Architecture & Source Layout

```text
src/
├── commandtop1600dn.c          CUPS command filter for ReportLevels (LEDM over USB, snmpInk)
├── p1600_airprint.c / .h            AirPrint / IPP Everywhere server: IPP/2.0, Bonjour, jobs to the CUPS queue
├── p1600_diagnostic.c / .h          Diagnostic page: CoreGraphics PDF printed through the CUPS queue
├── p1600_encoder_pool.c / .h        Parallel JBIG page encoding (performance cores, in-order hand-back)
├── p1600_notify.c / .h              Notifier core: cupsd subscriptions, snapshots, ReportLevels, notice decisions
├── p1600_raster_doc.c / .h          URF and PWG raster decoding and the CoreGraphics PDF writer for AirPrint
├── p1600_utility.c / .h             Printer utility: PJL formats, INQUIRE parsing, printer link
├── p1600_ews_proxy.c / .h           Printer web pages: EWS over USB (FF/02/10) on a loopback port
├── p1600_usb.c / .h                 Shared IOKit interface access (LEDM, printer interface)
├── p1600_net.c / .h                 AppSocket (TCP 9100) and Bonjour (DNS-SD) resolution
├── p1600_queues.c / .h              P1600dn series CUPS queues and their device URIs
├── p1600dn_utility.c           p1600dn-utility command line tool
├── p1600_status.c / p1600_status.h     PJL USTATUS backchannel reader, status code table, progress lines
├── p1600_supplies.c / p1600_supplies.h usb:// URIs, HTTP/LEDM and snmpInk parsing, marker attributes
├── p1600_usb_ledm.c                 IOKit LEDM transport over the vendor USB interface (FF/02/10)
├── p1600dn_status.c            Bilingual CUPS/IPP printer and supply status utility
├── jbig.c / jbig.h               Embedded ITU-T T.82 JBIG encoder (JBIG-KIT)
├── jbig_ar.c / jbig_ar.h         Arithmetic entropy coder
├── p1600_raster.c / p1600_raster.h     Raster validation, DMPAPER codes, page rendering (gray + legacy)
├── p1600_screen.c / p1600_screen.h     Halftone screening (NEON + scalar); p1600_screen_tables.h is generated by tools/gen_screens.py
├── pappl_p1600dn.c             OpenPrinting PAPPL Printer Application adapter (unfinished draft that cannot print)
├── rastertop1600dn.c           Native CUPS raster filter entry point
├── zjs.h                         ZjStream protocol constants and wire structures
├── zjs_encoder.c / zjs_encoder.h P1606dn PJL/ZjStream serializer and compressor
└── zjs_guard.c / zjs_guard.h     Page guard: padding forced white, geometry, framing and every pixel checked before sending
```

- `p1600dn_manager.command`: Bilingual single source of truth for installation, maintenance, testing, and queue management.
- `p1600dn_manager.sh`: Companion shell script completely identical to `p1600dn_manager.command`.

---

## Technical Specifications

### 1. Job Contract
- Raster: 600 × 600 dpi, 8-bit `CUPS_CSPACE_W`; `cupsCompression` bit 0 selects FastRes 1200 (`VIDEO_BPP = 2`), otherwise 600 dpi (`VIDEO_BPP = 1`). `cupsInteger0 = 1606` identifies the PPD. PPD 3.x queues (1200 × 600 1-bit DeviceK) remain accepted.
- Screening: the job format's FastRes screen layout (12 × 12, four levels) and 600 dpi screen layout (16 × 16), indexed by the descending line counter, with the project's own tables: 600 dpi round dots on pixel corners (`PHASE_1BPP`, `Q_*_1BPP`); FastRes elliptical dots on pixel centers (`PHASE_2BPP`, `Q_DOT_2BPP`) with a rim of level-1 and level-2 pixels (`SPREAD_2BPP`, narrower `SPREAD_DARK_2BPP` in the dark tones) and round holes past `ROUND_HOLES_2BPP`; tone curves `TONE_1BPP` / `TONE_2BPP`, Bernstein polynomials evaluated exactly. `tools/gen_screens.py` generates `src/p1600_screen_tables.h`; `make test-screens` checks that the committed header is its output.
- Geometry: `VIDEO_X = cupsWidth`, `RASTER_X = VIDEO_X * VIDEO_BPP`, `VIDEO_Y = RASTER_Y = JBIG Yd` = height rounded up to 12 lines; JBIG width aligned to 128 bits.
- Page guard: `zjs_encode_page_checked()` (in the encoder pool and for blank partner pages) clears the padding, encodes into memory and runs `zjs_guard_check()`. That check covers the geometry above, the START_PAGE items, the BIH, the BID framing and a full JBIG decode compared with the page. A page that fails is never written: the filter reports `ERROR: Page n was not sent to the printer: <reason>` and closes the job with END_DOC, `@PJL EOJ` and `@PJL USTATUSOFF`. The check always runs.
- Paper codes: the PPD's sizes and their landscape forms with their Windows DMPAPER codes, exact and then within one point (macOS truncates PageSize, so B5 is 515 × 728). The PPD's 16K sizes get the codes these printers receive from Windows, 264, 263 and 257, and other sizes get 256; 0 is never sent. The default source is Auto (7); Manual Feed is 4.
- JBIG: one image per page, L0 = 128, MX = MY = 0, order 3, options 0x5c, trailing stripe zeros kept (`JBG_KEEP_ZEROS`); BID chunks of exactly 64 KiB, last padded to a multiple of 4.
- Duplex: automatic for Letter/Legal/A4/8.5 × 13 on plain/light/mid-weight media with the duplex unit installed (DMDUPLEX 2, order 2, 1, 4, 3, …); manual otherwise (even pages, PAUSE chunk 11, odd pages). CUPS rotates long-edge back sides (`cupsBackSide: Rotated`).
- Page counts: the filter writes one `PAGE: <n> <copies>` line per raster page (one per printed side), also for duplex jobs, so `job-impressions-completed` is exact. cupsd derives `job-media-sheets-completed` from that count alone (`scheduler/job.c`, `update_job()`): impressions / 2 (rounded down) when the job's IPP `sides` attribute is not `one-sided`, otherwise impressions. It never looks at the PPD `Duplex` option, `PAGE: total` can only raise the count, and `ATTR:` does not accept `job-media-sheets-completed`, so the filter cannot correct it. Effects: `lp -o Duplex=DuplexNoTumble` (no `sides`) on 4 pages reports 4 impressions and 4 sheets for 2 sheets printed; `lp -o sides=two-sided-long-edge` (mapped to `Duplex` through the PPD) reports 2. macOS `cgpdftoraster` pads an odd duplex document with a blank back page, so a 3-page PDF reaches the filter as 4 raster pages: 4 impressions and, with `sides`, 2 sheets (checked on a P1606dn). Only raster that arrives with an odd page count (the filter then adds its own blank partner, without a `PAGE:` line) rounds down: 3 impressions, 1 sheet reported for 2 printed. Manual duplex counts the same way. Use `sides` when the sheet count matters; the diagnostic page sends it.
- Printer status: for `usb`, `socket`, `mdns` and `dnssd` queues a reader thread parses PJL USTATUS from the backchannel (fd 3) and maps the printer's status codes to `STATE:`/`INFO:` lines with the PPD's `cupsIPPReason` texts; the trailer waits for `USTATUS JOB END` between `@PJL EOJ` and `@PJL USTATUSOFF` (15 s if the printer never answers, 10 min after its last message). See `reference/zjstream/ZJS_STREAM_SPEC.md` section 6.
- Apple Silicon: the main thread reads and renders; pages are JBIG-encoded on `p1600_encoder_default_workers()` threads (performance cores, at most four; `P1600DN_ENCODER_THREADS` overrides, 0 = inline) with at most workers + 1 pages in flight, and a writer thread sends them in the job's page order as soon as they are ready (automatic duplex per sheet, manual duplex spooled). Job bytes never depend on the thread count. FastRes screening uses three NEON threshold compares per 16 pixels (valid because every cell of the FastRes table is monotonic, checked at startup); the 600 dpi screen compares 16 pixels with the row's thresholds.
- AirPrint: `p1600dn-airprint` (LaunchDaemon `com.berkbb.p1600dn.airprint`, user `nobody`, installed only by `--enable-airprint`) serves `ipp://<host>:8632/ipp/print/<queue>` for each P1600dn queue.
  - Bonjour: `_ipp._tcp,_universal,_print` plus a `_printer._tcp` placeholder, with TXT `URF=V1.4,W8,SRGB24,RS300-600,CP1,DM1,IS1,MT1-2-3-4-5-8,OB10,PQ4-5`, `pdl` and `rp`.
  - Documents: URF/PWG are decoded to a CoreGraphics PDF (page size = pixels × 72 / dpi); PDF, JPEG and PNG pass through. Each job is submitted with cupsCreateJob/cupsStartDocument and its CUPS state is mirrored.
  - Queue changes: the daemon re-lists the queues every 60 s and exits 75 on a change, so launchd restarts it.
- Notifications: `P1600dn Notifier.app` (LaunchAgent `com.berkbb.p1600dn.notifier`, Aqua sessions, LSUIElement) holds one ippget subscription per P1600dn queue (printer-state-changed, printer-stopped, job-completed). It asks every notify-get-interval (60 s idle, 5 s while printing) and turns snapshot changes into notices with `p1600_notify_compare()`. Attention reasons come from the filter's `STATE:` lines, with printer-state-message (the PPD's text) as the body. Toner buckets are 15 % and 5 %, plus empty. A ReportLevels command job follows printing, debounced 20 s and at most every 30 minutes (a query due sooner is put off, not dropped); one not finished after 12 s (the printer is off) is canceled, and the end of a ReportLevels job brings no new query. The PPD's `*cupsCommands` leaves ReportLevels out, so macOS never queues it itself. Reported reasons and the toner bucket are kept in UserDefaults. A click opens the utility with `'prdt'`.
- Diagnostic page: `p1600_diagnostic_write_pdf()` draws two pages (front, back) with CoreGraphics/CoreText: a bottom-edge bar 24–34 pt above the paper edge with nothing below it, gray levels, 1–8 dot lines, corner marks 12 pt in, orientation arrows, printer/queue/paper details. `p1600_diagnostic_print()` prints it with `cupsPrintFile2()` (sides, PageSize), so it goes through cgpdftoraster, the filter and the page guard. The design frame is laid out for A4 and scaled down for smaller paper; the bar and corner marks stay at the paper's edges.
- Printer utility: `*APPrinterUtilityPath` names `P1600dn Series Utility.app` (macOS passes the queue in the Open Application event's `'prdt'`); it and `p1600dn-utility` send the utility's PJL settings and page commands over the USB printer interface (07/01) or AppSocket, one session per action, and serve the printer's web pages over USB interface FF/02/10 (spec section 8).
- Toner level: `*cupsFilter: "application/vnd.cups-command 100 .../commandtop1600dn"` answers `ReportLevels` with `ATTR: marker-*` from the printer's LEDM consumables document over USB or Apple's `snmpInk` (spec section 7).

### 2. Hardware Model Coverage
Full family coverage with IEEE-1284 Device ID auto-matching for P1606dn, P1607dn, P1608dn, and P1609dn (`cupsInteger0 == 1606`).
