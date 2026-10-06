/* 极简 telnet 控制台: 独立线程监听 23 端口, 不依赖 httpd 与 MQTT。
 * 作用: 网页后台卡死(甚至 MQTT 不可用)时, 局域网内仍可用命令行控制设备、
 * 查看日志、在线升级救砖, 是比 curl 更直观的一道保底通道。
 * 无鉴权(与 web 后台同级别), 不要把设备暴露到公网。 */
#include "http_server/web_log.h"
#include "mico.h"
#include "main.h"
#include "user_gpio.h"
#include "user_wifi.h"
#include "mqtt_server/user_mqtt_client.h"
#include "ota_server/user_ota.h"
#include "telnet_server/user_telnet.h"

#define TELNET_PORT 23
#define TELNET_LINE_MAX 256
#define TELNET_IDLE_TIMEOUT_SEC (10 * 60) /* 无输入则断开, 让位给下一个客户端 */

/* telnet 协商(IAC)字节: 所有选项一律拒绝, 避免客户端反复协商 */
#define T_IAC  0xFF
#define T_WILL 0xFB
#define T_DONT 0xFE
#define T_DO   0xFD
#define T_WONT 0xFC

static void telnet_send_all(int sock, const char *data, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) break; /* 对端已断开: 由 recv 循环统一收尾 */
        sent += n;
    }
}

static void telnet_reply(int sock, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    telnet_send_all(sock, buf, strlen(buf));
    telnet_send_all(sock, "\r\n", 2);
}

static const char telnet_help[] =
    "help / ?                     显示本帮助\r\n"
    "version                      固件版本\r\n"
    "status                       设备状态\r\n"
    "log                          最近运行日志\r\n"
    "set socket <0-5> <0|1>       单路插座 0关1开\r\n"
    "set total_socket <0|1>       全部插座\r\n"
    "set led <0|1>                电源指示灯\r\n"
    "set childLock <0|1>          童锁\r\n"
    "ota <url>                    在线升级固件(设备自行下载校验后重启)\r\n"
    "reboot                       重启设备\r\n";

static void telnet_process(int sock, char *line)
{
    int i = 0, on = 0;

    if (line[0] == '\0') return;

    if (!strcmp(line, "help") || !strcmp(line, "?")) {
        telnet_send_all(sock, telnet_help, sizeof(telnet_help) - 1);
    } else if (!strcmp(line, "version")) {
        telnet_reply(sock, "version: " VERSION);
    } else if (!strcmp(line, "status")) {
        telnet_reply(sock,
            "version : " VERSION "\r\n"
            "wifi    : %s ip=%s rssi=%d\r\n"
            "mqtt    : %s\r\n"
            "sockets : %s (0=off,1=on)\r\n"
            "childLock=%d led=%d\r\n"
            "ota     : %d (-2 idle, 0-99 running, 100 ok)\r\n"
            "free    : %d bytes\r\n"
            "device  : %s",
            ip_status.mode == 0 ? "AP" : "Station", ip_status.ip, RssiGet(),
            UserMqttIsConnect() ? "connected" : "disconnected",
            GetSocketStatus(), childLockEnabled, user_config->power_led_enabled,
            ota_progress, (int) MicoGetMemoryInfo()->free_memory,
            sys_config->micoSystemConfig.name);
    } else if (!strcmp(line, "log")) {
        char *logs = GetLogRecord(0);
        telnet_send_all(sock, logs, strlen(logs));
    } else if (!strcmp(line, "reboot")) {
        telnet_reply(sock, "rebooting...");
        mico_rtos_thread_msleep(200); /* 等回复发出再重启 */
        MicoSystemReboot();
    } else if (sscanf(line, "set socket %d %d", &i, &on) == 2) {
        if (i < 0 || i >= SOCKET_NUM || (on != 0 && on != 1)) {
            telnet_reply(sock, "ERR: 参数范围 socket 0-%d, on 0|1", SOCKET_NUM - 1);
            return;
        }
        UserRelaySet((unsigned char) i, (char) on);
        UserMqttSendSocketState((char) i);
        UserMqttSendTotalSocketState();
        AppContextUpdate(sys_config);
        telnet_reply(sock, "OK socket %d -> %d", i, on);
    } else if (sscanf(line, "set total_socket %d", &on) == 1) {
        if (on != 0 && on != 1) {
            telnet_reply(sock, "ERR: 参数范围 on 0|1");
            return;
        }
        UserRelaySetAll((char) on);
        for (i = 0; i < SOCKET_NUM; i++) {
            UserMqttSendSocketState((char) i);
        }
        UserMqttSendTotalSocketState();
        AppContextUpdate(sys_config);
        telnet_reply(sock, "OK all sockets -> %d", on);
    } else if (sscanf(line, "set led %d", &on) == 1) {
        if (on != 0 && on != 1) {
            telnet_reply(sock, "ERR: 参数范围 on 0|1");
            return;
        }
        user_config->power_led_enabled = (char) on;
        if (RelayOut() && user_config->power_led_enabled) {
            UserLedSet(1);
        } else {
            UserLedSet(0);
        }
        UserMqttSendLedState();
        AppContextUpdate(sys_config);
        telnet_reply(sock, "OK led -> %d", on);
    } else if (sscanf(line, "set childLock %d", &on) == 1) {
        if (on != 0 && on != 1) {
            telnet_reply(sock, "ERR: 参数范围 on 0|1");
            return;
        }
        user_config->child_lock = (char) on;
        childLockEnabled = on;
        UserMqttSendChildLockState();
        AppContextUpdate(sys_config);
        telnet_reply(sock, "OK childLock -> %d", on);
    } else if (strncmp(line, "ota ", 4) == 0) {
        char *url = line + 4;
        if (!strstr(url, "://")) {
            telnet_reply(sock, "ERR: url 需以 http:// 或 https:// 开头");
            return;
        }
        if (ota_progress >= 0 && ota_progress < 100) {
            telnet_reply(sock, "ERR: OTA 正在进行中(%d)", ota_progress);
            return;
        }
        telnet_reply(sock, "OTA start: %s", url);
        UserOtaStart(url, NULL);
    } else {
        telnet_reply(sock, "未知命令, 输入 help 查看");
    }
}

