/**
 ******************************************************************************
 * @file    app_https.c
 * @author  QQ DING
 * @version V1.0.0
 * @date    1-September-2015
 * @brief   The main HTTPD server initialization and wsgi handle.
 ******************************************************************************
 *
 *  The MIT License
 *  Copyright (c) 2016 MXCHIP Inc.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is furnished
 *  to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
 *  IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 ******************************************************************************
 */

#include <time.h>
#include <httpd.h>
#include <http_parse.h>
#include <http-strings.h>
#include "stdlib.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mico.h"
#include "httpd_priv.h"
#include "app_httpd.h"
#include "user_gpio.h"
#include "user_wifi.h"
#include "user_power.h"
#include "main.h"
#include "web_data.c"
#include "web_log.h"
#include "timed_task/timed_task.h"
#include "ota_server/user_ota.h"
#include "mqtt_server/user_mqtt_client.h"

static bool is_http_init;
static bool is_handlers_registered;
const struct httpd_wsgi_call g_app_handlers[];
char power_info_json[2048] = {0};
char up_time[32] = "00:00:00";
#define CHUNK_SIZE 1024  // 鍖归厤SDK HTTPD_SEND_BODY_DATA_MAX_LEN锛屽噺灏憇end璋冪敤娆℃暟
#define OTA_BUFFER_SIZE 512
#define MAX_OTA_SIZE 1024*1024

/*
void GetPraFromUrl(char* url, char* pra, char* val)
{
    char* sub = strstr(url, pra);
    if (sub == NULL)
    {
        val[0] = 0;
        return;
    }
    sub = strstr(sub, "=");
    if (sub == NULL)
    {
        val[0] = 0;
        return;
    }
    int len = strlen(sub);
    int n = 0;
    for (int i = 0; i < len; i++)
    {
        if (sub[i] == '&' || i == len - 1)
        {
            n = len;
            break;
        }
    }
    if (n > 0)
    {
        strncpy(val, sub + 1, n - 1);
        val[n - 1] = 0;
        return;
    }
    val[0] = 0;
}
*/


static OSStatus send_in_chunks(int sock, const uint8_t *data, int total_len) {
    OSStatus err = kNoErr;
    for (int offset = 0; offset < total_len; offset += CHUNK_SIZE) {
        int chunk_len = (total_len - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total_len - offset);
        err = httpd_send_body(sock, data + offset, chunk_len);
        require_noerr_action(err, exit, http_log("ERROR: Send chunk failed at offset %d", offset));
    }
exit:
    return err;
}

static int HttpGetIndexPage(httpd_request_t *req) {
    OSStatus err = kNoErr;
    int total_sz = sizeof(web_index_html);


    err = httpd_send_all_header(req, HTTP_RES_200, total_sz, HTTP_CONTENT_HTML_ZIP);
    require_noerr_action(err, exit, http_log("ERROR: Unable to send index headers."));

    err = send_in_chunks(req->sock, web_index_html, total_sz);
    require_noerr_action(err, exit, http_log("ERROR: Unable to send index body."));

exit:
    return err;
}

static int HttpGetAssets(httpd_request_t *req) {
    OSStatus err = kNoErr;

    char *file_name = strstr(req->filename, "/assets/");
    if (!file_name) {
        http_log("HttpGetAssets url[%s] err", req->filename);
        return err;
    }

    int total_sz = 0;
    const unsigned char *file_data = NULL;
    const char *content_type = HTTP_CONTENT_JS_ZIP;

    if (strcmp(file_name + 8, "js_pack.js") == 0) {
        total_sz = sizeof(js_pack);
        file_data = js_pack;
    } else if (strcmp(file_name + 8, "css_pack.css") == 0) {
        total_sz = sizeof(css_pack);
        file_data = css_pack;
        content_type = HTTP_CONTENT_CSS_ZIP;
    } else if (strcmp(file_name + 8, "index.html") == 0) {
        total_sz = sizeof(web_index_html);
        file_data = web_index_html;
        content_type = HTTP_CONTENT_HTML_ZIP;
    }

    if (total_sz == 0 || file_data == NULL) {
        http_log("File not found: %s", req->filename);
        return err;
    }


    err = httpd_send_all_header(req, HTTP_RES_200, total_sz, content_type);
    require_noerr_action(err, exit, http_log("ERROR: Unable to send asset headers."));

    err = send_in_chunks(req->sock, file_data, total_sz);
    require_noerr_action(err, exit, http_log("ERROR: Unable to send asset body."));

exit:
    return err;
}

