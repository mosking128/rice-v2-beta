/* PicoC 应用层 — 串口 REPL 交互与脚本加载引擎
 *
 * 本文件是 PicoC 解释器在 STM32H7 上的顶层应用，负责：
 *   1. 初始化 PicoC 解释器实例并进入 REPL 交互循环
 *   2. 通过 UART 串口接收用户输入（单行 REPL 或批量文件加载）
 *   3. 解析结构化协议命令（:load / :end / :abort / :ping / :reset / :bkpt / :bkptclear）
 *   4. 源码完整性分析（括号匹配、字符串/注释跟踪），支持多行输入
 *   5. 自动识别表达式并包装为 printf 输出
 *   6. 文件加载模式：逐行接收代码，完成后在隔离 PicoC 实例中执行
 *
 * 工作流程：
 *   PicocApp_Init() → PICOC_APP_MODE_REPL
 *   PicocApp_ProcessChars() 处理 serialTask 传入的字符
 *     ├─ REPL 模式  → PicocApp_HandleReplChar() ／ PicocApp_ExecuteReplSource()
 *     └─ LOAD 模式  → PicocApp_HandleLoadChar() ／ PicocApp_ExecuteLoadSource()
 */

#include "picoc_app.h"

#include <string.h>

#include "serial_app.h"
#include "picoc.h"
#include "interpreter.h"
#include "cmsis_os2.h"
#include "task_msg.h"

/* PicoC 解释器堆栈大小 */
#define PICOC_APP_STACK_SIZE             (64 * 1024)
/* 行缓冲区大小（接收单行输入） */
#define PICOC_APP_SOURCE_BUFFER_SIZE     2048U
/* 执行缓冲区大小（自动表达式包装用） */
#define PICOC_APP_EXEC_BUFFER_SIZE       2304U
/* 加载模式累积缓冲区大小 */
#define PICOC_APP_LOAD_BUFFER_SIZE       8192U
/* 每次轮询读取的 UART 数据块大小 */
#define PICOC_APP_RX_CHUNK_SIZE          64U
/* 加载模式提示符 */
#define PICOC_APP_LOAD_PROMPT            "load> "
/* 排空模式空闲计数阈值 */
#define PICOC_APP_DRAIN_IDLE_THRESHOLD  10U

/* --- 结构化协议响应字符串 --- */
#define PICOC_APP_RESP_OK                ":ok\r\n"
#define PICOC_APP_RESP_OK_READY          ":ok ready\r\n"
#define PICOC_APP_RESP_PONG              ":pong\r\n"
#define PICOC_APP_RESP_ERR_BUFFER_FULL   ":err buffer full\r\n"
#define PICOC_APP_RESP_ERR_NO_SOURCE     ":err no source\r\n"
#define PICOC_APP_RESP_ERR_LINE_LONG     ":err line too long\r\n"
#define PICOC_APP_RESP_ERR_LOAD_CANCELLED ":err load cancelled\r\n"

/* 源码结构状态：跟踪括号深度、字符串/注释状态，用于判断语句完整性 */
typedef struct
{
    int paren_depth;        /* 圆括号 () 嵌套深度 */
    int bracket_depth;      /* 方括号 [] 嵌套深度 */
    int brace_depth;        /* 花括号 {} 嵌套深度 */
    int in_string;          /* 是否在双引号字符串内 */
    int in_char;            /* 是否在单引号字符字面量内 */
    int in_block_comment;   /* 是否在块注释内 */
    int ends_with_do_block; /* 是否以未闭合的 do 块结尾 */
    char last_non_space;    /* 最后一个非空白字符 */
} PicocApp_SourceState;

/* 工作模式 */
typedef enum
{
    PICOC_APP_MODE_REPL = 0,  /* REPL 交互模式 */
    PICOC_APP_MODE_LOAD,      /* 文件加载模式 */
    PICOC_APP_MODE_DRAIN      /* 排空模式（丢弃数据直到空闲） */
} PicocApp_Mode;

/* 协议命令类型 */
typedef enum
{
    PICOC_APP_COMMAND_NONE = 0,
    PICOC_APP_COMMAND_LOAD,
    PICOC_APP_COMMAND_END,
    PICOC_APP_COMMAND_ABORT,
    PICOC_APP_COMMAND_PING,
    PICOC_APP_COMMAND_RESET,
    PICOC_APP_COMMAND_BKPT,
    PICOC_APP_COMMAND_BKPTCLEAR,
    PICOC_APP_COMMAND_DEBUG     /* :cont / :step / :eval / :vars / :set
                                   仅由调试循环处理，REPL/LOAD 模式下静默消耗 */
} PicocApp_Command;

/* 全局状态 */
static Picoc g_picoc;
static uint8_t g_source_buffer[PICOC_APP_SOURCE_BUFFER_SIZE];
static uint8_t g_load_buffer[PICOC_APP_LOAD_BUFFER_SIZE];
static uint32_t g_source_length = 0U;
static uint32_t g_load_length = 0U;
static uint8_t g_prompt_pending = 0U;
static uint8_t g_reset_pending = 0U;
static uint8_t g_script_running = 0U;
static uint8_t g_last_char_was_cr = 0U;
static uint32_t g_drain_idle_count = 0U;
static PicocApp_Mode g_mode = PICOC_APP_MODE_REPL;
static const char *g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
static Picoc *g_active_picoc = NULL;  /* currently executing PicoC instance, for abort targeting */

/* 前向声明 */
static void PicocApp_WriteString(const char *text);
static void PicocApp_WriteByte(uint8_t ch);
static void PicocApp_SendResponse(const char *msg);
static void PicocApp_ShowPrompt(void);
static void PicocApp_HandleChar(uint8_t ch);
static void PicocApp_HandleReplChar(uint8_t ch);
static void PicocApp_HandleLoadChar(uint8_t ch);
static int PicocApp_AppendByte(uint8_t *buffer, uint32_t *length, uint32_t capacity, uint8_t ch);
static int PicocApp_AppendBlock(uint8_t *buffer, uint32_t *length, uint32_t capacity, const uint8_t *data, uint32_t size);
static void PicocApp_ResetSource(void);
static void PicocApp_ResetLoadBuffer(void);
static void PicocApp_ResetLineBuffer(void);
static void PicocApp_EnterLoadMode(void);
static void PicocApp_LeaveLoadMode(void);
static void PicocApp_ExecuteReplSource(const char *source);
void PicocApp_ExecuteLoadSource(void);
static int PicocApp_RunSource(Picoc *pc, const char *file_name, const char *source, int auto_print_expression, int auto_call_main);
static int PicocApp_HasMainFunction(Picoc *pc);
static PicocApp_Command PicocApp_ParseCommand(const uint8_t *buffer, uint32_t length, uint32_t *out_param);
static void PicocApp_HandleBkptCommand(const char *cmd_text, int set);
static void PicocApp_AnalyseSource(const char *source, PicocApp_SourceState *state);
static int PicocApp_IsSourceComplete(const char *source, const PicocApp_SourceState *state);
static int PicocApp_ShouldAutoPrintExpression(const char *source, const PicocApp_SourceState *state);
static const char *PicocApp_SkipLeadingSpace(const char *text);
static const char *PicocApp_FindTrimmedEnd(const char *text);
static int PicocApp_StartsWithKeyword(const char *text, const char *keyword);

