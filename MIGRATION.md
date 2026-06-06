# RICE v2 — Porting Workflow

Complete guide for porting RICE v2 (PicoC embedded C interpreter + FreeRTOS) to different MCU platforms.

---

## 1. Architecture Layers

```
┌───────────────────────────────────────────────────────────────────┐
│  Layer 1: PicoC Interpreter Core (fully reusable, no changes)     │
│  Files: picoc/*.c (lex.c, parse.c, expression.c, heap.c,        │
│         table.c, type.c, variable.c, clibrary.c, include.c,      │
│         debug.c, platform.c)                                      │
│  Note: Pure C logic, no hardware API dependencies                 │
├───────────────────────────────────────────────────────────────────┤
│  Layer 2: Platform Adaptation (porting focus: rewrite per chip)   │
│  Files: picoc/platform/platform_*.c — PicoC I/O bridge            │
│         picoc/platform/library_*.c  — Peripheral function bindings│
│         picoc/platform.h            — Heap size, feature flags    │
│         picoc/picoc.h               — Platform macro guard        │
│         picoc/interpreter.h         — Picoc_Struct sizing         │
├───────────────────────────────────────────────────────────────────┤
│  Layer 3: Application Layer (minor adaptation)                    │
│  Files: Core/Src/picoc_app.c   — REPL state machine (~98% reuse) │
│         Core/Src/serial_app.c  — Serial DMA driver (~80% reuse)  │
│         Core/Src/freertos.c    — Task definitions (~85% reuse)   │
│         Core/Inc/task_msg.h    — Inter-task messages (100% reuse)│
├───────────────────────────────────────────────────────────────────┤
│  Layer 4: HAL Driver Layer (CubeMX-generated, full rewrite)       │
│  Files: main.c, usart.c, dma.c, gpio.c, stm32h7xx_it.c,        │
│         stm32h7xx_hal_msp.c, stm32h7xx_hal_conf.h,              │
│         stm32h7xx_hal_timebase_tim.c, system_stm32h7xx.c        │
│         startup_stm32h750xx.s, FreeRTOSConfig.h                   │
└───────────────────────────────────────────────────────────────────┘
```

---

## 2. File Dependency Matrix

### Files that MUST be fully rewritten (CubeMX-generated)

| File | Lines | Reason for rewrite |
|------|-------|--------------------|
| `main.c` | 274 | Clock tree (HSE 25MHz → PLL → 480MHz), MPU, power config (LDO/SMPS), HAL callback routing |
| `usart.c` | 190 | UART peripheral init, DMA stream assignment (DMA1_Stream0 RX circular, DMA1_Stream1 TX normal), GPIO AF mapping (PA9/PA10 AF7), NVIC priorities |
| `dma.c` | 58 | DMA clock enable, IRQ priority setup |
| `gpio.c` | 66 | GPIO clock enable, pin config (PC13 output) |
| `stm32h7xx_it.c` | 224 | Interrupt vector names (DMA1_Stream0_IRQHandler, USART1_IRQHandler, TIM6_DAC_IRQHandler) |
| `stm32h7xx_hal_msp.c` | 83 | Global MSP init (SYSCFG clock, PendSV priority) |
| `stm32h7xx_hal_timebase_tim.c` | 132 | TIM6 as HAL timebase (required when FreeRTOS owns SysTick) |
| `stm32h7xx_hal_conf.h` | 515 | HAL module selection, HSE_VALUE, VDD_VALUE |
| `system_stm32h7xx.c` | ~450 | CMSIS system init (FPU enable, Flash latency, RCC reset) |
| `FreeRTOSConfig.h` | 172 | `CMSIS_device_header`, `configCPU_CLOCK_HZ`, `configTOTAL_HEAP_SIZE`, NVIC priority bits, `vPortSVCHandler` mapping |
| Startup file | — | Vector table (interrupt entry addresses) |
| Linker script | — | Memory region definitions |

### Files that need minor modifications