static int HttpGetTc1Status(httpd_request_t *req) {
    char *sockets = GetSocketStatus();
    char *short_click_config = GetButtonClickConfig();
    char *tc1_status = malloc(2048);
    char *socket_names = malloc(512);
    sprintf(socket_names, "%s,%s,%s,%s,%s,%s",
            user_config->socket_names[0],
            user_config->socket_names[1],
            user_config->socket_names[2],
            user_config->socket_names[3],
            user_config->socket_names[4],
            user_config->socket_names[5]);
    sprintf(tc1_status, TC1_STATUS_JSON, sockets, ip_status.mode,
            sys_config->micoSystemConfig.ssid, sys_config->micoSystemConfig.user_key,
            user_config->ap_name, user_config->ap_key, MQTT_SERVER, MQTT_SERVER_PORT,
            MQTT_SERVER_USR, MQTT_SERVER_PWD,
            VERSION, ip_status.ip, ip_status.mask, ip_status.gateway, str_mac,
            user_config->ip_mode, user_config->static_ip, user_config->static_mask,
            user_config->static_gateway, user_config->static_dns,
            user_config->mqtt_report_freq,
            user_config->power_led_enabled, 0L, socket_names, childLockEnabled,
            sys_config->micoSystemConfig.name, short_click_config,
            user_config->night_mode_enabled,
            user_config->night_mode_start,
            user_config->night_mode_end,
            UserMqttIsConnect() ? 1 : 0,
            RssiGet(),
            RESERVED_CFG->wifi_offline_delay,
            RESERVED_CFG->wifi_offline_action);

    OSStatus err = kNoErr;
    send_http(tc1_status, strlen(tc1_status), exit, &err);

    exit:
    if (socket_names) free(socket_names);
    if (tc1_status) free(tc1_status);
    return err;
}

static int HttpSetSocketStatus(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 512;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);

    SetSocketStatus(buf);

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpSetSocketName(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 70;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);
    int index;
    char name[64] = {0};
    if (sscanf(buf, "%d %63s", &index, name) < 2 || name[0] == '\0') {
        snprintf(name, sizeof(name), "Socket %d", index + 1);
    }
    if (index < 0 || index >= SOCKET_NUM) { free(buf); return kParamErr; }
    strncpy(user_config->socket_names[index], name, sizeof(user_config->socket_names[index]) - 1);
    user_config->socket_names[index][sizeof(user_config->socket_names[index]) - 1] = '\0';
    mico_system_context_update(sys_config);
    registerMqttEvents();
    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpSetButtonEvent(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 10;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);
    int index;
    int func;
    int longPress;
    sscanf(buf, "%d %d %d", &index, &func, &longPress);
    if (index < 0 || index >= maxNameLen) { free(buf); return kParamErr; }
    
    // Safety搴曠嚎锛氶粯璁や换鍔?5绉掗厤缃戙€?0绉掓仮澶嶅嚭鍘?涓嶅厑璁镐慨鏀?
    if ((index == 5 && longPress == 1) || (index == 10 && longPress == 1)) {
        http_log("Blocked: default task at %ds cannot be modified", index);
        free(buf);
        send_http("BLOCKED", 7, exit, &err);
        return err;
    }
    
    if (func == -1 || func == NO_FUNCTION) func = KEY_NONE;  // 未设置/无

    if (longPress == 1) {
        set_key_map(user_config->user,index, RESERVED_CFG->key_short[index], func);
    } else {
        set_key_map(user_config->user,index, func, RESERVED_CFG->key_long[index]);
    }
    key_log("WARNING:set KEY func %d short[%d] long[%d]", index, RESERVED_CFG->key_short[index], RESERVED_CFG->key_long[index]);
    mico_system_context_update(sys_config);

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

#define OTA_BUF_SIZE 5120
#define OTA_MIN_SIZE 32768

