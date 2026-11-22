---
type: llm
focus: trace
weight: 1
---

rx.cpp feeds the received chunks to the generated stream parser (Stream, make_stream or CallbackStream) with handlers for Heartbeat and SetLed
