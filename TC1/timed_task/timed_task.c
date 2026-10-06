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

            /* 旧固件夜灯任务用 8 当"每日"哨兵(与周三掩码冲突), 迁移为专用标记。
             * 只迁夜灯: 其时刻必等于当前 night_mode_start/end(旧固件改配置必删旧建新,
             * 升级残留的任务即最后一次设置); 用户手建的周三 LED(8) 时刻不匹配, 保留周三语义 */
            if (task->weekday == 8 && task->operation == SWITCH_LED_ENABLE) {
                int m = (int)((task->prs_time + 28800) % day_sec) / 60;
                if (m == user_config->night_mode_start || m == user_config->night_mode_end)
                    task->weekday = NIGHT_DAILY_WEEKDAY;
            }

            /* 旧循环任务编码迁移: 旧标志 bit7 压在 duration 字段内(致 +128), 新编码标志在 bit31。
             * 旧存储 dur 字段 = 原时长|0x80: 原值<128 可精确还原; >=128 无法区分, 还原偏小需重建。
             * 383 是夜灯"每日"哨兵(bit8 标记), 不参与迁移 */
            int ow = task->weekday;
            if (ow != NIGHT_DAILY_WEEKDAY && IS_LOOP_TASK_OLD(ow) && !IS_LOOP_TASK(ow)) {
                int dur = (ow & LOOP_MASK_MINUTES) - 0x80;
                task->weekday = MAKE_LOOP_WEEKDAY(dur < 1 ? 1 : dur, GET_LOOP_INTERVAL(ow))
                              | (GET_LOOP_START(ow) << LOOP_START_SHIFT);
            }

            if (task->weekday != 0 && task->prs_time <= now)
            {
                AddTask(task);
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

/* 计算 day_mask(Sun=bit0..Sat=bit6) 中下一个匹配时刻(严格晚于 from, 当日时刻为 hhmm 秒)。
 * include_today=true 时, 今天若匹配且时刻未过则可用(用于新建任务)。
 * hhmm 为北京时间当日秒数; 设备本地时区为 UTC, 统一 +8h 换算到北京域计算后再转回,
 * 否则北京 00:00-07:59 的周任务会错位到前一天(星期与时刻跨域不一致)。 */
static time_t FindNextMatchTime(int day_mask, time_t from, int hhmm, bool include_today)
{
    time_t bj = from + 28800;                     /* 北京域当前时刻 */
    time_t base = bj - bj % day_sec;
    int today = ((int)(bj / day_sec) + 4) % 7 + 1;   // 1=Sun..7=Sat, 1970-01-01=周四
    for (int d = 0; d < 14; d++)
    {
        int wd = ((today - 1 + d) % 7) + 1;
        if (day_mask & (1 << (wd - 1)))
        {
            time_t cand = base + d * day_sec + hhmm; /* 北京域候选 */
            if (cand > bj && (include_today || d > 0))
                return cand - 28800;              /* 转回 epoch */
        }
    }
    return from + 7 * day_sec;
}

bool AddTaskWeek(pTimedTask task)
{
    time_t now = time(NULL);
    int hhmm = (int)((task->prs_time + 28800) % day_sec);   // 保留用户设置的时分(北京)
    task->prs_time = FindNextMatchTime(task->weekday, now, hhmm, true);
    return AddTaskSingle(task);
}

bool AddTask(pTimedTask task)
{
    if (IS_LOOP_TASK(task->weekday) || task->weekday == 0)
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
        else
        {
            tmp->prs_time = FindNextMatchTime(tmp->weekday, time(NULL),
                (int)((tmp->prs_time + 28800) % day_sec), false);
            AddTask(tmp);
        }
        AppContextUpdate(sys_config);
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
    AppContextUpdate(sys_config);
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
    AppContextUpdate(sys_config);
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
    AppContextUpdate(sys_config);
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
        AppContextUpdate(sys_config);
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
            AppContextUpdate(sys_config);
            return true;
        }
        pre_tsk = tmp_tsk;
        tmp_tsk = tmp_tsk->next;
    }
    return false;
}

