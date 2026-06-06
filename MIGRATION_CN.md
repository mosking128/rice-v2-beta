# RICE v2 移植工作流 / Porting Workflow

将 RICE v2（PicoC 嵌入式 C 解释器 + FreeRTOS）移植到不同 MCU 平台的完整指南。

---

## 1. 架构分层

```
┌───────────────────────────────────────────────────────────────────┐
│  层 1：PicoC 解释器核心（完全复用，无需修改）                        │
│  文件：picoc/*.c（lex.c, parse.c, expression.c, heap.c,          │
│        table.c, type.c, variable.c, clibrary.c, include.c,       │
│        debug.c, platform.c）                                      │
│  特点：纯 C 逻辑，不依赖任何硬件 API                                │
├───────────────────────────────────────────────────────────────────┤
│  层 2：平台适配层（移植重点：需为新芯片重写）                        │
│  文件：picoc/platform/platform_*.c — PicoC I/O 桥接                │
│        picoc/platform/library_*.c  — 外设函数绑定                  │
│        picoc/platform.h            — 堆大小、功能开关               │
│        picoc/picoc.h               — 平台宏守卫                    │
│        picoc/interpreter.h         — Picoc_Struct 大小             │
├───────────────────────────────────────────────────────────────────┤
│  层 3：应用层（少量适配）                                           │
│  文件：Core/Src/picoc_app.c   — REPL 状态机、协议解析（~98% 复用）   │
│        Core/Src/serial_app.c  — 串口 DMA 收发（~80% 复用）          │
│        Core/Src/freertos.c    — 任务定义、队列（~85% 复用）          │
│        Core/Inc/task_msg.h    — 任务间消息（100% 复用）              │
├───────────────────────────────────────────────────────────────────┤
│  层 4：HAL 驱动层（CubeMX 生成，需完全重写）                        │
│  文件：main.c, usart.c, dma.c, gpio.c, stm32h7xx_it.c,           │
│        stm32h7xx_hal_msp.c, stm32h7xx_hal_conf.h,                │
│        stm32h7xx_hal_timebase_tim.c, system_stm32h7xx.c          │
│        startup_stm32h750xx.s, FreeRTOSConfig.h                    │
└───────────────────────────────────────────────────────────────────┘
```

---

## 2. 文件依赖矩阵

### 必须完全重写的文件（CubeMX 生成）

| 文件 | 行数 | 重写原因 |
|------|------|---------|
| `main.c` | 274 | 时钟树（HSE 25MHz → PLL → 480MHz）、MPU、电源配置（LDO/SMPS）、HAL 回调路由 |
| `usart.c` | 190 | UART 外设初始化、DMA 流分配（DMA1_Stream0 RX 循环、DMA1_Stream1 TX 普通）、GPIO AF 映射（PA9/PA10 AF7）、NVIC 优先级 |
| `dma.c` | 58 | DMA 时钟使能、IRQ 优先级设置 |
| `gpio.c` | 66 | GPIO 时钟使能、引脚配置（PC13 输出） |
| `stm32h7xx_it.c` | 224 | 中断向量名（DMA1_Stream0_IRQHandler、USART1_IRQHandler、TIM6_DAC_IRQHandler） |
| `stm32h7xx_hal_msp.c` | 83 | 全局 MSP 初始化（SYSCFG 时钟、PendSV 优先级） |
| `stm32h7xx_hal_timebase_tim.c` | 132 | TIM6 作为 HAL 时基（FreeRTOS 占用 SysTick 时必须用独立定时器） |
| `stm32h7xx_hal_conf.h` | 515 | HAL 模块选择、HSE_VALUE、VDD_VALUE |
| `system_stm32h7xx.c` | ~450 | CMSIS 系统初始化（FPU 使能、Flash 延迟、RCC 复位） |
| `FreeRTOSConfig.h` | 172 | `CMSIS_device_header`、`configCPU_CLOCK_HZ`、`configTOTAL_HEAP_SIZE`、NVIC 优先级位数、`vPortSVCHandler` 映射 |
| 启动文件 | - | 向量表（中断入口地址） |
| 链接脚本 | - | 内存区域定义 |

### 需要少量修改的文件

