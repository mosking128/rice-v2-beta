/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "serial_app.h"
#include "picoc_app.h"
#include "task_msg.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* REMOVED: defaultTask no longer needed */
/*osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};*/
/* Definitions for serialTask */
osThreadId_t serialTaskHandle;
const osThreadAttr_t serialTask_attributes = {
  .name = "serialTask",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for picocTask */
osThreadId_t picocTaskHandle;
const osThreadAttr_t picocTask_attributes = {
  .name = "picocTask",
  .stack_size = 8192 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for uartQuene */
osMessageQueueId_t uartQueneHandle;
const osMessageQueueAttr_t uartQuene_attributes = {
  .name = "uartQuene"
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

/*void StartDefaultTask(void *argument);*/
void StartSerialTask(void *argument);
void StartPicocTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  SerialApp_InitMutex();
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of uartQuene */
  uartQueneHandle = osMessageQueueNew (16, sizeof(TaskMsg), &uartQuene_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* REMOVED: defaultTask no longer needed */
  /*defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);*/

  /* creation of serialTask */
  serialTaskHandle = osThreadNew(StartSerialTask, NULL, &serialTask_attributes);

  /* creation of picocTask */
  picocTaskHandle = osThreadNew(StartPicocTask, NULL, &picocTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
/* REMOVED: defaultTask no longer needed */
/*void StartDefaultTask(void *argument)
{
  for(;;)
  {
    osDelay(1);
  }
}*/

/* USER CODE BEGIN Header_StartTask02 */
/**
* @brief Function implementing the serialTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTask02 */
void StartSerialTask(void *argument)
{
  /* USER CODE BEGIN StartSerialTask */
  uint8_t buf[256];
  TaskMsg msg;
  (void)argument;
  for(;;)
  {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    /* 调试模式下 picocTask 直接消费 rx_ring，serialTask 让出 */
    if (g_debug_input_active)
      continue;
    /* 排空 rx_ring：一次唤醒尽可能读完所有数据 */
    for (;;)
    {
      uint32_t len = SerialApp_Read(buf, sizeof(buf));
      if (len == 0U)
      {
        /* 无数据时触发延迟提示符检查 */
        (void)PicocApp_ProcessChars(buf, 0U, &msg);
        break;
      }
      if (PicocApp_ProcessChars(buf, len, &msg) != 0)
      {
        osMessageQueuePut(uartQueneHandle, &msg, 0U, 0U);
      }
    }
  }
  /* USER CODE END StartSerialTask */
}

/* USER CODE BEGIN Header_StartPicocTask */
/**
* @brief Function implementing the picocTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartPicocTask */
void StartPicocTask(void *argument)
{
  /* USER CODE BEGIN StartPicocTask */
  TaskMsg msg;
  (void)argument;
  for(;;)
  {
    if (osMessageQueueGet(uartQueneHandle, &msg, NULL, osWaitForever) == osOK)
    {
      switch (msg.type)
      {
        case MSG_SOURCE_LINE:
          PicocApp_RunSourceLine(msg.data);
          break;
        case MSG_LOAD_BEGIN:
          /* mode already transitioned by serialTask */
          break;
        case MSG_LOAD_END:
          PicocApp_ExecuteLoadSource();
          break;
        case MSG_LOAD_ABORT:
          /* load already cancelled by serialTask */
          break;
        case MSG_RESET:
          PicocApp_Reset();
          break;
        default:
          break;
      }
    }
  }
  /* USER CODE END StartPicocTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