/* 初始化 PicoC 应用：创建解释器实例，发送启动提示符 */
void PicocApp_Init(void)
{
    PicocInitialise(&g_picoc, PICOC_APP_STACK_SIZE);
    PicocApp_ResetSource();
    PicocApp_ResetLoadBuffer();
    PicocApp_WriteString(INTERACTIVE_PROMPT_START);
    g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
    PicocApp_ShowPrompt();
}

/* 请求中断正在执行的 PicoC 脚本（供 serialTask 收到 :abort 时调用） */
void PicocApp_Abort(void)
{
    g_picoc.AbortRequested = 1;
    if (g_active_picoc != NULL && g_active_picoc != &g_picoc)
        g_active_picoc->AbortRequested = 1;
}

/* 执行一行 REPL 源码（供 picocTask 收到 MSG_SOURCE_LINE 时调用） */
void PicocApp_RunSourceLine(const char *source)
{
    PicocApp_RunSource(&g_picoc, "serial", source, TRUE, FALSE);
}

/* 重置 PicoC 解释器实例（供 picocTask 收到 MSG_RESET 时调用） */
void PicocApp_Reset(void)
{
    extern volatile int g_debug_input_active;
    PicocCleanup(&g_picoc);
    PicocInitialise(&g_picoc, PICOC_APP_STACK_SIZE);
    g_debug_input_active = 0;
}

/* 处理一批 UART 字符，返回需要入队的消息
 * 返回 1 表示 out_msg 有效，0 表示无消息（内部处理或缓冲中） */
