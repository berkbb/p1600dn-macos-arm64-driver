# Homebrew Tap: berkbb/tap

Homebrew tap for the native Apple Silicon (`arm64`) driver for the HP LaserJet Professional P1600dn Series (P1606dn, P1607dn, P1608dn, P1609dn).

The cask has no `.pkg`. It downloads the source archive of the release and runs the driver's manager (`p1600dn_manager.command --install`), as a manual installation does. The manager builds the driver, runs every test, and installs only when all of them pass. This takes a few minutes and asks for the administrator password.

Requirements:

- an Apple Silicon Mac;
- macOS 11 or later;
- Xcode Command Line Tools with `clang`, `make` and `python3` (Homebrew already needs them).

> [!NOTE]
> The cask expects a public `berkbb/p1600dn-macos-arm64-driver` repository (public) with a `v6.0.1` tag, and this tap as `berkbb/homebrew-tap`. The repository is private and the tap does not exist yet; see `DISTRIBUTION_GUIDE.md`, section 5.

---

## Installation

### Single-line direct installation:

```bash
brew install --cask berkbb/tap/p1600dn
```

### Or by adding the tap first:

```bash
brew tap berkbb/tap
brew install --cask p1600dn
```

---

## Uninstallation

To remove the driver completely:

```bash
brew uninstall --cask p1600dn
```

This runs `p1600dn_manager.command --uninstall --yes`. It removes the driver, the utility, the notifier, AirPrint and the PPD. It also removes Gutenprint, unless another printer still uses it. Print queues stay; remove them in Printers & Scanners.