| File | What to change | What to keep |
|------|---------------|-------------|
| `serial_app.c` | `huart1` → new UART handle; `USART1` → new peripheral instance; `HAL_UARTEx_ReceiveToIdle_DMA` / `HAL_UART_Transmit_DMA` → new HAL API | Ring buffer logic (rx_ring/tx_ring/head/tail), `SerialApp_Read`/`SerialApp_Write`, `SerialApp_ProcessRxDma`, `SerialApp_RxRingWrite`, `SerialApp_TxDmaKick`, `g_debug_input_active` flag |
| `serial_app.h` | `UART_HandleTypeDef` type (if switching away from STM32 HAL) | Public API declarations, `g_debug_input_active` |
| `picoc/platform.h` | Add `#ifdef YOUR_MCU_HOST` block, set `HEAP_SIZE` per target RAM | Hash table sizes (97), `PARAMETER_MAX`, `LINEBUFFER_MAX`, prompt strings |
| `picoc/picoc.h` | Add `defined(YOUR_MCU_HOST)` to `#if` guard | Public API prototypes |
| `freertos.c` | Task stack sizes per target RAM, queue depth | Task loop logic, CMSIS-RTOS V2 API calls |
| `picoc_app.c` | `osDelay(1)` → target RTOS yield (if not using CMSIS-RTOS V2) | Entire state machine (REPL/LOAD/DRAIN), protocol parsing, source completeness analysis, auto-expression wrapping |

### Fully reusable files (zero modifications)

| File | Lines | Description |
|------|-------|-------------|
| `picoc_app.h` | 49 | Application layer API declarations |
| `task_msg.h` | 30 | Inter-task message struct |
| `picoc/debug.c` | 783 | Debugger (breakpoint hash table, step, eval, variable inspection) |
| `picoc/parse.c` | ~1000 | Parser (with AbortRequested checkpoints) |
| `picoc/expression.c` | ~2000 | Expression evaluator |
| `picoc/lex.c` | ~800 | Lexer |
| `picoc/heap.c` | ~200 | Memory allocator |
| `picoc/table.c` | ~300 | Hash table implementation |
| `picoc/type.c` | ~500 | Type system |
| `picoc/variable.c` | ~400 | Variable management |
| `picoc/clibrary.c` | ~200 | C standard library init |
| `picoc/platform.c` | ~100 | Platform-agnostic wrappers |
| `picoc/include.c` | ~200 | `#include` system |
| `picoc/cstdlib/*.c` | 9 files | Mini C stdlib (ctype, errno, math, stdbool, stdio, stdlib, string, time, unistd) |

---

## 3. Step-by-Step Porting Guide

### Step 1: CubeMX Project Setup

Create a CubeMX project for the target MCU with these peripherals:

**UART Configuration:**
- Mode: Asynchronous
- Baud: 115200, 8N1, no hardware flow control
- Pins: Select available TX/RX pins (current H750 uses PA9/PA10)
- GPIO speed: Very High

**DMA Configuration (critical):**
- RX DMA: Circular mode, byte alignment, high priority, FIFO disabled
- TX DMA: Normal mode, byte alignment, medium priority, FIFO disabled
- Must support DMA circular receive (otherwise must switch to interrupt mode)

**NVIC Configuration (v2 key difference):**
- DMA RX/TX interrupt priority: ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (currently 5)
- USART interrupt priority: same (≥ 5)
- **Reason:** FreeRTOS requires ISRs calling `FromISR` APIs to have priority ≥ 5. v1 bare-metal uses priority 0 — must adjust when porting to v2.

**FreeRTOS Configuration:**
- Interface: CMSIS_V2
- Timebase: SysTick (for FreeRTOS kernel)
- HAL timebase: Must use a separate timer (e.g., TIM6), cannot share with SysTick

**Clock Configuration:**
- HSE: Configure external crystal frequency (current 25 MHz)
- PLL: Calculate target SYSCLK (current 480 MHz)
- Note voltage scaling (Scale 0 is H7-series specific)

### Step 2: HAL Timebase Timer

When FreeRTOS uses SysTick as kernel timebase, HAL's `HAL_Delay()` and `HAL_IncTick()` need a separate timer.

