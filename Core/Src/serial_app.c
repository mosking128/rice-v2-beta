/**
 * @file    serial_app.c
 * @brief   串口应用层模块实现
 * 
 * @details 本模块实现了一个高效、非阻塞的串口通信框架。
 * 
 * **接收流程 (Rx)**：
 * 1. 系统启动时，`SerialApp_Init` 调用 `HAL_UARTEx_ReceiveToIdle_DMA`，
 *    使用 DMA 循环接收数据到 `rx_dma_buffer`。
 * 2. 当 DMA 接收完指定数量或 UART 空闲时，产生中断，最终调用
 *    `SerialApp_RxEventCallback`。
 * 3. 在回调中，`SerialApp_ProcessRxDma` 根据 DMA 计数器计算新数据的
 *    起始位置和长度，将数据从 DMA 缓冲区 (`rx_dma_buffer`) 拷贝到
 *    更大的接收环形缓冲区 (`rx_ring`) 中。
 * 4. 用户应用程序在主循环中调用 `SerialApp_Read` 从 `rx_ring` 中取出数据。
 * 
 * **接收环形缓冲区 (`rx_ring`)**:
 * - `rx_head`: 写入索引 (中断上下文更新)
 * - `rx_tail`: 读取索引 (主循环上下文更新)
 * - 条件 `(rx_head + 1) % SIZE == rx_tail` 表示缓冲区满。
 *   为了区分“空”和“满”，我们牺牲一个字节。
 * - `rx_overflow`: 如果写入时缓冲区已满，该计数器加 1。
 * 
 * **发送流程 (Tx)**:
 * 1. 用户调用 `SerialApp_Write` 将数据写入发送环形缓冲区 (`tx_ring`)。
 * 2. `SerialApp_Write` 内部调用 `SerialApp_TxDmaKick`，如果当前没有 DMA 
 *    发送正在进行，则启动一个新的 DMA 发送。
 * 3. DMA 发送完成时，硬件触发中断，调用 `SerialApp_TxCpltCallback`。
 * 4. 在回调中，更新 `tx_tail`，并再次调用 `SerialApp_TxDmaKick` 以
 *    检查并发送缓冲区中剩余的下一段数据。
 * 
 * **发送环形缓冲区 (`tx_ring`)**:
 * - `tx_head`: 写入索引 (用户 API 上下文更新)
 * - `tx_tail`: 读取/DMA 发送索引 (中断上下文更新)
 * - DMA 发送采用“分块发送”策略：一次 DMA 请求最多只能发送到缓冲区末尾。
 *   如果 `tx_head` 绕过末尾回到开头，`SerialApp_TxDmaKick` 会先发送从
 *   `tx_tail` 到缓冲区末尾的一段，下次完成中断时再发送开头的一段。
 * - `tx_busy`: 标志位，表示 DMA 发送正在进行中，防止重复启动。
 * - `tx_dma_len`: 记录当前 DMA 正在发送的数据长度，用于在完成回调中更新 `tx_tail`。
 */

#include "serial_app.h"
#include "usart.h" // 包含 huart1 等 UART 句柄
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"

/* 接收 DMA 缓冲区大小 (DMA 直接写入此区域) */
#define SERIAL_APP_RX_DMA_BUFFER_SIZE 1024U
/* 接收环形缓冲区大小 */
#define SERIAL_APP_RX_RING_SIZE       8192U
/* 发送环形缓冲区大小 */
#define SERIAL_APP_TX_RING_SIZE       8192U

/* --- 静态分配的缓冲区 --- */
/* DMA 接收缓冲区，DMA 直接将串口数据传送到此数组 */
static uint8_t rx_dma_buffer[SERIAL_APP_RX_DMA_BUFFER_SIZE];
/* 接收环形缓冲区，用于缓存从 DMA 缓冲区拷贝过来的数据，等待用户读取 */
static uint8_t rx_ring[SERIAL_APP_RX_RING_SIZE];
/* 发送环形缓冲区，用于缓存用户待发送的数据 */
static uint8_t tx_ring[SERIAL_APP_TX_RING_SIZE];

/* --- 接收状态变量 --- */
/* rx_head: 生产者索引，指向环形缓冲区中下一个要写入的空位。由中断上下文更新。 */
static volatile uint32_t rx_head = 0U;
/* rx_tail: 消费者索引，指向环形缓冲区中下一个要读取的字节。由主循环上下文更新。 */
static volatile uint32_t rx_tail = 0U;
/* 接收溢出计数器，当环形缓冲区满时，每尝试写入一个字节，该值加1 */
static volatile uint32_t rx_overflow = 0U;

