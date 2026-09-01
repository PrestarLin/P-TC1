#include "user_wifi.h"

#include "main.h"
#include "mico_socket.h"
#include "user_gpio.h"
#include "http_server/web_log.h"
#include "mqtt_server/user_mqtt_client.h"
#include <time.h>

char wifi_status = WIFI_STATE_NOCONNECT;

mico_timer_t wifi_led_timer;
mico_timer_t wifi_offline_timer;
static uint32_t wifi_offline_start_s = 0;
IpStatus ip_status = { 0, ZZ_AP_LOCAL_IP, ZZ_AP_LOCAL_IP, ZZ_AP_NET_MASK };

// WiFi获取到IP地址回调
static void WifiGetIpCallback(IPStatusTypedef *pnet, void * arg)
{
    strcpy(ip_status.ip, pnet->ip);
    strcpy(ip_status.gateway, pnet->gate);
    strcpy(ip_status.mask, pnet->mask);

    wifi_log("got IP:%s", pnet->ip);
    wifi_status = WIFI_STATE_CONNECTED;
}

// WiFi连接状态改变回调
static void WifiStatusCallback(WiFiEvent status, void* arg)
{
    if (status == NOTIFY_STATION_UP) // WiFi连接成功
    {
        mico_rtos_stop_timer(&wifi_offline_timer);
        wifi_offline_start_s = 0;
        sys_config->micoSystemConfig.reserved = status;
        mico_system_context_update(sys_config);

        OSStatus status = micoWlanSuspendSoftAP(); // 关闭AP
        if (status != kNoErr)
        {
            wifi_log("close ap error[%d]", status);
        }

        ip_status.mode = 1;
    }
    else if (status == NOTIFY_STATION_DOWN) // WiFi断开
    {
        sys_config->micoSystemConfig.reserved = status;
        mico_system_context_update(sys_config);

        wifi_status = WIFI_STATE_NOCONNECT;
        if (!mico_rtos_is_timer_running(&wifi_led_timer))
        {
            mico_rtos_start_timer(&wifi_led_timer);
        }

        int offline_delay = RESERVED_CFG->wifi_offline_delay;
        if (offline_delay > 0) {
            /* 延迟指定秒数后仍未恢复，则执行离线动作 */
            wifi_offline_start_s = (uint32_t)time(NULL);
            if (!mico_rtos_is_timer_running(&wifi_offline_timer)) {
                mico_rtos_start_timer(&wifi_offline_timer);
            }
        } else {
            /* 默认行为：立即开启AP */
            ApInit(true);
        }
    }
    else if (status == NOTIFY_AP_UP)
    {
        ip_status.mode = 0;
    }
}

/* WiFi断开延迟动作：每秒检查，断开满delay秒未恢复则执行动作 */
static void WifiOfflineTimerHandler(void *arg)
{
    if (wifi_offline_start_s == 0) {
        mico_rtos_stop_timer(&wifi_offline_timer);
        return;
    }
    uint32_t now = (uint32_t)time(NULL);
    if (now - wifi_offline_start_s >= (uint32_t)RESERVED_CFG->wifi_offline_delay) {
        mico_rtos_stop_timer(&wifi_offline_timer);
        wifi_offline_start_s = 0;
        wifi_log("WARNING: wifi offline action[%d]", RESERVED_CFG->wifi_offline_action);
        if (RESERVED_CFG->wifi_offline_action == 1) {
            MicoSystemReboot();
        } else {
            ApInit(true);
        }
    }
}

/* 获取当前连接WiFi的信号强度(dBm)，未连接返回0 */
int RssiGet(void)
{
    LinkStatusTypeDef ls;
    if (micoWlanGetLinkStatus(&ls) == kNoErr && ls.is_connected == 1) {
        return (int)ls.rssi;
    }
    return 0;
}

bool scaned = false;
char* wifi_ret = NULL;

