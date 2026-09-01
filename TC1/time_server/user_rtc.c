#include "http_server/web_log.h"
#include "main.h"
#include "user_gpio.h"
#include "sntp.h"
#include "mqtt_server/user_mqtt_client.h"

void RtcThread(mico_thread_arg_t arg);

OSStatus UserSntpGetTime()
{
    OSStatus err = kNoErr;
    ntp_timestamp_t current_time;

    struct hostent * hostent_content = NULL;
    char ** pptr = NULL;
    struct in_addr ipp;

    ipp.s_addr = 0xd248912c;
    err = sntp_get_time(&ipp, &current_time);

    if (err != kNoErr)
    {
        int ntp_count = 5;
        char* ntp_hosts[5] = {
            "pool.ntp.org",
            "cn.ntp.org.cn",
            "cn.pool.ntp.org",
            "s1a.time.edu.cn",
            "ntp.sjtu.edu.cn",
        };

        int i = 0;
        for (; i < ntp_count; i++)
        {
            hostent_content = gethostbyname(ntp_hosts[i]);
            if (hostent_content == NULL)
            {
                rtc_log("gethostbyname(%s)", ntp_hosts[i]);
                continue;
            }
            pptr = hostent_content->h_addr_list;
            ipp.s_addr = *(uint32_t *)(*pptr);
            err = sntp_get_time(&ipp, &current_time);
            if (err == kNoErr)
            {
                break;
            }
        }
    }

    if (err != kNoErr)
    {
        rtc_log("sntp_get_time4 err[%d]", err);
        return err;
    }

    mico_utc_time_ms_t utc_time_ms = (uint64_t)current_time.seconds * (uint64_t)1000
        + (current_time.microseconds / 1000);
    mico_time_set_utc_time_ms(&utc_time_ms);

    time_t now = (time_t)current_time.seconds + 28800;
    struct tm *t = localtime(&now);
    rtc_log("sntp synced: %04d-%02d-%02d %02d:%02d:%02d",
        t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
        t->tm_hour, t->tm_min, t->tm_sec);

    return kNoErr;
}

OSStatus UserRtcInit(void)
{
    OSStatus err = kNoErr;

    /* start rtc client */
    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "rtc",
                                   (mico_thread_function_t) RtcThread,
                                   0x800, 0);
    require_noerr_string(err, exit, "ERROR: Unable to start the rtc thread.");

    if (kNoErr != err) rtc_log("ERROR1, app thread exit err: %d kNoErr[%d]", err, kNoErr);

    exit:
    return err;
}

void RtcThread(mico_thread_arg_t arg)
{
    OSStatus err = kUnknownErr;
    LinkStatusTypeDef LinkStatus;

    mico_utc_time_t utc_time;
    mico_utc_time_t utc_time_last = 0;
    int last_sync_min = -1;
    while (1)
    {
        micoWlanGetLinkStatus(&LinkStatus);
        if (LinkStatus.is_connected == 1)
        {
            err = UserSntpGetTime();
            if (err == kNoErr)
            {
                rtc_log("sntp success!");
                rtc_init = 1;
                /* 时钟已同步, 若开启夜间模式则用正确时间重建每日 LED 任务
                 * (开机时 rtc_init!=1 不建, 避免 1970 基准时间导致任务乱触发)。 */
                if (user_config->night_mode_enabled) {
                    RemoveNightModeTasks();
                    CreateNightModeTask(user_config->night_mode_start / 60, user_config->night_mode_start % 60, 0);
                    CreateNightModeTask(user_config->night_mode_end / 60, user_config->night_mode_end % 60, 1);
                }
                break;
            }
            rtc_init = 2;
            mico_rtos_thread_sleep(3);
        }
        mico_rtos_thread_sleep(3);
    }

    while (1)
    {
        mico_time_get_utc_time(&utc_time);
        utc_time += 28800;

        if (utc_time_last != utc_time)
        {
            utc_time_last = utc_time;
            total_time++;
        }

        struct tm * currentTime = localtime((const time_t *) &utc_time);

        // 每10分钟同步一次NTP，避免依赖tm_sec==0的1秒窗口
        if (currentTime->tm_min % 10 == 0 && currentTime->tm_min != last_sync_min)
        {
            micoWlanGetLinkStatus(&LinkStatus);
            if (LinkStatus.is_connected == 1)
            {
                rtc_log("periodic ntp sync...");
                err = UserSntpGetTime();
                if (err == kNoErr) {
                    rtc_init = 1;
                    last_sync_min = currentTime->tm_min; // 成功才更新，失败会在同一窗口重试
                } else {
                    rtc_init = 2;
                }
            }
        }

        mico_rtos_thread_msleep(5000);
    }

    rtc_log("EXIT: rtc exit with err = %d.", err);
    mico_rtos_delete_thread(NULL);
}