void ProcessTask()
{
    char fname[FUNC_NAME_BUF_SIZE];
    task_log("process task time[%ld] operation[%s] on[%d]",
        user_config->task_top->prs_time, get_func_name(user_config->task_top->operation, fname, sizeof(fname)), user_config->task_top->on);

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
        MQTT_LED_ENABLED = on_val;
        if (RelayOut() && on_val) { UserLedSet(1); } else { UserLedSet(0); }
        UserMqttSendLedState();
    } else if (op == SWITCH_CHILD_LOCK_ENABLE) {
        user_config->child_lock = on_val;
        childLockEnabled = on_val;
        UserMqttSendChildLockState();
    } else if (op == REBOOT_SYSTEM) {
        DelFirstTask();
        AppContextUpdate(sys_config);
        MicoSystemReboot();
        return;
    } else if (op == CONFIG_WIFI) {
        DelFirstTask();
        AppContextUpdate(sys_config);
        micoWlanSuspendStation();
        ApInit(true);
        return;
    } else if (op == RESET_SYSTEM) {
        DelFirstTask();
        AppContextUpdate(sys_config);
        mico_system_context_restore(sys_config);
        mico_rtos_thread_sleep(1);
        MicoSystemReboot();
        return;
    }
    AppContextUpdate(sys_config);

    /* 循环任务：执行后检查是否在时间段内，是则重新调度 */
    if (IS_LOOP_TASK(user_config->task_top->weekday)) {
        int duration = GET_LOOP_DURATION(user_config->task_top->weekday);
        int interval = GET_LOOP_INTERVAL(user_config->task_top->weekday);
        int raw_end = user_config->task_top->loop_end;
        int loop_end = raw_end & 0xFFFF;
        int loop_daily = (raw_end >> 16) & 1;
        int loop_start_on = (raw_end >> 17) & 1;
        int saved_op = user_config->task_top->operation;
        int saved_on = user_config->task_top->on;
        int saved_wd = user_config->task_top->weekday;
        int saved_loop_end = user_config->task_top->loop_end;

        /* 检查当前时间是否在时间段内 (loop_end=0 表示不限制)。
         * 全部按北京时间比较: 设备 localtime=UTC, 直接用会与 loop_end(北京) 差 8 小时。
         * 起点来自 weekday 高位编码(prs_time 每轮会被改成下次触发时间, 不能当起点用);
         * 旧任务未编码(=0)回退原逻辑 */
        if (loop_end > 0) {
            time_t now = time(NULL);
            int now_min = (int)((now + 28800) % day_sec) / 60; /* 北京分钟 */
            int start_enc = GET_LOOP_START(saved_wd);
            int start_min = (start_enc > 0) ? (start_enc - 1)
                                            : (int)(user_config->task_top->prs_time) % 1440;
            {
                bool in_range;
                if (start_min <= loop_end) {
                    in_range = (now_min >= start_min && now_min < loop_end);
                } else {
                    in_range = (now_min >= start_min || now_min < loop_end);
                }
                if (!in_range) {
                    task_log("loop out of range, stop");
                    /* 超窗后按实际状态兜底: 仍为通则补一次"关"(覆盖开/关/切换三种动作), 避免插座停在开启态 */
                    if (op >= SWITCH_SOCKET_1 && op <= SWITCH_SOCKET_6 && user_config->socket_status[op - 1] != Relay_OFF) {
                        UserRelaySet(op - 1, 0);
                        UserMqttSendSocketState(op - 1);
                        UserMqttSendTotalSocketState();
                    } else if (op == SWITCH_ALL_SOCKETS && RelayOut()) {
                        UserRelaySetAll(0);
                        for (int i = 0; i < SOCKET_NUM; i++) UserMqttSendSocketState(i);
                        UserMqttSendTotalSocketState();
                    }
                    if (loop_daily) {
                        /* 每天重复: 不删除, 重挂到下一个窗口起点, 动作恢复为初始方向 */
                        int bj_now = (int)((now + 28800) % day_sec);
                        time_t next = now - bj_now + (time_t)start_min * 60;
                        if (next <= now) next += day_sec;
                        task_log("loop daily re-arm: next=%ld", next);
                        DelFirstTask();
                        pTimedTask newTask = NewTask();
                        if (newTask) {
                            newTask->prs_time = next;
                            newTask->operation = saved_op;
                            newTask->on = (saved_on == -1) ? -1 : (loop_start_on ? 1 : 0);
                            newTask->weekday = saved_wd;
                            newTask->loop_end = saved_loop_end;
                            AddTask(newTask);
                        }
                        AppContextUpdate(sys_config);
                        return;
                    }
                    DelFirstTask();
                    AppContextUpdate(sys_config);
                    return;
                }
            }
        }

        /* 开=保持 duration 后关, 关=保持 interval 后再开; 切换(-1)无开关锚点, 按单周期: 每隔 duration 分钟翻转, interval 忽略(前端已隐藏该输入) */
        int delay_min = (saved_on == 0) ? interval : duration;
        int delay_sec = (delay_min > 0 ? delay_min : 1) * 60;
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
        AppContextUpdate(sys_config);
        return;
    }

    DelFirstTask();
}

char* GetTaskStr()
{
    /* 每条目 256 字节: 条目录入格式最坏约 210(时间戳 10 + 负 weekday(循环编码 bit31) 11 +
     * 各数字字段)，f23fbe5 起 JSON 增加 loop_repeat 字段后循环任务实测 197~204 字节，
     * 原 192/条 会被 sprintf 逐条写穿堆块(web 失联的根因)。+3 = '[' + 条目尾 NUL 余量
     * + 收尾 ']' 与 '\0'; 只留 +2 时空列表分支会写到 tmp_str[2], 越界 1 字节 */
    char* str = (char*)malloc(sizeof(char)*(user_config->task_count*256+3));
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
        struct tm tm_buf;
        time_t prs_time = tmp_tsk->prs_time;
        tm_info = localtime_r(&prs_time, &tm_buf);
        if (tm_info) {
            strftime(buffer, 26, "%m-%d %H:%M", tm_info);
        } else {
            strcpy(buffer, "??:??");
        }

        int is_loop = IS_LOOP_TASK(tmp_tsk->weekday);
        int loop_dur = GET_LOOP_DURATION(tmp_tsk->weekday);
        int loop_int = GET_LOOP_INTERVAL(tmp_tsk->weekday);
        int loop_start = GET_LOOP_START(tmp_tsk->weekday) ? GET_LOOP_START(tmp_tsk->weekday) - 1 : -1;

        sprintf(tmp_str,
            "{'timestamp':%ld,'prs_time':'%s','operation':%d,'on':%d,'weekday':%d,"
            "'is_loop':%d,'loop_duration':%d,'loop_interval':%d,'loop_start':%d,'loop_end':%d,'loop_repeat':%d},",
            tmp_tsk->prs_time, buffer, tmp_tsk->operation, tmp_tsk->on, tmp_tsk->weekday,
            is_loop, loop_dur, loop_int, loop_start, tmp_tsk->loop_end & 0xFFFF, (tmp_tsk->loop_end >> 16) & 1);
        tmp_str += strlen(tmp_str);
        tmp_tsk = tmp_tsk->next;
    }
    if (user_config->task_count > 0) --tmp_str;
    *tmp_str = ']';
    return str;
}