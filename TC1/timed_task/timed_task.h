#pragma once
#include <time.h>

/* weekday 字段编码:
 * bit 0     : 循环标志 (1=循环任务, 0=定时任务)
 * bit 1-15  : 持续时间 (分钟, 0-32767)
 * bit 16-30 : 间隔时间 (分钟, 0-32767)
 * bit 31    : 保留
 * 定时任务时 bit1-31 仍用于星期重复 (原 weekday)
 */
#define LOOP_FLAG_BIT       0
#define LOOP_DURATION_SHIFT 1
#define LOOP_INTERVAL_SHIFT 16
#define LOOP_MASK_MINUTES   0x7FFF

#define IS_LOOP_TASK(w)         (((w) >> LOOP_FLAG_BIT) & 1)
#define GET_LOOP_DURATION(w)    (((w) >> LOOP_DURATION_SHIFT) & LOOP_MASK_MINUTES)
#define GET_LOOP_INTERVAL(w)    (((w) >> LOOP_INTERVAL_SHIFT) & LOOP_MASK_MINUTES)
#define MAKE_LOOP_WEEKDAY(dur, interval) \
    (1 | ((dur) & LOOP_MASK_MINUTES) << LOOP_DURATION_SHIFT | \
     ((interval) & LOOP_MASK_MINUTES) << LOOP_INTERVAL_SHIFT)

struct TimedTask;
typedef struct TimedTask* pTimedTask;
struct TimedTask
{
    bool on_use;     //正在使用
    time_t prs_time; //被执行的格林尼治时间戳
    int operation;  //要进行的操作
    int on;          //开或者关，-1=切换
    int weekday;     //星期重复 或 循环编码
    pTimedTask next; //下一个任务(按之间排序)
};

pTimedTask NewTask();
bool AddTask(pTimedTask task);
bool DelTask(int time);
bool DelFirstTask();
void ProcessTask();
char* GetTaskStr();
void TaskLock(void);
void TaskUnlock(void);
void TaskModuleInit(void);
void RebuildTaskList(void);
void ClearAllTasks(void);
