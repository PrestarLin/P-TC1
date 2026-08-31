#include "main.h"
#include "stdio.h"
#include "stdlib.h"
#include "time.h"
#include "unistd.h"
#include "TimeUtils.h"
#include "mico_system.h"

#include "user_gpio.h"
#include "user_wifi.h"
#include "time_server/user_rtc.h"
#include "user_power.h"
#include "http_server/app_httpd.h"
#include "timed_task/timed_task.h"
#include "mqtt_server/user_mqtt_client.h"

char rtc_init = 0; //sntp校时成功标志位
uint32_t total_time = 0;
char str_mac[16] = {0};
int last_check_day = 0;
int childLockEnabled = 0;

system_config_t *sys_config;
user_config_t *user_config;

mico_gpio_t Relay[Relay_NUM] = {Relay_0, Relay_1, Relay_2, Relay_3, Relay_4, Relay_5};

/* MICO system callback: Restore default configuration provided by application */
void appRestoreDefault_callback(void *const user_config_data, uint32_t size) {
    UNUSED_PARAMETER(size);

    mico_system_context_get()->micoSystemConfig.name[0] = 1; //在下次重启时使用默认名称
    mico_system_context_get()->micoSystemConfig.name[1] = 0;

    user_config_t *userConfigDefault = user_config_data;
    memset(userConfigDefault, 0, sizeof(user_config_t)); // 确保 reserved 尾部字节确定，CRC 可复现
    userConfigDefault->user[0] = 0;
    userConfigDefault->child_lock = 0;
    userConfigDefault->mqtt_ip[0] = 0;
    userConfigDefault->mqtt_port = 0;
    userConfigDefault->mqtt_user[0] = 0;
    userConfigDefault->mqtt_password[0] = 0;
    userConfigDefault->task_top = NULL;
    userConfigDefault->task_count = 0;
    userConfigDefault->mqtt_report_freq = 2;
    userConfigDefault->p_count_2_days_ago = 0;
    userConfigDefault->p_count_1_day_ago = 0;
    userConfigDefault->power_led_enabled = 1;
    userConfigDefault->version = USER_CONFIG_VERSION;
    set_key_map(userConfigDefault->user,1, SWITCH_ALL_SOCKETS, NO_FUNCTION);
    for (int i = 2; i < 32; i++) {
        int longFunc = NO_FUNCTION;
        //出厂设置，长按5秒开启配网模式，长按10秒恢复出厂设置
        if (i >=5 && i< 10) {
            longFunc = CONFIG_WIFI;
        } else if (i >= 10 && i< 15) {
            longFunc = RESET_SYSTEM;
        }
        set_key_map(userConfigDefault->user,i, NO_FUNCTION, longFunc);
    }

    for (int i = 0; i < SOCKET_NUM; i++) {
        userConfigDefault->socket_status[i] = 1;
        snprintf(userConfigDefault->socket_names[i], SOCKET_NAME_LENGTH, "插座-%d", i + 1);
    }
    for (int i = 0; i < MAX_TASK_NUM; i++) {
        userConfigDefault->timed_tasks[i].on_use = false;
    }
    RESERVED_CFG->key_init = KEY_CFG_MAGIC;  // 出厂默认已直接写入全字节按键配置
    mico_system_context_update(sys_config);
}

void recordDailyPCount() {
    // 获取当前时间
    mico_utc_time_t utc_time;
    mico_time_get_utc_time(&utc_time);
    utc_time += 28800;
    struct tm *current_time = localtime((const time_t *) &utc_time);
    // 判断上次检查的时间与当前时间的日期是否不同
    if (last_check_day != 0) {
        // 如果日期发生变化（即跨天了），则进行记录
        if (current_time->tm_mday != last_check_day) { tc1_log(
                    "WARNGIN: pcount day changed! now day %d hour %d min %d ,lastCheck day %d",
                    current_time->tm_mday, current_time->tm_hour, current_time->tm_min,
                    last_check_day);

//            tc1_log("WARNGIN: pcount day changed! ");
            // 记录数据
            if (user_config->p_count_1_day_ago != 0) {
                user_config->p_count_2_days_ago = user_config->p_count_1_day_ago;
            }
            user_config->p_count_1_day_ago = p_count;

            // 更新系统配置
            mico_system_context_update(sys_config);

            tc1_log("WARNGIN: p_count record! p_count_1_day_ago:%d p_count_2_days_ago:%d",
                    user_config->p_count_1_day_ago, user_config->p_count_2_days_ago);
        } else {
//        	tc1_log("WARNGIN: pcount day not changed , waiting for next run! ");
        }
    } else { tc1_log("WARNGIN: now day %d hour %d min %d ,lastCheck day %d", current_time->tm_mday,
                     current_time->tm_hour, current_time->tm_min, last_check_day);
    }
    // 更新上次检查时间
    last_check_day = current_time->tm_mday;
}