static int HttpSetOTAFile(httpd_request_t *req)
{
    tc1_log("[OTA] hdr_parsed=%d, remaining=%d, body_nbytes=%d, req.chunked=%d",
        req->hdr_parsed, req->remaining_bytes, req->body_nbytes, req->chunked);
    OSStatus err = kNoErr;
    int total = 0;
    int ret = 0;
    char *buffer = NULL;
    uint32_t offset = 0;
    bool upload_ok = false;
    mico_logic_partition_t *ota_partition = NULL;

    /* OTA 并发保护: 防止浏览器关闭后立即重试导致冲突 */
    if (ota_progress >= 0 && ota_progress < 100) {
        http_log("[OTA] skip, already in progress[%d]", ota_progress);
        send_http("BUSY", 4, exit, &err);
        return err;
    }
    ota_progress = 0;

    /* OTA 防呆保护: 只有完整收到、大小一致且镜像头合法的固件才会切换并重启。
     * 上传中途断电 / 浏览器被关闭 / 连接断开 / 传错文件时一律放弃:
     * 清空被动分区、不切换、不重启, 旧固件继续运行, 不会变砖。 */

    buffer = malloc(OTA_BUF_SIZE);
    require_action(buffer, exit, err = kNoMemoryErr);

    ota_partition = MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP);
    require_action(ota_partition, exit, err = kUnsupportedErr);

    MicoFlashErase(MICO_PARTITION_OTA_TEMP, 0x0, ota_partition->partition_length);

    CRC16_Context crc_context;
    CRC16_Init(&crc_context);

    while (1) {
        ret = httpd_get_data2(req, buffer, OTA_BUF_SIZE);

        if (ret > 0) {
            total += ret;
            if ((uint32_t)total > ota_partition->partition_length) {
                tc1_log("[OTA] file too large: %d > partition %d", total, ota_partition->partition_length);
                err = kSizeErr;
                break;
            }
            CRC16_Update(&crc_context, (uint8_t*)buffer, ret);
            err = MicoFlashWrite(MICO_PARTITION_OTA_TEMP, &offset, (uint8_t*)buffer, ret);
            require_noerr_quiet(err, exit);

            if (req->body_nbytes > 0 && total >= req->body_nbytes) {
                upload_ok = true;   /* Content-Length 已收满 */
                break;
            }
        } else if (ret == 0) {
            /* 对端关闭连接(浏览器被关闭/断网): 没收满 Content-Length 即为截断,
             * 无 Content-Length 的请求一律视为非法, 不接受无法校验完整性的上传 */
            tc1_log("[OTA] connection closed early: got %d of %d bytes", total, req->body_nbytes);
            err = kConnectionErr;
            break;
        } else {
            /* ret < 0: socket 错误 */
            tc1_log("[OTA] read error ret=%d, got %d bytes", ret, total);
            err = kConnectionErr;
            break;
        }
    }

    /* 校验 1: 接收字节数必须与 Content-Length 一致, 且不小于最小固件尺寸 */
    if (err == kNoErr && upload_ok) {
        if (req->body_nbytes > 0 && total != req->body_nbytes) {
            tc1_log("[OTA] size mismatch: got %d, expected %d", total, req->body_nbytes);
            err = kSizeErr;
        } else if (total < OTA_MIN_SIZE) {
            tc1_log("[OTA] file too small: %d bytes", total);
            err = kSizeErr;
        }
    }

    /* 校验 2: 固件镜像头 (MRVL + magic_sig), 防止上传了错误的文件 */
    if (err == kNoErr && upload_ok) {
        if (!OtaImageHeaderValid()) {
            tc1_log("[OTA] invalid image magic");
            err = kParamErr;
        }
    }

    if (err != kNoErr) {
        /* 失败: 清空被动分区, 不切换、不重启, 旧固件继续运行 */
        tc1_log("[OTA] upload aborted, old firmware keeps running");
        MicoFlashErase(MICO_PARTITION_OTA_TEMP, 0x0, ota_partition->partition_length);
        ota_progress = -2;
        httpd_send_all_header(req, HTTP_RES_400, 10, HTTP_CONTENT_PLAIN_TEXT_STR);
        httpd_send_body(req->sock, (const unsigned char*)"OTA FAILED", 10);
        goto exit;
    }

    uint16_t crc16;
    CRC16_Final(&crc_context, &crc16);

    err = mico_ota_switch_to_new_fw(total, crc16);
    tc1_log("[OTA] mico_ota_switch_to_new_fw err=%d", err);
    require_noerr(err, exit);

    char resp[128];
    snprintf(resp, sizeof(resp), "OK, total: %d bytes", total);
    send_http(resp, strlen(resp), exit, &err);

    mico_system_power_perform(mico_system_context_get(), eState_Software_Reset);
exit:
    if (buffer) free(buffer);
    return err;
}

