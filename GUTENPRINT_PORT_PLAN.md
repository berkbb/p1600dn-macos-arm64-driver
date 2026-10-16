# Gutenprint — HP LaserJet Pro P1606dn Compatibility

## Executive Summary

Gutenprint **partially supports the P1606dn over the network** — but via an entirely different protocol path. This document compares both printing paths.

---

## Verified User Experience

Prior to developing the native ARM64 driver, the user successfully drove the P1606dn over Ethernet using the **Gutenprint + HP LaserJet 1160 (`hp-lj_1160`) driver** combination.

---

## Why Did It Work? — Jetdirect PCL5 Path

The P1606dn interfaces support different protocols:

```text
USB Interface      ─── Host-based ──► ZjStream/JBIG only
Network Interface  ─── Jetdirect  ──► Also accepts PCL5 ✅
```

Gutenprint generates PCL5 output for the HP LaserJet 1160. This PCL5 stream reaches the P1606dn internal Jetdirect print server over TCP port 9100, which rasterizes it directly without requiring ZjStream encoding. This is an intentional hardware feature of the Jetdirect network interface.

---

## Comparison of Both Paths

| Feature | Gutenprint + LJ1160 (PCL5) | Native ARM64 Driver (ZjStream) |
|---|---|---|
| Transport | ⚠️ **Network** Only | ✅ Network **and** USB |
| Protocol | PCL5 | ZjStream / JBIG |
| Duplex (Two-Sided) | ⚠️ Limited (Generic PCL) | ✅ Full Hardware (PJL + ZjStream) |
| Density Control | ❌ No PJL command | ✅ `@PJL SET DENSITY=1..5` |
| Resolution | ⚠️ Generic 600 dpi | ✅ Native 1200×600 FastRes 1200 |
| JBIG Artifact Fix | ❌ None | ✅ Height and pixel synchronization |
| Apple Silicon Native | ❌ (x86 Gutenprint → Rosetta 2) | ✅ Pure C11 Native `arm64` |
| Print Quality | Moderate | Superior |

---

## Is a Gutenprint Port Needed?

### For P1606dn → No
The native ARM64 driver via ZjStream/JBIG is superior in every aspect: faster, zero emulation overhead, higher quality, with full USB and network support.

### As Secondary Fallback
No. The native driver is the only print path, and every page passes its page guard. The Gutenprint PCL5 path remains documented strictly as an architectural reference.

The manager removes Gutenprint without asking on the first installation and on every uninstallation. A queue whose name or device URI names a P1600dn model is switched to the native driver first. A Gutenprint queue that does not (for example an LJ 1160 queue on a bare `socket://` address) counts as another printer, and Gutenprint then stays.
