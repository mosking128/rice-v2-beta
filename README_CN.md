# RICE v2 �?Runtime Interactive C Environment (Beta)

> **状态：Beta 测试版�?* RICE v2 正在活跃开发中。生产环境请使用 [RICE v1](https://github.com/mosking128/PicoScript)（稳定裸机版）�?
[English](README.md) | [中文](README_CN.md)

RICE v2 �?**PicoC** C 语言解释器移植到 **STM32H750VBTx**（Cortex-M7, 480 MHz），运行�?**FreeRTOS** 之上，提供基于多任务的交互式 C 脚本环境。相�?v1 的核心架构改进是任务级隔离：串口 I/O 和脚本执行作为独立的 FreeRTOS 任务运行，使�?`:abort` 能够响应式地中断死循环，无需看门狗复位�?
用任意串口终端连�?`USART1`�?15200 8N1），即可开始交互式 C 编程�?
## v2 新特�?
| 特�?| v1（裸机） | v2（FreeRTOS�?|
|------|----------|----------------|
| 架构 | 超级循环 | 多任务（FreeRTOS�?|
| `:abort` 中断 `while(1){}` | 卡死 MCU | 协作式中断，脚本退�?|
| 脚本运行�?`:ping` | 无响�?| 始终响应 |
| 串口 I/O | 主循环轮�?| 独立高优先级任务 |
| 脚本执行 | 阻塞一�?| 隔离任务，较低优先级 |

### 协作式中断机�?
v2 的核心创新：PicoC 的循环结构（`while`、`for`、`do-while`）和阻塞 I/O（`scanf`、`getchar`）在每次迭代时检�?`AbortRequested` 标志。当串口任务收到 `:abort` 时设置该标志，解释器检测到后通过 `setjmp/longjmp` 在自己的任务内展开调用栈、清理资源并回到 REPL 提示符——整个过程不影响串口任务�?
## 架构

```
串口任务 (优先�?40)                    PicoC 任务 (优先�?24)
┌──────────────────────────�?           ┌─────────────────────────────�?�?SerialApp_Read() 轮询    │──消息队列�? �?osMessageQueueGet() 阻塞等待  �?�?协议解析                  �?           �?REPL 源码执行                 �?�?:ping / :abort 即时响应   �?           �?文件上传执行                  �?�?非协议行 �?入队           �?           �?调试命令处理                  �?└──────────────────────────�?           └─────────────────────────────�?         �?                                      �?         �?                                      �?  DMA ISR �?rx_ring[8192]              PicoC 解释�?  (无锁 SPSC)                          (setjmp/longjmp 错误恢复)
```

**ISR �?任务数据通路�?* DMA 环形缓冲 �?IDLE 中断 �?环形缓冲区（无锁 SPSC）→ 串口任务轮询 `SerialApp_Read()` �?协议行解析，源码行入�?�?PicoC 任务出队执行�?
**中断通路�?* `:abort` �?串口任务调用 `PicocApp_Abort()` �?设置 `AbortRequested=1` �?PicoC 循环条件失败 �?`ProgramFail` �?`PlatformExit` �?`longjmp` 跳回任务循环 �?REPL 提示符�?
## 当前进度（Beta�?
### 巚知问�
- 斂�卖止�Ȧstep`）和继续执��Ȧcont`）当前不可用—������޸�

### 已完�??- FreeRTOS 配置完成（CMSIS_V2, Kernel V10.6.2），编译通过
- ISR 优先级布局，DMA/USART 中断可安全调�?RTOS FromISR API
- 串口任务：协议解析，`:ping`/`:abort`/`:reset` 处理，源码行入队
- PicoC 协作�?abort 检查点，覆�?`while`/`for`/`do-while` 和阻�?I/O
- Abort 后清理：`longjmp` 后重置解释器，无内存泄漏
- Phase 3: 消息队列协议路由（所有命令）
- Phase 4: PicoC 任务完整逻辑（REPL + 文件执行在任务上下文中运行）
- Phase 5: 端到端 abort 验证（全部 5 个测试场景）
- Phase 6: 清理（移除死代码、重命名任务、完整测试套件）

### 已知问题
- 断点单步（:step）和继续执行（:cont）目前不可用——正在修复

## 快速开�?
1. �?Keil MDK 打开 `MDK-ARM/UART_DMA_H750.uvprojx`�?2. 编译（F7）并通过 ST-Link 烧录�?3. 用串口终端连�?`USART1`，波特率 `115200`，数据位 `8N1`�?4. 上电�?FreeRTOS 调度器启动，等待 `picoc>` 提示符�?
## 手动串口使用方法

所有交互通过串口发送纯文本命令完成�?
### REPL 模式

```
picoc> int x = 42;
picoc> printf("x = %d\n", x);
x = 42
```

### 文件上传

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

### 中断脚本执行

�?v1 不同，v2 可以中断正在运行的脚本：

```
picoc> while(1) { printf("loop\n"); }
loop
loop
loop
:abort             �?从终端随时发�?:err aborted
picoc>             �?立即回到 REPL
```

`:abort` 命令由高优先级的串口任务处理，设置标志后，解释器在每次循环迭代时检查该标志并干净退出�?
### 协议命令

| 命令 | 说明 |
|------|------|
| `:load [size]` | 进入文件上传模式 |
| `:end` | 执行已上传的源码 |
| `:abort` | 中断正在运行的脚本（协作式） |
| `:ping` | 心跳检测（脚本运行期间也能响应�?|
| `:reset` | 重置 PicoC 解释�?|

### 调试命令

| 命令 | 说明 |
|------|------|
| `:bkpt <文件> <行号>` | 设置断点 |
| `:bkptclear <文件> <行号>` | 清除断点 |
| `:cont` | 继续执行 |
| `:step` | 单步执行一条语�?|
| `:eval <表达�?` | 在当前作用域中求�?|
| `:vars` | 列出可见变量 |
| `:set <变量�? <�?` | 修改变量�?|

## 仓库结构

```
├── Core/               STM32 应用代码（串口、PicoC 应用、FreeRTOS 任务�?├── picoc/              PicoC 解释器源码及 STM32 平台适配
├── Drivers/            STM32 HAL �?CMSIS 驱动
├── Middlewares/        FreeRTOS 内核（CMSIS_V2, V10.6.2�?├── MDK-ARM/            Keil MDK 工程文件
├── README.md
├── README_CN.md
└── LICENSE
```

## 技术细�?
- **MCU:** STM32H750VBTx（Cortex-M7, 480 MHz, 128 KB SRAM�?- **RTOS:** FreeRTOS CMSIS_V2, Kernel V10.6.2, heap_4�?0 KB 堆）
- **任务:** serialTask（优先级 40�? KB 栈），picocTask（优先级 24�?6 KB 栈）
- **IPC:** CMSIS_V2 消息队列�?6 项，每项�?68 字节�?- **串口�?** DMA 环形缓冲�? KB）→ 环形缓冲区（8 KB RX + 8 KB TX，无�?SPSC）→ 任务级处�?- **PicoC �?** 64 KB（platform.h�?- **错误恢复:** `setjmp/longjmp` �?picocTask 内执行——脚本失败干净回到 REPL
- **Abort 安全�?** 协作式标志检查，不跨任务 `longjmp`（避免未定义行为），仅同任务展开

## 已知限制（Beta�?
- 移植尚未完成——部分功能可能不稳定
- PicoC 完整测试套件�?8+ 用例）尚未在 FreeRTOS 硬件上验�?- 任务优先级和栈大小为初始估算值，可能需要调�?- 未启用电源管�?睡眠模式

## 相关项目

- [RICE v1](https://github.com/mosking128/PicoScript) �?稳定裸机版本，推荐生产使�?
## 许可�?
MIT，详�?[LICENSE](LICENSE)�?