static int HttpSetDeviceName(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 70;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);
    char name[64];
    sscanf(buf, "%63s", name);
    strncpy(sys_config->micoSystemConfig.name, name, sizeof(sys_config->micoSystemConfig.name) - 1);
    sys_config->micoSystemConfig.name[sizeof(sys_config->micoSystemConfig.name) - 1] = '\0';
    mico_system_context_update(sys_config);
    registerMqttEvents();
    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpSetChildLock(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 32;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);
    int enableLock;
    sscanf(buf, "%d", &enableLock);
    user_config->child_lock = enableLock;
    childLockEnabled = enableLock;
    mico_system_context_update(sys_config);
    UserMqttSendChildLockState();
    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpGetPowerInfo(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[16];
    err = httpd_get_data(req, buf, 16);
    require_noerr(err, exit);

    int idx = 0;
    sscanf(buf, "%d", &idx);

    //璁＄畻绯荤粺杩愯鏃堕棿
    mico_time_t past_ms = 0;
    mico_time_get_time(&past_ms);
    int past = past_ms / 1000;
    int d = past / 3600 / 24;
    int h = past / 3600 % 24;
    int m = past / 60 % 60;
    int s = past % 60;
    sprintf(up_time, "%dd %02d:%02d:%02d", d, h, m, s);

    char *powers = GetPowerRecord(idx);
    char *sockets = GetSocketStatus();
    char *short_click_config = GetButtonClickConfig();
    char *socket_names = malloc(512);
    sprintf(socket_names, "%s,%s,%s,%s,%s,%s",
            user_config->socket_names[0],
            user_config->socket_names[1],
            user_config->socket_names[2],
            user_config->socket_names[3],
            user_config->socket_names[4],
            user_config->socket_names[5]);
    sprintf(power_info_json, POWER_INFO_JSON, sockets, power_record.idx, PW_NUM, p_count, powers,
            up_time, user_config->power_led_enabled, RelayOut() ? 1 : 0, socket_names,
            user_config->p_count_1_day_ago, user_config->p_count_2_days_ago, childLockEnabled,
            sys_config->micoSystemConfig.name, short_click_config);
    send_http(power_info_json, strlen(power_info_json), exit, &err);
    exit:
    if (socket_names) free(socket_names);
    return err;
}

static int HttpGetWifiConfig(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char *status = "test";
    send_http(status, strlen(status), exit, &err);
    exit:
    return err;
}


