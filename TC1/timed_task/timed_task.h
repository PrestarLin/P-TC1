#pragma once
#include <time.h>

/* weekday 字段编码:
 * 定时任务: bit0-6 为星期标志 (Sun=1,Mon=2,...,Sat=64), bit7=0
 * 循环任务: bit7=1, bit0-9=持续时间(分), bit10-19=间隔时间(分)
 */
#define LOOP_FLAG_BIT       7
#define LOOP_DURATION_SHIFT 0
#define LOOP_INTERVAL_SHIFT 10
#define LOOP_MASK_MINUTES   0x3FF

#define IS_LOOP_TASK(w)         (((w) >> LOOP_FLAG_BIT) & 1)
#define GET_LOOP_DURATION(w)    (((w) >> LOOP_DURATION_SHIFT) & LOOP_MASK_MINUTES)
#define GET_LOOP_INTERVAL(w)    (((w) >> LOOP_INTERVAL_SHIFT) & LOOP_MASK_MINUTES)
#define MAKE_LOOP_WEEKDAY(dur, interval) \
    (0x80 | ((dur) & LOOP_MASK_MINUTES) << LOOP_DURATION_SHIFT | \
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
    int loop_end;    //循环任务结束时间（分钟 since midnight），非循环任务=0
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
void ClearLoopTasks(void);
void ClearScheduledTasks(void);
