---
tags: [skill-creator]
max_turns: 30
timeout_seconds: 900
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

I'm adding a PX-210 barometric sensor to my firmware and want to use GMP for the decode. Here's the relevant part of the datasheet. I read the sensor over I2C, so each read gives me exactly these bytes, no framing.

Command 0x2C "Read measurement" returns 7 bytes:

| byte | content |
|------|---------|
| 0-1  | pressure raw, uint16, MSB first |
| 2    | CRC-8 of bytes 0-1 |
| 3-4  | temperature raw, int16 two's complement, MSB first |
| 5    | CRC-8 of bytes 3-4 |
| 6    | status |

Status byte: bit 7 = BUSY, bits 6..5 = MODE (00 sleep, 01 normal, 10 forced, 11 reserved), bits 4..1 reserved (read as 0), bit 0 = DRDY (new data ready).

Conversion: P [hPa] = 300 + raw_p * 800 / 65535, T [°C] = raw_t / 200.

CRC-8: polynomial 0x31, initial value 0xFF, no input/output reflection, no final XOR.

Write the GMP schema (save it as px210.yaml) and make sure gmpc accepts it.
