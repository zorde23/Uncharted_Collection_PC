---
name: build-test
description: Build the Uncharted port (MSYS2 CLANG64, build.sh) and run every C/C++ and Python test that works on Windows, reporting only new failures. Use before every commit and after any C/C++ change.
---

Run from the Bash tool:

```bash
bash .claude/skills/build-test/run.sh              # build + all tests
bash .claude/skills/build-test/run.sh readback     # build + only tests whose name contains "readback"
bash .claude/skills/build-test/run.sh --build-only
```

- Exit 2: the game is open. Never close it yourself unless this session launched it; ask the user.
- Exit 1: build failed (`out/gpu-build.log`) or a test outside the known-broken list failed
  (`out/test-build.log`, `out/<test>.log`). Diagnose with debugging-and-error-recovery before committing.
- Exit 0: safe to commit.

The known-broken list lives in `run.sh` (`KNOWN`). When a change fixes one of those tests, remove it
from the list in the same commit; never add a test to it to get a green run without the user's okay.
