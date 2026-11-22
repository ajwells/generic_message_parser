---
type: llm
focus: trace
weight: 1
---

pressure is a big-endian u16 followed by a field-level CRC equal to CRC-8 poly 0x31, init 0xFF, no reflection, no final XOR (crc8_nrsc5 or the same inline parameters)
