---
type: llm
focus: trace
weight: 1
---

status is a bits group with BUSY (bit 7), MODE (bits 6..5) as an enum with sleep=0, normal=1, forced=2, the 4 reserved bits as padding or const 0, and DRDY (bit 0)