**Current implementation (TIM6):**
```c
// stm32h7xx_hal_timebase_tim.c
HAL_InitTick(TICK_INT_PRIORITY):
  1. HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TickPriority, 0)
  2. __HAL_RCC_TIM6_CLK_ENABLE()
  3. Calculate prescaler: (PCLK1_freq / 10000) - 1
  4. Set period: 10000 - 1 (produces 1ms interrupt)
  5. HAL_TIM_Base_Init() + HAL_TIM_Base_Start_IT()
```

**When porting:**
- Select a basic timer (no output pins, e.g., TIM6/TIM7)
- If target MCU lacks basic timers, use a general-purpose timer
- Configure for 1ms interrupt period
- Call `HAL_IncTick()` in the interrupt handler

### Step 3: FreeRTOSConfig.h

Key settings to modify:

| Setting | Current value | Porting change |
|---------|---------------|----------------|
| `CMSIS_device_header` | `"stm32h7xx.h"` | Change to target chip header |
| `configCPU_CLOCK_HZ` | `SystemCoreClock` | Usually no change (auto-reads) |
| `configTOTAL_HEAP_SIZE` | 49152 (48 KB) | Adjust per target RAM |
| `configENABLE_FPU` | 1 | 1 if target has FPU, 0 if not |
| `configPRIO_BITS` | `__NVIC_PRIO_BITS` or 4 | Per target MCU's NVIC priority bits |
| `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` | 5 | Do not change (keep 5, ensures DMA/UART ISRs can call FromISR APIs) |
| `vPortSVCHandler` | `SVC_Handler` | Confirm target MCU exception handler names match |

**FreeRTOS Port Selection:**
- Cortex-M7/M4/M3: Use `portable/RVDS/ARM_CM4F/` (with FPU) or `ARM_CM3/` (no FPU)
- Cortex-M0/M0+: Use `portable/RVDS/ARM_CM0/`
- RISC-V/Xtensa etc.: Use corresponding port directory

### Step 4: Serial Driver Adaptation (serial_app.c)

**3 hardware call sites to modify:**

```c
// 1. DMA receive start (original: HAL_UARTEx_ReceiveToIdle_DMA)
SerialApp_StartRxDma():
  HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma_buffer, 1024)
  // Change to new UART handle

// 2. DMA transmit start (original: HAL_UART_Transmit_DMA)
SerialApp_TxDmaKick():
  HAL_UART_Transmit_DMA(&huart1, &tx_ring[tx_tail], len)
  // Change to new UART handle

// 3. Peripheral instance filter
SerialApp_RxEventCallback():
  if (huart->Instance == USART1)  // Change to new peripheral instance
```

**What NOT to modify:**
- Ring buffer arrays (rx_ring[8192], tx_ring[8192], rx_dma_buffer[1024])
- `SerialApp_Read()` / `SerialApp_Write()` — pure buffer operations
- `SerialApp_ProcessRxDma()` — DMA-to-ring copy with wrap-around handling
- `SerialApp_RxRingWrite()` — ring buffer write
- `SerialApp_TxDmaKick()` — TX DMA trigger logic (only the HAL call line changes)
- `g_debug_input_active` flag — hardware-independent debug coordination
- `osMutexNew` / `osMutexAcquire` / `osMutexRelease` — portable CMSIS-RTOS V2 APIs

**Non-STM32 serial adaptation:**
If target is not STM32 (no HAL), you need to:
1. Implement equivalent DMA circular receive (or switch to interrupt receive + callback filling rx_ring)
2. Implement equivalent DMA transmit (or switch to interrupt transmit + reading from tx_ring)
3. Call `xTaskNotifyFromISR()` in the receive completion callback to wake serialTask

### Step 5: Platform I/O Adaptation (platform_*.c)

Create `picoc/platform/platform_your_mcu.c` implementing these 8 functions:

```c
// Must implement 2 low-level I/O primitives:
int PicocApp_ConsoleGetCharBlocking(void);  // Blocking single-char read
// Implementation: loop calling SerialApp_Read(), osDelay(1) when no data
// Must check AbortRequested flag (see below)

void SerialApp_Write(const uint8_t *data, uint32_t len);
// Already implemented in serial_app.c, no rewrite needed

// The following can be copied directly from existing platform_stm32h7.c:
char *PlatformGetLineQuiet(char *Buf, int MaxLen);  // Silent line read
char *PlatformGetLine(char *Buf, int MaxLen, const char *Prompt);  // Echo line read
int PlatformGetCharacter(void);  // Calls PicocApp_ConsoleGetCharBlocking
void PlatformPutc(unsigned char ch, union OutputStreamInfo *Stream);  // Calls SerialApp_Write
void PlatformInit(Picoc *pc);  // Empty function
void PlatformCleanup(Picoc *pc);  // Empty function
void PlatformExit(Picoc *pc, int RetVal);  // longjmp(pc->PicocExitBuf, 1)
char *PlatformReadFile(Picoc *pc, const char *FileName);  // Returns error
void PicocPlatformScanFile(Picoc *pc, const char *FileName);  // Returns error
```

**v2-specific: AbortRequested check in PicocApp_ConsoleGetCharBlocking:**
```c
int PicocApp_ConsoleGetCharBlocking(void)
{
    uint8_t ch;
    Picoc *active = (g_active_picoc != NULL) ? g_active_picoc : &g_picoc;
    while (SerialApp_Read(&ch, 1U) == 0U)
    {
        if (active->AbortRequested)  // v2 cooperative abort check
        {
            PlatformExit(active, 1);
        }
        osDelay(1);  // Yield CPU to serialTask
    }
    return (int)ch;
}
```

### Step 6: Peripheral Function Bindings (library_*.c)

Create `picoc/platform/library_your_mcu.c` to expose target MCU peripheral functions to PicoC scripts.

**Three-step registration:**

```c
// Step 1: Write wrapper function
static void PicocHalDelay(struct ParseState *Parser,
                           struct Value *ReturnValue,
                           struct Value **Param, int NumArgs)
{
    (void)Parser; (void)ReturnValue; (void)NumArgs;
    HAL_Delay((uint32_t)Param[0]->Val->Integer);
}

// Step 2: Declare function prototype (for PicoC parser)
const char HalDelay[] = "void delay(int ms);";

// Step 3: Add to registration table
const LibraryFunction Stm32Functions[] = {
    { PicocHalDelay, HalDelay },
    // ... other functions
    { NULL, NULL }
};

void PlatformLibraryInit(Picoc *pc)
{
    IncludeRegister(pc, "stm32.h", &Stm32Defs, &Stm32Functions[0], NULL);
}
```

**Recommended peripheral bindings:**

| Peripheral | Wrapper example | PicoC prototype |
|-----------|----------------|-----------------|
| GPIO | `PicocHalGpioWritePin` | `void digitalWrite(void *port, int pin, int state)` |
| GPIO | `PicocHalGpioReadPin` | `int digitalRead(void *port, int pin)` |
| GPIO | `PicocHalGpioInit` | `void pinMode(void *port, int pin, int mode, int pull, int speed)` |
| Delay | `PicocHalDelay` | `void delay(int ms)` |
| ADC | `PicocAnalogRead` | `int analogRead(int channel)` |
| PWM | `PicocAnalogWrite` | `void analogWrite(int pin, int duty)` |
| I2C | `PicocI2cWrite` | `int i2cWrite(int addr, int *data, int len)` |
| SPI | `PicocSpiTransfer` | `int spiTransfer(int cs, int *tx, int *rx, int len)` |

**Constants to register:**
- GPIO port addresses (GPIOA ~ GPIOn)
- GPIO pin numbers (GPIO_PIN_0 ~ GPIO_PIN_15)
- GPIO modes (INPUT/OUTPUT_PP/OUTPUT_OD/ANALOG)
- GPIO pull (NOPULL/PULLUP/PULLDOWN)
- GPIO speed (LOW/MEDIUM/HIGH/VERY_HIGH)

### Step 7: PicoC Configuration (platform.h)

Add a platform configuration block in `picoc/platform.h`:

