# Changelog

All notable changes to the P1600dn Series macOS driver (for HP LaserJet Professional P1606dn, P1607dn, P1608dn and P1609dn printers) are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

## [6.0.0] - 2026-10-16

The first published release: a native Apple Silicon (arm64) CUPS driver for macOS 11 and later.

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
- **Homebrew cask** `p1600dn` (`packaging/homebrew/`), which installs through the manager.
- Own printer icon (`assets/P1600dnPrinter.icns`, `.png`) and app icon (`assets/P1600dnApp.icns`), drawn by `tools/draw_printer_icon.swift`.
- The job specification (`reference/zjstream/ZJS_STREAM_SPEC.md`) and a trademark notice (`README.md`, `NOTICE`).

### Changed (for installations of driver 5.0)
- New names: install folder `/Library/Printers/p1600dn` (was `hp_p1606dn`), filters `rastertop1600dn` and `commandtop1600dn`, tools `p1600dn-status`, `p1600dn-utility` and `p1600dn-airprint`, PPD `P1600dn-Series.ppd`, app and agent identifiers `com.berkbb.p1600dn.*`, manager `p1600dn_manager.command`, environment variables `P1600DN_*`.
- Installing over driver 5.0 moves its queues to the new PPD (their option defaults stay), keeps AirPrint on when it was on, and removes 5.0's agents, folder and PPD; uninstalling removes them too. The notifier takes over 5.0's remembered toner and attention state.
- Gray areas print with the new dot pattern. Status messages, the cleaning instructions and several option names have new wording in every language. Paper sizes the PPD does not list are sent as custom size 256.

## Earlier versions

Versions 1.0.0 to 5.0.0 (August to October 2026) were developed privately; their history is not published.