| 文件 | 需要改什么 | 不需要改什么 |
|------|-----------|-------------|
| `serial_app.c` | `huart1` → 新 UART 句柄；`USART1` → 新外设实例；`HAL_UARTEx_ReceiveToIdle_DMA` / `HAL_UART_Transmit_DMA` → 新 HAL API | 环形缓冲区逻辑（rx_ring/tx_ring/head/tail）、`SerialApp_Read`/`SerialApp_Write`、`SerialApp_ProcessRxDma`、`SerialApp_RxRingWrite`、`SerialApp_TxDmaKick`、`g_debug_input_active` 标志 |
| `serial_app.h` | `UART_HandleTypeDef` 类型（如果换非 STM32 HAL） | 公共 API 声明、`g_debug_input_active` |
| `picoc/platform.h` | 添加 `#ifdef YOUR_MCU_HOST` 块、设置 `HEAP_SIZE`（根据目标 RAM 调整） | 哈希表大小（97）、`PARAMETER_MAX`、`LINEBUFFER_MAX`、提示符字符串 |
| `picoc/picoc.h` | 在 `#if` 守卫中添加 `defined(YOUR_MCU_HOST)` | 公共 API 原型 |
| `freertos.c` | 任务栈大小（根据目标 RAM 调整）、队列深度 | 任务循环逻辑、CMSIS-RTOS V2 API 调用 |
| `picoc_app.c` | `osDelay(1)` → 目标 RTOS 的 yield 函数（如果不用 CMSIS-RTOS V2） | 整个状态机（REPL/LOAD/DRAIN）、协议解析、源码完整性分析、表达式自动包装 |

### 完全可复用的文件（零修改）

| 文件 | 行数 | 说明 |
|------|------|------|
| `picoc_app.h` | 49 | 应用层 API 声明 |
| `task_msg.h` | 30 | 任务间消息结构体 |
| `picoc/debug.c` | 783 | 调试器（断点哈希表、单步、求值、变量监视） |
| `picoc/parse.c` | ~1000 | 解析器（含 AbortRequested 检查点） |
| `picoc/expression.c` | ~2000 | 表达式求值器 |
| `picoc/lex.c` | ~800 | 词法分析器 |
| `picoc/heap.c` | ~200 | 内存分配器 |
| `picoc/table.c` | ~300 | 哈希表实现 |
| `picoc/type.c` | ~500 | 类型系统 |
| `picoc/variable.c` | ~400 | 变量管理 |
| `picoc/clibrary.c` | ~200 | C 标准库初始化 |
| `picoc/platform.c` | ~100 | 平台无关包装 |
| `picoc/include.c` | ~200 | `#include` 系统 |
| `picoc/cstdlib/*.c` | 9 文件 | 迷你 C 标准库（ctype, errno, math, stdbool, stdio, stdlib, string, time, unistd） |

---

## 3. 逐步移植指南

### 第 1 步：CubeMX 工程配置

在 CubeMX 中为新 MCU 创建工程，配置以下外设：

**UART 配置：**
- 模式：Asynchronous（异步）
- 波特率：115200，数据位 8，停止位 1，无校验
- 引脚：选择可用的 TX/RX 引脚（当前 H750 使用 PA9/PA10）
- GPIO 速度：Very High（高速）

**DMA 配置（关键）：**
- RX DMA：循环模式（Circular）、字节对齐、高优先级、FIFO 禁用
- TX DMA：普通模式（Normal）、字节对齐、中优先级、FIFO 禁用
- 必须支持 DMA 循环接收（否则需要改用中断模式）

**NVIC 配置（v2 关键区别）：**
- DMA RX/TX 中断优先级：≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`（当前设为 5）
- USART 中断优先级：同上（≥ 5）
- **原因：** FreeRTOS 要求调用 `FromISR` API 的中断优先级必须 ≥ 5。v1 裸机版优先级为 0，移植到 v2 时必须调整。

**FreeRTOS 配置：**
- 接口：CMSIS_V2
- 时基：SysTick（FreeRTOS 内核用）
- HAL 时基：必须改为独立定时器（如 TIM6），不能与 SysTick 共用

**时钟配置：**
- HSE：配置外部晶振频率（当前 25 MHz）
- PLL：计算目标 SYSCLK（当前 480 MHz）
- 注意电压等级（Scale 0 仅 H7 系列需要）

### 第 2 步：HAL 时基定时器

FreeRTOS 使用 SysTick 作为内核时基时，HAL 的 `HAL_Delay()` 和 `HAL_IncTick()` 需要一个独立定时器。

**当前实现（TIM6）：**
```c
// stm32h7xx_hal_timebase_tim.c
HAL_InitTick(TICK_INT_PRIORITY):
  1. HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TickPriority, 0)
  2. __HAL_RCC_TIM6_CLK_ENABLE()
  3. 计算预分频值：(PCLK1_freq / 10000) - 1
  4. 配置周期：10000 - 1（产生 1ms 中断）
  5. HAL_TIM_Base_Init() + HAL_TIM_Base_Start_IT()
