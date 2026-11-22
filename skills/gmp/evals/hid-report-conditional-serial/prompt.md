---
tags: [skill-creator]
max_turns: 40
timeout_seconds: 1200
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

I'm parsing a USB HID input report from our data logger. I get the report payload as a byte buffer, one report at a time. All multi-byte values are big-endian:

1. report id, always 0x05
2. mode: 0 idle, 1 run, 2 cal. Any other value means the report is corrupt and must be rejected.
3. flags: bit 0 has_error, bit 1 low_battery, bits 2-7 reserved
4. serial_hi (u16), serial_lo (u16). I want the full 32-bit serial as one value, and I also need to set it as one value when I build test reports.
5. error_code (u16), only present when has_error is set
6. count (u8) and then count samples, each u16. At most 16.

Two real reports:
- 05 01 02 00 C0 FF EE 03 00 64 00 C8 01 2C
- 05 02 01 12 34 56 78 01 02 00

Write the GMP schema (devinfo.yaml) and show me how I'd read the serial and the samples from a report in C++.