void schedule_p_count_task(mico_thread_arg_t arg) {
    mico_thread_sleep(20);tc1_log("WARNGIN: p_count timer thread created!");
    while (1) {
        recordDailyPCount();
        mico_thread_sleep(60);
    }
}

void reportMqttPowerInfoThread() {
    while (1) {
        UserMqttHassPower();
        int freq = user_config->mqtt_report_freq;

        if (freq == 0) {
            freq = 2;
        }

        mico_thread_msleep(1000 * freq);
    }
}

/* 配置布局迁移：将 flash 中旧版本布局逐字段投影到当前活动布局。
 * 返回 true 表示已迁移（或本就是最新）；返回 false 表示版本未知/不支持，
 * 由调用方回退到恢复出厂默认。仅在布局版本号变化时才会真正拷贝。
 */
bool user_config_migrate(void) {
    char old_version = user_config->version; // 首字节恒为版本号（定长驻块 offset 0 稳定）
    if (old_version == USER_CONFIG_VERSION) {
        return true; // 已是当前布局
    }

    tc1_log("WARNGIN: migrate user config from v%d to v%d", old_version, USER_CONFIG_VERSION);

    if (old_version >= 1 && old_version <= 10) {
        /* 从冻结的 v10 布局投影到当前布局. */
        user_config_v10_t *v10 = (user_config_v10_t *) user_config;
        snprintf(user_config->mqtt_ip, SETTING_MQTT_STRING_LENGTH_MAX, "%s", v10->mqtt_ip);
        user_config->mqtt_port = v10->mqtt_port;
        user_config->mqtt_report_freq = v10->mqtt_report_freq;
        snprintf(user_config->mqtt_user, SETTING_MQTT_STRING_LENGTH_MAX, "%s", v10->mqtt_user);
        snprintf(user_config->mqtt_password, SETTING_MQTT_STRING_LENGTH_MAX, "%s", v10->mqtt_password);
        memcpy(user_config->socket_status, v10->socket_status, sizeof(user_config->socket_status));
        memcpy(user_config->socket_names, v10->socket_names, sizeof(user_config->socket_names));
        memcpy(user_config->user, v10->user, maxNameLen);
        user_config->child_lock = v10->child_lock;
        user_config->last_wifi_status = v10->last_wifi_status;
        memcpy(user_config->ap_name, v10->ap_name, sizeof(user_config->ap_name));
        memcpy(user_config->ap_key, v10->ap_key, sizeof(user_config->ap_key));
        user_config->task_count = v10->task_count;
        user_config->p_count_2_days_ago = v10->p_count_2_days_ago;
        user_config->p_count_1_day_ago = v10->p_count_1_day_ago;
        user_config->power_led_enabled = v10->power_led_enabled;
        /* 逐任务复制：v10 的 TimedTask_v10_t 与当前 TimedTask 大小不同 */
        for (int i = 0; i < v10->task_count && i < MAX_TASK_NUM; i++) {
            user_config->timed_tasks[i].on_use = v10->timed_tasks[i].on_use;
            user_config->timed_tasks[i].prs_time = v10->timed_tasks[i].prs_time;
            user_config->timed_tasks[i].operation = v10->timed_tasks[i].operation;
            user_config->timed_tasks[i].on = v10->timed_tasks[i].on;
            user_config->timed_tasks[i].weekday = v10->timed_tasks[i].weekday;
            user_config->timed_tasks[i].loop_end = 0;
            user_config->timed_tasks[i].next = NULL;
        }
        user_config->task_top = NULL;
        user_config->ip_mode = 0;
        memset(user_config->static_ip, 0, sizeof(user_config->static_ip));
        memset(user_config->static_mask, 0, sizeof(user_config->static_mask));
        memset(user_config->static_gateway, 0, sizeof(user_config->static_gateway));
        memset(user_config->static_dns, 0, sizeof(user_config->static_dns));
        user_config->version = USER_CONFIG_VERSION;

        /* 迁移后写回 flash（含固定 CRC），此后布局即为最新。 */
        mico_system_context_update(sys_config);
        return true;
    }

    if (old_version == 11) {
        user_config_v11_t *v11 = (user_config_v11_t *) user_config;
        /* 逐任务复制：v11 的 TimedTask_v10_t 与当前 TimedTask 大小不同 */
        for (int i = 0; i < v11->task_count && i < MAX_TASK_NUM; i++) {
            user_config->timed_tasks[i].on_use = v11->timed_tasks[i].on_use;
            user_config->timed_tasks[i].prs_time = v11->timed_tasks[i].prs_time;
            user_config->timed_tasks[i].operation = v11->timed_tasks[i].operation;
            user_config->timed_tasks[i].on = v11->timed_tasks[i].on;
            user_config->timed_tasks[i].weekday = v11->timed_tasks[i].weekday;
            user_config->timed_tasks[i].loop_end = 0;
            user_config->timed_tasks[i].next = NULL;
        }
        user_config->task_top = NULL;
        user_config->night_mode_enabled = 0;
        user_config->night_mode_start = 23 * 60;  /* 23:00 */
        user_config->night_mode_end = 7 * 60;     /* 07:00 */
        user_config->version = USER_CONFIG_VERSION;
        mico_system_context_update(sys_config);
        return true;
    }

    tc1_log("WARNGIN: unsupported config version %d, restore to default!", old_version);
    return false;
}