```

**移植时：**
- 选择一个基本定时器（无输出引脚的定时器，如 TIM6/TIM7）
- 如果目标 MCU 没有基本定时器，可以用通用定时器
- 配置为 1ms 中断周期
- 在中断中调用 `HAL_IncTick()`

### 第 3 步：FreeRTOSConfig.h

需要修改的关键配置：

| 配置项 | 当前值 | 移植时修改 |
|--------|--------|-----------|
| `CMSIS_device_header` | `"stm32h7xx.h"` | 改为目标芯片头文件 |
| `configCPU_CLOCK_HZ` | `SystemCoreClock` | 通常不需要改（自动读取） |
| `configTOTAL_HEAP_SIZE` | 49152 (48 KB) | 根据目标 RAM 调整 |
| `configENABLE_FPU` | 1 | 有 FPU 设为 1，无 FPU 设为 0 |
| `configPRIO_BITS` | `__NVIC_PRIO_BITS` 或 4 | 根据目标 MCU 的 NVIC 优先级位数 |
| `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` | 5 | 不改（保持 5，确保 DMA/UART 中断可调用 FromISR API） |
| `vPortSVCHandler` | `SVC_Handler` | 确认目标 MCU 的异常处理函数名一致 |

**FreeRTOS Port 选择：**
- Cortex-M7/M4/M3：使用 `portable/RVDS/ARM_CM4F/`（有 FPU）或 `ARM_CM3/`（无 FPU）
- Cortex-M0/M0+：使用 `portable/RVDS/ARM_CM0/`
- RISC-V/Xtensa 等：使用对应的 port 目录

### 第 4 步：串口驱动适配（serial_app.c）

**需要修改的 3 处硬件调用：**

```c
// 1. DMA 接收启动（原：HAL_UARTEx_ReceiveToIdle_DMA）
SerialApp_StartRxDma():
  HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma_buffer, 1024)
  // 改为新 UART 句柄

// 2. DMA 发送启动（原：HAL_UART_Transmit_DMA）
SerialApp_TxDmaKick():
  HAL_UART_Transmit_DMA(&huart1, &tx_ring[tx_tail], len)
  // 改为新 UART 句柄

// 3. 外设实例过滤
SerialApp_RxEventCallback():
  if (huart->Instance == USART1)  // 改为新外设实例
```

**不需要修改的部分：**
- 环形缓冲区数组（rx_ring[8192]、tx_ring[8192]、rx_dma_buffer[1024]）
- `SerialApp_Read()` / `SerialApp_Write()` — 纯缓冲区操作
- `SerialApp_ProcessRxDma()` — DMA 到环形缓冲区的拷贝逻辑（含回卷处理）
- `SerialApp_RxRingWrite()` — 环形缓冲区写入
- `SerialApp_TxDmaKick()` — TX DMA 触发逻辑（仅 HAL 调用行需改）
- `g_debug_input_active` 标志 — 与硬件无关的调试协调机制
- `osMutexNew` / `osMutexAcquire` / `osMutexRelease` — CMSIS-RTOS V2 API，可移植

**非 STM32 平台的串口适配：**
如果目标不是 STM32（无 HAL），需要：
1. 实现等效的 DMA 循环接收（或改为中断接收 + 回调填充 rx_ring）
2. 实现等效的 DMA 发送（或改为中断发送 + 从 tx_ring 取数据）
3. 在接收完成回调中调用 `xTaskNotifyFromISR()` 唤醒 serialTask

### 第 5 步：平台 I/O 适配（platform_*.c）

创建 `picoc/platform/platform_your_mcu.c`，实现以下 8 个函数：

```c
// 必须实现的 2 个底层 I/O 原语：
int PicocApp_ConsoleGetCharBlocking(void);  // 阻塞读取一个字符
// 实现：循环调用 SerialApp_Read()，无数据时 osDelay(1) 让出 CPU
// 必须检查 AbortRequested 标志（见下方说明）

