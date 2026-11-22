---
tags: [skill-creator]
max_turns: 40
timeout_seconds: 1200
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

My UART frames start with 0xAA 0x55, then a 1-byte message id, a 2-byte little-endian length (number of payload bytes), the payload, and a CRC-16/MODBUS over id + length + payload, sent little-endian. Two messages so far:

- id 0x01 Heartbeat: uint32 uptime_ms (LE), uint8 battery_percent
- id 0x02 SetLed: uint8 led_index, uint8 brightness, uint16 fade_ms (LE)

Write the GMP schema (link.yaml) and the C++ that parses the incoming UART byte stream: I get bytes in chunks from an RX callback (rx.cpp). This runs on a Cortex-M4, so keep it embedded friendly.
