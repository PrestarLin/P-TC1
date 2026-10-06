#include "http_server/web_log.h"

#include "mico.h"
#include "ota_server.h"
#include "main.h"
#include "mqtt_server/user_mqtt_client.h"

volatile int ota_progress = -2;

/* MK3031 固件镜像以 kernel 的 img_hdr 开头: "MRVL" + 0x2e9cf17b */
#define OTA_IMG_MAGIC_STR 0x4C56524D  /* 'MRVL' (little-endian) */
#define OTA_IMG_MAGIC_SIG 0x2E9CF17B

/* 校验 OTA_TEMP(被动分区) 中固件镜像头是否合法, 防止把错误/损坏的文件切换为活动固件 */
int OtaImageHeaderValid(void)
{
    uint8_t hdr[8] = { 0 };
    uint32_t offset = 0;
    uint32_t magic_str;
    uint32_t magic_sig;

    if (MicoFlashRead(MICO_PARTITION_OTA_TEMP, &offset, hdr, sizeof(hdr)) != kNoErr)
        return 0;

    magic_str = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    magic_sig = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);

    return (magic_str == OTA_IMG_MAGIC_STR && magic_sig == OTA_IMG_MAGIC_SIG) ? 1 : 0;
}

static void OtaServerStatusHandler(OTA_STATE_E state, float progress)
{
    char str[64] = { 0 };
    switch (state)
    {
        case OTA_LOADING:
            ota_progress = (int) progress;
            ota_log("ota server is loading, progress %.2f%%", progress);
            if (((int) progress)%10 == 1)
                sprintf(str, "{\"mac\":\"%s\",\"ota_progress\":%d}", str_mac,((int) progress));
            break;
        case OTA_SUCCE:
            ota_progress = 100;
            ota_log("ota server daemons success");
            sprintf(str, "{\"mac\":\"%s\",\"ota_progress\":100}", str_mac);
            break;
        case OTA_FAIL:
            ota_progress = -2;
            ota_log("ota server daemons failed");
            sprintf(str, "{\"mac\":\"%s\",\"ota_progress\":-1}", str_mac);
            break;
        default:
            break;
    }
    /* 救砖场景下 web 不可用, MQTT state 主题是唯一能观察 OTA 进度的通道 */
    if (str[0]) UserMqttSend(str);
    /* 本回调返回后 ota_server 立即切换分区并重启, 留 1s 把成功消息发出去 */
    if (state == OTA_SUCCE) mico_rtos_thread_sleep(1);
}

void UserOtaStart(char *url, char *md5)
{
    ota_progress = 0;
    ota_log("ready to ota:%s",url);
    OSStatus err = ota_server_start(url, md5, OtaServerStatusHandler);
    if (err != kNoErr) {
        /* 启动失败(参数非法/已有任务/内存不足)不会触发回调, 必须在此恢复进度 */
        ota_log("ota_server_start failed, err=%d", err);
        ota_progress = -2;
    }
}

