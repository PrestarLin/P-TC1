#ifndef __MAIN_H_
#define __MAIN_H_

#include "mico.h"
#include "micokit_ext.h"
#include "timed_task/timed_task.h"

#define app_log(M, ...) custom_log("APP", M, ##__VA_ARGS__); web_log("APP", M, ##__VA_ARGS__);
#define key_log(M, ...) custom_log("KEY", M, ##__VA_ARGS__); web_log("KEY", M, ##__VA_ARGS__);
#define ota_log(M, ...) custom_log("OTA", M, ##__VA_ARGS__); web_log("OTA", M, ##__VA_ARGS__);
#define rtc_log(M, ...) custom_log("RTC", M, ##__VA_ARGS__); web_log("RTC", M, ##__VA_ARGS__);
#define tc1_log(M, ...) custom_log("TC1", M, ##__VA_ARGS__); web_log("TC1", M, ##__VA_ARGS__);
#define task_log(M, ...) custom_log("TASK", M, ##__VA_ARGS__); web_log("TASK", M, ##__VA_ARGS__);
#define http_log(M, ...) custom_log("HTTP", M, ##__VA_ARGS__); web_log("HTTP", M, ##__VA_ARGS__);
#define mqtt_log(M, ...) custom_log("MQTT", M, ##__VA_ARGS__); web_log("MQTT", M, ##__VA_ARGS__);
#define wifi_log(M, ...) custom_log("WIFI", M, ##__VA_ARGS__); web_log("WIFI", M, ##__VA_ARGS__);
#define power_log(M, ...) custom_log("POWER", M, ##__VA_ARGS__); web_log("POWER", M, ##__VA_ARGS__);

#ifndef COMMIT_HASH
#define COMMIT_HASH local
#endif
#define STR_(x) #x
#define STR(x) STR_(x)
#ifndef VERSION_STRING
#define VERSION_STRING "v4.1.3"
#endif
#define VERSION VERSION_STRING "-" STR(COMMIT_HASH)

#define TYPE 1
#define TYPE_NAME "TC1"

#define ZTC1_NAME "TC1-%s"

#define USER_CONFIG_VERSION 12
#define SETTING_MQTT_STRING_LENGTH_MAX 32 //必须4字节对齐。

/* 用户配置在 flash 上的固定落地大小（字节）。
 * 目的：让 sizeof(user_config_t) 在所有固件版本间保持不变，从而
 * MiCO 底层 MICOReadConfiguration/internal_update_config 的 CRC 长度、
 * 数据与 CRC 的 flash 偏移都恒定。这样将来任何布局调整都不会使旧配置
 * 因"长度变化"而在 MiCO 底层被误判损坏而清空。
 * 约束：该值必须满足 sizeof(system_config_t) + USER_CONFIG_STRUCT_CAP + 2(CRC)
 *       <= PARAMETER1/PARAMETER2 分区大小(当前 16KB)。
 */
#define USER_CONFIG_STRUCT_CAP 4096

#define SOCKET_NAME_LENGTH   64
#define SOCKET_NUM           6  //插座数量

#define Led    MICO_GPIO_5
#define Button MICO_GPIO_23
#define POWER  MICO_GPIO_15

#define Relay_ON  1
#define Relay_OFF 0
#define Relay_TOGGLE -1

#define Relay_0   MICO_GPIO_6
#define Relay_1   MICO_GPIO_8
#define Relay_2   MICO_GPIO_10
#define Relay_3   MICO_GPIO_7
#define Relay_4   MICO_GPIO_9
#define Relay_5   MICO_GPIO_18
#define Relay_NUM SOCKET_NUM

#define MAX_TASK_NUM 64

/* 冻结：TimedTask v10 布局（无 loop_end 字段，与版本 10 发布时一致） */
typedef struct TimedTask_v10
{
    bool on_use;
    time_t prs_time;
    int operation;
    int on;
    int weekday;
    struct TimedTask_v10 *next;
} TimedTask_v10_t;

/* ===================== 配置布局迁移机制 =====================
 * 目的：布局变更不再清空用户配置。
 *
 * 规则：
 *  1. user_config_v10_t 是"版本 10"发布时的落地结构体"冻结快照"，永远不要修改它。
 *  2. user_config_t 是"当前活动布局"，末尾带 reserved 补齐到固定 USER_CONFIG_STRUCT_CAP，
 *     因此 sizeof(user_config_t) 在所有版本间恒定（CRC/偏移稳定，MiCO 底层永不误判损坏）。
 *  3. 每次修改活动布局(user_config_t)时：
 *       a. 先把"当前"布局整体复制冻结为 user_config_v<N>_t（N=当前 USER_CONFIG_VERSION）；
 *       b. 再改动 user_config_t 字段（新字段一律加在 reserved 之前；改动时 UPDATE_VERSION+1）；
 *       c. 在 main.c 的 user_config_migrate() 中新增一段"从 vN 投影到当前"的字段拷贝。
 *   这样旧版本在 flash 上的字节仍在原偏移，新固件读取后经 user_config_migrate 逐字段
 *   投影到新布局并写回，全程不触发 MiCO 层 restore（不清空）。
 *
 * 注意：本项目首次从"变长"切换到"定长驻块"时，MiCO 底层会因长度变化对存量旧记录
 * 判定损坏而清空一次（已接受的一次性重置）；此后布局永不再因长度变化而清空。
 */