void SerialApp_Write(const uint8_t *data, uint32_t len);
// 已在 serial_app.c 中实现，无需重写

// 以下函数可直接复用现有 platform_stm32h7.c 的实现：
char *PlatformGetLineQuiet(char *Buf, int MaxLen);  // 无回显读一行
char *PlatformGetLine(char *Buf, int MaxLen, const char *Prompt);  // 带回显读一行
int PlatformGetCharacter(void);  // 调用 PicocApp_ConsoleGetCharBlocking
void PlatformPutc(unsigned char ch, union OutputStreamInfo *Stream);  // 调用 SerialApp_Write
void PlatformInit(Picoc *pc);  // 空函数
void PlatformCleanup(Picoc *pc);  // 空函数
void PlatformExit(Picoc *pc, int RetVal);  // longjmp(pc->PicocExitBuf, 1)
char *PlatformReadFile(Picoc *pc, const char *FileName);  // 返回错误
void PicocPlatformScanFile(Picoc *pc, const char *FileName);  // 返回错误
```

**v2 特有：PicocApp_ConsoleGetCharBlocking 中的 AbortRequested 检查：**
```c
int PicocApp_ConsoleGetCharBlocking(void)
{
    uint8_t ch;
    Picoc *active = (g_active_picoc != NULL) ? g_active_picoc : &g_picoc;
    while (SerialApp_Read(&ch, 1U) == 0U)
    {
        if (active->AbortRequested)  // v2 协作式中断检查
        {
            PlatformExit(active, 1);
        }
        osDelay(1);  // 让出 CPU 给 serialTask
    }
    return (int)ch;
}
```

### 第 6 步：外设函数绑定（library_*.c）

创建 `picoc/platform/library_your_mcu.c`，将目标 MCU 的外设函数暴露给 PicoC 脚本。

**三步注册流程：**

```c
// 第 1 步：写包装函数
static void PicocHalDelay(struct ParseState *Parser,
                           struct Value *ReturnValue,
                           struct Value **Param, int NumArgs)
{
    (void)Parser; (void)ReturnValue; (void)NumArgs;
    HAL_Delay((uint32_t)Param[0]->Val->Integer);
}

// 第 2 步：声明函数原型（PicoC 解析器使用）
const char HalDelay[] = "void delay(int ms);";

// 第 3 步：在注册表中添加
const LibraryFunction Stm32Functions[] = {
    { PicocHalDelay, HalDelay },
    // ... 其他函数
    { NULL, NULL }
};

void PlatformLibraryInit(Picoc *pc)
{
    IncludeRegister(pc, "stm32.h", &Stm32Defs, &Stm32Functions[0], NULL);
}
```

**建议暴露的外设函数：**

| 外设 | 包装函数示例 | PicoC 原型 |
|------|------------|-----------|
| GPIO | `PicocHalGpioWritePin` | `void digitalWrite(void *port, int pin, int state)` |
| GPIO | `PicocHalGpioReadPin` | `int digitalRead(void *port, int pin)` |
| GPIO | `PicocHalGpioInit` | `void pinMode(void *port, int pin, int mode, int pull, int speed)` |
| 延时 | `PicocHalDelay` | `void delay(int ms)` |
| ADC | `PicocAnalogRead` | `int analogRead(int channel)` |
| PWM | `PicocAnalogWrite` | `void analogWrite(int pin, int duty)` |
| I2C | `PicocI2cWrite` | `int i2cWrite(int addr, int *data, int len)` |
| SPI | `PicocSpiTransfer` | `int spiTransfer(int cs, int *tx, int *rx, int len)` |

**需要注册的常量：**
- GPIO 端口地址（GPIOA ~ GPIOn）
- GPIO 引脚编号（GPIO_PIN_0 ~ GPIO_PIN_15）
- GPIO 模式（INPUT/OUTPUT_PP/OUTPUT_OD/ANALOG）
- GPIO 上拉/下拉（NOPULL/PULLUP/PULLDOWN）
- GPIO 速度（LOW/MEDIUM/HIGH/VERY_HIGH）

### 第 7 步：PicoC 配置（platform.h）

在 `picoc/platform.h` 中添加新平台的配置块：

```c
#ifdef YOUR_MCU_HOST
# define BUILTIN_MINI_STDLIB        // 使用 PicoC 内置迷你标准库
# define HEAP_SIZE (64*1024)        // PicoC 堆大小（根据目标 RAM 调整）
# define PICOC_MATH_LIBRARY         // 启用数学函数
# define FEATURE_AUTO_DECLARE_VARIABLES  // 允许隐式 int 声明
# define FANCY_ERROR_MESSAGES       // 详细错误信息
#endif
```

**HEAP_SIZE 调整指南：**

| 目标 MCU SRAM | 推荐 HEAP_SIZE | 说明 |
|--------------|---------------|------|
| ≥ 256 KB | 64 KB | 当前 H750 配置 |
| 128~256 KB | 32 KB | 减小堆，功能基本完整 |
| 64~128 KB | 16 KB | 最小可用，复杂脚本可能失败 |
| < 64 KB | 不建议移植 | PicoC 本身需要较大内存 |

### 第 8 步：中断优先级调整（v2 关键）

v2 使用 FreeRTOS，中断优先级必须满足以下约束：

```
configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5