/* --- 发送状态变量 --- */
/* tx_head: 生产者索引，指向发送环形缓冲区中下一个要写入的空位。由用户 API 上下文更新。 */
static volatile uint32_t tx_head = 0U;
/* tx_tail: 消费者索引，指向下一次 DMA 发送要开始读取的字节。由中断上下文更新。 */
static volatile uint32_t tx_tail = 0U;
/* 当前正在进行的 DMA 发送的数据长度。在完成回调中用于准确更新 tx_tail。 */
static volatile uint32_t tx_dma_len = 0U;
/* 发送溢出计数器，当环形缓冲区满时未能写入的数据字节数 */
static volatile uint32_t tx_overflow = 0U;
/* DMA 发送忙标志: 0 = 空闲, 非0 = 正在进行DMA发送 */
static volatile uint8_t tx_busy = 0U;

/* DMA 缓冲区的上一次处理位置。用于在 DMA 中断回调中计算增量数据。 */
static uint16_t rx_dma_last_pos = 0U;

/* 调试输入激活标志：置 1 时 serialTask 停止消费 rx_ring */
volatile int g_debug_input_active = 0;

/* TX 互斥锁：保护 SerialApp_Write 在多任务环境下的并发访问 */
static osMutexId_t tx_mutex = NULL;

/* --- 内部静态函数声明 --- */
static void SerialApp_StartRxDma(void);
static void SerialApp_ProcessRxDma(uint16_t pos);
static void SerialApp_RxRingWrite(const uint8_t *data, uint32_t len);
static void SerialApp_TxDmaKick(void);

/**
 * @brief 初始化串口应用，启动首次 DMA 接收。
 */
void SerialApp_Init(void)
{
  SerialApp_StartRxDma();
}

void SerialApp_InitMutex(void)
{
  tx_mutex = osMutexNew(NULL);
}

/**
 * @brief 将数据写入发送环形缓冲区，并尝试触发 DMA 发送。
 * 
 * @param data 待发送数据的指针
 * @param len  数据长度
 * @return uint32_t 实际成功写入缓冲区的字节数
 */
uint32_t SerialApp_Write(const uint8_t *data, uint32_t len)
{
  uint32_t count = 0U;

  if (data == NULL)
  {
    return 0U;
  }

  if (tx_mutex != NULL) osMutexAcquire(tx_mutex, osWaitForever);

  /* 循环将数据逐字节写入发送环形缓冲区 */
  while (count < len)
  {
    /* 计算下一个 head 位置 (环形缓冲区，取模) */
    uint32_t next = (tx_head + 1U) % SERIAL_APP_TX_RING_SIZE;
    /* 如果下一个位置 == tail，说明缓冲区已满，无法写入 */
    if (next == tx_tail)
    {
      tx_overflow++; // 记录溢出
      break;         // 停止写入
    }

    /* 写入数据并移动 head */
    tx_ring[tx_head] = data[count++];
    tx_head = next;
  }

  /* 尝试启动 DMA 发送 (如果当前不忙且有数据) */
  SerialApp_TxDmaKick();

  if (tx_mutex != NULL) osMutexRelease(tx_mutex);
  return count; // 返回实际写入的数量
}

/**
 * @brief 从接收环形缓冲区读取数据到用户缓冲区。
 * 
 * @param data 用户提供的缓冲区，用于存放接收到的数据
 * @param len  期望读取的最大长度
 * @return uint32_t 实际读取到的字节数
 */
uint32_t SerialApp_Read(uint8_t *data, uint32_t len)
{
  uint32_t count = 0U;

  if (data == NULL)
  {
    return 0U;
  }

  /* 当用户请求未读完且缓冲区不为空时，持续读取 */
  while ((count < len) && (rx_tail != rx_head))
  {
    data[count++] = rx_ring[rx_tail]; // 从环形缓冲区取数据
    rx_tail = (rx_tail + 1U) % SERIAL_APP_RX_RING_SIZE; // 移动 tail
  }

  return count;
}

/**
 * @brief 获取接收溢出次数
 */
uint32_t SerialApp_GetRxOverflowCount(void)
{
  return rx_overflow;
}

/**
 * @brief 获取发送溢出次数
 */