// 鍗曚釜鍗佸叚杩涘埗瀛楃杞暟瀛楋紙瀹夊叏锛?
static int hex_char_to_int(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 鍋ュ．鐗?URL 瑙ｇ爜鍑芥暟
void url_decode(const char *src, char *dest, size_t max_len) {
    size_t i = 0;
    while (*src && i < max_len - 1) {
        if (*src == '%') {
            if (isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
                int high = hex_char_to_int(src[1]);
                int low = hex_char_to_int(src[2]);
                if (high >= 0 && low >= 0) {
                    dest[i++] = (char)((high << 4) | low);
                    src += 3;
                    continue;
                }
            }
            // 闈炴硶缂栫爜锛岃烦杩?%
            src++;
        } else if (*src == '+') {
            dest[i++] = ' ';
            src++;
        } else {
            dest[i++] = *src++;
        }
    }
    dest[i] = '\0';
}

static int HttpSetWifiConfig(httpd_request_t *req) {
    OSStatus err = kNoErr;

  char *buf = malloc(256);
  char *ssid_enc = malloc(128);
  char *key_enc = malloc(128);
  char *wifi_ssid = malloc(128);
  char *wifi_key = malloc(128);
  int mode = -1;

  if (!buf || !ssid_enc || !key_enc || !wifi_ssid || !wifi_key) {
      free(buf); free(ssid_enc); free(key_enc); free(wifi_ssid); free(wifi_key);
      return kNoMemoryErr;
  }

    err = httpd_get_data(req, buf, 256);
    require_noerr(err, exit);
  if (sscanf(buf, "%d %127s %127s", &mode, ssid_enc, key_enc) < 3)
  {
      err = kUnknownErr;
      send_http("ERR", 3, exit, &err);
      goto exit;
  }
  url_decode(ssid_enc, wifi_ssid, 128);
  url_decode(key_enc, wifi_key, 128);
    if (mode == 1) {
        WifiConnect(wifi_ssid, wifi_key);
    } else {
        ApConfig(wifi_ssid, wifi_key);
    }

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    if (ssid_enc) free(ssid_enc);
    if (key_enc) free(key_enc);
    if (wifi_ssid) free(wifi_ssid);
    if (wifi_key) free(wifi_key);
    return err;
}

static int HttpGetWifiScan(httpd_request_t *req) {
    OSStatus err = kNoErr;
    if (scaned && wifi_ret) {
        scaned = false;
        send_http(wifi_ret, strlen(wifi_ret), exit, &err);
    } else {
        send_http("NO", 2, exit, &err);
    }

    exit:
    if (wifi_ret) { free(wifi_ret); wifi_ret = NULL; }
    return err;
}

static int HttpSetWifiScan(httpd_request_t *req) {
    micoWlanStartScanAdv();
    OSStatus err = kNoErr;
    send_http("OK", 2, exit, &err);
    exit:
    return err;
}

static int HttpSetRebootSystem(httpd_request_t *req) {
    OSStatus err = kNoErr;

    send_http("OK", 2, exit, &err);

    MicoSystemReboot();

    exit:
    return err;
}

static int HttpFactoryReset(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[8] = {0};
    err = httpd_get_data(req, buf, sizeof(buf));
    require_noerr(err, exit);

    int keep = 0;
    sscanf(buf, "%d", &keep);

    /* keep bit0=WiFi, bit1=MQTT */
    char saved_ap_name[32] = {0};
    char saved_ap_key[32] = {0};
    char saved_mqtt_ip[SETTING_MQTT_STRING_LENGTH_MAX] = {0};
    int   saved_mqtt_port = 0;
    char saved_mqtt_user[SETTING_MQTT_STRING_LENGTH_MAX] = {0};
    char saved_mqtt_pass[SETTING_MQTT_STRING_LENGTH_MAX] = {0};

    if (keep & 1) {
        memcpy(saved_ap_name, user_config->ap_name, sizeof(saved_ap_name));
        memcpy(saved_ap_key, user_config->ap_key, sizeof(saved_ap_key));
    }
    if (keep & 2) {
        memcpy(saved_mqtt_ip, user_config->mqtt_ip, sizeof(saved_mqtt_ip));
        saved_mqtt_port = user_config->mqtt_port;
        memcpy(saved_mqtt_user, user_config->mqtt_user, sizeof(saved_mqtt_user));
        memcpy(saved_mqtt_pass, user_config->mqtt_password, sizeof(saved_mqtt_pass));
    }

    memset(user_config, 0, sizeof(user_config_t));
    user_config->version = USER_CONFIG_VERSION;
    user_config->power_led_enabled = 1;

    if (keep & 1) {
        memcpy(user_config->ap_name, saved_ap_name, sizeof(saved_ap_name));
        memcpy(user_config->ap_key, saved_ap_key, sizeof(saved_ap_key));
    }
    if (keep & 2) {
        memcpy(user_config->mqtt_ip, saved_mqtt_ip, sizeof(saved_mqtt_ip));
        user_config->mqtt_port = saved_mqtt_port;
        memcpy(user_config->mqtt_user, saved_mqtt_user, sizeof(saved_mqtt_user));
        memcpy(user_config->mqtt_password, saved_mqtt_pass, sizeof(saved_mqtt_pass));
    }

    for (int i = 0; i < SOCKET_NUM; i++) {
        snprintf(user_config->socket_names[i], SOCKET_NAME_LENGTH, "Socket %d", i + 1);
    }

    mico_system_context_update(sys_config);
    send_http("OK", 2, exit, &err);
    mico_rtos_thread_sleep(1);
    MicoSystemReboot();

    exit:
    return err;
}

static int HttpSetWifiStatic(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 128;
    char *buf = malloc(buf_size);
    if (!buf) return kNoMemoryErr;

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);

    int mode = 0;
    char ip[16] = {0}, mask[16] = {0}, gw[16] = {0}, dns[16] = {0};
    sscanf(buf, "%d %15s %15s %15s %15s", &mode, ip, mask, gw, dns);

    user_config->ip_mode = mode;
    if (mode == 1) {
        strncpy(user_config->static_ip, ip, sizeof(user_config->static_ip) - 1);
        strncpy(user_config->static_mask, mask, sizeof(user_config->static_mask) - 1);
        strncpy(user_config->static_gateway, gw, sizeof(user_config->static_gateway) - 1);
        strncpy(user_config->static_dns, dns, sizeof(user_config->static_dns) - 1);
        http_log("Static IP saved: %s/%s gw:%s dns:%s", ip, mask, gw, dns);
    } else {
        http_log("DHCP mode saved");
    }
    mico_system_context_update(sys_config);

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpSetWifiOffline(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 32;
    char *buf = malloc(buf_size);
    if (!buf) return kNoMemoryErr;

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);

    int delay = 0, action = 0;
    sscanf(buf, "%d %d", &delay, &action);
    if (delay < 0) delay = 0;
    if (delay > 3600) delay = 3600;
    if (action != 0 && action != 1) action = 0;
    RESERVED_CFG->wifi_offline_delay = delay;
    RESERVED_CFG->wifi_offline_action = action;
    http_log("wifi offline action saved: delay=%ds action=%d", delay, action);
    mico_system_context_update(sys_config);

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpSetMqttConfig(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 97;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);

    sscanf(buf, "%31s %d %31s %31s", MQTT_SERVER, &MQTT_SERVER_PORT, MQTT_SERVER_USR, MQTT_SERVER_PWD);
    mico_system_context_update(sys_config);
    if (!(MQTT_SERVER[0] < 0x20 || MQTT_SERVER[0] > 0x7f || MQTT_SERVER_PORT < 1)){
    err = UserMqttInit();
    require_noerr(err, exit);
    }
    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}