优先级 0-4：最高优先级，不能调用 FreeRTOS FromISR API
优先级 5-15：可以调用 FreeRTOS FromISR API

当前 v2 配置：
  DMA1_Stream0 (RX)：优先级 5  ← 可调用 xTaskNotifyFromISR
  DMA1_Stream1 (TX)：优先级 5  ← 可调用 xTaskNotifyFromISR
  USART1：优先级 5             ← 可调用 FromISR API
  TIM6 (HAL 时基)：优先级 15   ← 仅调用 HAL_IncTick
  SysTick：优先级 15           ← FreeRTOS 内核
  PendSV：优先级 15            ← FreeRTOS 上下文切换
  SVC：优先级 0                ← FreeRTOS 系统调用
```

**移植时必须确保：**
- DMA/UART 中断优先级 ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`
- PendSV 必须是最低优先级（15）
- SVC 优先级必须为 0

### 第 9 步：编译配置

**Keil MDK：**

预处理宏定义：
```
USE_PWR_LDO_SUPPLY    // STM32 电源模式（根据目标 MCU 调整）
USE_HAL_DRIVER        // 启用 HAL 库
STM32H750xx           // 芯片型号宏（改为目标型号）
YOUR_MCU_HOST         // PicoC 平台选择宏
```

包含路径：
```
../Core/Inc
../Drivers/<目标系列>_HAL_Driver/Inc
../Drivers/CMSIS/Device/ST/<目标系列>/Include
../Drivers/CMSIS/Include
../picoc
../picoc/platform
../picoc/cstdlib
../Middlewares/Third_Party/FreeRTOS/Source/include
../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
../Middlewares/Third_Party/FreeRTOS/Source/portable/RVDS/<对应Port>
../Drivers/CMSIS/RTOS2/Include
```

源文件分组（7 组）：
1. **启动** — startup_<芯片>.s
2. **应用/核心** — serial_app.c, picoc_app.c, main.c, gpio.c, freertos.c, dma.c, usart.c, stm32<系列>_it.c, stm32<系列>_hal_msp.c, stm32<系列>_hal_timebase_tim.c
3. **应用/PicoC** — clibrary.c, debug.c, expression.c, heap.c, include.c, lex.c, parse.c, platform.c, table.c, type.c, variable.c, platform_<芯片>.c, library_<芯片>.c
4. **HAL 驱动** — hal_uart, hal_dma, hal_gpio, hal_rcc, hal_flash, hal_pwr, hal_cortex, hal_tim 等
5. **CMSIS** — system_<系列>.c
6. **PicoC/cstdlib** — ctype.c, errno.c, math.c, stdbool.c, stdio.c, stdlib.c, string.c, time.c, unistd.c
7. **FreeRTOS** — port.c, heap_4.c, tasks.c, queue.c, list.c, timers.c, event_groups.c, stream_buffer.c, cmsis_os2.c

**GCC / CMake：**
```cmake
# FreeRTOS port 目录（根据目标核心选择）
set(FREERTOS_PORT "GCC/ARM_CM4F" CACHE STRING "")  # Cortex-M7/M4 with FPU

# PicoC 源文件
file(GLOB PICOCSRC picoc/*.c picoc/platform/*.c picoc/cstdlib/*.c)

# 编译定义
target_compile_definitions(${PROJECT_NAME} PRIVATE
    USE_HAL_DRIVER
    STM32H750xx       # 改为目标型号
    YOUR_MCU_HOST     # PicoC 平台选择
)
```