int PicocApp_ProcessChars(const uint8_t *data, uint32_t len, TaskMsg *out_msg)
{
    uint32_t index;
    int has_msg = 0;

    if (g_mode == PICOC_APP_MODE_DRAIN)
    {
        if (len == 0U)
        {
            g_drain_idle_count++;
            if (g_drain_idle_count >= PICOC_APP_DRAIN_IDLE_THRESHOLD)
            {
                g_mode = PICOC_APP_MODE_REPL;
                g_drain_idle_count = 0U;
            }
        }
        else
        {
            g_drain_idle_count = 0U;
        }
        return 0;
    }

    for (index = 0U; index < len; index++)
    {
        uint8_t ch = data[index];
        if (ch == 0U) continue;

        /* merge \r\n */
        if (ch == '\n' && g_last_char_was_cr != 0U)
        {
            g_last_char_was_cr = 0U;
            continue;
        }

        if (g_mode == PICOC_APP_MODE_LOAD)
        {
            if (ch == '\r' || ch == '\n')
            {
                PicocApp_Command command;
                uint32_t cmd_param = 0U;
                g_last_char_was_cr = (ch == '\r') ? 1U : 0U;
                g_source_buffer[g_source_length] = '\0';
                command = PicocApp_ParseCommand(g_source_buffer, g_source_length, &cmd_param);

                if (command == PICOC_APP_COMMAND_END)
                {
                    PicocApp_WriteString("\r\n");
                    PicocApp_ResetSource();
                    g_script_running = 1U;
                    out_msg->type = MSG_LOAD_END;
                    out_msg->len = 0;
                    out_msg->data[0] = '\0';
                    has_msg = 1;
                    break;  /* :end 后不继续处理 LOAD 数据 */
                }
                if (command == PICOC_APP_COMMAND_ABORT)
                {
                    PicocApp_WriteString("\r\n");
                    PicocApp_ResetSource();
                    PicocApp_ResetLoadBuffer();
                    g_mode = PICOC_APP_MODE_REPL;
                    g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
                    g_prompt_pending = 1U;
                    PicocApp_SendResponse(PICOC_APP_RESP_ERR_LOAD_CANCELLED);
                    /* 如果正在执行脚本，同时触发解释器 abort */
                    if (g_script_running != 0U)
                        PicocApp_Abort();
                    out_msg->type = MSG_LOAD_ABORT;
                    out_msg->len = 0;
                    out_msg->data[0] = '\0';
                    has_msg = 1;
                    continue;  /* 回到 REPL，继续处理剩余字节 */
                }
                if (command == PICOC_APP_COMMAND_PING)
                {
                    PicocApp_WriteString("\r\n");
                    PicocApp_ResetSource();
                    PicocApp_SendResponse(PICOC_APP_RESP_PONG);
                    g_prompt_pending = 1U;
                    continue;
                }
                if (command == PICOC_APP_COMMAND_RESET)
                {
                    PicocApp_WriteString("\r\n");
                    PicocApp_ResetSource();
                    PicocApp_ResetLoadBuffer();
                    g_mode = PICOC_APP_MODE_REPL;
                    g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
                    g_prompt_pending = 1U;
                    PicocApp_SendResponse(PICOC_APP_RESP_OK);
                    out_msg->type = MSG_RESET;
                    out_msg->len = 0;
                    out_msg->data[0] = '\0';
                    has_msg = 1;
                    continue;
                }

                if (command == PICOC_APP_COMMAND_DEBUG)
                {
                    PicocApp_WriteString("\r\n");
                    PicocApp_ResetSource();
                    g_prompt_pending = 1U;
                    continue;
                }

                /* non-command: accumulate into load_buffer (skip empty lines)
                 * 脚本执行期间跳过，防止写坏 g_load_buffer，但命令仍可处理 */
                if (g_script_running == 0U && g_source_length > 0U)
                {
                    (void)PicocApp_AppendBlock(g_load_buffer, &g_load_length,
                                               PICOC_APP_LOAD_BUFFER_SIZE,
                                               g_source_buffer, g_source_length);
                    (void)PicocApp_AppendByte(g_load_buffer, &g_load_length,
                                              PICOC_APP_LOAD_BUFFER_SIZE, '\n');
                }
                PicocApp_ResetSource();
                g_prompt_pending = 1U;
                continue;
            }
            /* non-newline character */
            g_last_char_was_cr = 0U;
            if (ch == '\b' || ch == 0x7fU)
            {
                if (g_source_length > 0U) g_source_length--;
                continue;
            }
            (void)PicocApp_AppendByte(g_source_buffer, &g_source_length,
                                      sizeof(g_source_buffer), ch);
        }
        else
        {
            /* REPL mode */
            if (ch == '\r' || ch == '\n')
            {
                PicocApp_SourceState state;
                PicocApp_Command command;
                uint32_t cmd_param = 0U;
                g_last_char_was_cr = (ch == '\r') ? 1U : 0U;
                g_source_buffer[g_source_length] = '\0';
                command = PicocApp_ParseCommand(g_source_buffer, g_source_length, &cmd_param);

                if (command == PICOC_APP_COMMAND_LOAD)
                {
                    PicocApp_WriteString("\r\n");
                    if (cmd_param > 0U && cmd_param > (PICOC_APP_LOAD_BUFFER_SIZE - 1U))
                    {
                        PicocApp_SendResponse(PICOC_APP_RESP_ERR_BUFFER_FULL);
                        PicocApp_ResetSource();
                        g_prompt_pending = 1U;
                        continue;
                    }
                    PicocApp_SendResponse(PICOC_APP_RESP_OK);
                    PicocApp_ResetSource();
                    PicocApp_EnterLoadMode();
                    g_prompt_pending = 1U;
                    out_msg->type = MSG_LOAD_BEGIN;
                    out_msg->len = 0;
                    out_msg->data[0] = '\0';
                    has_msg = 1;
                    continue;  /* 进入 LOAD 模式，剩余字节按 LOAD 处理 */
                }
                if (command == PICOC_APP_COMMAND_PING)
                {
                    PicocApp_WriteString("\r\n");
                    PicocApp_ResetSource();
                    PicocApp_SendResponse(PICOC_APP_RESP_PONG);
                    g_prompt_pending = 1U;
                    continue;
                }
                if (command == PICOC_APP_COMMAND_RESET)
                {
                    PicocApp_WriteString("\r\n");
                    PicocApp_ResetSource();
                    PicocApp_ResetLoadBuffer();
                    g_mode = PICOC_APP_MODE_REPL;
                    g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
                    g_prompt_pending = 1U;
                    PicocApp_SendResponse(PICOC_APP_RESP_OK);
                    out_msg->type = MSG_RESET;
                    out_msg->len = 0;
                    out_msg->data[0] = '\0';
                    has_msg = 1;
                    continue;
                }
                if (command == PICOC_APP_COMMAND_ABORT)
                {
                    PicocApp_WriteString("\r\n");
                    PicocApp_ResetSource();
                    PicocApp_Abort();
                    g_prompt_pending = 1U;
                    continue;
                }
                if (command == PICOC_APP_COMMAND_BKPT || command == PICOC_APP_COMMAND_BKPTCLEAR)
                {
                    PicocApp_WriteString("\r\n");
                    PicocApp_HandleBkptCommand((const char *)g_source_buffer,
                                               command == PICOC_APP_COMMAND_BKPT);
                    PicocApp_ResetSource();
                    g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
                    g_prompt_pending = 1U;
                    continue;
                }
                if (command == PICOC_APP_COMMAND_DEBUG)
                {
                    /* 调试命令 (:cont/:step/:eval/:vars/:set) 在调试模式外收到，
                     * 静默消耗，防止被当作 C 代码执行。 */
                    PicocApp_WriteString("\r\n");
                    PicocApp_ResetSource();
                    g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
                    g_prompt_pending = 1U;
                    continue;
                }

                /* non-command: send as C source to picocTask */
                PicocApp_WriteString("\r\n");
                (void)PicocApp_AppendByte(g_source_buffer, &g_source_length,
                                          sizeof(g_source_buffer), '\n');
                g_source_buffer[g_source_length] = '\0';

                PicocApp_AnalyseSource((const char *)g_source_buffer, &state);
                if (PicocApp_IsSourceComplete((const char *)g_source_buffer, &state) != 0)
                {
                    uint32_t copy_len = g_source_length;
                    if (copy_len > sizeof(out_msg->data) - 1U)
                        copy_len = sizeof(out_msg->data) - 1U;
                    memcpy(out_msg->data, g_source_buffer, copy_len);
                    out_msg->data[copy_len] = '\0';
                    out_msg->len = (uint16_t)copy_len;
                    out_msg->type = MSG_SOURCE_LINE;
                    PicocApp_ResetSource();
                    g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
                    g_prompt_pending = 1U;
                    has_msg = 1;
                    continue;
                }
                /* incomplete source, continue accumulating */
                g_prompt_text = INTERACTIVE_PROMPT_LINE;
                g_prompt_pending = 1U;
                continue;
            }

            g_last_char_was_cr = 0U;
            if (ch == '\b' || ch == 0x7fU)
            {
                if (g_source_length > 0U) g_source_length--;
                continue;
            }
            if (PicocApp_AppendByte(g_source_buffer, &g_source_length,
                                    sizeof(g_source_buffer), ch) == 0)
            {
                PicocApp_SendResponse(PICOC_APP_RESP_ERR_LINE_LONG);
                PicocApp_ResetSource();
                g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
                g_prompt_pending = 1U;
                continue;
            }
            PicocApp_WriteByte(ch);  /* echo */
        }
    }

    /* deferred prompt: send when no new data arrives, REPL mode, no script running */
    if (g_prompt_pending != 0U && len == 0U && g_mode == PICOC_APP_MODE_REPL && g_script_running == 0U)
    {
        PicocApp_ShowPrompt();
    }

    return has_msg;
}

/* 阻塞读取单个控制台字符（供 PicoC 平台层调用） */
int PicocApp_ConsoleGetCharBlocking(void)
{
    uint8_t ch;
    Picoc *active = (g_active_picoc != NULL) ? g_active_picoc : &g_picoc;

    while (SerialApp_Read(&ch, 1U) == 0U)
    {
        if (active->AbortRequested)
        {
            PlatformExit(active, 1);  /* longjmp back to PicocApp_RunSource, flag cleared there */
        }
        osDelay(1);  /* yield CPU to serialTask */
    }

    return (int)ch;
}

