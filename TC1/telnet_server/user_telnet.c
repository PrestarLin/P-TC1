/* Minimal telnet console: a dedicated thread listens on port 23 and does not
 * depend on httpd or MQTT. When the web UI is stuck (or MQTT is down) it still
 * allows command-line control, log inspection and rescue OTA over the LAN.
 * No authentication (same level as the web UI): never expose it to the Internet. */
#include <stdarg.h>
#include "http_server/web_log.h"
#include "mico.h"
#include "main.h"
#include "user_gpio.h"
#include "user_wifi.h"
#include "mqtt_server/user_mqtt_client.h"
#include "ota_server/user_ota.h"
#include "telnet_server/user_telnet.h"

/* httpd 内部诊断输出 (mico-os/libraries/daemons/http_server/httpd.c) */
extern char *httpd_debug_info(void);
/* 配置落盘因 malloc 失败被跳过的累计次数 (mico_system_para_storage.c) */
extern uint32_t mico_para_update_skip_count(void);

#define TELNET_PORT 23
#define TELNET_LINE_MAX 256
/* 每个终端最多空闲 5 分钟(可被下一位使用者连接), 不会再有单个掉线终端长期占满控制台 */
#define TELNET_IDLE_TIMEOUT_SEC (5 * 60)
/* 最多同时服务 3 个终端; 单线程 select 多路复用, 任一终端掉线不影响其他人 */
#define TELNET_MAX_CLIENTS 3

/* telnet negotiation (IAC) bytes */
#define T_IAC  0xFF
#define T_WILL 0xFB
#define T_DONT 0xFE
#define T_DO   0xFD
#define T_WONT 0xFC
#define T_ECHO 1

/* The server echoes every keystroke, so offer "I WILL ECHO" at session start:
 * well-behaved clients then stop their local echo and characters show up once.
 * All other options are refused so clients do not keep re-negotiating. */
static const unsigned char telnet_will_echo[3] = { T_IAC, T_WILL, T_ECHO };

static void telnet_send_all(int sock, const char *data, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) break; /* peer closed: the recv loop will finish the session */
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
    "help / ?                     show this help\r\n"
    "version                      firmware version\r\n"
    "status                       device status\r\n"
    "log                          recent logs\r\n"
    "httpd                        web server internal state (diagnostics)\r\n"
    "set socket <0-5> <0|1>       single socket, 0=off 1=on\r\n"
    "set total_socket <0|1>       all sockets\r\n"
    "set led <0|1>                power LED\r\n"
    "set childLock <0|1>          child lock\r\n"
    "set wifi <ssid> [key]        connect wifi station (ssid/key no spaces, saved to flash)\r\n"
    "ota <url>                    firmware OTA update (device downloads, verifies, reboots)\r\n"
    "reboot                       reboot device\r\n";

