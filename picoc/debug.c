/* picoc interactive debugger — 交互式调试器
 *
 * 实现 PicoC 脚本解释器的运行时调试功能，包括：
 *   - 断点管理（哈希表存储，支持设置/清除/复制）
 *   - 单步执行（:step）
 *   - 继续执行（:cont）
 *   - 表达式求值（:eval）
 *   - 变量枚举（:vars）
 *   - 变量修改（:set）
 *   - 中止执行（:abort）
 *
 * 核心入口是 DebugCheckStatement()，在每个语句执行前被调用，
 * 检查断点命中、手动中断、单步标志，然后进入调试命令循环。
 */

#ifndef NO_DEBUGGER

#include "picoc.h"
#include "interpreter.h"
#include <string.h>
#include <stdlib.h>

/* 断点哈希值计算：基于文件名指针和行号 */
#define BREAKPOINT_HASH(p) ( ((unsigned long)(p)->FileName) ^ ((p)->Line << 16) )

/* 单步执行状态标志 */
static int DebugStepNext = FALSE;

/* 初始化调试器：清空断点哈希表 */
void DebugInit(Picoc *pc)
{
    TableInitTable(&pc->BreakpointTable, &pc->BreakpointHashTable[0], BREAKPOINT_TABLE_SIZE, TRUE);
    pc->BreakpointCount = 0;
}

/* 释放断点表中所有表项的内存 */
void DebugCleanup(Picoc *pc)
{
    struct TableEntry *Entry;
    struct TableEntry *NextEntry;
    int Count;

    for (Count = 0; Count < pc->BreakpointTable.Size; Count++)
    {
        for (Entry = pc->BreakpointHashTable[Count]; Entry != NULL; Entry = NextEntry)
        {
            NextEntry = Entry->Next;
            HeapFreeMem(pc, Entry);
        }
    }
}

/* 在断点哈希表中查找断点
 * 找到则返回表项指针，未找到则通过 AddAt 返回应插入的哈希桶索引 */
static struct TableEntry *DebugTableSearchBreakpoint(struct ParseState *Parser, int *AddAt)
{
    struct TableEntry *Entry;
    Picoc *pc = Parser->pc;
    int HashValue = BREAKPOINT_HASH(Parser) % pc->BreakpointTable.Size;

    for (Entry = pc->BreakpointHashTable[HashValue]; Entry != NULL; Entry = Entry->Next)
    {
        if (Entry->p.b.FileName == Parser->FileName && Entry->p.b.Line == Parser->Line)
            return Entry;   /* 找到已存在的断点 */
    }

    *AddAt = HashValue;    /* 未找到，返回应插入的桶位置 */
    return NULL;
}

/* 在当前位置设置断点 */
void DebugSetBreakpoint(struct ParseState *Parser)
{
    int AddAt;
    struct TableEntry *FoundEntry = DebugTableSearchBreakpoint(Parser, &AddAt);
    Picoc *pc = Parser->pc;

    if (FoundEntry == NULL)
    {
        /* 新断点，插入哈希表 */
        struct TableEntry *NewEntry = HeapAllocMem(pc, sizeof(struct TableEntry));
        if (NewEntry == NULL)
            ProgramFailNoParser(pc, "out of memory");

        NewEntry->p.b.FileName = Parser->FileName;
        NewEntry->p.b.Line = Parser->Line;
        NewEntry->p.b.CharacterPos = Parser->CharacterPos;
        NewEntry->Next = pc->BreakpointHashTable[AddAt];
        pc->BreakpointHashTable[AddAt] = NewEntry;
        pc->BreakpointCount++;
    }
}

/* 删除当前位置的断点，成功返回 TRUE */
int DebugClearBreakpoint(struct ParseState *Parser)
{
    struct TableEntry **EntryPtr;
    Picoc *pc = Parser->pc;
    int HashValue = BREAKPOINT_HASH(Parser) % pc->BreakpointTable.Size;

    for (EntryPtr = &pc->BreakpointHashTable[HashValue]; *EntryPtr != NULL; EntryPtr = &(*EntryPtr)->Next)
    {
        struct TableEntry *DeleteEntry = *EntryPtr;
        if (DeleteEntry->p.b.FileName == Parser->FileName && DeleteEntry->p.b.Line == Parser->Line)
        {
            *EntryPtr = DeleteEntry->Next;
            HeapFreeMem(pc, DeleteEntry);
            pc->BreakpointCount--;

            return TRUE;
        }
    }

    return FALSE;
}