/* 通过串口发送一个字符串 */
static void PicocApp_WriteString(const char *text)
{
    if (text != NULL)
    {
        const uint8_t *buffer = (const uint8_t *)text;
        uint32_t remaining = (uint32_t)strlen(text);

        while (remaining > 0U)
        {
            uint32_t written = SerialApp_Write(buffer, remaining);
            buffer += written;
            remaining -= written;
        }
    }
}

/* 通过串口发送单个字节 */
static void PicocApp_WriteByte(uint8_t ch)
{
    while (SerialApp_Write(&ch, 1U) == 0U)
    {
    }
}

/* 发送协议响应消息 */
static void PicocApp_SendResponse(const char *msg)
{
    PicocApp_WriteString(msg);
}

/* 显示当前提示符并清除挂起标志 */
static void PicocApp_ShowPrompt(void)
{
    PicocApp_WriteString(g_prompt_text);
    g_prompt_pending = 0U;
}

/* 字符分发器：根据当前模式将字符路由到对应的处理函数 */
static void PicocApp_HandleChar(uint8_t ch)
{
    if (ch == 0U)
    {
        return;
    }

    /* 合并 \r\n 为单个换行 */
    if (ch == '\n' && g_last_char_was_cr != 0U)
    {
        g_last_char_was_cr = 0U;
        return;
    }

    if (g_mode == PICOC_APP_MODE_DRAIN)
    {
        g_last_char_was_cr = (ch == '\r') ? 1U : 0U;
        return;
    }

    if (g_mode == PICOC_APP_MODE_LOAD)
    {
        PicocApp_HandleLoadChar(ch);
    }
    else
    {
        PicocApp_HandleReplChar(ch);
    }
}

/* REPL 模式字符处理
 *
 * 收到换行时：
 *   1. 先检查是否为协议命令（:load / :ping / :reset / :bkpt / :bkptclear）
 *   2. 非命令则进行源码完整性分析
 *   3. 语句完整则立即执行，不完整则切换为续行提示符等待更多输入
 */
static void PicocApp_HandleReplChar(uint8_t ch)
{
    if (ch == '\r' || ch == '\n')
    {
        PicocApp_SourceState state;
        PicocApp_Command command;
        uint32_t cmd_param = 0U;

        g_last_char_was_cr = (ch == '\r') ? 1U : 0U;
        g_source_buffer[g_source_length] = '\0';
        command = PicocApp_ParseCommand(g_source_buffer, g_source_length, &cmd_param);

        if (command == PICOC_APP_COMMAND_LOAD)
        {
            PicocApp_WriteString("\r\n");
            if (cmd_param > 0U && cmd_param > (PICOC_APP_LOAD_BUFFER_SIZE - 1U))
            {
                PicocApp_SendResponse(PICOC_APP_RESP_ERR_BUFFER_FULL);
                PicocApp_ResetSource();
                g_prompt_pending = 1U;
                return;
            }
            PicocApp_SendResponse(PICOC_APP_RESP_OK);
            PicocApp_ResetSource();
            PicocApp_EnterLoadMode();
            g_prompt_pending = 1U;
            return;
        }

        if (command == PICOC_APP_COMMAND_PING)
        {
            PicocApp_WriteString("\r\n");
            PicocApp_SendResponse(PICOC_APP_RESP_PONG);
            PicocApp_ResetSource();
            g_prompt_pending = 1U;
            return;
        }

        if (command == PICOC_APP_COMMAND_RESET)
        {
            PicocApp_WriteString("\r\n");
            PicocApp_ResetSource();
            PicocApp_ResetLoadBuffer();
            g_mode = PICOC_APP_MODE_REPL;
            g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
            g_reset_pending = 1U;
            PicocApp_SendResponse(PICOC_APP_RESP_OK);
            g_prompt_pending = 1U;
            return;
        }

        if (command == PICOC_APP_COMMAND_BKPT)
        {
            PicocApp_WriteString("\r\n");
            PicocApp_HandleBkptCommand((const char *)g_source_buffer, TRUE);
            PicocApp_ResetSource();
            g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
            g_prompt_pending = 1U;
            return;
        }

        if (command == PICOC_APP_COMMAND_BKPTCLEAR)
        {
            PicocApp_WriteString("\r\n");
            PicocApp_HandleBkptCommand((const char *)g_source_buffer, FALSE);
            PicocApp_ResetSource();
            g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
            g_prompt_pending = 1U;
            return;
        }

        /* 非命令，作为 C 代码处理 */
        PicocApp_WriteString("\r\n");
        (void)PicocApp_AppendByte(g_source_buffer, &g_source_length, sizeof(g_source_buffer), '\n');
        g_source_buffer[g_source_length] = '\0';

        PicocApp_AnalyseSource((const char *)g_source_buffer, &state);

        if (PicocApp_IsSourceComplete((const char *)g_source_buffer, &state) != 0)
        {
            /* 语句完整，立即执行 */
            PicocApp_ExecuteReplSource((const char *)g_source_buffer);
            PicocApp_ResetSource();
            g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
        }
        else
        {
            /* 语句不完整，等待续行 */
            g_prompt_text = INTERACTIVE_PROMPT_LINE;
        }

        g_prompt_pending = 1U;
        return;
    }

    g_last_char_was_cr = 0U;

    /* 退格处理 */
    if (ch == '\b' || ch == 0x7fU)
    {
        if (g_source_length > 0U)
        {
            g_source_length--;
            g_source_buffer[g_source_length] = '\0';
            PicocApp_WriteString("\b \b");
        }
        return;
    }

    /* 追加字符到行缓冲区 */
    if (PicocApp_AppendByte(g_source_buffer, &g_source_length, sizeof(g_source_buffer), ch) == 0)
    {
        PicocApp_SendResponse(PICOC_APP_RESP_ERR_LINE_LONG);
        PicocApp_ResetSource();
        g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
        g_prompt_pending = 1U;
        return;
    }

    /* 回显 */
    PicocApp_WriteByte(ch);
}

/* 加载模式字符处理
 *
 * 收到换行时：
 *   1. 检查协议控制命令（:load / :end / :abort / :ping / :reset / :bkpt / :bkptclear）
 *   2. 非命令则作为代码行累积到加载缓冲区
 *   3. 收到 :end 时执行累积的全部代码，收到 :abort 时丢弃并退出加载模式
 */