static void telnet_process(int sock, char *line)
{
    int i = 0, on = 0;
    char ssid[32], key[64];

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
            "para_skip: %u (落盘因内存不足被跳过, 应为 0)\r\n"
            "device  : %s",
            ip_status.mode == 0 ? "AP" : "Station", ip_status.ip, RssiGet(),
            UserMqttIsConnect() ? "connected" : "disconnected",
            GetSocketStatus(), childLockEnabled, user_config->power_led_enabled,
            ota_progress, (int) MicoGetMemoryInfo()->free_memory,
            (unsigned) mico_para_update_skip_count(),
            sys_config->micoSystemConfig.name);
    } else if (!strcmp(line, "log")) {
        char *logs = GetLogRecord(0);
        telnet_send_all(sock, logs, strlen(logs));
    } else if (!strcmp(line, "httpd")) {
        /* 相隔 1 秒采两次样: loop 计数是否增长、哪个 in_*=1,
         * 直接给出 httpd 线程卡死点或"活着但不服务"的证据 */
        char *info = httpd_debug_info();
        telnet_send_all(sock, info, strlen(info));
        mico_rtos_thread_msleep(1000);
        info = httpd_debug_info();
        telnet_send_all(sock, info, strlen(info));
    } else if (!strcmp(line, "reboot")) {
        telnet_reply(sock, "rebooting...");
        mico_rtos_thread_msleep(200); /* let the reply go out before rebooting */
        MicoSystemReboot();
    } else if (sscanf(line, "set socket %d %d", &i, &on) == 2) {
        if (i < 0 || i >= SOCKET_NUM || (on != 0 && on != 1)) {
            telnet_reply(sock, "ERR: usage: set socket <0-%d> <0|1>", SOCKET_NUM - 1);
            return;
        }
        UserRelaySet((unsigned char) i, (char) on);
        UserMqttSendSocketState((char) i);
        UserMqttSendTotalSocketState();
        AppContextUpdate(sys_config);
        telnet_reply(sock, "OK socket %d -> %d", i, on);
    } else if (sscanf(line, "set total_socket %d", &on) == 1) {
        if (on != 0 && on != 1) {
            telnet_reply(sock, "ERR: value must be 0 or 1");
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
            telnet_reply(sock, "ERR: value must be 0 or 1");
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
            telnet_reply(sock, "ERR: value must be 0 or 1");
            return;
        }
        user_config->child_lock = (char) on;
        childLockEnabled = on;
        UserMqttSendChildLockState();
        AppContextUpdate(sys_config);
        telnet_reply(sock, "OK childLock -> %d", on);
    } else if (!strncmp(line, "set wifi ", 9)) {
        /* 与网页 /wifi 同路径: WifiConnect 内部含 micoWlanStart + 保存 ssid/key 到 Flash */
        int n = sscanf(line, "set wifi %31s %63s", ssid, key);
        if (n < 1) {
            telnet_reply(sock, "ERR: usage: set wifi <ssid> [key]");
            return;
        }
        if (n == 1) key[0] = '\0'; /* 开放网络 */
        WifiConnect(ssid, key);
        telnet_reply(sock, "OK wifi connecting: %s (saved), IP may change", ssid);
    } else if (strncmp(line, "ota ", 4) == 0) {
        char *url = line + 4;
        if (!strstr(url, "://")) {
            telnet_reply(sock, "ERR: url must start with http:// or https://");
            return;
        }
        if (ota_progress >= 0 && ota_progress < 100) {
            telnet_reply(sock, "ERR: OTA already in progress (%d)", ota_progress);
            return;
        }
        telnet_reply(sock, "OTA start: %s", url);
        UserOtaStart(url, NULL);
    } else {
        telnet_reply(sock, "Unknown command, type 'help' for a list of commands");
    }
}

typedef struct {
    int sock;
    int len;
    int iac;
    unsigned char iac_cmd;
    time_t last_active;
    char line[TELNET_LINE_MAX];
} telnet_client_t;

static telnet_client_t telnet_clients[TELNET_MAX_CLIENTS];

static int telnet_client_slot(void)
{
    int i;
    for (i = 0; i < TELNET_MAX_CLIENTS; i++) {
        if (telnet_clients[i].sock < 0) return i;
    }
    return -1;
}

static void telnet_client_close(telnet_client_t *c)
{
    if (c->sock >= 0) close(c->sock);
    c->sock = -1;
    c->len = 0;
    c->iac = 0;
    tc1_log("telnet: client disconnected");
}

/* 处理一个终端的一个输入字节(IAC 协商+行编辑); 会话状态全部在 c 内 */
static void telnet_feed(telnet_client_t *c, unsigned char ch)
{
    if (c->iac == 1) {             /* IAC <cmd> */
        c->iac_cmd = ch;
        c->iac = 2;
        return;
    }
    if (c->iac == 2) {             /* IAC <cmd> <opt> */
        unsigned char r[3];
        if (c->iac_cmd == T_DO && ch == T_ECHO) {
            r[0] = T_IAC; r[1] = T_WILL; r[2] = T_ECHO; /* client asks us to echo: accept */
            telnet_send_all(c->sock, (char *) r, 3);
        } else if (c->iac_cmd == T_WILL && ch == T_ECHO) {
            r[0] = T_IAC; r[1] = T_DONT; r[2] = T_ECHO; /* client offers to echo: refuse, server echoes */
            telnet_send_all(c->sock, (char *) r, 3);
        } else if (c->iac_cmd == T_WILL) {
            r[0] = T_IAC; r[1] = T_DONT; r[2] = ch; /* refuse all other options */
            telnet_send_all(c->sock, (char *) r, 3);
        } else if (c->iac_cmd == T_DO) {
            r[0] = T_IAC; r[1] = T_WONT; r[2] = ch;
            telnet_send_all(c->sock, (char *) r, 3);
        }
        c->iac = 0;
        return;
    }
    if (ch == T_IAC) { c->iac = 1; return; }

    if (ch == '\r') return;
    if (ch == '\n') {
        telnet_send_all(c->sock, "\r\n", 2);
        c->line[c->len] = '\0';
        telnet_process(c->sock, c->line);
        c->len = 0;
        telnet_send_all(c->sock, "tc1> ", 5);
        return;
    }
    if (ch == 0x08 || ch == 0x7F) { /* backspace */
        if (c->len > 0) {
            c->len--;
            telnet_send_all(c->sock, "\b \b", 3);
        }
        return;
    }
    if (ch < 0x20) return;
    if (c->len < TELNET_LINE_MAX - 1) {
        c->line[c->len++] = (char) ch;
        telnet_send_all(c->sock, (char *) &ch, 1); /* echo */
    }
}

