# RICE v2 鈥?Runtime Interactive C Environment (Beta)

> **Status: Beta.** RICE v2 is under active development. For production use, see [RICE v1](https://github.com/mosking128/rice-v1) (stable, bare-metal).

[English](README.md) | [涓枃](README_CN.md)

RICE v2 ports the **PicoC** C interpreter to **STM32H750VBTx** (Cortex-M7, 480 MHz) on top of **FreeRTOS**, providing a multi-task interactive C scripting environment. The key architectural improvement over v1 is task-level isolation: serial I/O and script execution run as separate FreeRTOS tasks, enabling responsive `:abort` of infinite loops without a watchdog reset.

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

The core innovation in v2: PicoC's loop structures (`while`, `for`, `do-while`) and blocking I/O (`scanf`, `getchar`) check an `AbortRequested` flag on each iteration. When the serial task receives `:abort`, it sets this flag. The interpreter detects it, unwinds via `setjmp/longjmp` within its own task, cleans up, and returns to the REPL prompt 鈥?all without affecting the serial task.

## Architecture

```
Serial Task (priority 40)                PicoC Task (priority 24)
鈹屸攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹?           鈹屸攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹?鈹?SerialApp_Read() polling 鈹傗攢鈹€msgQ鈹€鈹€鈫?  鈹?osMessageQueueGet() block    鈹?鈹?Protocol parsing         鈹?           鈹?REPL source execution        鈹?鈹?:ping / :abort immediate 鈹?           鈹?File upload execution        鈹?鈹?Non-protocol lines 鈫?Q   鈹?           鈹?Debug command handling       鈹?鈹斺攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹?           鈹斺攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹?         鈹?                                      鈹?         鈻?                                      鈻?  DMA ISR 鈫?rx_ring[8192]              PicoC interpreter
  (lock-free SPSC)                     (setjmp/longjmp error recovery)
```

**ISR 鈫?Task data path:** DMA circular buffer 鈫?IDLE interrupt 鈫?ring buffer (lock-free SPSC) 鈫?serialTask polls `SerialApp_Read()` 鈫?protocol lines parsed, source lines queued 鈫?picocTask dequeues and executes.

**Abort path:** `:abort` 鈫?serialTask calls `PicocApp_Abort()` 鈫?sets `AbortRequested=1` 鈫?PicoC loop condition fails 鈫?`ProgramFail` 鈫?`PlatformExit` 鈫?`longjmp` back to task loop 鈫?REPL prompt.

## Current Status (Beta)

### Known Issue
- Breakpoint single-step (`:step`) and continue (`:cont`) are currently broken — investigating

### Done
- FreeRTOS (CMSIS_V2, Kernel V10.6.2) fully configured and compiling
- ISR priority layout for safe RTOS API calls from DMA/USART interrupts
- serialTask: protocol parsing, `:ping`/`:abort`/`:reset` handling, source line queuing
- PicoC cooperative abort checkpoints in `while`/`for`/`do-while` and blocking I/O
- Abort cleanup: interpreter reset after `longjmp`, no memory leaks



## Quick Start

1. Open `MDK-ARM/UART_DMA_H750.uvprojx` in Keil MDK.
2. Build (F7) and flash via ST-Link.
3. Open a serial terminal on `USART1` at `115200 8N1`.
4. After boot and FreeRTOS scheduler start, you should see the `picoc>` prompt.

## Manual Serial Usage

All interaction through plain-text serial commands. Any terminal emulator works.

### REPL Mode

```
picoc> int x = 42;
picoc> printf("x = %d\n", x);
x = 42
```

### File Upload

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

### Aborting Scripts

Unlike v1, v2 can interrupt running scripts:

```
picoc> while(1) { printf("loop\n"); }
loop
loop
loop
:abort             鈫?sent from terminal at any time
:err aborted
picoc>             鈫?immediately back to REPL
```

The `:abort` command is processed by the high-priority serial task, which sets a flag. The interpreter checks this flag on every loop iteration and cleanly exits.

### Protocol Commands

| Command | Description |
|---------|-------------|
| `:load [size]` | Enter file upload mode |
| `:end` | Execute uploaded source |
| `:abort` | Abort running script (cooperative) |
| `:ping` | Heartbeat (always responds, even during script execution) |
| `:reset` | Reset PicoC interpreter |

### Debug Commands

| Command | Description |
|---------|-------------|
| `:bkpt <file> <line>` | Set breakpoint |
| `:bkptclear <file> <line>` | Clear breakpoint |
| `:cont` | Continue execution |
| `:step` | Single-step one statement |
| `:eval <expr>` | Evaluate C expression in current scope |
| `:vars` | List visible variables |
| `:set <name> <value>` | Modify a variable |

## Repository Layout

```
鈹溾攢鈹€ Core/               STM32 application code (serial, PicoC app, FreeRTOS tasks)
鈹溾攢鈹€ picoc/              PicoC interpreter source plus STM32 platform port
鈹溾攢鈹€ Drivers/            STM32 HAL and CMSIS drivers
鈹溾攢鈹€ Middlewares/        FreeRTOS kernel (CMSIS_V2, V10.6.2)
鈹溾攢鈹€ MDK-ARM/            Keil MDK project files
鈹溾攢鈹€ README.md
鈹溾攢鈹€ README_CN.md
鈹斺攢鈹€ LICENSE
```

## Technical Details

- **MCU:** STM32H750VBTx (Cortex-M7, 480 MHz, 128 KB SRAM)
- **RTOS:** FreeRTOS CMSIS_V2, Kernel V10.6.2, heap_4 (30 KB heap)
- **Tasks:** serialTask (prio 40, 4 KB stack), picocTask (prio 24, 16 KB stack)
- **IPC:** CMSIS_V2 message queue (16 items of ~68 bytes each)
- **Serial stack:** DMA circular (1 KB) 鈫?ring buffers (8 KB RX + 8 KB TX, lock-free SPSC) 鈫?task-level processing
- **PicoC heap:** 64 KB (platform.h)
- **Error recovery:** `setjmp/longjmp` within picocTask 鈥?script failures return to REPL cleanly
- **Abort safety:** Cooperative flag check, no cross-task `longjmp` (undefined behavior avoided), same-task unwind only

## Known Limitations (Beta)

- Migration not yet complete 鈥?some features may be unstable
- Full PicoC test suite (68+ cases) not yet verified on hardware with FreeRTOS
- Task priorities and stack sizes are initial estimates; may need tuning
- No power management / sleep modes enabled

## Related Projects

- [RICE v1](https://github.com/mosking128/rice-v1) 鈥?Stable bare-metal release, recommended for production use

## License

MIT. See [LICENSE](LICENSE).