static void PicocApp_HandleLoadChar(uint8_t ch)
{
    if (ch == '\r' || ch == '\n')
    {
        PicocApp_Command command;
        uint32_t cmd_param = 0U;

        g_last_char_was_cr = (ch == '\r') ? 1U : 0U;
        g_source_buffer[g_source_length] = '\0';
        command = PicocApp_ParseCommand(g_source_buffer, g_source_length, &cmd_param);

        if (command == PICOC_APP_COMMAND_LOAD)
        {
            PicocApp_WriteString("\r\n");
            if (cmd_param > 0U && cmd_param > (PICOC_APP_LOAD_BUFFER_SIZE - 1U))
            {
                PicocApp_SendResponse(PICOC_APP_RESP_ERR_BUFFER_FULL);
                PicocApp_ResetLineBuffer();
                PicocApp_ResetLoadBuffer();
                PicocApp_LeaveLoadMode();
                g_prompt_pending = 1U;
                return;
            }
            PicocApp_SendResponse(PICOC_APP_RESP_OK);
            PicocApp_ResetLineBuffer();
            PicocApp_ResetLoadBuffer();
            g_prompt_pending = 1U;
            return;
        }

        if (command == PICOC_APP_COMMAND_END)
        {
            PicocApp_WriteString("\r\n");
            PicocApp_ResetLineBuffer();
            PicocApp_ExecuteLoadSource();
            PicocApp_ResetLoadBuffer();
            PicocApp_SendResponse(PICOC_APP_RESP_OK_READY);
            g_prompt_text = PICOC_APP_LOAD_PROMPT;
            g_prompt_pending = 1U;
            return;
        }

        if (command == PICOC_APP_COMMAND_ABORT)
        {
            PicocApp_WriteString("\r\n");
            PicocApp_ResetLineBuffer();
            PicocApp_ResetLoadBuffer();
            PicocApp_SendResponse(PICOC_APP_RESP_ERR_LOAD_CANCELLED);
            PicocApp_LeaveLoadMode();
            g_prompt_pending = 1U;
            return;
        }

        if (command == PICOC_APP_COMMAND_PING)
        {
            PicocApp_WriteString("\r\n");
            PicocApp_SendResponse(PICOC_APP_RESP_PONG);
            PicocApp_ResetLineBuffer();
            g_prompt_pending = 1U;
            return;
        }

        if (command == PICOC_APP_COMMAND_RESET)
        {
            PicocApp_WriteString("\r\n");
            PicocApp_ResetLineBuffer();
            PicocApp_ResetLoadBuffer();
            PicocApp_LeaveLoadMode();
            g_reset_pending = 1U;
            PicocApp_SendResponse(PICOC_APP_RESP_OK);
            g_prompt_pending = 1U;
            return;
        }

        if (command == PICOC_APP_COMMAND_BKPT)
        {
            PicocApp_WriteString("\r\n");
            PicocApp_HandleBkptCommand((const char *)g_source_buffer, TRUE);
            PicocApp_ResetLineBuffer();
            g_prompt_pending = 1U;
            return;
        }

        if (command == PICOC_APP_COMMAND_BKPTCLEAR)
        {
            PicocApp_WriteString("\r\n");
            PicocApp_HandleBkptCommand((const char *)g_source_buffer, FALSE);
            PicocApp_ResetLineBuffer();
            g_prompt_pending = 1U;
            return;
        }

        /* 非命令，作为代码行累积到加载缓冲区 */
        if (PicocApp_AppendBlock(g_load_buffer,
                                 &g_load_length,
                                 sizeof(g_load_buffer),
                                 g_source_buffer,
                                 g_source_length) == 0 ||
            PicocApp_AppendByte(g_load_buffer, &g_load_length, sizeof(g_load_buffer), '\n') == 0)
        {
            PicocApp_SendResponse(PICOC_APP_RESP_ERR_BUFFER_FULL);
            PicocApp_ResetLineBuffer();
            PicocApp_ResetLoadBuffer();
            PicocApp_LeaveLoadMode();
            g_mode = PICOC_APP_MODE_DRAIN;
            g_drain_idle_count = 0U;
            g_last_char_was_cr = 0U;
            g_prompt_pending = 1U;
            return;
        }

        PicocApp_ResetLineBuffer();
        g_prompt_pending = 1U;
        return;
    }

    g_last_char_was_cr = 0U;

    if (ch == '\b' || ch == 0x7fU)
    {
        if (g_source_length > 0U)
        {
            g_source_length--;
            g_source_buffer[g_source_length] = '\0';
        }
        return;
    }

    if (PicocApp_AppendByte(g_source_buffer, &g_source_length, sizeof(g_source_buffer), ch) == 0)
    {
        PicocApp_SendResponse(PICOC_APP_RESP_ERR_LINE_LONG);
        PicocApp_ResetLineBuffer();
        PicocApp_ResetLoadBuffer();
        PicocApp_LeaveLoadMode();
        g_mode = PICOC_APP_MODE_DRAIN;
        g_drain_idle_count = 0U;
        g_last_char_was_cr = 0U;
        g_prompt_pending = 1U;
        return;
    }
}

/* 向缓冲区追加一个字节，尾部保持空终止。
 * 返回 1 表示成功，返回 0 表示缓冲区已满。 */
static int PicocApp_AppendByte(uint8_t *buffer, uint32_t *length, uint32_t capacity, uint8_t ch)
{
    if (*length >= (capacity - 1U))
    {
        return 0;
    }

    buffer[*length] = ch;
    (*length)++;
    buffer[*length] = '\0';
    return 1;
}

/* 向缓冲区追加一个数据块，尾部保持空终止。
 * 返回 1 表示成功，返回 0 表示缓冲区空间不足。 */
static int PicocApp_AppendBlock(uint8_t *buffer, uint32_t *length, uint32_t capacity, const uint8_t *data, uint32_t size)
{
    if ((*length + size) >= capacity)
    {
        return 0;
    }

    (void)memcpy(&buffer[*length], data, size);
    *length += size;
    buffer[*length] = '\0';
    return 1;
}

/* 重置行缓冲区（清空当前行输入） */
static void PicocApp_ResetSource(void)
{
    g_source_length = 0U;
    g_source_buffer[0] = '\0';
}