static int GetMinutesSinceMidnight(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) return -1;
    return t->tm_hour * 60 + t->tm_min;
}

void RemoveNightModeTasks(void) {
    TaskLock();
    pTimedTask tsk = user_config->task_top;
    pTimedTask prev = NULL;
    while (tsk) {
        pTimedTask next = tsk->next;
        if (tsk->operation == SWITCH_LED_ENABLE && tsk->weekday == 8) {
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
    TaskUnlock();
    mico_system_context_update(sys_config);
}

void CreateNightModeTask(int hour, int minute, int on) {
    pTimedTask task = NewTask();
    if (!task) { tc1_log("night mode: no free task slot"); return; }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) { task->on_use = false; return; }

    time_t target = now - (t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec)
                    + (hour * 3600 + minute * 60);
    if (target <= now) target += 86400;

    task->prs_time = target;
    task->operation = SWITCH_LED_ENABLE;
    task->on = on;
    task->weekday = 8;
    task->loop_end = 0;

    TaskLock();
    AddTask(task);
    TaskUnlock();
    mico_system_context_update(sys_config);
}

int application_start(void) {
    int i;
    OSStatus err = kNoErr;

    // Create mico system context and read application's config data from flash
    sys_config = mico_system_context_init(sizeof(user_config_t));
    user_config = ((system_context_t *) sys_config)->user_config_data;
    require_action(user_config, exit, err = kNoMemoryErr);

    err = mico_system_init(sys_config);
    require_noerr(err, exit);

    TaskModuleInit();
    LogMutexInit();
    tc1_log("start version[%s]", VERSION);

    uint8_t mac[8];
    mico_wlan_get_mac_address(mac);
    sprintf(str_mac, "%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);tc1_log("str_mac[%s]", str_mac);

    bool open_ap = false;
    MicoGpioInitialize((mico_gpio_t) Button, INPUT_PULL_UP);
    if (!MicoGpioInputGet(Button)) {   //开机时按钮状态
        tc1_log("press ap_init");
        ApInit(true);
        open_ap = true;
    }

    MicoGpioInitialize((mico_gpio_t) Led, OUTPUT_PUSH_PULL);
    for (i = 0; i < Relay_NUM; i++) {
        MicoGpioInitialize(Relay[i], OUTPUT_PUSH_PULL);
        UserRelaySet(i, user_config->socket_status[i]);
    }
    MicoSysLed(0);

    if (!user_config_migrate()) { tc1_log("WARNGIN: user params restored!");
        err = mico_system_context_restore(sys_config);
        require_noerr(err, exit);
    }

    childLockEnabled = user_config->child_lock;

    /* 旧固件升级：把 user[] nibble 编码解包到全字节按键功能码(仅首次) */
    ButtonConfigInit();
    if (RESERVED_CFG->key_init != KEY_CFG_MAGIC) {
        mico_system_context_update(sys_config);
    }

    /* 5s/10s 长按为受保护出厂任务(配网/恢复出厂)，强制回写为出厂值，
     * 修复旧固件(可任意改5s/10s)残留的非法配置导致默认任务显示与按键行为错误 */
    bool heal_key = false;
    if (get_long_func(RESERVED_CFG->key_long[5]) != CONFIG_WIFI) {
        set_key_map(user_config->user, 5, RESERVED_CFG->key_short[5], CONFIG_WIFI);
        heal_key = true;
    }
    if (get_long_func(RESERVED_CFG->key_long[10]) != RESET_SYSTEM) {
        set_key_map(user_config->user, 10, RESERVED_CFG->key_short[10], RESET_SYSTEM);
        heal_key = true;
    }
    if (heal_key) {
        tc1_log("WARNGIN: heal protected default tasks (5s/10s)");
        mico_system_context_update(sys_config);
    }

    RebuildTaskList();

    if (user_config->night_mode_start == 0 && user_config->night_mode_end == 0
        && !user_config->night_mode_enabled) {
        user_config->night_mode_enabled = 1;
        user_config->night_mode_start = 23 * 60;
        user_config->night_mode_end = 7 * 60;
        mico_system_context_update(sys_config);
        CreateNightModeTask(23, 0, 0);
        CreateNightModeTask(7, 0, 1);
    }

    if (sys_config->micoSystemConfig.name[0] == 1) {
        sprintf(sys_config->micoSystemConfig.name, ZTC1_NAME, str_mac + 8);
    }

    for (i = 0; i < SOCKET_NUM; i++) {
        if (user_config->socket_names[i][0] == '\0') {
            snprintf(user_config->socket_names[i], SOCKET_NAME_LENGTH, "Socket %d", i + 1);
        }
    }

    tc1_log("device name:%s",
            sys_config->micoSystemConfig.name);tc1_log(
            "mqtt_ip:%s", user_config->mqtt_ip);tc1_log("mqtt_port:%d",
                                                        user_config->mqtt_port);tc1_log(
            "mqtt_user:%s", user_config->mqtt_user);
    //tc1_log("mqtt_password:%s",user_config->mqtt_password);
    tc1_log("version:%d", user_config->version);

    WifiInit();
    if (!open_ap) {
        if (sys_config->micoSystemConfig.reserved != NOTIFY_STATION_UP) {
            ApInit(true);
        } else {
            WifiConnect(sys_config->micoSystemConfig.ssid,
                        sys_config->micoSystemConfig.user_key);
        }
    }
    KeyInit();
    err = UserRtcInit();
    require_noerr(err, exit);
    PowerInit();
    AppHttpdStart(); // start http server thread

    UserLedSet(user_config->power_led_enabled);

    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "p_count",
                                  (mico_thread_function_t) schedule_p_count_task,
                                  0x800, 0);
    require_noerr_string(err, exit, "ERROR: Unable to start the p_count thread.");

    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "mqtt_power_report",
                                  (mico_thread_function_t) reportMqttPowerInfoThread,
                                  0x800, 0);
    require_noerr_string(err, exit, "ERROR: Unable to start the mqtt_power_report thread.");


    while (1) {
        time_t now = time(NULL);
        TaskLock();
        if (user_config->task_top && now >= user_config->task_top->prs_time) {
            ProcessTask();
        }
        TaskUnlock();
        mico_thread_msleep(1000);
    }

    exit:tc1_log("application_start ERROR!");
    return 0;
}