### 第 10 步：内存预算验证

移植前必须计算目标 MCU 的 RAM 是否足够：

**v2 内存预算：**

| 消费者 | 大小 | 来源 |
|--------|------|------|
| FreeRTOS 堆 | 48 KB | `configTOTAL_HEAP_SIZE`（动态分配） |
| ├─ serialTask 栈 | 4 KB | 从 FreeRTOS 堆分配 |
| ├─ picocTask 栈 | 32 KB | 从 FreeRTOS 堆分配 |
| ├─ 消息队列 (16×68B) | 1088 B | 从 FreeRTOS 堆分配 |
| └─ 互斥锁 + 定时器任务 | ~1.1 KB | 从 FreeRTOS 堆分配 |
| PicoC 堆 (`HeapMemory`) | 64 KB | `Picoc_Struct` 中的静态数组 |
| RX DMA 缓冲区 | 1 KB | `serial_app.c` 静态分配 |
| RX 环形缓冲区 | 8 KB | `serial_app.c` 静态分配 |
| TX 环形缓冲区 | 8 KB | `serial_app.c` 静态分配 |
| 源码行缓冲区 | 2 KB | `picoc_app.c` 静态分配 |
| 加载缓冲区 | 8 KB | `picoc_app.c` 静态分配 |
| ISR 栈 + 全局变量 | ~2 KB | 静态分配 |
| **总计** | **~144 KB** | |

**最低 RAM 要求：192 KB**

**如果目标 RAM 不足，可以调整：**

| 参数 | 当前值 | 最小值 | 调整影响 |
|------|--------|--------|---------|
| `HEAP_SIZE` | 64 KB | 16 KB | 复杂脚本可能内存不足 |
| `rx_ring` / `tx_ring` | 各 8 KB | 各 2 KB | 高速数据传输可能溢出 |
| `g_load_buffer` | 8 KB | 2 KB | 大文件上传受限 |
| `configTOTAL_HEAP_SIZE` | 48 KB | 24 KB | 减小 picocTask 栈 |
| picocTask 栈 | 32 KB | 16 KB | 深层递归可能栈溢出 |

---

## 4. 芯片族移植参考

### STM32 系列间移植

| 原芯片 | 目标芯片 | 改动量 | 关键差异 |
|--------|---------|--------|---------|
| STM32H750 | STM32H743 | 极小 | 同系列，改型号宏和 Flash 大小即可 |
| STM32H750 | STM32F407 | 中 | DMA 控制器不同（Stream vs Channel）、无 FIFO 阈值配置、时钟树不同、无 MPU（可删除 MPU_Config） |
| STM32H750 | STM32F103 | 大 | 无 DMA 循环接收（需改用中断模式）、无 `HAL_UARTEx_ReceiveToIdle_DMA`（需手动检测 IDLE）、时钟树完全不同 |
| STM32H750 | STM32G0B1 | 中 | DMA 较简单、无 MPU、时钟树不同 |
| STM32H750 | STM32L476 | 中 | 低功耗系列、DMA 控制器不同、时钟树不同 |

### 跨厂商移植

| 厂商 | 推荐方案 | 串口适配策略 |
|------|---------|-------------|
| NXP (LPC/IMXRT) | MCUXpresso + LPUART + eDMA | 重写 serial_app.c 的 HAL 调用，环形缓冲区复用 |
| Nuvoton (M480) | NuMaker + UART + PDMA | HAL 风格类似 STM32，改动较小 |
| RP2040 | Pico SDK + UART + DMA | DMA API 简单，`uart_read_blocking` / `uart_write_blocking` 可替代 |
| ESP32 | ESP-IDF + UART + DMA | FreeRTOS 原生支持，`uart_driver_install` + `uart_read_bytes` |
| GD32 | GD32 HAL + USART + DMA | 与 STM32 HAL 高度兼容，改动极小 |

---

## 5. 移植验证清单

按以下顺序逐层验证，每步通过后再进入下一步：