/* 重置加载累积缓冲区 */
static void PicocApp_ResetLoadBuffer(void)
{
    g_load_length = 0U;
    g_load_buffer[0] = '\0';
}

/* 重置行缓冲区（加载模式中也叫行缓冲区） */
static void PicocApp_ResetLineBuffer(void)
{
    PicocApp_ResetSource();
}

/* 进入文件加载模式 */
static void PicocApp_EnterLoadMode(void)
{
    g_mode = PICOC_APP_MODE_LOAD;
    g_prompt_text = PICOC_APP_LOAD_PROMPT;
    PicocApp_ResetLineBuffer();
    PicocApp_ResetLoadBuffer();
}

/* 离开文件加载模式，回到 REPL */
static void PicocApp_LeaveLoadMode(void)
{
    g_mode = PICOC_APP_MODE_REPL;
    g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
    PicocApp_ResetLineBuffer();
    PicocApp_ResetLoadBuffer();
}

/* 执行 REPL 累积的单行/多行源代码 */
static void PicocApp_ExecuteReplSource(const char *source)
{
    PicocApp_RunSource(&g_picoc, "serial", source, TRUE, FALSE);
}

/* 执行加载模式下累积的全部源代码
 *
 * 在隔离的 PicoC 实例中运行，执行完毕后清理实例并重置断点。
 * 这样加载的脚本不会污染 REPL 全局环境。
 */
void PicocApp_ExecuteLoadSource(void)
{
    static Picoc isolated_picoc;
    int aborted;

    if (g_load_length == 0U)
    {
        PicocApp_SendResponse(PICOC_APP_RESP_ERR_NO_SOURCE);
        return;
    }

    PicocInitialise(&isolated_picoc, PICOC_APP_STACK_SIZE);
    DebugCopyBreakpoints(&g_picoc, &isolated_picoc);
    aborted = PicocApp_RunSource(&isolated_picoc, "serial_load", (const char *)g_load_buffer, FALSE, TRUE);
    if (aborted)
    {
        PicocCleanup(&isolated_picoc);
        PicocInitialise(&isolated_picoc, PICOC_APP_STACK_SIZE);
    }
    PicocCleanup(&isolated_picoc);
    DebugClearAllBreakpoints(&g_picoc);
    PicocApp_ResetLoadBuffer();
    g_mode = PICOC_APP_MODE_REPL;
    g_prompt_text = INTERACTIVE_PROMPT_STATEMENT;
    g_script_running = 0U;
    PicocApp_SendResponse(PICOC_APP_RESP_OK_READY);
    g_prompt_pending = 1U;
}

/* 运行源代码的核心逻辑
 *
 * auto_print_expression: 如果为 TRUE 且源码是表达式，自动包装为 printf 输出
 * auto_call_main: 如果为 TRUE 且解析后产生了 main 函数，自动调用 main()
 *
 * 返回值: 0=正常完成或 PicoC 错误, 1=被 abort 中断
 */
static int PicocApp_RunSource(Picoc *pc, const char *file_name, const char *source, int auto_print_expression, int auto_call_main)
{
    PicocApp_SourceState state;
    char exec_buffer[PICOC_APP_EXEC_BUFFER_SIZE];
    const char *parse_source = source;
    int had_main_before = 0;

    if (source == NULL || source[0] == '\0')
    {
        return 0;
    }

    PicocApp_AnalyseSource(source, &state);

    /* 表达式自动打印：包装为 printf("%d\n",(表达式)); */
    if (auto_print_expression != 0 && PicocApp_ShouldAutoPrintExpression(source, &state) != 0)
    {
        const char *start = PicocApp_SkipLeadingSpace(source);
        const char *end = PicocApp_FindTrimmedEnd(start);
        uint32_t expr_len = (uint32_t)(end - start);

        if ((sizeof("printf(\"%d\\n\",(") - 1U) + expr_len + (sizeof("));\n") - 1U) < sizeof(exec_buffer))
        {
            (void)memcpy(exec_buffer, "printf(\"%d\\n\",(", sizeof("printf(\"%d\\n\",(") - 1U);
            (void)memcpy(exec_buffer + (sizeof("printf(\"%d\\n\",(") - 1U), start, expr_len);
            (void)memcpy(exec_buffer + (sizeof("printf(\"%d\\n\",(") - 1U) + expr_len, "));\n", sizeof("));\n"));
            parse_source = exec_buffer;
        }
    }

    had_main_before = PicocApp_HasMainFunction(pc);

    g_active_picoc = pc;
    if (PicocPlatformSetExitPoint(pc) == 0)
    {
        PicocParse(pc,
                   file_name,
                   parse_source,
                   (int)strlen(parse_source),
                   TRUE,
                   TRUE,
                   FALSE,
                   TRUE);
        DebugCancelStep();

        /* 如果解析产生了新的 main 函数且之前没有，则自动调用 */
        if (auto_call_main != 0 && had_main_before == 0 && PicocApp_HasMainFunction(pc) != 0)
        {
            PicocCallMain(pc, 0, NULL);
        }

        g_active_picoc = NULL;
        return 0;
    }
    else
    {
        DebugCancelStep();
        g_active_picoc = NULL;
        if (pc->AbortRequested)
        {
            pc->AbortRequested = 0;
            return 1;  /* aborted */
        }
        return 0;  /* PicoC error */
    }
}

/* 检查当前 PicoC 实例中是否已定义了 main 函数 */
static int PicocApp_HasMainFunction(Picoc *pc)
{
    return VariableDefined(pc, TableStrRegister(pc, "main"));
}

/* 解析协议命令：识别 :load / :ping / :reset / :end / :abort / :bkpt / :bkptclear
 * 对于 :load 命令，同时解析可选的大小参数 */
