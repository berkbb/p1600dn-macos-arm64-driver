# Homebrew Tap: berkbb/tap

Homebrew tap for the native Apple Silicon (`arm64`) driver for the HP LaserJet Professional P1600dn Series (P1606dn, P1607dn, P1608dn, P1609dn).

The cask has no `.pkg`. It downloads the source archive of the release and runs the driver's manager (`p1600dn_manager.command --install`), as a manual installation does. The manager builds the driver, runs every test, and installs only when all of them pass. This takes a few minutes and asks for the administrator password.

Requirements:

- an Apple Silicon Mac;
- macOS 11 or later;
- Xcode Command Line Tools with `clang`, `make` and `python3` (Homebrew already needs them).

> [!NOTE]
> The cask installs the `v6.0.1` release of the public `berkbb/p1600dn-macos-arm64-driver` repository (its source archive, checked by SHA-256). This folder is the source of the public tap [berkbb/homebrew-tap](https://github.com/berkbb/homebrew-tap); see `DISTRIBUTION_GUIDE.md`, section 5.

---

## Installation

```bash
brew tap berkbb/tap
brew install --cask berkbb/tap/p1600dn
```

Homebrew 7 treats taps outside Homebrew as untrusted: `brew install` takes the full name `berkbb/tap/p1600dn` once the tap is added. To find the cask with `brew search p1600dn` and use the short name `p1600dn`, trust the tap once with `brew trust berkbb/tap`.

```bash
brew trust berkbb/tap
brew search p1600dn
brew install --cask p1600dn
```

---

## Uninstallation

To remove the driver completely:

```bash
brew uninstall --cask berkbb/tap/p1600dn
```

This runs `p1600dn_manager.command --uninstall --yes`. It removes the driver, the utility, the notifier, AirPrint and the PPD. It also removes Gutenprint, unless another printer still uses it. Print queues stay; remove them in Printers & Scanners.
