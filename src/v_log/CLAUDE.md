# v_log

Logging facility. Not a plan.md-specified module (plan.md just says
`V_LOG(level, module, ...)` with levels `DEV, DEBUG, INFO, ERR, WARNING,
CRIT` and modules `PFCP, DPDB, MEM`) — this is the concrete
implementation Stage 1 needed since, unlike the sibling pfcp-endpoint
project, nothing here provides `V_LOG` externally at link time.

## Public API (`inc/v_log.h`)

- `V_LOG(level, module, fmt, ...)` — the macro every other module uses.
  `level` is one of `DEV/DEBUG/INFO/WARNING/ERR/CRIT` (no quotes, no
  `V_LOG_` prefix — the macro adds it). `module` is a string literal:
  `"PFCP"` (session/txn/flow/dispatch control logic), `"DPDB"`
  (Redis/DB/id-alloc/retrans-cache logic), or `"MEM"` (mempool/ctx
  allocation logic) — see the header for the full mapping convention.
- `v_log_set_min_level(level)` — filters what actually prints. Tests
  default to `WARNING` (override via `VPE_TEST_LOG_LEVEL=DEV|DEBUG|INFO`
  env var, see `test/main_test.c`).

## Invariants an agent must not break

- **Never define a competing `V_LOG` macro or logging wrapper.** Every
  module includes `v_log.h` and calls the one macro.
- Stick to the three module strings above even when a new module's
  natural label would be something else (e.g. `v_txn`, `v_dispatch`) —
  map it to whichever of PFCP/DPDB/MEM fits closest, per each module's
  own `CLAUDE.md`. This keeps log filtering/grepping consistent instead
  of accumulating a module string per source file.

## Tests

No dedicated test file — exercised implicitly by every other module's
tests (log output is visible in `make test`'s output and was used
throughout Stage 1 to diagnose real bugs, e.g. the NOSCRIPT recovery
and TEID-local-0 bugs were both first spotted in this module's output).