static PicocApp_Command PicocApp_ParseCommand(const uint8_t *buffer, uint32_t length, uint32_t *out_param)
{
    uint32_t start = 0U;
    uint32_t end = length;

    *out_param = 0U;

    /* 去除前导空白 */
    while (start < length &&
           (buffer[start] == ' ' || buffer[start] == '\t' || buffer[start] == '\r' || buffer[start] == '\n'))
    {
        start++;
    }

    /* 去除尾部空白 */
    while (end > start &&
           (buffer[end - 1U] == ' ' || buffer[end - 1U] == '\t' || buffer[end - 1U] == '\r' || buffer[end - 1U] == '\n'))
    {
        end--;
    }

    length = end - start;

    /* :load [<大小>] — 进入加载模式，可选大小参数用于预检查缓冲区 */
    if (length >= 5U && memcmp(&buffer[start], ":load", 5U) == 0)
    {
        if (length > 5U && (buffer[start + 5U] == ' ' || buffer[start + 5U] == '\t'))
        {
            uint32_t pos = start + 5U;
            while (pos < end && (buffer[pos] == ' ' || buffer[pos] == '\t'))
            {
                pos++;
            }
            while (pos < end && buffer[pos] >= '0' && buffer[pos] <= '9')
            {
                *out_param = (*out_param * 10U) + (uint32_t)(buffer[pos] - '0');
                pos++;
            }
        }
        return PICOC_APP_COMMAND_LOAD;
    }

    if (length == 5U && memcmp(&buffer[start], ":ping", 5U) == 0)
    {
        return PICOC_APP_COMMAND_PING;
    }

    if (length == 6U && memcmp(&buffer[start], ":reset", 6U) == 0)
    {
        return PICOC_APP_COMMAND_RESET;
    }

    if (length == 4U && memcmp(&buffer[start], ":end", 4U) == 0)
    {
        return PICOC_APP_COMMAND_END;
    }

    if (length == 6U && memcmp(&buffer[start], ":abort", 6U) == 0)
    {
        return PICOC_APP_COMMAND_ABORT;
    }

    if (length >= 6U && memcmp(&buffer[start], ":bkpt", 5U) == 0 && (buffer[start + 5U] == ' ' || buffer[start + 5U] == '\t'))
    {
        return PICOC_APP_COMMAND_BKPT;
    }

    if (length >= 11U && memcmp(&buffer[start], ":bkptclear", 10U) == 0 && (buffer[start + 10U] == ' ' || buffer[start + 10U] == '\t'))
    {
        return PICOC_APP_COMMAND_BKPTCLEAR;
    }

    /* 调试命令：仅在 DebugCheckStatement() 调试循环内有效。
     * REPL/LOAD 模式下收到时识别为 DEBUG 类型，静默消耗，避免被当 C 代码执行。 */
    if (length == 5U && memcmp(&buffer[start], ":vars", 5U) == 0)
        return PICOC_APP_COMMAND_DEBUG;

    if (length >= 6U && memcmp(&buffer[start], ":eval ", 6U) == 0)
        return PICOC_APP_COMMAND_DEBUG;

    if (length >= 5U && memcmp(&buffer[start], ":set ", 5U) == 0)
        return PICOC_APP_COMMAND_DEBUG;

    if (length >= 5U && memcmp(&buffer[start], ":cont", 5U) == 0 && (length == 5U || buffer[start + 5U] == ' ' || buffer[start + 5U] == '\t'))
        return PICOC_APP_COMMAND_DEBUG;

    if (length >= 5U && memcmp(&buffer[start], ":step", 5U) == 0 && (length == 5U || buffer[start + 5U] == ' ' || buffer[start + 5U] == '\t'))
        return PICOC_APP_COMMAND_DEBUG;

    return PICOC_APP_COMMAND_NONE;
}

/* 处理断点设置/清除命令：解析 "文件名 行号" 参数并调用调试器接口 */
static void PicocApp_HandleBkptCommand(const char *cmd_text, int set)
{
    const char *ptr;
    const char *filename_start;
    int filename_len;
    int line_no;
    char filename_buf[64];
    char *reg_file;
    struct ParseState bkpt_parser;

    /* 跳过命令前缀 ":bkpt " 或 ":bkptclear " */
    ptr = cmd_text + 6;
    if (!set)
        ptr += 5;

    while (*ptr == ' ' || *ptr == '\t')
        ptr++;

    /* 提取文件名 */
    filename_start = ptr;
    while (*ptr != ' ' && *ptr != '\t' && *ptr != '\0')
        ptr++;
    filename_len = (int)(ptr - filename_start);
    if (filename_len <= 0 || filename_len >= (int)sizeof(filename_buf))
    {
        PicocApp_SendResponse(":err bkpt invalid filename\r\n");
        return;
    }
    memcpy(filename_buf, filename_start, (size_t)filename_len);
    filename_buf[filename_len] = '\0';

    /* 提取行号 */
    while (*ptr == ' ' || *ptr == '\t')
        ptr++;
    line_no = 0;
    while (*ptr >= '0' && *ptr <= '9')
    {
        line_no = line_no * 10 + (*ptr - '0');
        ptr++;
    }
    if (line_no <= 0)
    {
        PicocApp_SendResponse(":err bkpt invalid line\r\n");
        return;
    }

    reg_file = TableStrRegister(&g_picoc, filename_buf);
    memset(&bkpt_parser, 0, sizeof(bkpt_parser));
    bkpt_parser.pc = &g_picoc;
    bkpt_parser.FileName = reg_file;
    bkpt_parser.Line = line_no;
    bkpt_parser.CharacterPos = 0;

    if (set)
        DebugSetBreakpoint(&bkpt_parser);
    else
        DebugClearBreakpoint(&bkpt_parser);

    PicocApp_SendResponse(set ? ":ok bkpt\r\n" : ":ok bkptclear\r\n");
}

/* 分析源码结构状态
 *
 * 遍历源码字符串，跟踪：
 *   - 括号深度（圆括号、方括号、花括号）
 *   - 字符串/字符字面量状态
 *   - 行注释和块注释状态
 *   - 最后一个非空白字符
 *   - 是否以未闭合的 do 块结尾
 *
 * 这些信息用于判断输入是否构成完整的 C 语句，
 * 以及是否应将输入作为表达式自动打印其值。
 */