/* 冻结：版本 10 发布时的落地布局（请勿修改） */
typedef struct
{
    char version;
    char mqtt_ip[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_names[SOCKET_NUM][SOCKET_NAME_LENGTH];
    int mqtt_port;
    int mqtt_report_freq;
    char mqtt_user[SETTING_MQTT_STRING_LENGTH_MAX];
    char mqtt_password[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_status[SOCKET_NUM]; //记录当前开关
    char user[maxNameLen];
    char child_lock;
    WiFiEvent last_wifi_status;
    char ap_name[32];
    char ap_key[32];
    int task_count;
    int p_count_2_days_ago;
    int p_count_1_day_ago;
    int power_led_enabled;
    TimedTask_v10_t *task_top;
    TimedTask_v10_t timed_tasks[MAX_TASK_NUM];
} user_config_v10_t;

/* 冻结：版本 11 布局（v10 + static IP 字段） */
typedef struct
{
    char version;
    char mqtt_ip[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_names[SOCKET_NUM][SOCKET_NAME_LENGTH];
    int mqtt_port;
    int mqtt_report_freq;
    char mqtt_user[SETTING_MQTT_STRING_LENGTH_MAX];
    char mqtt_password[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_status[SOCKET_NUM];
    char user[maxNameLen];
    char child_lock;
    WiFiEvent last_wifi_status;
    char ap_name[32];
    char ap_key[32];
    int task_count;
    int p_count_2_days_ago;
    int p_count_1_day_ago;
    int power_led_enabled;
    TimedTask_v10_t *task_top;
    TimedTask_v10_t timed_tasks[MAX_TASK_NUM];
    char ip_mode;
    char static_ip[16];
    char static_mask[16];
    char static_gateway[16];
    char static_dns[16];
} user_config_v11_t;

/* 当前活动布局：v12 = v11 + night mode */
typedef struct
{
    char version;
    char mqtt_ip[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_names[SOCKET_NUM][SOCKET_NAME_LENGTH];
    int mqtt_port;
    int mqtt_report_freq;
    char mqtt_user[SETTING_MQTT_STRING_LENGTH_MAX];
    char mqtt_password[SETTING_MQTT_STRING_LENGTH_MAX];
    char socket_status[SOCKET_NUM]; //记录当前开关
    char user[maxNameLen];
    char child_lock;
    WiFiEvent last_wifi_status;
    char ap_name[32];
    char ap_key[32];
    int task_count;
    int p_count_2_days_ago;
    int p_count_1_day_ago;
    int power_led_enabled;
    pTimedTask task_top;
    struct TimedTask timed_tasks[MAX_TASK_NUM];
    char ip_mode;           // 0=DHCP, 1=static
    char static_ip[16];
    char static_mask[16];
    char static_gateway[16];
    char static_dns[16];
    char night_mode_enabled;    // 0=off, 1=on
    int  night_mode_start;      // minutes since midnight (0-1439)
    int  night_mode_end;        // minutes since midnight (0-1439)
    char reserved[USER_CONFIG_STRUCT_CAP - sizeof(user_config_v10_t) - 77 - (sizeof(struct TimedTask) - sizeof(TimedTask_v10_t)) * MAX_TASK_NUM]; // 定长补齐，必须保持为最后一个字段
} user_config_t;

#if defined(__GNUC__)
_Static_assert(sizeof(user_config_t) == USER_CONFIG_STRUCT_CAP,
               "user_config_t must stay exactly USER_CONFIG_STRUCT_CAP bytes (fixed on-disk size)");
#endif

/* reserved 前部字节按需复用(布局不变，随配置持久化)。
 * 1) WiFi 断开延迟动作: delay>0 时断开 delay 秒未恢复执行 action；0=断开立即开启AP。
 * 2) 按键功能码(全字节): 取代旧 4 位 nibble 编码，支持 开/关/切换；
 *    旧固件升级时由 key_init!=MAGIC 触发从 user[] nibble 解包一次。 */
#define KEY_CFG_MAGIC 0x5A
typedef struct {
    int  wifi_offline_delay;   // offset 0
    char wifi_offline_action;  // offset 4, 0=开启AP, 1=重启
    char _pad[3];              // offset 5
    char key_short[32];        // offset 8, 点按x次功能码
    char key_long[32];         // offset 40, 长按x秒功能码
    char key_init;             // offset 72
    char _pad2[3];
} reserved_cfg_t;
#define RESERVED_CFG ((reserved_cfg_t *)user_config->reserved)

extern char rtc_init;
extern uint32_t total_time;
extern char str_mac[16];
extern system_config_t* sys_config;
extern user_config_t* user_config;
extern mico_gpio_t Relay[Relay_NUM];
extern int childLockEnabled;

void RemoveNightModeTasks(void);
void CreateNightModeTask(int hour, int minute, int on);


#endif
