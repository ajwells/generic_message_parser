---
tags: [skill-creator]
max_turns: 30
timeout_seconds: 900
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

gmpc says `radio.yaml:13:5: error: 'escape' needs delimited, fixed or length_prefixed framing`, what's wrong? The radio sends HDLC-style frames: 0x7E, the payload, a CRC-16/X.25 (little-endian), 0x7E, with 0x7D/0x20 byte stuffing. Here's my schema:

```yaml
schema_version: 1
namespace: radio

messages:
  Beacon:
    fields:
      - { name: node, type: u8 }
      - { name: rssi, type: i8 }
      - { name: seq, type: u16 }

protocols:
  Radio:
    framing: { type: none, escape: { char: 0x7D, xor: 0x20 } }
    checksum: { algo: crc16_x25, endian: little }
    messages: [Beacon]
```

Please fix it and save the corrected schema as radio.yaml.
