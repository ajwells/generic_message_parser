---
tags: [skill-creator]
max_turns: 40
timeout_seconds: 1200
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

I'm decoding a sub-GHz radio link. Frames are HDLC-like: 0x7E, a 1-byte kind, the payload, a CRC-16/KERMIT (little-endian), 0x7E. Byte stuffing: 0x7E and 0x7D inside the frame are sent as 0x7D followed by the byte XOR 0x20. Quirk of this vendor: the CRC is calculated over kind + payload *as they are on the wire, after stuffing*. The CRC bytes themselves are then stuffed like everything else.

- kind 1 Telemetry: u16 batt_mv, i16 temp in 0.01 degC
- kind 2 Ack: u8 seq

All little-endian. A capture:

7E 01 0C 7D 5E 69 08 2C 0F 7E 7E 02 7D 5D 64 7C 7E 7E 01 74 0E 64 FE 32 2F 7E

Write the schema (radio.yaml) and check it against the capture.
