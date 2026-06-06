/* picoc external interface. This should be the only header you need to use if
 * you're using picoc as a library. Internal details are in interpreter.h */

/* picoc 外部 API 头文件 — 将 picoc 作为库使用时，只需包含此文件即可 */

#ifndef PICOC_H
#define PICOC_H

/* picoc version number */
#ifdef VER
#define PICOC_VERSION "v2.1.1 r" VER         /* VER is the subversion version number, obtained via the Makefile */
#else
#define PICOC_VERSION "v2.1.1"
#endif

/* handy definitions */
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#include "interpreter.h"


#if defined(UNIX_HOST) || defined(WIN32) || defined(STM32H750_HOST)
#include <setjmp.h>

/* this has to be a macro, otherwise errors will occur due to the stack being corrupt */
/* 必须为宏：函数调用会破坏栈帧，导致 setjmp 失效 */
#define PicocPlatformSetExitPoint(pc) setjmp((pc)->PicocExitBuf)
#endif

#ifdef SURVEYOR_HOST
/* mark where to end the program for platforms which require this */
extern int PicocExitBuf[];

#define PicocPlatformSetExitPoint(pc) setjmp((pc)->PicocExitBuf)
#endif

/* parse.c */
/* 解析并执行 C 源文件（或字符串） */
void PicocParse(Picoc *pc, const char *FileName, const char *Source, int SourceLen, int RunIt, int CleanupNow, int CleanupSource, int EnableDebugger);
/* 启动交互式解析循环 */
void PicocParseInteractive(Picoc *pc);

/* platform.c */
/* 调用用户定义的 main 函数 */
void PicocCallMain(Picoc *pc, int argc, char **argv);
/* 初始化 picoc 运行时 */
void PicocInitialise(Picoc *pc, int StackSize);
/* 清理 picoc 运行时资源 */
void PicocCleanup(Picoc *pc);
/* 扫描并加载指定的源文件 */
void PicocPlatformScanFile(Picoc *pc, const char *FileName);

/* include.c */
/* 注册所有内置系统头文件 */
void PicocIncludeAllSystemHeaders(Picoc *pc);

#endif /* PICOC_H */
