# Listing a macOS driver for the HP LaserJet Pro P1606dn?

Hello,

I maintain [p1600dn-macos-arm64-driver](https://github.com/berkbb/p1600dn-macos-arm64-driver), a free software (GPL-3.0-or-later) CUPS driver for the HP LaserJet Professional P1606dn, P1607dn, P1608dn and P1609dn on macOS 11 or later on Apple Silicon. It is a native CUPS raster filter with its own PPD: it writes the printer's PJL/ZjStream job with JBIG-compressed pages at 600 dpi and FastRes 1200, does automatic duplex, reads the printer's status and toner level, and includes an AirPrint / IPP Everywhere server. It can be installed with Homebrew (`brew install --cask berkbb/tap/p1600dn`).

The driver runs on macOS only; on Linux these printers are already served by foo2zjs.

Would a driver entry for a macOS-only driver be welcome in foomatic-db, linked from `printer/HP-LaserJet_Pro_P1606dn`? If so, I would open a pull request adding `db/source/driver/p1600dn-macos.xml` (draft below), and printer entries for the P1607dn, P1608dn and P1609dn if you want them. If foomatic-db is meant for Linux drivers only, I understand, and a pointer to a better place would be appreciated.

<details>
<summary>Draft: db/source/driver/p1600dn-macos.xml</summary>

```xml
<driver id="driver/p1600dn-macos">
  <name>p1600dn-macos</name>
  <url>https://github.com/berkbb/p1600dn-macos-arm64-driver</url>
  <supplier>berkbb</supplier>
  <thirdpartysupplied />
  <license>GPL 3 or later</license>
  <supportcontacts>
    <supportcontact url="https://github.com/berkbb/p1600dn-macos-arm64-driver/issues" level="voluntary">GitHub issues of the p1600dn-macos-arm64-driver project</supportcontact>
  </supportcontacts>
  <shortdescription>
    <en>
      Free software CUPS driver for the HP LaserJet Professional P1600dn series on macOS (Apple Silicon)
    </en>
  </shortdescription>
  <functionality>
    <maxresx>1200</maxresx>
    <maxresy>600</maxresy>
    <monochrome />
  </functionality>
  <execution>
    <cups />
  </execution>
  <comments>
    <en>

      Native CUPS raster driver for the HP LaserJet Professional
      P1606dn, P1607dn, P1608dn and P1609dn on macOS 11 or later on
      Apple Silicon (arm64). It writes the printer's PJL/ZjStream job
      with JBIG-compressed pages (600 dpi and FastRes 1200), including
      automatic duplex, and comes with its own PPD, printer status from
      the printer's PJL replies, toner level, a printer utility,
      notifications and an AirPrint / IPP Everywhere server.

      &lt;p&gt;

      This driver runs on macOS only. On Linux, use the foo2zjs driver,
      which is available in the Ghostscript Printer Application.

      &lt;p&gt;

      Install it with Homebrew (brew install --cask berkbb/tap/p1600dn)
      or from source; it is built and tested on the Mac before it is
      installed.

    </en>
  </comments>
  <printers>
    <printer>
      <id>printer/HP-LaserJet_Pro_P1606dn</id>
    </printer>
  </printers>
</driver>
```

</details>

Thank you.
