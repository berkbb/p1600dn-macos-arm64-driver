# Changelog

All notable changes to the P1600dn Series macOS driver (for HP LaserJet Professional P1606dn, P1607dn, P1608dn and P1609dn printers) are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

## [6.0.1] - 2026-10-22

The first published release: a native Apple Silicon (arm64) CUPS driver for macOS 11 and later. Version 6.0.0 (2026-10-16) was withdrawn; 6.0.1 replaces it.

### Added
- **Raster filter** `rastertop1600dn`: 600 × 600 dpi 8-bit gray from macOS becomes the printer's PJL/ZjStream job, one JBIG image per page, in FastRes 1200 (four levels per pixel) or 600 dpi. Pages are encoded in parallel on the performance cores, and every page is proven by the page guard before it is sent. Automatic duplex (pages 2, 1, 4, 3, … with a blank partner for an odd last page) and manual duplex; 18 paper sizes, custom sizes and 16 media types; EconoMode, print density, jam recovery and media source.
- **Halftone screens** of the project's own (`tools/gen_screens.py` writes `src/p1600_screen_tables.h`; `make test-screens` checks it): 45° round dots that grow into crosses at 600 dpi; elliptical dots with soft edges of partial toner levels, chains in the midtones and round holes in the dark tones in FastRes 1200; tone curves as Bernstein polynomials with a few constants.
- **Printer status** during a job from the printer's PJL USTATUS replies (out of paper, jams, open doors, manual feed and duplex, engine errors), with the PPD's texts in the job's language, and page progress lines.
- **Toner level**: `commandtop1600dn` answers CUPS `ReportLevels`: the LEDM consumables document over the USB printer's vendor interface, or `snmpInk` for network printers.
- **`P1600dn Series Utility.app`** and `p1600dn-utility`: Sleep after and Turn off after (Energy Saving), Quiet Mode, the printer's Configuration, Demo, Supplies Status and Cleaning pages, the driver's diagnostic page, and the printer's web page over USB; for USB and network printers.
- **`P1600dn Notifier.app`**: macOS notifications for the printer's states and the toner level.
- **AirPrint and IPP Everywhere** server, off until turned on, that prints through the CUPS queue.
- **16 languages** in the PPD, the utility and the notifier: English, Danish, German, Spanish, Finnish, French, Italian, Korean, Dutch, Norwegian, Swedish, Portuguese, Simplified and Traditional Chinese, Turkish and Romanian.
- **Manager** `p1600dn_manager.command` (and `.sh`): builds the driver, runs every test, installs only when all pass, adds and updates queues, turns AirPrint on and off, removes Gutenprint, and uninstalls.
- **Homebrew cask** `p1600dn` in the tap [berkbb/homebrew-tap](https://github.com/berkbb/homebrew-tap) (source: `packaging/homebrew/`), which installs through the manager: `brew tap berkbb/tap`, then `brew install --cask berkbb/tap/p1600dn` (`brew trust berkbb/tap` lets `brew search` find it and the short name `p1600dn` work).
- Own printer icon (`assets/P1600dnPrinter.icns`, `.png`) and app icon (`assets/P1600dnApp.icns`), drawn by `tools/draw_printer_icon.swift`.
- The job specification (`reference/zjstream/ZJS_STREAM_SPEC.md`) and a trademark notice (`README.md`, `NOTICE`).

### Fixed (since driver 5.0)
- Opening **Options & Supplies** or the utility no longer queues a toner query job again and again: the PPD's `*cupsCommands` no longer lists `ReportLevels`. The notifier still asks for the toner level after printing, and `commandtop1600dn` still answers `ReportLevels`.
- The AirPrint server no longer restarts every 60 seconds on its own advertisement: printers `cupsGetDests()` finds over Bonjour and driverless queues (`<model>-AirPrint`, `IPP Everywhere`) are no longer P1600dn queues, also in the utility, the notifier and `p1600dn-status`. A moment without cupsd is reported as an error instead of "no queues".
- A canceled job (cupsd's SIGTERM) stops after the last whole page and is closed with END_DOC, `@PJL EOJ` and `@PJL USTATUSOFF` instead of dying mid-page.
- Print Density, Jam Recovery, EconoMode and TersKose fall back to the queue PPD's `*Default` value when the job does not set them (settings made with `lpadmin -o`, AirPrint jobs).
- HTML Config over USB: an idle proxy closes the printer's USB interface and its loopback address at once; pages stay served after the window closes until idle; the Bonjour lookup and stopping an old proxy no longer block the window. Requests from other sites (Host, Origin, Referer, `Sec-Fetch-Site`) are refused with 403; 304, 204, 1xx and HEAD replies end with their header; a late reply after a failed exchange is dropped.
- AirPrint: a raster document may decode to at most 32 GiB and a PDF be at most 1 GiB, and Cancel-Job and shutdown stop the conversion between pages; at most 8 connections from one address and 10 seconds for a request header; uploads cut off or stalled are refused; uploads under way count against the queue limit; Bonjour is retried when unavailable at the start; `orientation-requested` 4/5 is passed on only for JPEG, PNG and landscape PDF pages; `print-scaling=fit` draws raster pages into the printable area; gzip documents of several members and zlib deflate with a one-byte first read are read correctly.
- The utility: a printer that answers no INQUIRE leaves the settings disabled with Try Again; the header's state and toner level are looked up again every 5 seconds; a running utility switches to the printer of a later Open Application event.
- The notifier: its own ReportLevels job is canceled after 12 seconds when unfinished (printer off); a toner query due within 30 minutes is put off, not dropped; the end of a ReportLevels job brings no new query; a toner level that fell while the agent was not watching is reported at its first observation.
- Diagnostic page: named and printed at the queue's resolution; the bottom bar ends clear of the corner marks.
- The manager: the first install asks before it removes Gutenprint (`--install --yes` does not ask); signed binaries are replaced (`rm -f`, then `cp`) instead of overwritten; the install folder is 755 under any umask; the apps are readable by every user and registered with Launch Services; the notifier is handled in every GUI session; `build/` goes back to the sudo user also when `--install` stops halfway; Add Queue refuses an existing queue name; uninstall also removes the OpenPrinting fallback's `COPYING`; the AirPrint duplicate-printer warning shows only while Printer Sharing is on; driver 5.0's paths follow `INSTALL_DIR`, so tests never touch `/Library`.
- Tests: UBSan errors stop `make test-sanitize`, which now builds what it runs; the `--self-test` source checks can fail; the test PPDs name the filter through a short link, so `make test` works from long folder paths; each CLI action and alias is checked alone.

### Changed (for installations of driver 5.0)
- New names: install folder `/Library/Printers/p1600dn` (was `hp_p1606dn`), filters `rastertop1600dn` and `commandtop1600dn`, tools `p1600dn-status`, `p1600dn-utility` and `p1600dn-airprint`, PPD `P1600dn-Series.ppd`, app and agent identifiers `com.berkbb.p1600dn.*`, manager `p1600dn_manager.command`, environment variables `P1600DN_*`.
- Installing over driver 5.0 moves its queues to the new PPD (their option defaults stay), keeps AirPrint on when it was on, and removes 5.0's agents, folder and PPD; uninstalling removes them too. The notifier takes over 5.0's remembered toner and attention state.
- Gray areas print with the new dot pattern. Status messages, the cleaning instructions and several option names have new wording in every language. Paper sizes the PPD does not list are sent as custom size 256.
- **License: GPL-3.0-or-later** (5.0 was GPL-2.0-or-later): `LICENSE` holds the GNU GPL version 3 text, and the SPDX identifiers, the apps' copyright, `README.md`, `NOTICE` and the distribution guide say so. JBIG-KIT stays GPL-2.0-or-later, which allows its use under GPL-3.0.
- The manager's titles, the Homebrew cask and the PAPPL app are named "P1600dn Series" instead of carrying HP's name as if HP made them; HP's printer names remain where they identify the supported printers.
- The utility's and the notifier's copyright (`NSHumanReadableCopyright`, shown in Finder's Get Info) name the author, berkbb, instead of describing the apps as HP's.
- The PAPPL adapter is described as an unfinished draft that cannot print.

## Earlier versions

Versions 1.0.0 to 5.0.0 (August to October 2026) were developed privately; their history is not published.
