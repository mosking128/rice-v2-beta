/**
 * @file    serial_app.h
 * @brief   串口应用层模块头文件
 * 
 * 该模块在 STM32 HAL 库的 UART 和 DMA 驱动之上，封装了一层带双缓冲（环形缓冲区）
 * 的串口收发逻辑。它通过空闲中断 (IDLE) 和 DMA 实现高效的数据接收，并支持
 * 非阻塞的 DMA 发送。
 * 
 * 主要特性:
 * - 接收: 使用 DMA + 空闲中断，数据自动从 DMA 缓冲区拷贝到接收环形缓冲区，防止溢出。
 * - 发送: 使用发送环形缓冲区 + DMA，数据先写入缓冲区，再通过 DMA 分块发送。
 * - 线程安全: ISR 写入(生产者) / RTOS task 读取(消费者)，无需锁。
 */
#ifndef __SERIAL_APP_H__
#define __SERIAL_APP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h" // 包含 HAL 库和 UART_HandleTypeDef 等定义

/**
 * @brief 初始化串口应用层
 * 
 * 在系统启动时调用一次。它会启动首次 DMA 接收，为接收数据做准备。
 */
void SerialApp_Init(void);

/**
 * @brief 初始化串口 TX 互斥锁（需在 osKernelInitialize 之后调用）
 */
void SerialApp_InitMutex(void);

/**
 * @brief UART 接收事件回调
 * 
 * 当 DMA 完成接收指定数量数据或检测到线路空闲 (IDLE) 时，
 * HAL 库会调用此函数。此函数处理接收到的数据并将其放入环形缓冲区。
 * 
 * @param huart 触发事件的 UART 句柄
 * @param size  本次事件中实际接收到的数据字节数
 */
void SerialApp_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size);

/**
 * @brief UART DMA 发送完成回调
 * 
 * 当一次 DMA 发送请求完成后，HAL 库会调用此函数。
 * 它更新发送环形缓冲区的状态，并尝试启动下一次 DMA 发送（如果缓冲区中还有数据）。
 * 
 * @param huart 触发事件的 UART 句柄
 */
void SerialApp_TxCpltCallback(UART_HandleTypeDef *huart);

/**
 * @brief 将数据写入发送环形缓冲区
 * 
 * 用户应用程序调用此函数来发送数据。数据会先被复制到内部发送环形缓冲区，
 * 如果当前没有正在进行的 DMA 发送，则会立即启动一次发送。
 * 
 * @param data 指向待发送数据的指针
 * @param len  待发送数据的长度
 * @return uint32_t 实际写入缓冲区的字节数。如果缓冲区满，可能会小于 len。
 */
uint32_t SerialApp_Write(const uint8_t *data, uint32_t len);

/**
 * @brief 从接收环形缓冲区读取数据
 * 
 * 用户应用程序调用此函数来获取已接收的数据。
 * 
 * @param data 指向存储读取数据的缓冲区的指针
 * @param len  期望读取的最大字节数
 * @return uint32_t 实际读取到的字节数
 */
uint32_t SerialApp_Read(uint8_t *data, uint32_t len);

/**
 * @brief 获取接收溢出次数
 * 
 * 当接收环形缓冲区已满，无法写入新数据时，会发生溢出。此计数器累计溢出次数。
 * 
 * @return uint32_t 接收溢出次数
 */
uint32_t SerialApp_GetRxOverflowCount(void);

/**
 * @brief 获取发送溢出次数
 * 
 * 当发送环形缓冲区已满，无法写入新数据时，会发生溢出。此计数器累计溢出次数。
 * 
 * @return uint32_t 发送溢出次数
 */
uint32_t SerialApp_GetTxOverflowCount(void);

/*
 * 调试模式标志：当 picocTask 进入调试命令循环（DebugCheckStatement）时置 1，
 * serialTask 检测到此标志后跳过 SerialApp_Read()，避免两个任务
 * 竞争消费 rx_ring 中的数据。
 */
extern volatile int g_debug_input_active;

#ifdef __cplusplus
}
#endif

#endif /* __SERIAL_APP_H__ */
