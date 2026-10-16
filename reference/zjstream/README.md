# P1600dn Series Protocol Reference

`ZJS_STREAM_SPEC.md` describes the job format and protocols of the P1600dn
series printers (P1606dn, P1607dn, P1608dn, P1609dn) and how the driver uses
them:

- **Print job** (sections 1–5): ZjStream framing, page header items, JBIG
  image data and screening, duplex and page order, and the filter's page guard.
- **Status** (section 6): PJL USTATUS messages on the backchannel and the CUPS
  reasons they set.
- **Toner level** (section 7): the LEDM query over USB and snmpInk for network
  queues.
- **Utility** (section 8): the PJL settings, information pages and web pages
  of the printer utility.

The halftone screens and their tone curves are the project's own
(`tools/gen_screens.py`).