| 步骤 | 验证内容 | 测试方法 | 预期结果 |
|------|---------|---------|---------|
| 1 | 串口基本通信 | `SerialApp_Read` + `SerialApp_Write` 循环回显 | 输入什么回显什么 |
| 2 | REPL 模式 | 输入 `printf("hello\n");` | 输出 `hello` |
| 3 | 表达式求值 | 输入 `3 + 5 * 2` | 输出 `13` |
| 4 | 多行输入 | 输入 `for` 循环 | 自动检测完整性并执行 |
| 5 | 文件上传 | `:load` → 源码 → `:end` | 执行并输出结果 |
| 6 | 心跳检测 | `:ping` | 收到 `:pong` |
| 7 | 重置 | `:reset` | 收到 `:ok` |
| 8 | 中断脚本 | 执行 `while(1){}` 后发 `:abort` | 收到 `:err aborted`，回到 REPL |
| 9 | 脚本运行中心跳 | 执行 `while(1){}` 时发 `:ping` | 收到 `:pong` |
| 10 | 设置断点 | `:bkpt serial_load 5` | 收到 `:ok bkpt` |
| 11 | 断点命中 | 上传含断点的脚本并执行 | 收到 `:break serial_load 5 0` |
| 12 | 单步执行 | `:step` | 收到 `:step serial_load 6 0` |
| 13 | 继续执行 | `:cont` | 继续到下一个断点或结束 |
| 14 | 表达式求值（调试中） | `:eval x + 1` | 输出求值结果 |
| 15 | 变量监视 | `:vars` | 收到 `:var` 列表和 `:ok vars` |
| 16 | 修改变量 | `:set x 100` | 收到 `:ok set` |
| 17 | 多断点 | 设置 3 个断点，逐个命中 | 全部正常暂停和继续 |
| 18 | 外设绑定 | PicoC 脚本中调用 `delay(100)` | 正常延时 |
| 19 | 压力测试 | 上传 68+ PicoC 测试用例 | 全部通过 |

---

## 6. 常见问题

**Q: 编译报 `undefined reference to PlatformXxx`**
A: 未实现平台适配函数。检查 `platform_your_mcu.c` 是否包含所有 8 个必需函数。

**Q: 编译报 `undefined reference to xTaskNotifyFromISR`**
A: FreeRTOS 配置中未启用 `configUSE_TASK_NOTIFICATIONS`。在 `FreeRTOSConfig.h` 中添加 `#define configUSE_TASK_NOTIFICATIONS 1`。

**Q: 串口能收不能发**
A: 检查 DMA TX 配置是否正确、`HAL_UART_TxCpltCallback` 是否路由到 `SerialApp_TxCpltCallback`、NVIC 中断是否使能。

**Q: REPL 无响应，看不到 `picoc>` 提示符**
A: 检查 `main()` 中调用顺序：`SerialApp_Init()` → `PicocApp_Init()` → `osKernelInitialize()` → `MX_FREERTOS_Init()` → `osKernelStart()`。

**Q: 文件上传后无输出**
A: 检查源码中是否定义了 `main()` 函数。PicoC 会自动调用 `main()`。

**Q: `:abort` 不生效，脚本无法中断**
A: 检查 `parse.c` 中的 `AbortRequested` 检查点是否正确添加。确认 `PicocApp_Abort()` 被 serialTask 调用时设置了 `g_picoc.AbortRequested = 1`。

**Q: 断点不生效**
A: 1) 确认断点文件名与上传文件名一致（默认 `serial_load`）。2) 确认 `DebugCheckStatement()` 在每个语句前被调用（检查 `picoc/parse.c` 中的调用点）。3) 确认 `g_debug_input_active` 标志在断点命中时被正确设置。

**Q: 堆栈溢出（HardFault）**
A: 1) 增大 `configTOTAL_HEAP_SIZE`（FreeRTOS 堆）。2) 增大 picocTask 栈大小。3) 增大 `HEAP_SIZE`（PicoC 堆）。4) 检查目标 MCU 的总 SRAM 是否 ≥ 192 KB。

**Q: 编译报 `jmp_buf` 未定义**
A: 确认编译器支持 `<setjmp.h>`。ARMCC、GCC、IAR 均支持。确认 `picoc/picoc.h` 中的平台宏守卫包含了你的目标宏。

**Q: FreeRTOS 启动后立即 HardFault**
A: 1) 检查堆栈对齐（Cortex-M 要求 8 字节对齐）。2) 检查 `FreeRTOSConfig.h` 中的 `configTOTAL_HEAP_SIZE` 是否超过实际可用 RAM。3) 检查启动文件中的堆栈大小设置。