uint32_t SerialApp_GetTxOverflowCount(void)
{
  return tx_overflow;
}

/**
 * @brief UART 接收事件回调 (在 HAL 的 UART Rx 中断中调用)
 * 
 * @details 该函数被 HAL 库的 UART 中断处理函数调用，在发生以下两种事件时触发:
 *  - DMA 半满/全满中断
 *  - UART 空闲中断 (Receive To Idle)
 *  参数 `size` 表示从 DMA 缓冲区起始位置到现在累计接收了多少数据。
 *  对于用 `HAL_UARTEx_ReceiveToIdle_DMA` 启动的接收，`size` 即是当前
 *  DMA 写入的总字节数 (DMA 自动循环)。
 * 
 * @param huart 触发事件的 UART 句柄，用于过滤是否为我们关心的 USART1
 * @param size  当前已接收的数据总数 (DMA 计数器)
 */
void SerialApp_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
  /* 过滤事件，只处理 USART1 */
  if (huart->Instance == USART1)
  {
    /* size 参数不能超过 DMA 缓冲区大小 */
    if (size <= SERIAL_APP_RX_DMA_BUFFER_SIZE)
    {
      /* 处理DMA缓冲区中的数据，将其转移到接收环形缓冲区 */
      SerialApp_ProcessRxDma(size);
    }
  }

  /* 唤醒 serialTask 处理接收到的数据 */
  {
    extern osThreadId_t serialTaskHandle;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (serialTaskHandle != NULL)
    {
      xTaskNotifyFromISR(serialTaskHandle, 0, eNoAction, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

/**
 * @brief UART DMA 发送完成回调 (在 DMA 发送完成中断中调用)
 * 
 * @details 一次 DMA 发送成功完成后调用。它根据 `tx_dma_len` 更新
 *  发送环形缓冲区的消费者索引 `tx_tail`，然后清除忙标志，并尝试
 *  启动下一次 DMA 发送（如果缓冲区中有待发数据）。
 * 
 * @param huart 触发事件的 UART 句柄
 */
void SerialApp_TxCpltCallback(UART_HandleTypeDef *huart)
{
  /* 过滤事件，只处理 USART1 */
  if (huart->Instance == USART1)
  {
    /* 根据发送的长度更新 tx_tail 位置 (注意取模) */
    tx_tail = (tx_tail + tx_dma_len) % SERIAL_APP_TX_RING_SIZE;
    tx_dma_len = 0U; // 清除发送长度
    tx_busy = 0U;    // 清除忙标志

    /* 尝试启动下一次发送，处理缓冲区中可能剩余的数据 */
    SerialApp_TxDmaKick();
  }
}

/**
 * @brief 启动 DMA 接收
 * 
 * @details 调用 `HAL_UARTEx_ReceiveToIdle_DMA` 启动非阻塞的 DMA 循环接收。
 *  该函数一旦设置成功，DMA 将持续接收数据到 `rx_dma_buffer`，直到溢出为止。
 *  当接收到数据或发生空闲中断时，会调用 `HAL_UARTEx_RxEventCallback`，
 *  最终路由到我们的 `SerialApp_RxEventCallback`。
 *  需要注意，此函数在初始化时和每次 `RxEventCallback` 处理后都需要
 *  重新调用 (通常由 HAL 库内部或应用手动重启，本示例中未显式重启，
 *  依赖循环 DMA 模式)。
 */
static void SerialApp_StartRxDma(void)
{
  if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma_buffer, SERIAL_APP_RX_DMA_BUFFER_SIZE) != HAL_OK)
  {
    Error_Handler(); // 启动失败，进入错误处理
  }
  /* 重置上次处理位置为起始位置 */
  rx_dma_last_pos = 0U;
}

/**
 * @brief 处理 DMA 缓冲区中的新数据，并将其写入接收环形缓冲区
 * 
 * @details 此函数在 `SerialApp_RxEventCallback` 中被调用。
 *  它通过比较当前 DMA 计数器 `pos` 和上次记录的位置 `rx_dma_last_pos`
 *  来判断新接收到的数据块。
 *  - 如果 `pos > last_pos`: 新数据在线性区域，直接处理 [last_pos, pos)。
 *  - 如果 `pos < last_pos`: DMA 缓冲区发生了回卷 (wrap-around)。
 *    新数据被分成两段: [last_pos, 缓冲区末尾) 和 [0, pos)。
 *  - 如果 `pos == last_pos`: 没有新数据 (可能由其他中断触发)，直接返回。
 * 
 * @param pos DMA 计数器的当前值，即 DMA 已写入缓冲区的字节总数。
 */
static void SerialApp_ProcessRxDma(uint16_t pos)
{
  if (pos == rx_dma_last_pos)
  {
    return; // 无新数据
  }

  /* 情况一: pos 在线性区域内大于 last_pos，数据连续 */
  if (pos > rx_dma_last_pos)
  {
    /* 将 [rx_dma_last_pos, pos) 区间的数据写入环形缓冲区 */
    SerialApp_RxRingWrite(&rx_dma_buffer[rx_dma_last_pos], (uint32_t)pos - rx_dma_last_pos);
  }
  else // 情况二: DMA 缓冲区发生回卷 (pos < rx_dma_last_pos)
  {
    /* 先处理从 last_pos 到缓冲区末尾的一段 */
    SerialApp_RxRingWrite(&rx_dma_buffer[rx_dma_last_pos],
                          SERIAL_APP_RX_DMA_BUFFER_SIZE - rx_dma_last_pos);
    /* 再处理从缓冲区开头到 pos 的一段 (如果 pos > 0) */
    if (pos > 0U)
    {
      SerialApp_RxRingWrite(rx_dma_buffer, pos);
    }
  }

  /* 更新 last_pos 为当前 pos，为下次处理做准备 */
  rx_dma_last_pos = pos;
}

/**
 * @brief 将指定长度的数据写入接收环形缓冲区
 * 
 * @details 这是一个内部辅助函数。它逐字节将数据写入接收环形缓冲区 `rx_ring`。
 *  如果缓冲区满，则停止写入并记录溢出。
 * 
 * @param data 数据源指针
 * @param len  待写入长度
 */
static void SerialApp_RxRingWrite(const uint8_t *data, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++)
  {
    /* 计算下一个 head 位置 */
    uint32_t next = (rx_head + 1U) % SERIAL_APP_RX_RING_SIZE;
    /* 检查缓冲区是否已满 */
    if (next == rx_tail)
    {
      rx_overflow++; // 记录溢出
      break;         // 缓冲区满，停止写入
    }

    /* 写入数据并移动 head */
    rx_ring[rx_head] = data[i];
    rx_head = next;
  }
}