```c
#ifdef YOUR_MCU_HOST
# define BUILTIN_MINI_STDLIB        // Use PicoC's built-in mini stdlib
# define HEAP_SIZE (64*1024)        // PicoC heap size (adjust per target RAM)
# define PICOC_MATH_LIBRARY         // Enable math functions
# define FEATURE_AUTO_DECLARE_VARIABLES  // Allow implicit int declarations
# define FANCY_ERROR_MESSAGES       // Detailed error messages
#endif
```

**HEAP_SIZE adjustment guide:**

| Target MCU SRAM | Recommended HEAP_SIZE | Notes |
|----------------|----------------------|-------|
| ≥ 256 KB | 64 KB | Current H750 config |
| 128~256 KB | 32 KB | Smaller heap, features mostly intact |
| 64~128 KB | 16 KB | Minimum usable, complex scripts may fail |
| < 64 KB | Not recommended | PicoC itself needs substantial memory |

### Step 8: Interrupt Priority Adjustment (v2 key)

v2 uses FreeRTOS, interrupt priorities must satisfy:

```
configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5

Priority 0-4: Highest priority, cannot call FreeRTOS FromISR APIs
Priority 5-15: Can call FreeRTOS FromISR APIs

Current v2 configuration:
  DMA1_Stream0 (RX): priority 5  ← can call xTaskNotifyFromISR
  DMA1_Stream1 (TX): priority 5  ← can call xTaskNotifyFromISR
  USART1: priority 5             ← can call FromISR APIs
  TIM6 (HAL timebase): priority 15  ← only calls HAL_IncTick
  SysTick: priority 15           ← FreeRTOS kernel
  PendSV: priority 15            ← FreeRTOS context switch
  SVC: priority 0                ← FreeRTOS system calls
```

**When porting, ensure:**
- DMA/UART interrupt priority ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`
- PendSV must be lowest priority (15)
- SVC priority must be 0

### Step 9: Build Configuration

**Keil MDK:**

Preprocessor defines:
```
USE_PWR_LDO_SUPPLY    // STM32 power mode (adjust per target MCU)
USE_HAL_DRIVER        // Enable HAL library
STM32H750xx           // Chip model macro (change to target)
YOUR_MCU_HOST         // PicoC platform selection macro
```

Include paths:
```
../Core/Inc
../Drivers/<target_series>_HAL_Driver/Inc
../Drivers/CMSIS/Device/ST/<target_series>/Include
../Drivers/CMSIS/Include
../picoc
../picoc/platform
../picoc/cstdlib
../Middlewares/Third_Party/FreeRTOS/Source/include
../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
../Middlewares/Third_Party/FreeRTOS/Source/portable/RVDS/<corresponding_port>
../Drivers/CMSIS/RTOS2/Include
```

Source groups (7 groups):
1. **Startup** — startup_<chip>.s
2. **App/Core** — serial_app.c, picoc_app.c, main.c, gpio.c, freertos.c, dma.c, usart.c, stm32<series>_it.c, stm32<series>_hal_msp.c, stm32<series>_hal_timebase_tim.c
3. **App/PicoC** — clibrary.c, debug.c, expression.c, heap.c, include.c, lex.c, parse.c, platform.c, table.c, type.c, variable.c, platform_<chip>.c, library_<chip>.c
4. **HAL Driver** — hal_uart, hal_dma, hal_gpio, hal_rcc, hal_flash, hal_pwr, hal_cortex, hal_tim, etc.
5. **CMSIS** — system_<series>.c
6. **PicoC/cstdlib** — ctype.c, errno.c, math.c, stdbool.c, stdio.c, stdlib.c, string.c, time.c, unistd.c
7. **FreeRTOS** — port.c, heap_4.c, tasks.c, queue.c, list.c, timers.c, event_groups.c, stream_buffer.c, cmsis_os2.c

**GCC / CMake:**
```cmake
# FreeRTOS port directory (select per target core)
set(FREERTOS_PORT "GCC/ARM_CM4F" CACHE STRING "")  # Cortex-M7/M4 with FPU

