#pragma once
#include <time.h>
#include <stdbool.h>
#include "main.h"

struct TimedTask;
typedef struct TimedTask* pTimedTask;
struct TimedTask
{
    bool on_use;     //正在使用
    time_t prs_time; //被执行的格林尼治时间戳
    int operation;  //要进行的操作
    int on;          //开或者关，或者其他操作
    int weekday;     //星期重复 0代表不重复 8代表每日重复
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

// 倒计时任务函数
void CountdownTaskInit(void);
void CountdownTaskStart(int total_seconds, int operation);
void CountdownTaskStop(void);
void CountdownTaskTick(void);
char* CountdownTaskGetStatus(void);

// 循环任务函数
void CycleTaskInit(void);
void CycleTaskStart(int on_seconds, int off_seconds, int operation);
void CycleTaskStop(void);
void CycleTaskTick(void);
char* CycleTaskGetStatus(void);
