#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<stdbool.h>
#include<time.h>

#include"main.h"
#include"user_gpio.h"
#include "mqtt_server/user_mqtt_client.h"
#include"timed_task/timed_task.h"
#include"http_server/web_log.h"
#include "user_wifi.h"

int day_sec = 86400;
mico_mutex_t task_mutex;

bool AddTaskSingle(pTimedTask task);

void TaskModuleInit(void)
{
    mico_rtos_init_mutex(&task_mutex);
}

void TaskLock(void)
{
    mico_rtos_lock_mutex(&task_mutex);
}

void TaskUnlock(void)
{
    mico_rtos_unlock_mutex(&task_mutex);
}

void RebuildTaskList(void)
{
    user_config->task_top = NULL;
    user_config->task_count = 0;
    time_t now = time(NULL);
    for (int i = 0; i < MAX_TASK_NUM; i++)
    {
        if (user_config->timed_tasks[i].on_use)
        {
            user_config->timed_tasks[i].next = NULL;
            pTimedTask task = &user_config->timed_tasks[i];

            if (task->weekday != 0 && task->prs_time <= now)
            {
                if (task->weekday == 8)
                {
                    task->prs_time = (now - now % day_sec) + task->prs_time % day_sec;
                    if (task->prs_time <= now)
                        task->prs_time += day_sec;
                }
                else
                {
                    AddTask(task);
                }
            }
            else
            {
                AddTaskSingle(task);
            }
        }
    }
}

pTimedTask NewTask()
{
    for (int i = 0; i < MAX_TASK_NUM; i++)
    {
        pTimedTask task = &user_config->timed_tasks[i];
        if (!task->on_use)
        {
            task->on_use = true;
            return task;
        }
    }
    return NULL;
}

bool AddTaskSingle(pTimedTask task)
{
    user_config->task_count++;
    if (user_config->task_top == NULL)
    {
        task->next = NULL;
        user_config->task_top = task;
        return true;
    }

    if (task->prs_time <= user_config->task_top->prs_time)
    {
        task->next = user_config->task_top;
        user_config->task_top = task;
        return true;
    }

    pTimedTask tmp = user_config->task_top;
    while (tmp)
    {
        if (tmp->next == NULL
            || (task->prs_time >= tmp->prs_time
             && task->prs_time < tmp->next->prs_time))
        {
            task->next = tmp->next;
            tmp->next = task;
            return true;
        }
        tmp = tmp->next;
    }
    user_config->task_count--;
    return false;
}

bool AddTaskWeek(pTimedTask task)
{
    time_t now = time(NULL);
    int today_weekday = (now / day_sec + 3) % 7 + 1;
    int next_day = task->weekday - today_weekday;
    bool next_day_is_today = next_day == 0 && task->prs_time % day_sec > now % day_sec;
    next_day = next_day > 0 || next_day_is_today ? next_day : next_day + 7;
    task->prs_time = (now - now % day_sec) + (next_day * day_sec) + task->prs_time % day_sec;

    return AddTaskSingle(task);
}

bool AddTask(pTimedTask task)
{
    if (task->weekday == 0 || task->weekday == 8)
        return AddTaskSingle(task);
    return AddTaskWeek(task);
}

bool DelFirstTask()
{
    if (user_config->task_top)
    {
        pTimedTask tmp = user_config->task_top;
        user_config->task_top = user_config->task_top->next;
        user_config->task_count--;
        if (tmp->weekday == 0)
        {
            tmp->on_use = false;
        }
        else if (tmp->weekday == 8)
        {
            tmp->prs_time += day_sec;
            AddTask(tmp);
        }
        else
        {
            tmp->prs_time += 7 * day_sec;
            AddTask(tmp);
        }
        mico_system_context_update(sys_config);
        return true;
    }
    return false;
}

bool DelTask(int time)
{
    if (user_config->task_top == NULL)
    {
        return false;
    }

    if (time == user_config->task_top->prs_time)
    {
        pTimedTask tmp = user_config->task_top;
        user_config->task_top = user_config->task_top->next;
        tmp->on_use = false;
        user_config->task_count--;
        mico_system_context_update(sys_config);
        return true;
    }
    else if (user_config->task_top->next == NULL)
    {
        return false;
    }

    pTimedTask pre_tsk = user_config->task_top;
    pTimedTask tmp_tsk = user_config->task_top->next;
    while (tmp_tsk)
    {
        if (time == tmp_tsk->prs_time)
        {
            pre_tsk->next = tmp_tsk->next;
            tmp_tsk->on_use = false;
            user_config->task_count--;
            mico_system_context_update(sys_config);
            return true;
        }
        tmp_tsk = tmp_tsk->next;
    }
    return false;
}