/**
 * @brief 检查发送缓冲区并启动一次新的 DMA 发送 (如果需要且可能)
 * 
 * @details 这是发送逻辑的核心。它会被 `SerialApp_Write` 和 
 *  `SerialApp_TxCpltCallback` 调用，确保只要缓冲区中有数据且没有
 *  正在进行的 DMA 发送，就能立即启动新的发送。
 * 
 *  它计算一个“可用块”的长度：如果 tx_head 在 tx_tail 前面，
 *  则长度是 `tx_head - tx_tail`；否则 (wrapped around)，
 *  只能发送从 tx_tail 到缓冲区末尾的一段，剩下的需要等
 *  当前发送完成后的下一次中断再发送。
 * 
 * @note 此函数假设调用时可能处于中断上下文，因此不能长时间阻塞。
 */
static void SerialApp_TxDmaKick(void)
{
  uint32_t len;

  /* 如果 DMA 正在忙，或者缓冲区是空的 (head == tail)，则不执行任何操作 */
  if ((tx_busy != 0U) || (tx_tail == tx_head))
  {
    return;
  }

  /* 计算本次可发送的长度: 取 head 到 tail 之间的连续可用数据 */
  if (tx_head > tx_tail)
  {
    /* head 在 tail 之后，可以一次性发送到 head */
    len = tx_head - tx_tail;
  }
  else
  {
    /* head 已经回卷，本次只能发送到缓冲区的物理末尾 */
    len = SERIAL_APP_TX_RING_SIZE - tx_tail;
  }

  /* 记录本次发送的长度，供完成回调使用 */
  tx_dma_len = len;
  /* 设置忙标志 */
  tx_busy = 1U;

  /* 启动 DMA 发送，从 tx_ring[tx_tail] 开始，长度为 len */
  if (HAL_UART_Transmit_DMA(&huart1, &tx_ring[tx_tail], (uint16_t)len) != HAL_OK)
  {
    /* 如果启动失败，清除标志位，避免系统死锁 */
    tx_busy = 0U;
    tx_dma_len = 0U;
  }
}