static int HttpSetMqttReportFreq(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 97;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);

    sscanf(buf, "%d", &MQTT_REPORT_FREQ);
    mico_system_context_update(sys_config);

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int HttpGetMqttReportFreq(httpd_request_t *req) {
    OSStatus err = kNoErr;
    int buf_size = 97;
    char *freq = malloc(buf_size);
    sprintf(freq, "%d", MQTT_REPORT_FREQ);

    send_http(freq, strlen(freq), exit, &err);

    exit:
    if (freq) free(freq);
    return err;
}

static int HttpGetLog(httpd_request_t *req) {
    OSStatus err = kNoErr;
    int since = 0;
    char *p = strchr(req->filename, '?');
    if (p) {
        char *q = strstr(p, "since=");
        if (q) since = atoi(q + 6);
    }
    char *logs = GetLogRecord(since);
    send_http(logs, strlen(logs), exit, &err);

    exit:
    return err;
}

static int HttpGetTasks(httpd_request_t *req) {
    OSStatus err = kNoErr;
    TaskLock();
    char *tasks_str = GetTaskStr();
    TaskUnlock();
    send_http(tasks_str, strlen(tasks_str), exit, &err);

    exit:
    if (tasks_str) free(tasks_str);
    return err;
}

static int HttpGetButtonEvents(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char *clicks = GetButtonClickConfig();
    send_http(clicks, strlen(clicks), exit, &err);

    exit:
    return err;
}

static int HttpAddTask(httpd_request_t *req) {
    OSStatus err = kNoErr;

    char buf[64] = {0};
    err = httpd_get_data(req, buf, sizeof(buf));
    require_noerr(err, exit);

    TaskLock();
    pTimedTask task = NewTask();
    if (task == NULL) {
        TaskUnlock();
        http_log("NewTask() error, max task num = %d!", MAX_TASK_NUM);
        char *mess = "NO SPACE";
        send_http(mess, strlen(mess), exit, &err);
        return err;
    }
    int saved_on_use = task->on_use;
    memset(task, 0, sizeof(struct TimedTask));
    task->on_use = saved_on_use;

    int loop_dur = 0, loop_int = 0, loop_end = 0;
    int re = sscanf(buf, "%ld %d %d %d %d %d %d", &task->prs_time, &task->operation, &task->on,
                    &task->weekday, &loop_dur, &loop_int, &loop_end);
    http_log("AddTask buf[%s] re[%d]", buf, re);

    /* 濡傛灉浼犱簡寰幆鍙傛暟锛岀紪鐮佸埌 weekday */
    if (re >= 6 && loop_dur > 0) {
        task->weekday = MAKE_LOOP_WEEKDAY(loop_dur, loop_int);
        task->loop_end = loop_end;
        http_log("Loop task: dur=%d int=%d end=%d weekday=0x%X", loop_dur, loop_int, loop_end, task->weekday);
    }

    if (task->prs_time < 1577428136 || task->prs_time > 9577428136
        || task->operation < 0 || task->operation > 13
        || task->on < -1 || task->on > 1) { http_log("AddTask Error!");
        re = 0;
    }

    char *mess = "OK";
    if (re < 4 || !AddTask(task)) {
        task->on_use = false;
        mess = "NO";
    } else {
        mico_system_context_update(sys_config);
    }
    TaskUnlock();

    send_http(mess, strlen(mess), exit, &err);

    exit:
    return err;
}

static int HttpDelTask(httpd_request_t *req) {
    OSStatus err = kNoErr;

    char *time_str = strstr(req->filename, "/task/");
    if (!time_str) { http_log("HttpDelTask url[%s] err", req->filename);
        return err;
    }http_log("HttpDelTask url[%s] time_str[%s][%s]", req->filename, time_str, time_str + 6);

    int time1 = 0;
    int ret = sscanf(time_str + 6, "%d", &time1);

    TaskLock();
    char *mess = (ret == 1 && DelTask(time1)) ? "OK" : "NO";
    TaskUnlock();

    send_http(mess, strlen(mess), exit, &err);
    exit:
    return err;
}