// WiFi扫描结果回调
void WifiScanCallback(ScanResult_adv* scan_ret, void* arg)
{
    int count = (int)scan_ret->ApNum;

    int i = 0;
    size_t buf_size = (size_t)count * 40 + 64;
    char* new_wifi_ret = malloc(buf_size);
    char* ssids = malloc((size_t)count * 36 + 1);
    char* secs = malloc((size_t)count * 2 + 1);
    if (!new_wifi_ret || !ssids || !secs)
    {
        free(new_wifi_ret);
        free(ssids);
        free(secs);
        return;
    }
    ssids[0] = '\0';
    secs[0] = '\0';
    char* tmp1 = ssids;
    char* tmp2 = secs;
    for (; i < count; i++)
    {
        char* ssid = scan_ret->ApList[i].ssid;
        if (ssid[0] == 0 || strstr(ssid, "'") || strstr(ssid, "\"")) continue;
        sprintf(tmp1, "'%s',", ssid);
        tmp1 += (strlen(ssid) + 3);
        sprintf(tmp2, "%d,", scan_ret->ApList[i].security%10);
        tmp2 += 2;
    }
    if (tmp1 > ssids) *(--tmp1) = 0;
    if (tmp2 > secs) *(--tmp2) = 0;

    sprintf(new_wifi_ret, WIFI_SCAN_RESULT_JSON, 1, ssids, secs);

    free(ssids);
    free(secs);

    // 释放旧的 wifi_ret，替换为新的
    if (wifi_ret) free(wifi_ret);
    wifi_ret = new_wifi_ret;
    scaned = true;
}

// 100ms定时器回调
static void WifiLedTimerCallback(void* arg)
{
    static unsigned int num = 0;
    num++;

    switch (wifi_status)
    {
        case WIFI_STATE_FAIL:
            wifi_log("wifi connect fail");
            UserLedSet(0);
            mico_rtos_stop_timer(&wifi_led_timer);
            break;
        case WIFI_STATE_NOCONNECT:
            break;
        case WIFI_STATE_CONNECTING:
            num = 0;
            UserLedSet(-1);
            break;
        case WIFI_STATE_CONNECTED:
            if (!(MQTT_SERVER[0] < 0x20 || MQTT_SERVER[0] > 0x7f || MQTT_SERVER_PORT < 1)){
                UserMqttInit();
            }
            UserLedSet(0);
            mico_rtos_stop_timer(&wifi_led_timer);
            if (RelayOut()&&user_config->power_led_enabled)
                UserLedSet(1);
            else
                UserLedSet(0);
            break;
    }
}

void WifiConnect(char* wifi_ssid, char* wifi_key)
{
    wifi_log("WifiConnect wifi_ssid[%s] wifi_key[******]", wifi_ssid);
    // WiFi配置初始化
    network_InitTypeDef_st wNetConfig;

    memset(&wNetConfig, 0, sizeof(network_InitTypeDef_st));
    wNetConfig.wifi_mode = Station;
    snprintf(wNetConfig.wifi_ssid, sizeof(wNetConfig.wifi_ssid), "%s", wifi_ssid);
    snprintf((char*)wNetConfig.wifi_key, sizeof(wNetConfig.wifi_key), "%s", wifi_key);
    wNetConfig.wifi_retry_interval = 6000;

    if (user_config->ip_mode == 1) {
        wNetConfig.dhcpMode = DHCP_Disable;
        strncpy(wNetConfig.local_ip_addr, user_config->static_ip, sizeof(wNetConfig.local_ip_addr) - 1);
        strncpy(wNetConfig.net_mask, user_config->static_mask, sizeof(wNetConfig.net_mask) - 1);
        strncpy(wNetConfig.gateway_ip_addr, user_config->static_gateway, sizeof(wNetConfig.gateway_ip_addr) - 1);
        strncpy(wNetConfig.dnsServer_ip_addr, user_config->static_dns, sizeof(wNetConfig.dnsServer_ip_addr) - 1);
        wifi_log("Static IP: %s/%s gw:%s dns:%s", wNetConfig.local_ip_addr, wNetConfig.net_mask, wNetConfig.gateway_ip_addr, wNetConfig.dnsServer_ip_addr);
    } else {
        wNetConfig.dhcpMode = DHCP_Client;
    }

    micoWlanStart(&wNetConfig);

    // 保存wifi及密码到Flash
    snprintf(sys_config->micoSystemConfig.ssid, sizeof(sys_config->micoSystemConfig.ssid), "%s", wifi_ssid);
    snprintf(sys_config->micoSystemConfig.user_key, sizeof(sys_config->micoSystemConfig.user_key), "%s", wifi_key);
    sys_config->micoSystemConfig.user_keyLength = strlen(wifi_key);
    mico_system_context_update(sys_config);
    wifi_status = WIFI_STATE_NOCONNECT;
}

