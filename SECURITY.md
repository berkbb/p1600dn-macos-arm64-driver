# Security Policy

## Supported Versions

| Version | Security fixes |
|---|---|
| 6.0.2 (latest) | Yes |
| 6.0.1 | No: update to 6.0.2 |
| 6.0.0 | No: withdrawn, update to 6.0.2 |
| Before 6.0.0 | Never published |

Security fixes go into a new release of the latest version. To see the installed version:

```bash
grep '^\*FileVersion' /Library/Printers/PPDs/Contents/Resources/P1600dn-Series.ppd
```

## Reporting a Vulnerability

Please do not report a vulnerability in a public issue. Report it privately on GitHub: open the repository's **Security** tab and choose **Report a vulnerability** (<https://github.com/berkbb/p1600dn-macos-arm64-driver/security/advisories/new>). Only you and the maintainer see the report.

Please include:

- the driver version and the macOS version;
- the part of the driver (see Scope) and how the printer is connected (USB or network);
- the steps that show the problem, and what an attacker gains.

This project is maintained by one person in their spare time. You will get an answer within 14 days. A confirmed vulnerability is fixed in a private draft advisory, released, and then published as a GitHub security advisory, with credit to you if you want it.

## Scope

In scope is everything this repository builds and installs:

- the CUPS filters `rastertop1600dn` and `commandtop1600dn`, which cupsd runs for every job;
- `P1600dn Series Utility.app`, `p1600dn-utility` and the printer web page proxy on the loopback address (USB printers);
- the AirPrint / IPP Everywhere server `p1600dn-airprint` (TCP port 8632, off until turned on);
- `P1600dn Notifier.app` and `p1600dn-status`;
- the manager `p1600dn_manager.command` / `p1600dn_manager.sh` and the Homebrew cask, which install with administrator rights.

Out of scope:

- the printer's own firmware and web pages (report those to HP);
- macOS, CUPS and other system components (report those to Apple or OpenPrinting);
- documented behavior: the AirPrint server takes print jobs from anyone on the local network, without passwords or TLS, as an AirPrint printer does. A way to do more through it than print a job is in scope.

## Published Advisories

Published advisories are listed at <https://github.com/berkbb/p1600dn-macos-arm64-driver/security/advisories>, and each release's security fixes are in the **Security** section of `CHANGELOG.md`.