static int HttpClearTasks(httpd_request_t *req) {
    OSStatus err = kNoErr;
    TaskLock();
    ClearAllTasks();
    TaskUnlock();
    send_http("OK", 2, exit, &err);
    exit:
    return err;
}

static int HttpClearLoopTasks(httpd_request_t *req) {
    OSStatus err = kNoErr;
    TaskLock();
    ClearLoopTasks();
    TaskUnlock();
    send_http("OK", 2, exit, &err);
    exit:
    return err;
}

static int HttpClearScheduledTasks(httpd_request_t *req) {
    OSStatus err = kNoErr;
    TaskLock();
    ClearScheduledTasks();
    TaskUnlock();
    send_http("OK", 2, exit, &err);
    exit:
    return err;
}

static int LedStatus(httpd_request_t *req) {
    OSStatus err = kNoErr;
    int buf_size = 97;
    char *led = malloc(buf_size);
    sprintf(led, "%d", MQTT_LED_ENABLED);

    send_http(led, strlen(led), exit, &err);

    exit:
    if (led) free(led);
    return err;
}

static int LedSetEnabled(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 97;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);

    sscanf(buf, "%d", &MQTT_LED_ENABLED);
    if (RelayOut() && MQTT_LED_ENABLED) {
        UserLedSet(1);
    } else {
        UserLedSet(0);
    }
    UserMqttSendLedState();
    mico_system_context_update(sys_config);

    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int TotalSocketSetEnabled(httpd_request_t *req) {
    OSStatus err = kNoErr;

    int buf_size = 97;
    int on;
    char *buf = malloc(buf_size);

    err = httpd_get_data(req, buf, buf_size);
    require_noerr(err, exit);

    sscanf(buf, "%d", &on);
    UserRelaySetAll(on);
    int i = 0;
    for (; i < SOCKET_NUM; i++) {
        UserMqttSendSocketState(i);
    }
    UserMqttSendTotalSocketState();
    send_http("OK", 2, exit, &err);

    exit:
    if (buf) free(buf);
    return err;
}

static int Otastatus(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[16] = {0};
    sprintf(buf, "%d", ota_progress);
    send_http(buf, strlen(buf), exit, &err);
    exit:
    return err;
}

static int OtaStart(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[64] = {0};
    err = httpd_get_data(req, buf, 64);
    require_noerr(err, exit);

    if (ota_progress >= 0 && ota_progress < 100) {
        http_log("OtaStart skip, ota already in progress[%d]", ota_progress);
        send_http("BUSY", 4, exit, &err);
        return err;
    }

    http_log("OtaStart ota_url[%s]", buf);
    UserOtaStart(buf, NULL);

    send_http("OK", 2, exit, &err);
    exit:
    return err;
}

static int HttpSetNightMode(httpd_request_t *req) {
    OSStatus err = kNoErr;
    char buf[32] = {0};
    err = httpd_get_data(req, buf, sizeof(buf));
    require_noerr(err, exit);

    int enabled = 0, start_h = 0, start_m = 0, end_h = 0, end_m = 0;
    sscanf(buf, "%d %d:%d %d:%d", &enabled, &start_h, &start_m, &end_h, &end_m);

    user_config->night_mode_enabled = enabled ? 1 : 0;
    user_config->night_mode_start = (start_h * 60 + start_m) % 1440;
    user_config->night_mode_end = (end_h * 60 + end_m) % 1440;
    mico_system_context_update(sys_config);

    RemoveNightModeTasks();
    if (enabled) {
        CreateNightModeTask(start_h, start_m, 0);
        CreateNightModeTask(end_h, end_m, 1);
    }

    send_http("OK", 2, exit, &err);
    exit:
    return err;
}

