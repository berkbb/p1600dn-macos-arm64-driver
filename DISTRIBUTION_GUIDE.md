# Distribution & Ecosystem Guide: Apple Notarization, Homebrew & OpenPrinting

This document outlines the step-by-step integration and distribution workflows for the **HP LaserJet Professional P1600dn Series macOS Native ARM64 Driver** across the Apple ecosystem, the Homebrew package manager, and The Linux Foundation / OpenPrinting consortium.

---

## Table of Contents

1. [Strategic Overview & Licensing](#1-strategic-overview--licensing)
2. [Apple Ecosystem & macOS Distribution](#2-apple-ecosystem--macos-distribution)
3. [Homebrew Integration](#3-homebrew-integration)
4. [OpenPrinting Consortium](#4-openprinting-consortium)
5. [Step-by-Step Distribution Checklist](#5-step-by-step-distribution-checklist)

---

## 1. Strategic Overview & Licensing

- **Driver Architecture:** Pure C11, zero external dynamic library dependencies (Zero Bloat), native Apple Silicon (`arm64`) 64-bit Mach-O binary.
- **Licensing:** **GNU General Public License v3 (GPL-3.0-or-later)**.
  - The driver statically embeds Markus Kuhn's open-source **JBIG-KIT** (`src/jbig.c`, `src/jbig_ar.c`) for ITU-T T.82 JBIG encoding.
  - JBIG-KIT is GPL-2.0-or-later, which allows its use under GPL-3.0; the entire project is distributed under GPL-3.0-or-later.
  - Dynamic linking against Apple's `libcups` (Apache 2.0, compatible with GPL-3.0) is fully compliant.
- **Code and Data Provenance:**
  - The job format was learned by static analysis of the printer maker's macOS driver (`reference/zjstream/ZJS_STREAM_SPEC.md`); none of that driver's code, tables, texts or data is in the repository or its published history.
  - The halftone screens are the project's own (`tools/gen_screens.py` generates `src/p1600_screen_tables.h`): 45° round dots that grow into crosses and meet in a checkerboard at 600 dpi, elliptical dots with soft edges of partial toner levels (about 18 partial pixels per 12 × 12 tile), chains in the midtones and round holes in a solid net in the dark tones in FastRes 1200, in the job format's screen geometry (8 dots per 16 × 16 or 12 × 12 tile), with a tone curve of its own (a Bernstein polynomial with a few constants, chosen during development so that grays print at their target darkness, within 2% coverage).
  - The published history starts with one root commit of the 6.0.0 tree (October 2026) in a newly created GitHub repository; the earlier development history is not published.
  - All texts are the project's own: the utility, notifier and PPD translations (16 languages) were written from the project's English texts. String keys and PPD option keywords are identifiers, not texts.
  - The printer icon (`assets/P1600dnPrinter.png`, `.icns`, for the PPD and AirPrint) and the apps' icon (`assets/P1600dnApp.icns`, the printer on a rounded square on the macOS app icon grid) are the project's own drawings, made by `tools/draw_printer_icon.swift` (`tools/make_printer_icon.sh` writes the three files); no product photo is used.

---

## 2. Apple Ecosystem & macOS Distribution

### 2.1 Apple's Current Printer Strategy

1. **AirPrint & Mopria Priority:** Apple has discontinued bundling vendor-specific proprietary drivers in OS updates, standardizing on driverless AirPrint/IPP across macOS 10.15+ and modern macOS (Sonoma, Sequoia, Tahoe).
2. **Direct Upstream Boundary:** Because the P1606dn is a host-based ZjStream printer without hardware AirPrint chips, Apple does not bundle this driver directly into macOS system updates.

### 2.2 Solution: Signed & Apple Notarized Installer (`.pkg`)

> [!NOTE]
> Not used (October 2026). The driver is installed by `p1600dn_manager.command --install`, which builds it, runs every test and installs only when all of them pass; the Homebrew cask runs the same command. This section is kept for reference only.

An **Apple Notarized `.pkg`** installer is the usual way to ship macOS drivers to end users.

#### Advantages:

- **Gatekeeper Clearance:** macOS Gatekeeper opens the package cleanly without unsigned developer warnings, displaying the badge _"Software scanned and verified by Apple"_.
- **User Experience:** Users install the driver via a standard double-click GUI installer wizard without Terminal exposure.

#### Technical Build Steps:

1. **Stage Driver Files:**
   ```bash
   mkdir -p pkg_root/Library/Printers/p1600dn
   mkdir -p pkg_root/Library/Printers/PPDs/Contents/Resources
   cp build/rastertop1600dn pkg_root/Library/Printers/p1600dn/
   cp build/p1600dn-status pkg_root/Library/Printers/p1600dn/
   cp build/commandtop1600dn pkg_root/Library/Printers/p1600dn/
   cp build/p1600dn-utility pkg_root/Library/Printers/p1600dn/
   ditto "build/P1600dn Series Utility.app" "pkg_root/Library/Printers/p1600dn/P1600dn Series Utility.app"
   ditto "build/P1600dn Notifier.app" "pkg_root/Library/Printers/p1600dn/P1600dn Notifier.app"
   cp build/p1600dn-airprint assets/P1600dnPrinter.png pkg_root/Library/Printers/p1600dn/
   mkdir -p pkg_root/Library/LaunchAgents
   cp notifier/com.berkbb.p1600dn.notifier.plist pkg_root/Library/LaunchAgents/
   gzip -c ppd/P1600dn-Series.ppd > pkg_root/Library/Printers/PPDs/Contents/Resources/P1600dn-Series.ppd.gz
   ```
   The AirPrint LaunchDaemon (`airprint/com.berkbb.p1600dn.airprint.plist`) stays out of the package: AirPrint is turned on by the user (`--enable-airprint`). A postinstall script should `launchctl bootstrap gui/<console uid>` the notifier LaunchAgent for the logged-in user.
   `make` already builds the binaries for macOS 11.0+ with the hardened runtime and an ad-hoc signature. Notarization needs them signed with your Developer ID and a secure timestamp:
   ```bash
   for binary in pkg_root/Library/Printers/p1600dn/*; do
       [ -f "$binary" ] || [ -d "$binary" ] || continue
       codesign --force --options runtime --timestamp \
                --sign "Developer ID Application: Full Name (TEAM_ID)" "$binary"
   done
   ```
2. **Build Package (`pkgbuild`):**
   ```bash
   pkgbuild --root pkg_root \
            --identifier com.berkbb.pkg.p1600dn \
            --version 6.0.1 \
            --install-location / \
            --sign "Developer ID Installer: Full Name (TEAM_ID)" \
            P1600dn-Driver-v6.0.1.pkg
   ```
3. **Submit to Apple Notarization (`notarytool`):**
   ```bash
   xcrun notarytool submit P1600dn-Driver-v6.0.1.pkg \
                    --keychain-profile "AC_PASSWORD" \
                    --wait
   ```
4. **Staple Ticket (`stapler`):**
   ```bash
   xcrun stapler staple P1600dn-Driver-v6.0.1.pkg
   ```

---

## 3. Homebrew Integration

Homebrew is the standard package manager for macOS developers and power users.

### 3.1 Why Homebrew Cask?

- Standard `homebrew-core` formulae are restricted to non-root userland tools.
- Printer drivers install to root system locations (`/Library/Printers/` and CUPS directories), requiring Homebrew **Cask** packaging.
- The cask (`packaging/homebrew/Casks/p1600dn.rb`, version 6.0.1) has no `.pkg`. It downloads the release's source archive, runs `p1600dn_manager.command --install` with `sudo`, and `--uninstall --yes` on `brew uninstall`. It passes `brew style` and `brew audit --cask --strict` (checked in a temporary local tap).

### 3.2 Method A: Official Homebrew Cask

`Homebrew/homebrew-cask` expects prebuilt, signed artifacts. A cask that builds from source is not accepted there, so without a `.pkg` the official repository is not an option. Use the tap (Method B).

### 3.3 Method B: Custom Developer Tap (`brew tap berkbb/tap`)

To distribute immediately without waiting for upstream review, host your own tap.

Included tap assets in this repository:

- Cask Definition: [packaging/homebrew/Casks/p1600dn.rb](packaging/homebrew/Casks/p1600dn.rb)
- Tap Guide: [packaging/homebrew/README.md](packaging/homebrew/README.md)

#### Setting Up Your Tap:

1. Create a public GitHub repository named **`homebrew-tap`**.
   _(Homebrew naming convention requires `homebrew-<name>`; `homebrew-tap` enables the clean `berkbb/tap` shorthand)._
2. Upload the files from `packaging/homebrew/` to that repository (`Casks/p1600dn.rb`).
3. Make `berkbb/p1600dn-macos-driver` public with the tag `v6.0.1` (check section 1 first), and put the SHA-256 of `https://github.com/berkbb/p1600dn-macos-driver/archive/refs/tags/v6.0.1.tar.gz` into the cask in place of `:no_check`.
4. Users anywhere can now install the driver with a single command:
   ```bash
   brew install --cask berkbb/tap/p1600dn
   ```
   _(Homebrew automatically fetches the tap and installs the cask in one seamless step!)_

#### Upgrading & Uninstalling:

- When a new version is released:
  ```bash
  brew upgrade --cask p1600dn
  ```
- To remove the driver completely:
  ```bash
  brew uninstall --cask p1600dn
  ```
  This runs the manager's uninstall: driver, utility, notifier, AirPrint, PPD and Gutenprint (unless another printer uses it). Print queues stay.

---

## 4. OpenPrinting Consortium

OpenPrinting (The Linux Foundation) coordinates open-source CUPS and driver architectures globally.

### 4.1 Relationship to macOS

- macOS printing has been built on the **CUPS** standard since 2002.
- Apple officially transitioned open-source CUPS project leadership to **OpenPrinting (The Linux Foundation)** in 2020.
- Consequently, this native driver is 100% compliant with both macOS CUPS and OpenPrinting standards.

### 4.2 OpenPrinting Registry (`openprinting.org`)

- On the official P1606dn printer registry page (`openprinting.org/printer/HP/HP-LaserJet_Professional_P1606dn`):
  - Current status: Lists legacy Linux `foo2zjs`, with Apple Silicon native support unlisted.
  - Registration target: Add `berkbb/p1600dn-macos-driver` under **"Recommended macOS Apple Silicon (ARM64) Driver"**.
- **Submission:** Submit an entry/notification to OpenPrinting curators (Till Kamppeter and team) via Foomatic/Driver XML or GitHub issue.

### 4.3 PAPPL (Printer Application) Future

- OpenPrinting's next-generation roadmap transitions from static PPD files to standalone **Printer Applications** emulating IPP Everywhere / AirPrint locally.
- Our native C11 core (`zjs_encoder.c`, `jbig.c`) is portable and ready for direct PAPPL integration without third-party dependencies.
- The adapter (`src/pappl_p1600dn.c`, `make pappl`, `./p1600dn_manager.command --build-pappl`) is kept for this submission. A completed Printer Application must encode its pages with `zjs_encode_page_checked()` (`src/zjs_guard.c`), so they are verified like every other path to the printer.
- On macOS, `p1600dn-airprint` already provides AirPrint / IPP Everywhere; a PAPPL application would bring the same to Linux and CUPS 3.

---

## 5. Step-by-Step Distribution Checklist

1. [x] **Coding & Unit Tests:** 6.0.1 passes `make test` (263 checks), the sanitizers, Guard Malloc and the static analyzer.
2. [x] **PPD Conformance:** 0 errors and 0 warnings under `cupstestppd -W all`.
3. [ ] **Hardware Verification:** Physical test print verification over USB and network on HP LaserJet P1606dn (diagnostic page, the old stripe document, duplex, utility settings, notifications, AirPrint).
4. [x] **Own Data:** The current tree has its own screens, tone curves and texts (section 1). The new screens still need a test print (item 3).
4a. [x] **Clean History:** One root commit of the 6.0.0 tree in a newly created repository (October 2026); no published commit holds third-party tables, strings or logos (section 1).
5. [ ] **Public Repository and Release:** `berkbb/p1600dn-macos-driver` (private, tagged `v6.0.1`) made public; no release assets were published.
6. [ ] **Homebrew Integration:** Publish the 6.0.1 cask (`packaging/homebrew/`) to `berkbb/homebrew-tap` with the archive's SHA-256.
7. [ ] **OpenPrinting Registry:** Submit entry to OpenPrinting database as the native macOS ARM64 driver.
8. [-] **PKG Build:** Not planned (October 2026); installation goes through the manager.
