---
tags: [skill-creator]
max_turns: 40
timeout_seconds: 1200
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

Our motor controller talks RS-485. Each frame is: 0xA5 start byte, LEN (1 byte), CMD (1 byte), payload, CRC-8/MAXIM (1-Wire CRC). LEN is the number of bytes after LEN, so CMD + payload + CRC. The CRC covers LEN, CMD and the payload. All multi-byte values are little-endian.

- CMD 0x10 Status: u16 rpm, i8 temp_c, u8 fault
- CMD 0x11 SetSpeed: u16 rpm

Here's a capture from the bus (it has some noise and one frame I know got corrupted):

A5 06 10 DC 05 FB 00 52 00 FF A5 04 11 60 09 29 A5 06 10 D2 01 47 03 EA A5 06 10 00 00 16 04 E9

Write the GMP schema (motor.yaml) and show me that it decodes the capture correctly.