void WifiInit(void)
{
    // WiFi状态下led闪烁定时器初始化
    mico_rtos_init_timer(&wifi_led_timer, 100, (void*)WifiLedTimerCallback, NULL);
    // WiFi断开延迟动作定时器 1秒周期
    mico_rtos_init_timer(&wifi_offline_timer, 1000, (void*)WifiOfflineTimerHandler, NULL);
    // WiFi已连接获取到IP地址 回调
    mico_system_notify_register(mico_notify_DHCP_COMPLETED, (void*)WifiGetIpCallback, NULL);
    // WiFi连接状态改变 回调
    mico_system_notify_register(mico_notify_WIFI_STATUS_CHANGED, (void*)WifiStatusCallback, NULL);
    // WiFi扫描结果 回调
    mico_system_notify_register(mico_notify_WIFI_SCAN_ADV_COMPLETED, (void*)WifiScanCallback, NULL);

    // 启动定时器开始进行wifi连接
    if (!mico_rtos_is_timer_running(&wifi_led_timer)) mico_rtos_start_timer(&wifi_led_timer);
}

void ApConfig(char* name, char* key)
{
    strncpy(user_config->ap_name, name, sizeof(user_config->ap_name) - 1);
    user_config->ap_name[sizeof(user_config->ap_name) - 1] = '\0';
    strncpy(user_config->ap_key, key, sizeof(user_config->ap_key) - 1);
    user_config->ap_key[sizeof(user_config->ap_key) - 1] = '\0';
    wifi_log("ApConfig ap_name[%s] ap_key[******]", user_config->ap_name);
    micoWlanSuspendStation();
    ApInit(false);
    mico_system_context_update(sys_config);
}

void ApInit(bool use_defaul)
{
    if (use_defaul)
    {
        sprintf(user_config->ap_name, ZZ_AP_NAME, str_mac + 6);
        sprintf(user_config->ap_key, "%s", ZZ_AP_KEY);
        wifi_log("ApInit use_defaul[true] key[]");
    }

    network_InitTypeDef_st wNetConfig;
    memset(&wNetConfig, 0x0, sizeof(network_InitTypeDef_st));
    strcpy((char *)wNetConfig.wifi_ssid, user_config->ap_name);
    strcpy((char *)wNetConfig.wifi_key, user_config->ap_key);
    wNetConfig.wifi_mode = Soft_AP;
    wNetConfig.dhcpMode = DHCP_Server;
    wNetConfig.wifi_retry_interval = 100;
    strcpy((char *)wNetConfig.local_ip_addr, ZZ_AP_LOCAL_IP);
    strcpy((char *)wNetConfig.net_mask, ZZ_AP_NET_MASK);
    strcpy((char *)wNetConfig.dnsServer_ip_addr, ZZ_AP_DNS_SERVER);
    micoWlanStart(&wNetConfig);

    wifi_log("ApInit ssid[%s] key[******]", wNetConfig.wifi_ssid);
}