# PicoC source files
file(GLOB PICOCSRC picoc/*.c picoc/platform/*.c picoc/cstdlib/*.c)

# Compile definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    USE_HAL_DRIVER
    STM32H750xx       # Change to target model
    YOUR_MCU_HOST     # PicoC platform selection
)
```

### Step 10: Memory Budget Verification

Before porting, calculate whether the target MCU has enough RAM:

**v2 Memory Budget:**

| Consumer | Size | Source |
|----------|------|--------|
| FreeRTOS heap | 48 KB | `configTOTAL_HEAP_SIZE` (dynamic allocation) |
| ├─ serialTask stack | 4 KB | Allocated from FreeRTOS heap |
| ├─ picocTask stack | 32 KB | Allocated from FreeRTOS heap |
| ├─ Message queue (16×68B) | 1088 B | Allocated from FreeRTOS heap |
| └─ Mutex + Timer task | ~1.1 KB | Allocated from FreeRTOS heap |
| PicoC heap (`HeapMemory`) | 64 KB | Static array in `Picoc_Struct` |
| RX DMA buffer | 1 KB | Static in `serial_app.c` |
| RX ring buffer | 8 KB | Static in `serial_app.c` |
| TX ring buffer | 8 KB | Static in `serial_app.c` |
| Source line buffer | 2 KB | Static in `picoc_app.c` |
| Load buffer | 8 KB | Static in `picoc_app.c` |
| ISR stack + globals | ~2 KB | Static allocation |
| **Total** | **~144 KB** | |

**Minimum RAM required: 192 KB**

**If target RAM is insufficient, adjust:**

| Parameter | Current | Minimum | Impact |
|-----------|---------|---------|--------|
| `HEAP_SIZE` | 64 KB | 16 KB | Complex scripts may run out of memory |
| `rx_ring` / `tx_ring` | 8 KB each | 2 KB each | High-speed data transfer may overflow |
| `g_load_buffer` | 8 KB | 2 KB | Large file upload limited |
| `configTOTAL_HEAP_SIZE` | 48 KB | 24 KB | Reduce picocTask stack |
| picocTask stack | 32 KB | 16 KB | Deep recursion may stack overflow |

---

## 4. MCU Family Porting Reference

### Between STM32 Families

| Source | Target | Effort | Key differences |
|--------|--------|--------|-----------------|
| STM32H750 | STM32H743 | Minimal | Same family, change model macro and Flash size |
| STM32H750 | STM32F407 | Medium | Different DMA controller (Stream vs Channel), no FIFO threshold config, different clock tree, no MPU (can remove MPU_Config) |
| STM32H750 | STM32F103 | Large | No DMA circular receive (must use interrupt mode), no `HAL_UARTEx_ReceiveToIdle_DMA` (must manually detect IDLE), completely different clock tree |
| STM32H750 | STM32G0B1 | Medium | Simpler DMA, no MPU, different clock tree |
| STM32H750 | STM32L476 | Medium | Low-power series, different DMA controller, different clock tree |

### Cross-Vendor Porting

| Vendor | Recommended approach | Serial adaptation strategy |
|--------|---------------------|---------------------------|
| NXP (LPC/IMXRT) | MCUXpresso + LPUART + eDMA | Rewrite serial_app.c HAL calls, reuse ring buffers |
| Nuvoton (M480) | NuMaker + UART + PDMA | HAL style similar to STM32, minimal changes |
| RP2040 | Pico SDK + UART + DMA | Simple DMA API, `uart_read_blocking`/`uart_write_blocking` can substitute |
| ESP32 | ESP-IDF + UART + DMA | Native FreeRTOS support, `uart_driver_install` + `uart_read_bytes` |
| GD32 | GD32 HAL + USART + DMA | Highly compatible with STM32 HAL, minimal changes |

---

## 5. Porting Verification Checklist

Verify in this bottom-up order. Only proceed to the next step after the current one passes:

| Step | What to verify | Test method | Expected result |
|------|---------------|-------------|-----------------|
| 1 | Basic serial communication | `SerialApp_Read` + `SerialApp_Write` echo loop | Input echoed back |
| 2 | REPL mode | Enter `printf("hello\n");` | Output `hello` |
| 3 | Expression evaluation | Enter `3 + 5 * 2` | Output `13` |
| 4 | Multi-line input | Enter `for` loop | Auto-detect completeness, execute |
| 5 | File upload | `:load` → source → `:end` | Execute and output results |
| 6 | Heartbeat | `:ping` | Receive `:pong` |
| 7 | Reset | `:reset` | Receive `:ok` |
| 8 | Abort script | Execute `while(1){}`, then `:abort` | Receive `:err aborted`, return to REPL |
| 9 | Heartbeat during script | Send `:ping` while `while(1){}` runs | Receive `:pong` |
| 10 | Set breakpoint | `:bkpt serial_load 5` | Receive `:ok bkpt` |
| 11 | Breakpoint hit | Upload script with breakpoint, execute | Receive `:break serial_load 5 0` |
| 12 | Single-step | `:step` | Receive `:step serial_load 6 0` |
| 13 | Continue | `:cont` | Continue to next breakpoint or end |
| 14 | Expression eval (debug) | `:eval x + 1` | Output evaluation result |
| 15 | Variable inspection | `:vars` | Receive `:var` list and `:ok vars` |
| 16 | Variable modification | `:set x 100` | Receive `:ok set` |
| 17 | Multiple breakpoints | Set 3 breakpoints, hit each | All pause and continue correctly |
| 18 | Peripheral bindings | Call `delay(100)` in PicoC script | Normal delay |
| 19 | Stress test | Upload 68+ PicoC test cases | All pass |

---

## 6. FAQ

**Q: Compile error `undefined reference to PlatformXxx`**
A: Platform adaptation functions not implemented. Check that `platform_your_mcu.c` contains all 8 required functions.

**Q: Compile error `undefined reference to xTaskNotifyFromISR`**
A: `configUSE_TASK_NOTIFICATIONS` not enabled in FreeRTOS config. Add `#define configUSE_TASK_NOTIFICATIONS 1` in `FreeRTOSConfig.h`.

**Q: Serial can receive but not transmit**
A: Check DMA TX configuration, verify `HAL_UART_TxCpltCallback` routes to `SerialApp_TxCpltCallback`, confirm NVIC interrupt is enabled.

**Q: REPL unresponsive, no `picoc>` prompt**
A: Check `main()` call order: `SerialApp_Init()` → `PicocApp_Init()` → `osKernelInitialize()` → `MX_FREERTOS_Init()` → `osKernelStart()`.

**Q: No output after file upload**
A: Check that the source code defines a `main()` function. PicoC auto-calls `main()`.

**Q: `:abort` doesn't work, script can't be interrupted**
A: Check that `AbortRequested` checkpoints are correctly added in `parse.c`. Confirm `PicocApp_Abort()` is called by serialTask and sets `g_picoc.AbortRequested = 1`.

**Q: Breakpoints don't work**
A: 1) Confirm breakpoint filename matches upload filename (default `serial_load`). 2) Confirm `DebugCheckStatement()` is called before every statement (check call sites in `picoc/parse.c`). 3) Confirm `g_debug_input_active` flag is correctly set when breakpoint is hit.

**Q: Stack overflow (HardFault)**
A: 1) Increase `configTOTAL_HEAP_SIZE` (FreeRTOS heap). 2) Increase picocTask stack size. 3) Increase `HEAP_SIZE` (PicoC heap). 4) Check target MCU total SRAM ≥ 192 KB.

**Q: Compile error `jmp_buf` undefined**
A: Confirm compiler supports `<setjmp.h>`. ARMCC, GCC, IAR all support it. Confirm platform macro guard in `picoc/picoc.h` includes your target macro.

**Q: HardFault immediately after FreeRTOS starts**
A: 1) Check stack alignment (Cortex-M requires 8-byte alignment). 2) Check `configTOTAL_HEAP_SIZE` doesn't exceed actual available RAM. 3) Check startup file heap/stack size settings.