const struct httpd_wsgi_call g_app_handlers[] = {
        {"/",                 HTTPD_HDR_DEFORT, 0,                             HttpGetIndexPage, NULL,                       NULL, NULL},
        {"/assets", HTTPD_HDR_ADD_SERVER |
                    HTTPD_HDR_ADD_CONN_CLOSE,   APP_HTTP_FLAGS_NO_EXACT_MATCH, HttpGetAssets,    NULL,                       NULL, NULL},
        {"/socket",           HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetSocketStatus,   NULL, NULL},
        {"/status",           HTTPD_HDR_DEFORT, 0,                             HttpGetTc1Status, NULL,                       NULL, NULL},
        {"/power",            HTTPD_HDR_DEFORT, 0,                             HttpGetPowerInfo,      HttpGetPowerInfo,      NULL, NULL},
        {"/wifi/config",      HTTPD_HDR_DEFORT, 0,                             HttpGetWifiConfig,     HttpSetWifiConfig,     NULL, NULL},
        {"/wifi/scan",        HTTPD_HDR_DEFORT, 0,                             HttpGetWifiScan,       HttpSetWifiScan,       NULL, NULL},
        {"/wifi/static",      HTTPD_HDR_DEFORT, 0,                             NULL,                  HttpSetWifiStatic,     NULL, NULL},
        {"/wifi/offline",     HTTPD_HDR_DEFORT, 0,                             NULL,                  HttpSetWifiOffline,    NULL, NULL},
        {"/mqtt/config",      HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetMqttConfig,     NULL, NULL},
        {"/reboot",           HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetRebootSystem,   NULL, NULL},
        {"/factory-reset",    HTTPD_HDR_DEFORT, 0, NULL,                                              HttpFactoryReset,      NULL, NULL},
        {"/night-mode",       HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetNightMode,      NULL, NULL},
        {"/mqtt/report/freq", HTTPD_HDR_DEFORT, 0,                             HttpGetMqttReportFreq, HttpSetMqttReportFreq, NULL, NULL},
        {"/log",              HTTPD_HDR_DEFORT, 0,                             HttpGetLog,       NULL,                       NULL, NULL},
        {"/task/clear",       HTTPD_HDR_DEFORT, 0,                             NULL,                  HttpClearTasks,        NULL, NULL},
        {"/task/clear/loop",  HTTPD_HDR_DEFORT, 0,                             NULL,                  HttpClearLoopTasks,    NULL, NULL},
        {"/task/clear/scheduled", HTTPD_HDR_DEFORT, 0,                        NULL,                  HttpClearScheduledTasks, NULL, NULL},
        {"/task",             HTTPD_HDR_DEFORT, APP_HTTP_FLAGS_NO_EXACT_MATCH, HttpGetTasks,          HttpAddTask,           NULL, HttpDelTask},
        {"/ota",              HTTPD_HDR_DEFORT, 0,                             Otastatus,             OtaStart,              NULL, NULL},
        {"/led",              HTTPD_HDR_DEFORT, 0,                             LedStatus,             LedSetEnabled,         NULL, NULL},
        {"/socketAll",        HTTPD_HDR_DEFORT, 0, NULL,                                              TotalSocketSetEnabled, NULL, NULL},
        {"/socketNames",      HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetSocketName,     NULL, NULL},
        {"/childLock",        HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetChildLock,      NULL, NULL},
        {"/deviceName",       HTTPD_HDR_DEFORT, 0, NULL,                                              HttpSetDeviceName,     NULL, NULL},
        {"/buttonEvents",     HTTPD_HDR_DEFORT, 0,                             HttpGetButtonEvents,   HttpSetButtonEvent,    NULL, NULL},
        {"/ota/fileUpload",     HTTPD_HDR_DEFORT, 0,                             NULL,   HttpSetOTAFile,    NULL, NULL},
};

static int g_app_handlers_no = sizeof(g_app_handlers) / sizeof(struct httpd_wsgi_call);

static void AppHttpRegisterHandlers() {
    int rc;
    rc = httpd_register_wsgi_handlers((struct httpd_wsgi_call *) g_app_handlers, g_app_handlers_no);
    if (rc) { http_log("failed to register test web handler");
    }
}

static int _AppHttpdStart() {
    OSStatus err = kNoErr;http_log("initializing web-services");

    /*Initialize HTTPD*/
    if (is_http_init == false) {
        err = httpd_init();
        require_noerr_action(err, exit, http_log("failed to initialize httpd"));
        is_http_init = true;
    }

    /*Start http thread*/
    err = httpd_start();
    if (err != kNoErr) { http_log("failed to start httpd thread");
        httpd_shutdown();
    }
    exit:
    return err;
}

int AppHttpdStart(void) {
    OSStatus err = kNoErr;

    err = _AppHttpdStart();
    require_noerr(err, exit);

    if (is_handlers_registered == false) {
        AppHttpRegisterHandlers();
        is_handlers_registered = true;
    }

    exit:
    return err;
}

int AppHttpdStop() {
    OSStatus err = kNoErr;

    /* HTTPD and services */
    http_log("stopping down httpd");
    err = httpd_stop();
    require_noerr_action(err, exit, http_log("failed to halt httpd"));

    exit:
    return err;
}