void ProcessTask()
{
    task_log("process task time[%ld] operation[%s] on[%d]",
        user_config->task_top->prs_time, get_func_name(user_config->task_top->operation), user_config->task_top->on);
    switch (user_config->task_top->operation) {
            case SWITCH_ALL_SOCKETS:
                UserRelaySetAll(user_config->task_top->on);
                mico_system_context_update(sys_config);
                for (int i = 0; i < SOCKET_NUM; i++) {
                    UserMqttSendSocketState(i);
                }
                UserMqttSendTotalSocketState();
                break;
            case SWITCH_SOCKET_1:
            case SWITCH_SOCKET_2:
            case SWITCH_SOCKET_3:
            case SWITCH_SOCKET_4:
            case SWITCH_SOCKET_5:
            case SWITCH_SOCKET_6:
                UserRelaySet(user_config->task_top->operation - 1, user_config->task_top->on);
                UserMqttSendSocketState(user_config->task_top->operation - 1);
                UserMqttSendTotalSocketState();
                mico_system_context_update(sys_config);
                break;
            case SWITCH_LED_ENABLE:

                if (RelayOut() && user_config->task_top->on) {
                    UserLedSet(1);
                } else {
                    UserLedSet(0);
                }
                UserMqttSendLedState();
                mico_system_context_update(sys_config);
                break;
            case SWITCH_CHILD_LOCK_ENABLE:
                user_config->child_lock = user_config->task_top->on;
                childLockEnabled = user_config->child_lock;
                mico_system_context_update(sys_config);
                UserMqttSendChildLockState();
                break;
            case REBOOT_SYSTEM:
                MicoSystemReboot();
                break;
            case CONFIG_WIFI:

                micoWlanSuspendStation();
                ApInit(true);
                break;
            case RESET_SYSTEM:

                mico_system_context_restore(sys_config);
                mico_rtos_thread_sleep(1);
                MicoSystemReboot();
                break;
            default:
                break;
        }
    DelFirstTask();
}

char* GetTaskStr()
{
    char* str = (char*)malloc(sizeof(char)*(user_config->task_count*128+2));
    if (!str) return NULL;
    pTimedTask tmp_tsk = user_config->task_top;
    char* tmp_str = str;
    tmp_str[0] = '[';
    if (user_config->task_count == 0) {
        tmp_str[1] = ']';
        tmp_str[2] = '\0';
        return str;
    }
    tmp_str[1] = '\0';
    tmp_str++;
    while (tmp_tsk)
    {
        char buffer[26];
        struct tm* tm_info;
        time_t prs_time = tmp_tsk->prs_time + 28800;
        tm_info = localtime(&prs_time);
        strftime(buffer, 26, "%m-%d %H:%M", tm_info);

        sprintf(tmp_str, "{'timestamp':%ld,'prs_time':'%s','operation':%d,'on':%d,'weekday':%d},",
            tmp_tsk->prs_time, buffer, tmp_tsk->operation, tmp_tsk->on, tmp_tsk->weekday);
        tmp_str += strlen(tmp_str);
        tmp_tsk = tmp_tsk->next;
    }
    if (user_config->task_count > 0) --tmp_str;
    *tmp_str = ']';
    return str;
}

/* ==================== 倒计时任务 ==================== */
static CountdownTask countdown_task = {0};

void CountdownTaskInit(void)
{
    // 从user_config恢复倒计时状态
    if (user_config->countdown.enabled && user_config->countdown.remaining_seconds > 0) {
        task_log("Countdown restored: %d seconds remaining, operation=%d",
                 user_config->countdown.remaining_seconds, user_config->countdown.operation);
    } else {
        memset(&countdown_task, 0, sizeof(CountdownTask));
    }
}

void CountdownTaskStart(int total_seconds, int operation)
{
    TaskLock();
    user_config->countdown.enabled = true;
    user_config->countdown.total_seconds = total_seconds;
    user_config->countdown.remaining_seconds = total_seconds;
    user_config->countdown.operation = operation;
    mico_system_context_update(sys_config);
    TaskUnlock();
    
    task_log("Countdown started: %d seconds, operation=%d", total_seconds, operation);
}

void CountdownTaskStop(void)
{
    TaskLock();
    user_config->countdown.enabled = false;
    user_config->countdown.remaining_seconds = 0;
    mico_system_context_update(sys_config);
    TaskUnlock();
    
    task_log("Countdown stopped");
}

void CountdownTaskTick(void)
{
    if (!user_config->countdown.enabled || user_config->countdown.remaining_seconds <= 0) {
        return;
    }
    
    user_config->countdown.remaining_seconds--;
    
    if (user_config->countdown.remaining_seconds <= 0) {
        // 倒计时完成，执行操作
        task_log("Countdown completed, executing operation=%d", user_config->countdown.operation);
        
        int operation = user_config->countdown.operation;
        switch (operation) {
            case SWITCH_ALL_SOCKETS:
                UserRelaySetAll(1);
                for (int i = 0; i < SOCKET_NUM; i++) {
                    UserMqttSendSocketState(i);
                }
                UserMqttSendTotalSocketState();
                break;
            case SWITCH_SOCKET_1:
            case SWITCH_SOCKET_2:
            case SWITCH_SOCKET_3:
            case SWITCH_SOCKET_4:
            case SWITCH_SOCKET_5:
            case SWITCH_SOCKET_6:
                UserRelaySet(operation - 1, 1);
                UserMqttSendSocketState(operation - 1);
                UserMqttSendTotalSocketState();
                break;
            case REBOOT_SYSTEM:
                MicoSystemReboot();
                break;
            default:
                break;
        }
        
        // 停止倒计时
        user_config->countdown.enabled = false;
        user_config->countdown.remaining_seconds = 0;
    }
    
    // 每秒保存一次状态（减少flash写入）
    if (user_config->countdown.remaining_seconds % 10 == 0) {
        mico_system_context_update(sys_config);
    }
}