static void telnet_serve(int sock)
{
    static const char banner[] = "\r\nP-TC1 telnet console\r\n输入 help 查看命令\r\ntc1> ";
    char buf[128];
    char line[TELNET_LINE_MAX];
    int len = 0, iac = 0, n, i;
    unsigned char iac_cmd = 0;
    fd_set readfds;
    struct timeval tv;

    telnet_send_all(sock, banner, sizeof(banner) - 1);

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        tv.tv_sec = TELNET_IDLE_TIMEOUT_SEC;
        tv.tv_usec = 0;
        if (select(sock + 1, &readfds, NULL, NULL, &tv) <= 0) break; /* 超时/错误 */

        n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;

        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char) buf[i];

            if (iac == 1) {            /* IAC <cmd> */
                iac_cmd = c;
                iac = 2;
                continue;
            }
            if (iac == 2) {            /* IAC <cmd> <opt>: 一律拒绝(WILL->DONT, DO->WONT) */
                if (iac_cmd == T_WILL) {
                    unsigned char r[3] = { T_IAC, T_DONT, c };
                    telnet_send_all(sock, (char *) r, 3);
                } else if (iac_cmd == T_DO) {
                    unsigned char r[3] = { T_IAC, T_WONT, c };
                    telnet_send_all(sock, (char *) r, 3);
                }
                iac = 0;
                continue;
            }
            if (c == T_IAC) { iac = 1; continue; }

            if (c == '\r') continue;
            if (c == '\n') {
                telnet_send_all(sock, "\r\n", 2);
                line[len] = '\0';
                telnet_process(sock, line);
                len = 0;
                telnet_send_all(sock, "tc1> ", 5);
                continue;
            }
            if (c == 0x08 || c == 0x7F) { /* 退格 */
                if (len > 0) {
                    len--;
                    telnet_send_all(sock, "\b \b", 3);
                }
                continue;
            }
            if (c < 0x20) continue;
            if (len < TELNET_LINE_MAX - 1) {
                char ch = (char) c;
                line[len++] = ch;
                telnet_send_all(sock, &ch, 1); /* 回显 */
            }
        }
    }
}

static void telnet_thread(mico_thread_arg_t arg)
{
    int listen_sock = -1, client = -1;
    int one = 1;
    struct sockaddr_in addr;
    struct sockaddr_in from;
    socklen_t from_len;

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        tc1_log("ERROR: telnet socket create failed");
        goto exit;
    }
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(TELNET_PORT);
    if (bind(listen_sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        tc1_log("ERROR: telnet bind port %d failed", TELNET_PORT);
        goto exit;
    }
    if (listen(listen_sock, 1) < 0) {
        tc1_log("ERROR: telnet listen failed");
        goto exit;
    }

    tc1_log("telnet console listening on port %d", TELNET_PORT);

    while (1) {
        from_len = sizeof(from);
        client = accept(listen_sock, (struct sockaddr *) &from, &from_len);
        if (client < 0) {
            mico_rtos_thread_sleep(1); /* 异常时避免忙等 */
            continue;
        }
        tc1_log("telnet: client connected");
        telnet_serve(client);
        close(client);
        client = -1;
        tc1_log("telnet: client disconnected");
    }

    exit:
    if (client >= 0) close(client);
    if (listen_sock >= 0) close(listen_sock);
    mico_rtos_delete_thread(NULL);
}

void UserTelnetStart(void)
{
    OSStatus err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "telnet",
                                           (mico_thread_function_t) telnet_thread, 0x1000, 0);
    if (err != kNoErr) tc1_log("ERROR: telnet thread create failed err=%d", err);
}
