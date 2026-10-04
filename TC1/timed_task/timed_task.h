#pragma once
#include <time.h>

/* weekday 字段编码:
 * 定时任务: bit0-6 为星期标志 (Sun=1,Mon=2,...,Sat=64), bit7=0
 *   - 8=仅周三(旧固件曾把 8 当"每日"哨兵, 已废弃)
 *   - 127=每日(全周); 383(127+bit8)=夜间模式每日 LED 专用标记
 * 循环任务: bit7=1, bit0-9=持续时间(分), bit10-19=间隔时间(分),
 *           bit20-30=起点分钟+1(北京分钟 since midnight, 0=旧任务未编码)
 */
#define LOOP_FLAG_BIT       7
#define LOOP_DURATION_SHIFT 0
#define LOOP_INTERVAL_SHIFT 10
#define LOOP_MASK_MINUTES   0x3FF
#define LOOP_START_SHIFT    20
#define LOOP_START_MASK     0x7FF

#define IS_LOOP_TASK(w)         (((w) >> LOOP_FLAG_BIT) & 1)
#define GET_LOOP_DURATION(w)    (((w) >> LOOP_DURATION_SHIFT) & LOOP_MASK_MINUTES)
#define GET_LOOP_INTERVAL(w)    (((w) >> LOOP_INTERVAL_SHIFT) & LOOP_MASK_MINUTES)
#define MAKE_LOOP_WEEKDAY(dur, interval) \
    (0x80 | ((dur) & LOOP_MASK_MINUTES) << LOOP_DURATION_SHIFT | \
     ((interval) & LOOP_MASK_MINUTES) << LOOP_INTERVAL_SHIFT)
/* 起点编码(存 start_min+1, 0 表示旧任务未编码, 回退旧逻辑) */
#define GET_LOOP_START(w)       (((w) >> LOOP_START_SHIFT) & LOOP_START_MASK)
#define MAKE_LOOP_START(min)    (((((min) + 1) & LOOP_START_MASK)) << LOOP_START_SHIFT)

/* 夜间模式每日 LED 任务标记: 全周掩码 + bit8(避开周三=8 冲突, bit7=循环标志不可用) */
#define NIGHT_DAILY_WEEKDAY     (127 | (1 << 8))

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
