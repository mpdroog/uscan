# Claude Code Guidelines for uscan

## Code Quality Rules

- Never use workarounds that mask the real problem (`_Exit`, try/catch to silence errors, disabling warnings, etc.)
- Before fixing a bug, explain the root cause first
- If you don't understand why something crashes, investigate more before proposing a fix
- Prefer a failing test over a hack that makes it pass