/* 处理断点设置/清除命令：":bkpt <文件名> <行号>" 或 ":bkptclear <文件名> <行号>" */
static void DebugHandleBkpt(Picoc *pc, const char *cmd, int set)
{
    const char *ptr;
    const char *filename_start;
    int filename_len;
    int line_no;
    char filename_buf[64];
    char *reg_file;
    struct ParseState bkpt_parser;

    ptr = cmd + 6; /* 跳过 ":bkpt " 或 ":bkptc"，后面如果是 clear 则再跳过 "lear " */
    if (!set)
        ptr += 5; /* 跳过 "lear " */

    /* 跳过前导空格 */
    while (*ptr == ' ')
        ptr++;

    /* 提取文件名 */
    filename_start = ptr;
    while (*ptr != ' ' && *ptr != '\0')
        ptr++;
    filename_len = (int)(ptr - filename_start);
    if (filename_len <= 0 || filename_len >= (int)sizeof(filename_buf))
        return;

    memcpy(filename_buf, filename_start, (size_t)filename_len);
    filename_buf[filename_len] = '\0';

    /* 提取行号 */
    while (*ptr == ' ')
        ptr++;
    line_no = 0;
    while (*ptr >= '0' && *ptr <= '9')
    {
        line_no = line_no * 10 + (*ptr - '0');
        ptr++;
    }

    if (line_no <= 0)
        return;

    /* 注册文件名字符串并构造 ParseState */
    reg_file = TableStrRegister(pc, filename_buf);
    memset(&bkpt_parser, 0, sizeof(bkpt_parser));
    bkpt_parser.pc = pc;
    bkpt_parser.FileName = reg_file;
    bkpt_parser.Line = line_no;
    bkpt_parser.CharacterPos = 0;

    if (set)
        DebugSetBreakpoint(&bkpt_parser);
    else
        DebugClearBreakpoint(&bkpt_parser);

    PlatformPrintf(pc->CStdOut, ":ok %s\r\n", set ? "bkpt" : "bkptclear");
}

/* 处理表达式求值命令：":eval <表达式>"
 * 将表达式包装为 printf("%d\n",(表达式));  然后解析执行 */
static void DebugHandleEval(Picoc *pc, const char *cmd)
{
    const char *expr;
    char exec_buf[512];
    int expr_len;
    int total_len;
    const char *prefix = "printf(\"%d\\n\",(";
    int prefix_len = (int)strlen(prefix);
    const char *suffix = "));\n";
    int suffix_len = (int)strlen(suffix);
    jmp_buf saved_exit_buf;

    expr = cmd + 6; /* 跳过 ":eval " */
    while (*expr == ' ')
        expr++;

    expr_len = (int)strlen(expr);
    if (expr_len == 0)
    {
        PlatformPrintf(pc->CStdOut, ":err eval empty expression\r\n");
        return;
    }

    total_len = prefix_len + expr_len + suffix_len;
    if (total_len >= (int)sizeof(exec_buf))
    {
        PlatformPrintf(pc->CStdOut, ":err eval expression too long\r\n");
        return;
    }

    /* 拼接完整可执行语句 */
    memcpy(exec_buf, prefix, (size_t)prefix_len);
    memcpy(exec_buf + prefix_len, expr, (size_t)expr_len);
    memcpy(exec_buf + prefix_len + expr_len, suffix, (size_t)suffix_len);
    exec_buf[total_len] = '\0';

    /* 保存并恢复退出点，避免 eval 错误跳出调试循环 */
    memcpy(&saved_exit_buf, &pc->PicocExitBuf, sizeof(jmp_buf));

    if (setjmp(pc->PicocExitBuf) == 0)
    {
        PicocParse(pc, "debug_eval", exec_buf, total_len, TRUE, TRUE, TRUE, FALSE);
        PlatformPrintf(pc->CStdOut, ":ok eval\r\n");
    }
    else
    {
        PlatformPrintf(pc->CStdOut, ":err eval failed\r\n");
    }

    memcpy(&pc->PicocExitBuf, &saved_exit_buf, sizeof(jmp_buf));
}