static void PicocApp_AnalyseSource(const char *source, PicocApp_SourceState *state)
{
    uint32_t index;
    int escape = 0;
    int line_comment = 0;

    (void)memset(state, 0, sizeof(*state));

    for (index = 0U; source[index] != '\0'; index++)
    {
        char ch = source[index];
        char next = source[index + 1U];

        /* 行注释：持续到换行为止 */
        if (line_comment != 0)
        {
            if (ch == '\n')
            {
                line_comment = 0;
            }
            continue;
        }

        /* 块注释：持续到 *‍/ 为止 */
        if (state->in_block_comment != 0)
        {
            if (ch == '*' && next == '/')
            {
                state->in_block_comment = 0;
                index++;
            }
            continue;
        }

        /* 字符串字面量内的字符 */
        if (state->in_string != 0)
        {
            if (escape != 0)
            {
                escape = 0;
            }
            else if (ch == '\\')
            {
                escape = 1;
            }
            else if (ch == '"')
            {
                state->in_string = 0;
            }
            continue;
        }

        /* 字符字面量内的字符 */
        if (state->in_char != 0)
        {
            if (escape != 0)
            {
                escape = 0;
            }
            else if (ch == '\\')
            {
                escape = 1;
            }
            else if (ch == '\'')
            {
                state->in_char = 0;
            }
            continue;
        }

        /* 检测注释起始 */
        if (ch == '/' && next == '/')
        {
            line_comment = 1;
            index++;
            continue;
        }

        if (ch == '/' && next == '*')
        {
            state->in_block_comment = 1;
            index++;
            continue;
        }

        /* 检测字符串和字符字面量起始 */
        if (ch == '"')
        {
            state->in_string = 1;
            continue;
        }

        if (ch == '\'')
        {
            state->in_char = 1;
            continue;
        }

        /* 跟踪括号深度 */
        if (ch == '(')
        {
            state->paren_depth++;
        }
        else if (ch == ')' && state->paren_depth > 0)
        {
            state->paren_depth--;
        }
        else if (ch == '[')
        {
            state->bracket_depth++;
        }
        else if (ch == ']' && state->bracket_depth > 0)
        {
            state->bracket_depth--;
        }
        else if (ch == '{')
        {
            state->brace_depth++;
        }
        else if (ch == '}' && state->brace_depth > 0)
        {
            state->brace_depth--;
        }

        /* 跟踪最后一个非空白字符 */
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
        {
            state->last_non_space = ch;
        }
    }

    /* 检测 "do { ... }" 模式：如果花括号深度为零且最后一个非空白字符是 }，
     * 且源码以 "do" 开头且没有 while，则标记为未闭合的 do 块 */
    if (state->brace_depth == 0 && state->last_non_space == '}')
    {
        const char *trimmed = PicocApp_SkipLeadingSpace(source);
        if (PicocApp_StartsWithKeyword(trimmed, "do") != 0)
        {
            const char *while_pos = strstr(trimmed, "while");
            state->ends_with_do_block = (while_pos == NULL) ? 1 : 0;
        }
    }
}

/* 判断源码是否为完整的 C 语句（可以提交给解释器执行）
 *
 * 返回真（非零）如果：
 *   - 输入为空
 *   - 括号全部匹配且最后字符是 ; 或 } 或 # 预处理指令
 *   - 是完整的表达式（可以自动打印）
 *
 * 返回假如果：
 *   - 字符串/字符/注释未闭合
 *   - 括号未匹配
 *   - do 块缺少 while
 */
static int PicocApp_IsSourceComplete(const char *source, const PicocApp_SourceState *state)
{
    const char *trimmed = PicocApp_SkipLeadingSpace(source);

    if (*trimmed == '\0')
    {
        return 1;
    }

    if (state->in_string != 0 || state->in_char != 0 || state->in_block_comment != 0)
    {
        return 0;
    }

    if (state->paren_depth != 0 || state->bracket_depth != 0 || state->brace_depth != 0)
    {
        return 0;
    }

    if (state->last_non_space == ';')
    {
        return 1;
    }

    if (state->last_non_space == '}')
    {
        return (state->ends_with_do_block == 0) ? 1 : 0;
    }

    if (*trimmed == '#')
    {
        return 1;
    }

    return PicocApp_ShouldAutoPrintExpression(source, state);
}

/* 判断源码是否应被视为表达式并自动打印其值
 *
 * 将输入包装为 printf("%d\n", (表达式)); 以便在 REPL 中
 * 直接输入表达式即可看到结果。
 *
 * 排除以下情况：
 *   - 以分号、花括号、冒号等结尾的语句
 *   - 控制流关键字（if/for/while/switch/do 等）
 *   - 类型声明关键字（int/char/struct 等）
 *   - 预处理指令
 *   - 包含花括号的代码块
 */
static int PicocApp_ShouldAutoPrintExpression(const char *source, const PicocApp_SourceState *state)
{
    const char *trimmed = PicocApp_SkipLeadingSpace(source);
    const char *end = PicocApp_FindTrimmedEnd(trimmed);
    uint32_t index;

    if (*trimmed == '\0')
    {
        return 0;
    }

    if (state->brace_depth != 0 || state->paren_depth != 0 || state->bracket_depth != 0)
    {
        return 0;
    }

    if (state->last_non_space == ';' || state->last_non_space == '}' || state->last_non_space == ':' || state->last_non_space == '{')
    {
        return 0;
    }

    if (*trimmed == '#')
    {
        return 0;
    }

    if (PicocApp_StartsWithKeyword(trimmed, "if") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "for") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "while") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "switch") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "do") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "else") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "return") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "typedef") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "struct") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "union") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "enum") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "int") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "char") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "short") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "long") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "void") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "unsigned") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "signed") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "static") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "extern") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "auto") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "register") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "break") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "continue") != 0 ||
        PicocApp_StartsWithKeyword(trimmed, "goto") != 0)
    {
        return 0;
    }

    for (index = 0U; trimmed[index] != '\0' && &trimmed[index] < end; index++)
    {
        if (trimmed[index] == '{')
        {
            return 0;
        }
    }

    return 1;
}

/* 跳过字符串开头的空白字符（空格、制表符、回车、换行） */
static const char *PicocApp_SkipLeadingSpace(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
    {
        text++;
    }

    return text;
}

/* 找到去除尾部空白后的字符串末尾位置 */
static const char *PicocApp_FindTrimmedEnd(const char *text)
{
    const char *end = text + strlen(text);

    while (end > text)
    {
        char ch = end[-1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
        {
            break;
        }
        end--;
    }

    return end;
}

/* 检查字符串是否以指定关键字开头
 * 关键字必须是完整的标识符（后续字符不能是字母、数字或下划线） */
static int PicocApp_StartsWithKeyword(const char *text, const char *keyword)
{
    uint32_t len = (uint32_t)strlen(keyword);
    char next = text[len];

    if (strncmp(text, keyword, len) != 0)
    {
        return 0;
    }

    if ((next >= 'a' && next <= 'z') ||
        (next >= 'A' && next <= 'Z') ||
        (next >= '0' && next <= '9') ||
        next == '_')
    {
        return 0;
    }

    return 1;
}
