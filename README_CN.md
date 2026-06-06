# RICE v2 — Runtime Interactive C Environment

[English](README.md) | [中文](README_CN.md)

RICE v2 将 **PicoC** C 语言解释器移植到 **STM32H750VBTx**（Cortex-M7, 480 MHz），运行在 **FreeRTOS** 之上，提供基于多任务的交互式 C 脚本环境，具备完整的调试支持——断点、单步执行、变量监视和表达式求值。相比 v1 的核心架构改进是任务级隔离：串口 I/O 和脚本执行作为独立的 FreeRTOS 任务运行，使得 `:abort` 能够响应式地中断死循环，无需看门狗复位。

用任意串口终端连接 `USART1`（115200 8N1），即可开始交互式 C 编程。

## v2 新特性

| 特性 | v1（裸机） | v2（FreeRTOS） |
|------|----------|----------------|
| 架构 | 超级循环 | 多任务（FreeRTOS） |
| `:abort` 中断 `while(1){}` | 卡死 MCU | 协作式中断，脚本退出 |
| 脚本运行中 `:ping` | 无响应 | 始终响应 |
| 串口 I/O | 主循环轮询 | 独立高优先级任务 |
| 脚本执行 | 阻塞一切 | 隔离任务，较低优先级 |

### 协作式中断机制

v2 的核心创新：PicoC 的循环结构（`while`、`for`、`do-while`）和阻塞 I/O（`scanf`、`getchar`）在每次迭代时检查 `AbortRequested` 标志。当串口任务收到 `:abort` 时设置该标志，解释器检测到后通过 `setjmp/longjmp` 在自己的任务内展开调用栈、清理资源并回到 REPL 提示符——整个过程不影响串口任务。

## 架构

```
串口任务 (优先级 40)                    PicoC 任务 (优先级 24)
┌──────────────────────────┐            ┌─────────────────────────────┐
│ SerialApp_Read() 轮询    │──消息队列→  │ osMessageQueueGet() 阻塞等待  │
│ 协议解析                  │            │ REPL 源码执行                 │
│ :ping / :abort 即时响应   │            │ 文件上传执行                  │
│ 非协议行 → 入队           │            │ 调试命令处理                  │
└──────────────────────────┘            └─────────────────────────────┘
         │                                       │
         ▼                                       ▼
  DMA ISR → rx_ring[8192]              PicoC 解释器
  (无锁 SPSC)                          (setjmp/longjmp 错误恢复)
```

**ISR → 任务数据通路：** DMA 环形缓冲 → IDLE 中断 → 环形缓冲区（无锁 SPSC）→ 串口任务轮询 `SerialApp_Read()` → 协议行解析，源码行入队 → PicoC 任务出队执行。

**中断通路：** `:abort` → 串口任务调用 `PicocApp_Abort()` → 设置 `AbortRequested=1` → PicoC 循环条件失败 → `ProgramFail` → `PlatformExit` → `longjmp` 跳回任务循环 → REPL 提示符。

## 快速开始

1. 用 Keil MDK 打开 `MDK-ARM/UART_DMA_H750.uvprojx`。
2. 编译（F7）并通过 ST-Link 烧录。
3. 用串口终端连接 `USART1`，波特率 `115200`，数据位 `8N1`。
4. 上电后 FreeRTOS 调度器启动，等待 `picoc>` 提示符。

## 使用指南

所有交互通过串口发送纯文本命令完成。任意串口终端均可使用。

### REPL 模式

默认模式。直接输入 C 语句，立即查看结果。

```
picoc> int x = 42;
picoc> printf("x = %d\n", x);
x = 42
picoc> 3 + 5 * 2
13
```

表达式自动打印结果。支持多行代码块：

```
picoc> for (int i = 0; i < 3; i++) {
...     printf("i = %d\n", i);
... }
i = 0
i = 1
i = 2
```

### 文件上传

上传完整的 C 源文件进行执行。文件在隔离的 PicoC 实例中运行，不会污染 REPL 环境。

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

`:load` 进入上传模式（显示 `load>` 提示符）。发送源码行后用 `:end` 执行。在上传模式中可用 `:abort` 取消上传，不执行。

### 中断脚本执行

与 v1 不同，v2 可以随时中断正在运行的脚本：

```
picoc> while(1) { printf("loop\n"); }
loop
loop
loop
:abort             ← 从终端随时发送
:err aborted
picoc>             ← 立即回到 REPL
```

即使在死循环、嵌套循环和阻塞 I/O 场景下也有效：

```
picoc> for(;;) { while(1) { printf("nested\n"); } }
nested
nested
:abort
:err aborted
picoc>
```

### 心跳检测

`:ping` 命令始终响应，即使脚本正在运行：

```
picoc> while(1) {}
:ping              ← 脚本运行期间发送
:pong              ← 立即响应
:abort
:err aborted
picoc>
```

### 重置

将解释器重置为干净状态（清除所有变量、函数和断点）：

```
:reset
:ok
picoc>
```

## 调试功能

RICE v2 包含完整的交互式调试器，支持断点、单步执行、变量监视和表达式求值。

### 设置断点

在上传文件的任意行设置断点。上传文件的文件名为 `serial_load`。

```
:bkpt serial_load 5
:ok bkpt
:bkpt serial_load 12
:ok bkpt
:bkpt serial_load 20
:ok bkpt
```

支持同时设置多个断点。当执行到断点行时，调试器暂停并报告位置：

```
:break serial_load 5 0
```

清除指定断点：

```
:bkptclear serial_load 5
:ok bkptclear
```

### 单步执行

命中断点后，使用 `:step` 逐条语句执行：