/* 枚举所有可见变量并发送给调试客户端
 * 先导出局部变量（如果有活动栈帧），再导出全局变量
 * 输出格式：":var <类型字符> <变量名> <值>\r\n" */
static void DebugSendVars(Picoc *pc)
{
    struct TableEntry *Entry;
    struct Table *table;
    int table_idx;

    /* 先处理局部变量（活动栈帧） */
    if (pc->TopStackFrame != NULL)
    {
        table = &pc->TopStackFrame->LocalTable;
        for (table_idx = 0; table_idx < table->Size; table_idx++)
        {
            for (Entry = table->HashTable[table_idx]; Entry != NULL; Entry = Entry->Next)
            {
                struct Value *val = Entry->p.v.Val;
                char *key = Entry->p.v.Key;
                char type_char;

                if (key == NULL || val == NULL || val->OutOfScope)
                    continue;

                /* 类型映射到单字符标识 */
                switch (val->Typ->Base)
                {
                    case TypeInt:            type_char = 'i'; break;
                    case TypeShort:          type_char = 's'; break;
                    case TypeChar:           type_char = 'c'; break;
                    case TypeLong:           type_char = 'l'; break;
                    case TypeUnsignedInt:    type_char = 'I'; break;
                    case TypeUnsignedShort:  type_char = 'S'; break;
                    case TypeUnsignedChar:   type_char = 'C'; break;
                    case TypeUnsignedLong:   type_char = 'L'; break;
#ifndef NO_FP
                    case TypeFP:             type_char = 'f'; break;
#endif
                    case TypePointer:        type_char = 'p'; break;
                    default: continue;  /* 跳过函数、宏、类型、数组、结构体 */
                }

                /* 输出变量信息头：:var <类型> <名称> */
                PrintStr(":var ", pc->CStdOut);
                PrintCh(type_char, pc->CStdOut);
                PrintCh(' ', pc->CStdOut);
                PrintStr(key, pc->CStdOut);
                PrintCh(' ', pc->CStdOut);

                /* 根据类型输出变量值 */
                switch (val->Typ->Base)
                {
                    case TypeInt:            PrintSimpleInt((long)val->Val->Integer, pc->CStdOut); break;
                    case TypeShort:          PrintSimpleInt((long)val->Val->ShortInteger, pc->CStdOut); break;
                    case TypeChar:           PrintSimpleInt((long)val->Val->Character, pc->CStdOut); break;
                    case TypeLong:           PrintSimpleInt(val->Val->LongInteger, pc->CStdOut); break;
                    case TypeUnsignedInt:
                    {
                        char buf[32];
                        int pos = 0;
                        unsigned int v = val->Val->UnsignedInteger;
                        if (v == 0) { buf[pos++] = '0'; }
                        else { while (v > 0) { buf[pos++] = '0' + (char)(v % 10); v /= 10; } }
                        while (pos > 0) PrintCh(buf[--pos], pc->CStdOut);
                        break;
                    }
                    case TypeUnsignedShort:
                    {
                        char buf[16];
                        int pos = 0;
                        unsigned short v = val->Val->UnsignedShortInteger;
                        if (v == 0) { buf[pos++] = '0'; }
                        else { while (v > 0) { buf[pos++] = '0' + (char)(v % 10); v /= 10; } }
                        while (pos > 0) PrintCh(buf[--pos], pc->CStdOut);
                        break;
                    }
                    case TypeUnsignedChar:
                    {
                        char buf[8];
                        int pos = 0;
                        unsigned char v = val->Val->UnsignedCharacter;
                        if (v == 0) { buf[pos++] = '0'; }
                        else { while (v > 0) { buf[pos++] = '0' + (char)(v % 10); v /= 10; } }
                        while (pos > 0) PrintCh(buf[--pos], pc->CStdOut);
                        break;
                    }
                    case TypeUnsignedLong:
                    {
                        char buf[32];
                        int pos = 0;
                        unsigned long v = val->Val->UnsignedLongInteger;
                        if (v == 0) { buf[pos++] = '0'; }
                        else { while (v > 0) { buf[pos++] = '0' + (char)(v % 10); v /= 10; } }
                        while (pos > 0) PrintCh(buf[--pos], pc->CStdOut);
                        break;
                    }
#ifndef NO_FP
                    case TypeFP:             PrintFP(val->Val->FP, pc->CStdOut); break;
#endif
                    case TypePointer:
                    {
                        /* 指针以十六进制 "0x..." 格式输出 */
                        PrintStr("0x", pc->CStdOut);
                        {
                            unsigned long addr = (unsigned long)val->Val->Pointer;
                            char hex[16];
                            int pos = 0;
                            if (addr == 0) { hex[pos++] = '0'; }
                            else {
                                while (addr > 0) {
                                    int nibble = (int)(addr & 0xF);
                                    hex[pos++] = (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
                                    addr >>= 4;
                                }
                            }
                            while (pos > 0) PrintCh(hex[--pos], pc->CStdOut);
                        }
                        break;
                    }
                    default: break;
                }

                PrintCh('\r', pc->CStdOut);
                PrintCh('\n', pc->CStdOut);
            }
        }
    }

    /* 全局变量 */
    table = &pc->GlobalTable;
    for (table_idx = 0; table_idx < table->Size; table_idx++)
    {
        for (Entry = table->HashTable[table_idx]; Entry != NULL; Entry = Entry->Next)
        {
            struct Value *val = Entry->p.v.Val;
            char *key = Entry->p.v.Key;
            char type_char;

            if (key == NULL || val == NULL || val->OutOfScope)
                continue;

            switch (val->Typ->Base)
            {
                case TypeInt:            type_char = 'i'; break;
                case TypeShort:          type_char = 's'; break;
                case TypeChar:           type_char = 'c'; break;
                case TypeLong:           type_char = 'l'; break;
                case TypeUnsignedInt:    type_char = 'I'; break;
                case TypeUnsignedShort:  type_char = 'S'; break;
                case TypeUnsignedChar:   type_char = 'C'; break;
                case TypeUnsignedLong:   type_char = 'L'; break;
#ifndef NO_FP
                case TypeFP:             type_char = 'f'; break;
#endif
                case TypePointer:        type_char = 'p'; break;
                default: continue;
            }

            PrintStr(":var ", pc->CStdOut);
            PrintCh(type_char, pc->CStdOut);
            PrintCh(' ', pc->CStdOut);
            PrintStr(key, pc->CStdOut);
            PrintCh(' ', pc->CStdOut);

            switch (val->Typ->Base)
            {
                case TypeInt:            PrintSimpleInt((long)val->Val->Integer, pc->CStdOut); break;
                case TypeShort:          PrintSimpleInt((long)val->Val->ShortInteger, pc->CStdOut); break;
                case TypeChar:           PrintSimpleInt((long)val->Val->Character, pc->CStdOut); break;
                case TypeLong:           PrintSimpleInt(val->Val->LongInteger, pc->CStdOut); break;
                case TypeUnsignedInt:
                {
                    char buf[32]; int pos = 0;
                    unsigned int v = val->Val->UnsignedInteger;
                    if (v == 0) { buf[pos++] = '0'; }
                    else { while (v > 0) { buf[pos++] = '0' + (char)(v % 10); v /= 10; } }
                    while (pos > 0) PrintCh(buf[--pos], pc->CStdOut);
                    break;
                }
                case TypeUnsignedShort:
                {
                    char buf[16]; int pos = 0;
                    unsigned short v = val->Val->UnsignedShortInteger;
                    if (v == 0) { buf[pos++] = '0'; }
                    else { while (v > 0) { buf[pos++] = '0' + (char)(v % 10); v /= 10; } }
                    while (pos > 0) PrintCh(buf[--pos], pc->CStdOut);
                    break;
                }
                case TypeUnsignedChar:
                {
                    char buf[8]; int pos = 0;
                    unsigned char v = val->Val->UnsignedCharacter;
                    if (v == 0) { buf[pos++] = '0'; }
                    else { while (v > 0) { buf[pos++] = '0' + (char)(v % 10); v /= 10; } }
                    while (pos > 0) PrintCh(buf[--pos], pc->CStdOut);
                    break;
                }
                case TypeUnsignedLong:
                {
                    char buf[32]; int pos = 0;
                    unsigned long v = val->Val->UnsignedLongInteger;
                    if (v == 0) { buf[pos++] = '0'; }
                    else { while (v > 0) { buf[pos++] = '0' + (char)(v % 10); v /= 10; } }
                    while (pos > 0) PrintCh(buf[--pos], pc->CStdOut);
                    break;
                }
#ifndef NO_FP
                case TypeFP:             PrintFP(val->Val->FP, pc->CStdOut); break;
#endif
                case TypePointer:
                {
                    unsigned long addr = (unsigned long)val->Val->Pointer;
                    char hex[16]; int pos = 0;
                    PrintStr("0x", pc->CStdOut);
                    if (addr == 0) { PrintCh('0', pc->CStdOut); }
                    else {
                        while (addr > 0) {
                            int nibble = (int)(addr & 0xF);
                            hex[pos++] = (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
                            addr >>= 4;
                        }
                        while (pos > 0) PrintCh(hex[--pos], pc->CStdOut);
                    }
                    break;
                }
                default: break;
            }

            PrintCh('\r', pc->CStdOut);
            PrintCh('\n', pc->CStdOut);
        }
    }

    PrintStr(":ok vars\r\n", pc->CStdOut);
}

/* 处理变量修改命令：":set <变量名> <值>"
 * 支持整型、短整型、字符型、长整型、无符号各类型、浮点、指针 */
static void DebugHandleSet(Picoc *pc, const char *cmd)
{
    const char *ptr;
    const char *varname_start;
    int varname_len;
    char varname_buf[64];
    char *reg_name;
    struct Value *varValue = NULL;
    struct ParseState tempParser;
    const char *value_str;
    jmp_buf saved_exit_buf;

    ptr = cmd + 5; /* 跳过 ":set " */
    while (*ptr == ' ')
        ptr++;

    /* 提取变量名 */
    varname_start = ptr;
    while (*ptr != ' ' && *ptr != '\0')
        ptr++;
    varname_len = (int)(ptr - varname_start);
    if (varname_len <= 0 || varname_len >= (int)sizeof(varname_buf))
    {
        PlatformPrintf(pc->CStdOut, ":err set invalid name\r\n");
        return;
    }
    memcpy(varname_buf, varname_start, (size_t)varname_len);
    varname_buf[varname_len] = '\0';

    /* 跳过空白，定位到值 */
    while (*ptr == ' ')
        ptr++;
    value_str = ptr;
    if (*value_str == '\0')
    {
        PlatformPrintf(pc->CStdOut, ":err set missing value\r\n");
        return;
    }

    reg_name = TableStrRegister(pc, varname_buf);
    memset(&tempParser, 0, sizeof(tempParser));
    tempParser.pc = pc;

    /* 用 setjmp 捕获变量查找过程中的错误 */
    memcpy(&saved_exit_buf, &pc->PicocExitBuf, sizeof(jmp_buf));

    if (setjmp(pc->PicocExitBuf) == 0)
    {
        VariableGet(pc, &tempParser, reg_name, &varValue);
    }
    else
    {
        memcpy(&pc->PicocExitBuf, &saved_exit_buf, sizeof(jmp_buf));
        PlatformPrintf(pc->CStdOut, ":err set '%s' not found\r\n", varname_buf);
        return;
    }

    memcpy(&pc->PicocExitBuf, &saved_exit_buf, sizeof(jmp_buf));

    if (varValue == NULL || !varValue->IsLValue)
    {
        PlatformPrintf(pc->CStdOut, ":err set '%s' is not modifiable\r\n", varname_buf);
        return;
    }

    /* 根据变量类型解析并设置值 */
    switch (varValue->Typ->Base)
    {
        case TypeInt:
            varValue->Val->Integer = (int)strtol(value_str, NULL, 0);
            break;
        case TypeShort:
            varValue->Val->ShortInteger = (short)strtol(value_str, NULL, 0);
            break;
        case TypeChar:
        {
            /* 支持字符字面量，如 'A' */
            const char *v = value_str;
            while (*v == ' ') v++;
            if (*v == '\'' && v[1] != '\0' && v[2] == '\'')
                varValue->Val->Character = v[1];
            else
                varValue->Val->Character = (char)strtol(value_str, NULL, 0);
            break;
        }
        case TypeLong:
            varValue->Val->LongInteger = strtol(value_str, NULL, 0);
            break;
        case TypeUnsignedInt:
            varValue->Val->UnsignedInteger = (unsigned int)strtoul(value_str, NULL, 0);
            break;
        case TypeUnsignedShort:
            varValue->Val->UnsignedShortInteger = (unsigned short)strtoul(value_str, NULL, 0);
            break;
        case TypeUnsignedChar:
            varValue->Val->UnsignedCharacter = (unsigned char)strtoul(value_str, NULL, 0);
            break;
        case TypeUnsignedLong:
            varValue->Val->UnsignedLongInteger = strtoul(value_str, NULL, 0);
            break;
#ifndef NO_FP
        case TypeFP:
            varValue->Val->FP = strtod(value_str, NULL);
            break;
#endif
        case TypePointer:
            varValue->Val->Pointer = (void *)(intptr_t)strtol(value_str, NULL, 0);
            break;
        default:
            PlatformPrintf(pc->CStdOut, ":err set unsupported type\r\n");
            return;
    }

    PlatformPrintf(pc->CStdOut, ":ok set\r\n");
}

/* 语句执行前检查——调试器核心入口
 *
 * 在每个语句执行前被 PicoC 解释器调用，检查以下触发条件：
 *   1. 单步标志 DebugStepNext（上一条 :step 命令设置）
 *   2. 手动中断标志 DebugManualBreak
 *   3. 当前位置是否命中断点
 *
 * 任一条件满足则进入调试命令循环，等待并处理以下命令：
 *   :cont   — 继续执行
 *   :step   — 单步执行下一条语句
 *   :abort  — 中止执行
 *   :bkpt   — 设置断点
 *   :bkptclear — 清除断点
 *   :eval   — 表达式求值
 *   :vars   — 枚举所有可见变量
 *   :set    — 修改变量值
 */
void DebugCheckStatement(struct ParseState *Parser)
{
    int DoBreak = FALSE;
    int WasStep = FALSE;
    int AddAt;
    Picoc *pc = Parser->pc;
    char LineBuf[256];
    char *line;
    int len;
    extern volatile int g_debug_input_active;

    /* 检查单步标志 */
    if (DebugStepNext)
    {
        DoBreak = TRUE;
        DebugStepNext = FALSE;
        WasStep = TRUE;
    }

    /* 检查手动中断 */
    if (pc->DebugManualBreak)
    {
        DoBreak = TRUE;
        pc->DebugManualBreak = FALSE;
    }

    /* 检查断点命中 */
    if (pc->BreakpointCount != 0 && DebugTableSearchBreakpoint(Parser, &AddAt) != NULL)
        DoBreak = TRUE;

    /* 进入调试命令循环 */
    while (DoBreak)
    {
        /* 通知客户端当前暂停位置 */
        PlatformPrintf(pc->CStdOut, "%s %s %d %d\r\n",
            WasStep ? ":step" : ":break",
            Parser->FileName ? Parser->FileName : "(none)",
            Parser->Line, Parser->CharacterPos);
        WasStep = FALSE;  /* 重置，后续再进循环不会误报为 step */

        /* 接管串口输入：serialTask 检测到此标志后停止消费 rx_ring */
        g_debug_input_active = 1;

        /* 调试命令循环 */
        for (;;)
        {
            line = PlatformGetLineQuiet(LineBuf, sizeof(LineBuf));
            if (line == NULL)
                break;

            /* 去除尾部换行符 */
            len = (int)strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = '\0';

            /* 跳过空行（\r\n 组合产生的尾随 \n 也会到这里） */
            if (len == 0)
                continue;

            /* :cont — 继续执行 */
            if (len >= 5 && strncmp(line, ":cont", 5) == 0 && (len == 5 || line[5] == ' '))
            {
                g_debug_input_active = 0;
                return;
            }

            /* :step — 单步执行（flag 保持置位，下一条语句进入调试循环前 serialTask 不可读） */
            if (len >= 5 && strncmp(line, ":step", 5) == 0 && (len == 5 || line[5] == ' '))
            {
                DebugStepNext = TRUE;
                return;
            }

            /* :abort — 中止执行 */
            if (len == 6 && strncmp(line, ":abort", 6) == 0)
            {
                PlatformPrintf(pc->CStdOut, ":err load cancelled\r\n");
                g_debug_input_active = 0;
                longjmp(pc->PicocExitBuf, 1);
            }

            /* :bkptclear <文件名> <行号> — 清除断点 */
            if (len >= 12 && strncmp(line, ":bkptclear ", 11) == 0)
            {
                DebugHandleBkpt(pc, line, FALSE);
                continue;
            }

            /* :bkpt <文件名> <行号> — 设置断点 */
            if (len >= 6 && strncmp(line, ":bkpt ", 6) == 0)
            {
                DebugHandleBkpt(pc, line, TRUE);
                continue;
            }

            /* :eval <表达式> — 表达式求值 */
            if (len >= 6 && strncmp(line, ":eval ", 6) == 0)
            {
                DebugHandleEval(pc, line);
                continue;
            }

            /* :vars — 列出所有可见变量 */
            if (len == 5 && strncmp(line, ":vars", 5) == 0)
            {
                DebugSendVars(pc);
                continue;
            }

            /* :set <变量名> <值> — 修改变量 */
            if (len >= 6 && strncmp(line, ":set ", 5) == 0)
            {
                DebugHandleSet(pc, line);
                continue;
            }

            /* 未知命令 — 回显错误 */
            PlatformPrintf(pc->CStdOut, ":err debug unknown: %s\r\n", line);
        }

        /* PlatformGetLine 返回 NULL，退出调试循环 */
        g_debug_input_active = 0;
        break;
    }
    g_debug_input_active = 0;
}

/* 将 src 实例中的所有断点复制到 dest 实例 */
void DebugCopyBreakpoints(Picoc *src, Picoc *dest)
{
    int Count;
    struct TableEntry *Entry;
    struct TableEntry *NextEntry;

    for (Count = 0; Count < src->BreakpointTable.Size; Count++)
    {
        for (Entry = src->BreakpointHashTable[Count]; Entry != NULL; Entry = NextEntry)
        {
            int DestHash;

            NextEntry = Entry->Next;

            struct TableEntry *NewEntry = HeapAllocMem(dest, sizeof(struct TableEntry));
            if (NewEntry == NULL)
                return;

            NewEntry->p.b.FileName = TableStrRegister(dest, Entry->p.b.FileName);
            NewEntry->p.b.Line = Entry->p.b.Line;
            NewEntry->p.b.CharacterPos = Entry->p.b.CharacterPos;

            DestHash = ((unsigned long)NewEntry->p.b.FileName ^
                        ((unsigned long)NewEntry->p.b.Line << 16)) %
                       (unsigned long)dest->BreakpointTable.Size;

            NewEntry->Next = dest->BreakpointHashTable[DestHash];
            dest->BreakpointHashTable[DestHash] = NewEntry;
            dest->BreakpointCount++;
        }
    }
}

/* 设置单步标志，下一条语句将暂停 */
void DebugStep()
{
    DebugStepNext = TRUE;
}

/* 取消单步标志，释放串口输入给 serialTask */
void DebugCancelStep(void)
{
    extern volatile int g_debug_input_active;
    DebugStepNext = FALSE;
    g_debug_input_active = 0;
}

/* 清除所有断点并重新初始化断点表 */
void DebugClearAllBreakpoints(Picoc *pc)
{
    DebugCleanup(pc);
    DebugInit(pc);
}
#endif /* !NO_DEBUGGER */
