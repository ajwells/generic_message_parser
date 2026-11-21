# GMP (generic_message_parser)

Start every session by reading `docs/session-log.md`. It has the feature inventory, design decisions, known issues,
planned tasks, toolchain setup and test commands. At the end of a session, update its "Session history" and any
sections that changed.

Essentials:
- Build and test: `cmake --build build && ctest --test-dir build` (with the CLion MinGW/Ninja/CMake on PATH; see the log).
- After an intentional generator change: `GMP_UPDATE_GOLDEN=1 ctest --test-dir build -R golden`.
- A feature is complete when generator, runtime, Studio (`tools/schema-studio`), docs (`README.md`, `docs/schema.md`)
  and tests all cover it; refresh the AI skill with `py skills/gmp/build_references.py`.
- This directory is a git repository (branch `master`). Commit only when asked.
