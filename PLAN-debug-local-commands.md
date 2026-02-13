# Plan: Local Command Dispatch in Debug Console

## Problem

The `fx3_cmd debug` interactive console holds the USB interface
exclusively.  To issue host-side commands (like `stop_gpif_state`,
`stats`, `start`, `stop`) you must Ctrl-C out of the debug session,
run the command as a separate `fx3_cmd` invocation, then re-enter
`fx3_cmd debug`.  This is painful during live debugging.

## Approach

Add a `!` escape prefix to the debug console loop, following the
established serial-console escape convention (SSH `~`, gdb `!`).

When the user types `!` during the debug session:

1. The loop enters **local command mode** — prints a `fx3> ` prompt
2. Subsequent keystrokes are echoed locally and buffered (not sent to device)
3. On Enter, the buffered text is parsed and dispatched to the
   corresponding `do_*()` function using the already-open USB handle
4. Debug output polling (READINFODEBUG) continues between keystrokes
5. After command completion, the loop returns to normal console mode
6. Backspace edits the buffer; Ctrl-C / Escape cancels

## Available Local Commands

All existing `fx3_cmd` subcommands except `debug` itself:

| No-arg commands | With-arg commands |
|-----------------|-------------------|
| `test`, `start`, `stop`, `reset` | `adc <freq>`, `att <val>`, `vga <val>` |
| `stats`, `stats_i2c`, `stats_pib`, `stats_pll` | `gpio <bits>`, `raw <code>` |
| `stop_gpif_state`, `stop_start_cycle` | `i2cr <addr> <reg> <len>` |
| `pll_preflight`, `wedge_recovery` | `i2cw <addr> <reg> <byte>...` |
| `ep0_overflow`, `oob_brequest`, `oob_setarg` | |
| `console_fill`, `debug_race`, `debug_poll` | |
| `pib_overflow`, `stack_check` | |

`!help` or `!?` prints the command list.

## Changes

| File | Change |
|------|--------|
| `tests/fx3_cmd.c` | Add `dispatch_local_cmd()` function and local-mode state machine in `do_debug()` loop |
| `SDDC_FX3/docs/debugging.md` | Document the `!` escape mechanism in section 6.1 |

## Notes

- During local command execution (which may block for seconds on
  tests like `wedge_recovery`), debug output polling is paused.
  This is acceptable — the user explicitly initiated the command.
- Re-entering debug mode (`TESTFX3 wValue=1`) from within local
  commands is idempotent.
- `do_debug` is excluded from the dispatch table (would recurse).
- `do_reset` is available but will disconnect the device, requiring
  Ctrl-C to exit the debug loop afterward.