```
:break serial_load 5 0       ← 命中断点
:step
:step serial_load 6 0        ← 前进到下一行
:step
:step serial_load 7 0        ← 继续
```

每条 `:step` 响应显示新的执行位置（文件名、行号、列号）。

### 继续执行

恢复正常执行，直到下一个断点或程序结束：

```
:break serial_load 12 0      ← 命中断点
:cont
:ok ready                    ← 脚本执行完毕
picoc>
```

### 表达式求值

在断点暂停时，求值当前作用域中的任意 C 表达式：

```
:break serial_load 10 0
:eval x + 1
43
:ok eval
:eval arr[2]
99
:ok eval
```

### 查看变量

列出所有可见变量（局部变量和全局变量）及其类型和值：

```
:vars
:var i 5
:var x 42
:var c A
:var p 0x20001000
:ok vars
```

类型标识符：`i`=int, `s`=short, `c`=char, `l`=long, `I`=unsigned int, `S`=unsigned short, `C`=unsigned char, `L`=unsigned long, `f`=float, `p`=pointer。

### 修改变量

在断点暂停时修改变量的值：

```
:break serial_load 10 0
:set x 100
:ok set
:eval x
100
:ok eval
```

支持所有整数类型、浮点、指针和字符字面量（`'A'`）。

### 完整调试示例

完整的调试会话流程：

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

设置断点并运行：

```
:bkpt serial_load 5
:ok bkpt
:bkpt serial_load 7
:ok bkpt
:end                          ← 开始执行
:break serial_load 5 0        ← 在第 5 行暂停
:vars
:var sum 0
:ok vars
:step
:step serial_load 6 0         ← 进入循环
:step
:step serial_load 7 0         ← 到达 sum += i
:eval sum
1
:ok eval
:eval i
1
:ok eval
:cont                         ← 继续到下一个断点
:break serial_load 7 0        ← 再次命中（循环迭代）
:eval sum
6
:ok eval
:cont
:ok ready                     ← 脚本执行完毕
picoc>
```

### 调试命令速查

| 命令 | 说明 |
|------|------|
| `:bkpt <文件> <行号>` | 设置断点 |
| `:bkptclear <文件> <行号>` | 清除断点 |
| `:cont` | 继续执行 |
| `:step` | 单步执行一条语句 |
| `:eval <表达式>` | 在当前作用域中求值 |
| `:vars` | 列出可见变量 |
| `:set <变量名> <值>` | 修改变量值 |

## 协议参考

### 主机 → 设备命令

| 命令 | 说明 |
|------|------|
| `:load [size]` | 进入文件上传模式。可选 `size` 参数用于缓冲区预检查。 |
| `:end` | 执行已上传的源码 |
| `:abort` | 中断正在运行的脚本（协作式） |
| `:ping` | 心跳检测（脚本运行期间也能响应） |
| `:reset` | 重置 PicoC 解释器 |
| `:bkpt <文件> <行号>` | 在文件的指定行设置断点 |
| `:bkptclear <文件> <行号>` | 清除断点 |
| `:cont` | 断点后继续执行 |
| `:step` | 单步执行一条语句 |
| `:eval <表达式>` | 在当前调试作用域中求值表达式 |
| `:vars` | 枚举所有可见变量 |
| `:set <变量名> <值>` | 修改变量值 |

### 设备 → 主机响应

| 响应 | 说明 |
|------|------|
| `:ok [data]` | 成功。data 可选：`ready`, `bkpt`, `bkptclear`, `eval`, `set`, `vars` |
| `:err <msg>` | 错误，附带错误消息 |
| `:pong` | 对 `:ping` 的响应 |
| `:break <文件> <行号> <列号>` | 执行在断点处暂停 |
| `:step <文件> <行号> <列号>` | 单步后执行暂停 |
| `:var <类型> <名称> <值>` | 变量数据（每行一条，出现在 `:vars` 和 `:ok vars` 之间） |

## 仓库结构

```
├── Core/               STM32 应用代码（串口、PicoC 应用、FreeRTOS 任务）
├── picoc/              PicoC 解释器源码及 STM32 平台适配
├── Drivers/            STM32 HAL 和 CMSIS 驱动
├── Middlewares/        FreeRTOS 内核（CMSIS_V2, V10.6.2）
├── MDK-ARM/            Keil MDK 工程文件
├── README.md
├── README_CN.md
└── LICENSE
```

## 技术细节

- **MCU:** STM32H750VBTx（Cortex-M7, 480 MHz, 128 KB SRAM）
- **RTOS:** FreeRTOS CMSIS_V2, Kernel V10.6.2, heap_4（30 KB 堆）
- **任务:** serialTask（优先级 40，4 KB 栈），picocTask（优先级 24，16 KB 栈）
- **IPC:** CMSIS_V2 消息队列（16 项，每项约 68 字节）
- **串口栈:** DMA 环形缓冲（1 KB）→ 环形缓冲区（8 KB RX + 8 KB TX，无锁 SPSC）→ 任务级处理
- **PicoC 堆:** 64 KB（platform.h）
- **错误恢复:** `setjmp/longjmp` 在 picocTask 内执行——脚本失败干净回到 REPL
- **Abort 安全性:** 协作式标志检查，不跨任务 `longjmp`（避免未定义行为），仅同任务展开

## 已知限制

- PicoC 完整测试套件（68+ 用例）尚未在 FreeRTOS 硬件上验证
- 任务优先级和栈大小为初始估算值，复杂负载下可能需要调优
- 未启用电源管理/睡眠模式

## 相关项目

- [RICE v1](https://github.com/mosking128/rice-v1) — 稳定裸机版本，推荐生产使用

## 许可证

MIT，详见 [LICENSE](LICENSE)。
