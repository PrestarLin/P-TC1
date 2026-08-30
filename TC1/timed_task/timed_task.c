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
    if (IS_LOOP_TASK(task->weekday) || task->weekday == 0 || task->weekday == 8)
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
        if (IS_LOOP_TASK(tmp->weekday) || tmp->weekday == 0)
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

void ClearAllTasks()
{
    pTimedTask tsk = user_config->task_top;
    while (tsk) {
        pTimedTask next = tsk->next;
        tsk->on_use = false;
        tsk = next;
    }
    user_config->task_top = NULL;
    user_config->task_count = 0;
    mico_system_context_update(sys_config);
}

void ClearLoopTasks()
{
    pTimedTask tsk = user_config->task_top;
    pTimedTask prev = NULL;
    while (tsk) {
        pTimedTask next = tsk->next;
        if (IS_LOOP_TASK(tsk->weekday)) {
            if (prev) {
                prev->next = next;
            } else {
                user_config->task_top = next;
            }
            tsk->on_use = false;
            user_config->task_count--;
        } else {
            prev = tsk;
        }
        tsk = next;
    }
    mico_system_context_update(sys_config);
}

void ClearScheduledTasks()
{
    pTimedTask tsk = user_config->task_top;
    pTimedTask prev = NULL;
    while (tsk) {
        pTimedTask next = tsk->next;
        if (!IS_LOOP_TASK(tsk->weekday)) {
            if (prev) {
                prev->next = next;
            } else {
                user_config->task_top = next;
            }
            tsk->on_use = false;
            user_config->task_count--;
        } else {
            prev = tsk;
        }
        tsk = next;
    }
    mico_system_context_update(sys_config);
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

    int op = user_config->task_top->operation;
    int on_val = user_config->task_top->on;

    if (op >= SWITCH_SOCKET_1 && op <= SWITCH_SOCKET_6) {
        UserRelaySet(op - 1, on_val);
        UserMqttSendSocketState(op - 1);
        UserMqttSendTotalSocketState();
    } else if (op == SWITCH_ALL_SOCKETS) {
        UserRelaySetAll(on_val);
        for (int i = 0; i < SOCKET_NUM; i++) {
            UserMqttSendSocketState(i);
        }
        UserMqttSendTotalSocketState();
    } else if (op == SWITCH_LED_ENABLE) {
        if (RelayOut() && on_val) { UserLedSet(1); } else { UserLedSet(0); }
        UserMqttSendLedState();
    } else if (op == SWITCH_CHILD_LOCK_ENABLE) {
        user_config->child_lock = on_val;
        childLockEnabled = on_val;
        UserMqttSendChildLockState();
    } else if (op == REBOOT_SYSTEM) {
        DelFirstTask();
        mico_system_context_update(sys_config);
        MicoSystemReboot();
        return;
    } else if (op == CONFIG_WIFI) {
        DelFirstTask();
        mico_system_context_update(sys_config);
        micoWlanSuspendStation();
        ApInit(true);
        return;
    } else if (op == RESET_SYSTEM) {
        DelFirstTask();
        mico_system_context_update(sys_config);
        mico_system_context_restore(sys_config);
        mico_rtos_thread_sleep(1);
        MicoSystemReboot();
        return;
    }
    mico_system_context_update(sys_config);

    /* 循环任务：执行后检查是否在时间段内，是则重新调度 */
    if (IS_LOOP_TASK(user_config->task_top->weekday)) {
        int duration = GET_LOOP_DURATION(user_config->task_top->weekday);
        int interval = GET_LOOP_INTERVAL(user_config->task_top->weekday);
        int loop_end = user_config->task_top->loop_end;
        int saved_op = user_config->task_top->operation;
        int saved_on = user_config->task_top->on;
        int saved_wd = user_config->task_top->weekday;
        int saved_loop_end = user_config->task_top->loop_end;

        /* 检查当前时间是否在时间段内 (loop_end=0 表示不限制) */
        if (loop_end > 0) {
            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            if (t) {
                int now_min = t->tm_hour * 60 + t->tm_min;
                int start_min = (int)(user_config->task_top->prs_time) % 1440;
                bool in_range;
                if (start_min <= loop_end) {
                    in_range = (now_min >= start_min && now_min < loop_end);
                } else {
                    in_range = (now_min >= start_min || now_min < loop_end);
                }
                if (!in_range) {
                    task_log("loop out of range, stop");
                    DelFirstTask();
                    mico_system_context_update(sys_config);
                    return;
                }
            }
        }

        int delay_sec = (duration > 0 ? duration : 1) * 60;
        if (delay_sec < 60) delay_sec = 60;
        time_t next = time(NULL) + delay_sec;
        if (saved_on >= 0) {
            saved_on = (saved_on == 0) ? 1 : 0;
        }
        task_log("loop reschedule: next=%ld on=%d delay=%d", next, saved_on, delay_sec);
        DelFirstTask();
        pTimedTask newTask = NewTask();
        if (newTask) {
            newTask->prs_time = next;
            newTask->operation = saved_op;
            newTask->on = saved_on;
            newTask->weekday = saved_wd;
            newTask->loop_end = saved_loop_end;
            AddTask(newTask);
        }
        mico_system_context_update(sys_config);
        return;
    }

    DelFirstTask();
}

char* GetTaskStr()
{
    char* str = (char*)malloc(sizeof(char)*(user_config->task_count*192+2));
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
        if (tm_info) {
            strftime(buffer, 26, "%m-%d %H:%M", tm_info);
        } else {
            strcpy(buffer, "??:??");
        }

        int is_loop = IS_LOOP_TASK(tmp_tsk->weekday);
        int loop_dur = GET_LOOP_DURATION(tmp_tsk->weekday);
        int loop_int = GET_LOOP_INTERVAL(tmp_tsk->weekday);

        sprintf(tmp_str,
            "{'timestamp':%ld,'prs_time':'%s','operation':%d,'on':%d,'weekday':%d,"
            "'is_loop':%d,'loop_duration':%d,'loop_interval':%d,'loop_end':%d},",
            tmp_tsk->prs_time, buffer, tmp_tsk->operation, tmp_tsk->on, tmp_tsk->weekday,
            is_loop, loop_dur, loop_int, tmp_tsk->loop_end);
        tmp_str += strlen(tmp_str);
        tmp_tsk = tmp_tsk->next;
    }
    if (user_config->task_count > 0) --tmp_str;
    *tmp_str = ']';
    return str;
}