static void telnet_client_start(int sock)
{
    static const char banner[] = "\r\nP-TC1 telnet console\r\nType 'help' for a list of commands\r\ntc1> ";
    int slot = telnet_client_slot();
    if (slot < 0) {
        /* 终端位已满: 明确拒绝并立即关闭, 不占 backlog 不放任挂起 */
        static const char busy[] = "\r\nP-TC1 telnet console busy, try again later\r\n";
        telnet_send_all(sock, busy, sizeof(busy) - 1);
        close(sock);
        return;
    }
    telnet_client_t *c = &telnet_clients[slot];
    c->sock = sock;
    c->len = 0;
    c->iac = 0;
    c->iac_cmd = 0;
    c->last_active = time(NULL);
    telnet_send_all(sock, (const char *) telnet_will_echo, sizeof(telnet_will_echo));
    telnet_send_all(sock, banner, sizeof(banner) - 1);
    tc1_log("telnet: client connected");
}

static void telnet_thread(mico_thread_arg_t arg)
{
    int listen_sock = -1, i;
    int one = 1;
    struct sockaddr_in addr;
    struct sockaddr_in from;
    socklen_t from_len;
    char buf[128];

    for (i = 0; i < TELNET_MAX_CLIENTS; i++) telnet_clients[i].sock = -1;

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
    if (listen(listen_sock, TELNET_MAX_CLIENTS + 1) < 0) {
        tc1_log("ERROR: telnet listen failed");
        goto exit;
    }

    /* 与 httpd 同理: select 报可读后排队连接可能已被撤销, 阻塞的 accept
     * 会永久卡死这个救援控制台线程; 非阻塞后 accept 无连接时立即返回 */
    fcntl(listen_sock, F_SETFL, O_NONBLOCK);

    tc1_log("telnet console listening on port %d", TELNET_PORT);

    while (1) {
        fd_set readfds;
        struct timeval tv;
        int maxfd = listen_sock;

        FD_ZERO(&readfds);
        FD_SET(listen_sock, &readfds);
        for (i = 0; i < TELNET_MAX_CLIENTS; i++) {
            if (telnet_clients[i].sock >= 0) {
                FD_SET(telnet_clients[i].sock, &readfds);
                if (telnet_clients[i].sock > maxfd) maxfd = telnet_clients[i].sock;
            }
        }

        tv.tv_sec = 1;
        tv.tv_usec = 0;
        select(maxfd + 1, &readfds, NULL, NULL, &tv);

        if (FD_ISSET(listen_sock, &readfds)) {
            from_len = sizeof(from);
            int client = accept(listen_sock, (struct sockaddr *) &from, &from_len);
            if (client >= 0) telnet_client_start(client);
        }

        for (i = 0; i < TELNET_MAX_CLIENTS; i++) {
            telnet_client_t *c = &telnet_clients[i];
            if (c->sock < 0 || !FD_ISSET(c->sock, &readfds)) continue;
            int n = recv(c->sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                telnet_client_close(c);
                continue;
            }
            c->last_active = time(NULL);
            int j;
            for (j = 0; j < n; j++) telnet_feed(c, (unsigned char) buf[j]);
        }

        /* 空闲终端自动断开, 释放终端位 */
        time_t now = time(NULL);
        for (i = 0; i < TELNET_MAX_CLIENTS; i++) {
            telnet_client_t *c = &telnet_clients[i];
            if (c->sock >= 0 && now > c->last_active && now - c->last_active > TELNET_IDLE_TIMEOUT_SEC) {
                static const char bye[] = "\r\nidle timeout, bye\r\n";
                telnet_send_all(c->sock, bye, sizeof(bye) - 1);
                telnet_client_close(c);
            }
        }
    }

    exit:
    for (i = 0; i < TELNET_MAX_CLIENTS; i++) {
        if (telnet_clients[i].sock >= 0) close(telnet_clients[i].sock);
    }
    if (listen_sock >= 0) close(listen_sock);
    mico_rtos_delete_thread(NULL);
}

void UserTelnetStart(void)
{
    OSStatus err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "telnet",
                                           (mico_thread_function_t) telnet_thread, 0x1000, 0);
    if (err != kNoErr) tc1_log("ERROR: telnet thread create failed err=%d", err);
}