char* CountdownTaskGetStatus(void)
{
    char* str = (char*)malloc(128);
    if (!str) return NULL;
    
    sprintf(str, "{'enabled':%d,'remaining':%d,'total':%d,'operation':%d}",
            user_config->countdown.enabled ? 1 : 0,
            user_config->countdown.remaining_seconds,
            user_config->countdown.total_seconds,
            user_config->countdown.operation);
    return str;
}

/* ==================== 循环任务 ==================== */
static CycleTask cycle_task = {0};

void CycleTaskInit(void)
{
    // 从user_config恢复循环任务状态
    if (user_config->cycle.enabled && user_config->cycle.on_seconds > 0) {
        task_log("Cycle task restored: on=%d, off=%d, operation=%d",
                 user_config->cycle.on_seconds, user_config->cycle.off_seconds,
                 user_config->cycle.operation);
    } else {
        memset(&cycle_task, 0, sizeof(CycleTask));
    }
}

void CycleTaskStart(int on_seconds, int off_seconds, int operation)
{
    TaskLock();
    user_config->cycle.enabled = true;
    user_config->cycle.is_on_phase = true;
    user_config->cycle.on_seconds = on_seconds;
    user_config->cycle.off_seconds = off_seconds;
    user_config->cycle.remaining_seconds = on_seconds;
    user_config->cycle.operation = operation;
    mico_system_context_update(sys_config);
    TaskUnlock();
    
    // 立即开启插座
    UserRelaySet(operation - 1, 1);
    UserMqttSendSocketState(operation - 1);
    
    task_log("Cycle started: on=%d, off=%d, operation=%d", on_seconds, off_seconds, operation);
}

void CycleTaskStop(void)
{
    TaskLock();
    int operation = user_config->cycle.operation;
    user_config->cycle.enabled = false;
    user_config->cycle.remaining_seconds = 0;
    mico_system_context_update(sys_config);
    TaskUnlock();
    
    // 停止时关闭插座
    if (operation >= SWITCH_SOCKET_1 && operation <= SWITCH_SOCKET_6) {
        UserRelaySet(operation - 1, 0);
        UserMqttSendSocketState(operation - 1);
    }
    
    task_log("Cycle stopped");
}

void CycleTaskTick(void)
{
    if (!user_config->cycle.enabled || user_config->cycle.remaining_seconds <= 0) {
        return;
    }
    
    user_config->cycle.remaining_seconds--;
    
    if (user_config->cycle.remaining_seconds <= 0) {
        int operation = user_config->cycle.operation;
        
        if (user_config->cycle.is_on_phase) {
            // 开启阶段结束，切换到关闭阶段
            user_config->cycle.is_on_phase = false;
            user_config->cycle.remaining_seconds = user_config->cycle.off_seconds;
            
            // 关闭插座
            if (operation >= SWITCH_SOCKET_1 && operation <= SWITCH_SOCKET_6) {
                UserRelaySet(operation - 1, 0);
                UserMqttSendSocketState(operation - 1);
            }
            
            task_log("Cycle: switching to OFF phase for %d seconds", user_config->cycle.off_seconds);
        } else {
            // 关闭阶段结束，切换到开启阶段
            user_config->cycle.is_on_phase = true;
            user_config->cycle.remaining_seconds = user_config->cycle.on_seconds;
            
            // 开启插座
            if (operation >= SWITCH_SOCKET_1 && operation <= SWITCH_SOCKET_6) {
                UserRelaySet(operation - 1, 1);
                UserMqttSendSocketState(operation - 1);
            }
            
            task_log("Cycle: switching to ON phase for %d seconds", user_config->cycle.on_seconds);
        }
    }
    
    // 每10秒保存一次状态
    if (user_config->cycle.remaining_seconds % 10 == 0) {
        mico_system_context_update(sys_config);
    }
}

char* CycleTaskGetStatus(void)
{
    char* str = (char*)malloc(128);
    if (!str) return NULL;
    
    sprintf(str, "{'enabled':%d,'is_on':%d,'remaining':%d,'on_sec':%d,'off_sec':%d,'operation':%d}",
            user_config->cycle.enabled ? 1 : 0,
            user_config->cycle.is_on_phase ? 1 : 0,
            user_config->cycle.remaining_seconds,
            user_config->cycle.on_seconds,
            user_config->cycle.off_seconds,
            user_config->cycle.operation);
    return str;
}