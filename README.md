# RICE v2 — Runtime Interactive C Environment

[English](README.md) | [中文](README_CN.md)

RICE v2 ports the **PicoC** C interpreter to **STM32H750VBTx** (Cortex-M7, 480 MHz) on top of **FreeRTOS**, providing a multi-task interactive C scripting environment with full debug support — breakpoints, single-stepping, variable inspection, and expression evaluation. The key architectural improvement over v1 is task-level isolation: serial I/O and script execution run as separate FreeRTOS tasks, enabling responsive `:abort` of infinite loops without a watchdog reset.

Connect via `USART1` (115200 8N1) with any serial terminal and start writing C interactively.

## What's New in v2

| Feature | v1 (bare-metal) | v2 (FreeRTOS) |
|---------|-----------------|----------------|
| Architecture | Super-loop | Multi-task (FreeRTOS) |
| `:abort` on `while(1){}` | Hangs MCU | Cooperative abort kills script |
| `:ping` during script run | Unresponsive | Always responds |
| Serial I/O | Main loop polls | Dedicated high-priority task |
| Script execution | Blocks everything | Isolated task, lower priority |

### Cooperative Abort Mechanism

The core innovation in v2: PicoC's loop structures (`while`, `for`, `do-while`) and blocking I/O (`scanf`, `getchar`) check an `AbortRequested` flag on each iteration. When the serial task receives `:abort`, it sets this flag. The interpreter detects it, unwinds via `setjmp/longjmp` within its own task, cleans up, and returns to the REPL prompt — all without affecting the serial task.

## Architecture

```
Serial Task (priority 40)                PicoC Task (priority 24)
┌──────────────────────────┐            ┌─────────────────────────────┐
│ SerialApp_Read() polling │──msgQ──→   │ osMessageQueueGet() block    │
│ Protocol parsing         │            │ REPL source execution        │
│ :ping / :abort immediate │            │ File upload execution        │
│ Non-protocol lines → Q   │            │ Debug command handling       │
└──────────────────────────┘            └─────────────────────────────┘
         │                                       │
         ▼                                       ▼
  DMA ISR → rx_ring[8192]              PicoC interpreter
  (lock-free SPSC)                     (setjmp/longjmp error recovery)
```

**ISR → Task data path:** DMA circular buffer → IDLE interrupt → ring buffer (lock-free SPSC) → serialTask polls `SerialApp_Read()` → protocol lines parsed, source lines queued → picocTask dequeues and executes.

**Abort path:** `:abort` → serialTask calls `PicocApp_Abort()` → sets `AbortRequested=1` → PicoC loop condition fails → `ProgramFail` → `PlatformExit` → `longjmp` back to task loop → REPL prompt.

## Quick Start

1. Open `MDK-ARM/UART_DMA_H750.uvprojx` in Keil MDK.
2. Build (F7) and flash via ST-Link.
3. Open a serial terminal on `USART1` at `115200 8N1`.
4. After boot and FreeRTOS scheduler start, you should see the `picoc>` prompt.

## Usage Guide

All interaction through plain-text serial commands. Any terminal emulator works.

### REPL Mode

The default mode. Type C statements directly and see results immediately.

```
picoc> int x = 42;
picoc> printf("x = %d\n", x);
x = 42
picoc> 3 + 5 * 2
13
```

Expressions are automatically printed. Multi-line code blocks are supported:

```
picoc> for (int i = 0; i < 3; i++) {
...     printf("i = %d\n", i);
... }
i = 0
i = 1
i = 2
```

### File Upload

Upload a complete C source file for execution. The file is buffered and executed in an isolated PicoC instance (does not pollute REPL state).

```
:load
#include <stdio.h>

int main() {
    for (int i = 0; i < 10; i++) {
        printf("count: %d\n", i);
    }
    return 0;
}
:end
```

`:load` enters upload mode (`load>` prompt). Send source lines, then `:end` to execute. Use `:abort` in upload mode to cancel without executing.

### Aborting Scripts

Unlike v1, v2 can interrupt running scripts at any time:

```
picoc> while(1) { printf("loop\n"); }
loop
loop
loop
:abort             ← send from terminal at any time
:err aborted
picoc>             ← immediately back to REPL
```

This works even for infinite loops, nested loops, and scripts blocking on I/O:

```
picoc> for(;;) { while(1) { printf("nested\n"); } }
nested
nested
:abort
:err aborted
picoc>
```

### Heartbeat

The `:ping` command is always responsive, even during script execution:

```
picoc> while(1) {}
:ping              ← send while loop is running
:pong              ← response arrives immediately
:abort
:err aborted
picoc>
```

### Reset

Reset the interpreter to a clean state (clears all variables, functions, and breakpoints):

```
:reset
:ok
picoc>
```

## Debugging

RICE v2 includes a full interactive debugger with breakpoints, single-stepping, variable inspection, and expression evaluation.

### Setting Breakpoints

Set a breakpoint on any line of an uploaded file. The filename for uploaded files is `serial_load`.

```
:bkpt serial_load 5
:ok bkpt
:bkpt serial_load 12
:ok bkpt
:bkpt serial_load 20
:ok bkpt
```

Multiple breakpoints can be set simultaneously. When execution reaches a breakpoint line, the debugger pauses and reports the location:

```
:break serial_load 5 0
```

Clear a specific breakpoint:

```
:bkptclear serial_load 5
:ok bkptclear
```

### Single-Stepping

After hitting a breakpoint, use `:step` to execute one statement at a time:

```
:break serial_load 5 0       ← breakpoint hit
:step
:step serial_load 6 0        ← advanced to next line
:step
:step serial_load 7 0        ← and the next
```

Each `:step` response shows the new execution position (filename, line, column).

### Continue Execution

Resume normal execution until the next breakpoint or program end:

```
:break serial_load 12 0      ← breakpoint hit
:cont
:ok ready                    ← script finished
picoc>
```

### Evaluating Expressions

Evaluate any C expression in the current scope while paused at a breakpoint:

```
:break serial_load 10 0
:eval x + 1
43
:ok eval
:eval arr[2]
99
:ok eval
```

### Inspecting Variables

List all visible variables (locals and globals) with their types and values:

```
:vars
:var i 5
:var x 42
:var c A
:var p 0x20001000
:ok vars
```

Type characters: `i`=int, ``=short, `c`=char, `l`=long, `I`=unsigned int, `S`=unsigned short, `C`=unsigned char, `L`=unsigned long, `f`=float, `p`=pointer.

### Modifying Variables

Change a variable's value while paused:

```
:break serial_load 10 0
:set x 100
:ok set
:eval x
100
:ok eval
```

Supports all integer types, floats, pointers, and character literals (`'A'`).

### Debug Workflow Example

Complete debugging session:

```
:load
#include <stdio.h>

int main() {
    int sum = 0;
    for (int i = 1; i <= 5; i++) {
        sum += i;
    }
    printf("sum = %d\n", sum);
    return 0;
}
:end
```

Set breakpoints and run:

```
:bkpt serial_load 5
:ok bkpt
:bkpt serial_load 7
:ok bkpt
:end                          ← start execution
:break serial_load 5 0        ← paused at line 5
:vars
:var sum 0
:ok vars
:step
:step serial_load 6 0         ← entered loop
:step
:step serial_load 7 0         ← at sum += i
:eval sum
1
:ok eval
:eval i
1
:ok eval
:cont                         ← continue to next breakpoint
:break serial_load 7 0        ← hit again (loop iteration)
:eval sum
6
:ok eval
:cont
:ok ready                     ← script finished
picoc>
```

### Debug Command Reference

| Command |描述|
|---------|-------------|
| `:bkpt <file> <line>` | Set breakpoint |
| `:bkptclear <file> <line>` | Clear breakpoint |
| `:cont` | Continue execution |
| `:step` | Single-step one statement |
| `:eval <expr>` | Evaluate C expression in current scope |
| `:vars` | List visible variables |
| `:set <name> <value>` | Modify a variable |

## Protocol Reference

### Host → Device Commands

| Command |描述|
|---------|-------------|
| `:load [size]` | Enter file upload mode. Optional `size` for buffer pre-check. |
| `:end` | Execute uploaded source |
| `:abort` | Abort running script (cooperative) |
| `:ping` | Heartbeat (always responds, even during script execution) |
| `:reset` | Reset PicoC interpreter |
| `:bkpt <file> <line>` | Set breakpoint at line in file |
| `:bkptclear <file> <line>` | Clear breakpoint |
| `:cont` | Continue execution after breakpoint |
| `:step` | Single-step one statement |
| `:eval <expr>` | Evaluate expression in current debug scope |
| `:vars` | Enumerate all visible variables |
| `:set <name> <value>` | Modify variable value |

### Device → Host Responses

| Response |描述|
|----------|-------------|
| `:ok [data]` | Success. Data: `ready`, `bkpt`, `bkptclear`, `eval`, `set`, `vars` |
| `:err <msg>` | Error with message |
| `:pong` | Response to `:ping` |
| `:break <file> <line> <col>` | Execution paused at breakpoint |
| `:step <file> <line> <col>` | Execution paused after single-step |
| `:var <type> <name> <value>` | Variable data (one per line, between `:vars` and `:ok vars`) |

## Repository Layout

```
├── Core/               STM32 application code (serial, PicoC app, FreeRTOS tasks)
├── picoc/              PicoC interpreter source plus STM32 platform port
├── Drivers/            STM32 HAL and CMSIS drivers
├── Middlewares/        FreeRTOS kernel (CMSIS_V2, V10.6.2)
├── MDK-ARM/            Keil MDK project files
├── README.md
├── README_CN.md
└── LICENSE
```

## Technical Details

- **MCU:** STM32H750VBTx (Cortex-M7, 480 MHz, 128 KB SRAM)
- **RTOS:** FreeRTOS CMSIS_V2, Kernel V10.6.2, heap_4 (30 KB heap)
- **Tasks:** serialTask (prio 40, 4 KB stack), picocTask (prio 24, 16 KB stack)
- **IPC:** CMSIS_V2 message queue (16 items of ~68 bytes each)
- **Serial stack:** DMA circular (1 KB) → ring buffers (8 KB RX + 8 KB TX, lock-free SPSC) → task-level processing
- **PicoC heap:** 64 KB (platform.h)
- **Error recovery:** `setjmp/longjmp` within picocTask — script failures return to REPL cleanly
- **Abort safety:** Cooperative flag check, no cross-task `longjmp` (undefined behavior avoided), same-task unwind only

## Known Limitations

- Task priorities and stack sizes are initial estimates; may need tuning for complex workloads
- No power management / sleep modes enabled

## Related Projects

- [RICE v1](https://github.com/mosking128/rice-v1) — Bare-metal release

## License

MIT. See [LICENSE](LICENSE).
