---
tags: [skill-creator]
max_turns: 40
timeout_seconds: 1200
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

In my GMP schema I have this field for a humidity sensor:

```yaml
      - { name: humidity, type: u16, scale: 0.01, unit: "%RH", max: 10000 }
```

Valid readings are 0 to 100 %RH. Anything outside means the sensor is broken, so I added `max: 10000` to make decode reject it. gmpc accepts the schema, but a reading of 0xFFFF still decodes fine. What am I doing wrong, and how do I get out-of-range readings rejected?
