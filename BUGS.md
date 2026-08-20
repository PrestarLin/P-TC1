# P-TC1 未修复 Bug 清单

经过全面代码审计，按优先级分级列出。每条均已核实。

---

## 🔴 Critical（崩溃 / 内存破坏）

### C1 — WifiConnect `strcpy` 栈溢出
- **文件**: `TC1/user_wifi.c:148`
- **问题**: `strcpy((char*)wNetConfig.wifi_key, wifi_key)`。HTTP 传入的 `wifi_key` 最长 127 字节（`HttpSetWifiConfig:%127s`），`wNetConfig.wifi_key` 仅 64 字节 → 栈溢出 ~63 字节
- **影响**: 局域网攻击者可通过 `/wifi/config` POST 控制栈、RCE 或崩溃

### C2 — WifiConnect 配置区写入越界
- **文件**: `TC1/user_wifi.c:154-155`
- **问题**: `strcpy(sys_config->micoSystemConfig.ssid, wifi_ssid)` 写入 `ssid[32]`；`strcpy(sys_config->micoSystemConfig.user_key, wifi_key)` 写入 `user_key[64]` → 越界覆盖相邻配置

### C3 — WifiScanCallback 堆溢出 + 越界读
- **文件**: `TC1/user_wifi.c:73-102`
- **问题**: `ssids=malloc(count*32)` 但每个 SSID 写入 `'%s',`(最多 34 字节) → 堆溢出；`tmp1 += strlen+3` 比实际写入少 1 字节 → 指针错位；`count==0` 时日志读 `ApList[0]`（越界）；全部过滤后缓冲区未终止拼 JSON

### C4 — MQTT 接收 payload 堆溢出
- **文件**: `TC1/mqtt_server/user_mqtt_client.c:395-399`
- **问题**: `memcpy(p_recv_msg->data[2048], message->payload, payloadlen)` 无上限 → broker 发 >2048 字节即堆溢出；恰好 2048 时无 NUL，`%s` 越读

### C5 — 定时任务链表无锁并发
- **文件**: `TC1/timed_task.c` 全文 + `TC1/main.c:208` + `TC1/http_server/app_httpd.c:630-677`
- **问题**: `task_top` 链表由主循环线程（`ProcessTask`/`DelFirstTask`）与 HTTP 线程（增删改查）并发访问，无任何互斥锁 → 链表损坏、空指针、硬崩溃

---

## 🟠 Major（功能失效 / 资源泄漏 / 数据损坏）

### M1 — MQTT 命令解析死代码，HA 无法控制
- **文件**: `TC1/mqtt_server/user_mqtt_client.c:435-475`
- **问题**: `ProcessHaCmd` 用 `strcmp(cmd, "set socket") == 0`，但 HA 通过 `cmd_t=device/ztc1/set` 下发的是完整串 `"set socket <MAC> <n> <on>"` → 五条命令分支全匹配失败，MQTT 控制完全失效

### M2 — MQTT sscanf 无宽度 + 索引越界（修 M1 后触发）
- **文件**: `TC1/mqtt_server/user_mqtt_client.c:437`
- **问题**: `%s` 写入 `mac[20]` 无宽度；`i` 未校验 `SOCKET_NUM` → 修好 M1 后立即越界读 `socket_status[]`

### M3 — `user[0]` 双重用途：童锁 vs 按钮 0 配置
- **文件**: `TC1/http_server/app_httpd.c:383` / `TC1/user_gpio.c:69,220` / `TC1/main.c:160` / `TC1/timed_task/timed_task.c:180`
- **问题**: `user_config->user[0]` 同时存储童锁状态和按钮 0 功能编码（`set_key_map` 允许 index=0）。设童锁毁按钮 0 配置，反之亦然

### M4 — 定时任务不持久化 + 裸指针跨重启
- **文件**: `TC1/timed_task/timed_task.c` / `TC1/main.h` / `TC1/main.c:208`
- **问题**: 增删从不调用 `mico_system_context_update` → 重启后所有定时任务丢失；`task_top` 裸指针被直接写进 flash，重启后未校验直接解引用 → 布局变化即崩溃

### M5 — `web_log` 宏全局变量多线程竞态
- **文件**: `TC1/http_server/web_log.h:23-29`
- **问题**: 宏用全局 `LOG_TMP`/`LOG_NOW`，多线程并发调用 → 指针互相覆盖（泄漏）、写坏他人缓冲区；malloc 未判空

### M6 — HttpSetWifiConfig 内存泄漏 + 空指针
- **文件**: `TC1/http_server/app_httpd.c:480-510`
- **问题**: `ssid_enc`/`key_enc` 从未 free（每次 POST 泄漏 256B）；任一 malloc 失败时 sscanf/url_decode 写 NULL → 崩溃

### M7 — OTA 断点续传 HTTP 206 不兼容
- **文件**: `mico-os/libraries/daemons/ota_server/ota_server.c:316-320`
- **问题**: 续传后要求 `statusCode == 200`，但 Range 请求返回 206 → 即使下载完成也被判失败丢弃；首次微小抖动即整包失败

### M8 — OTA url_parse 返回 NULL 未检查
- **文件**: `mico-os/libraries/daemons/ota_server/ota_server.c:400-401`
- **问题**: `require_action(url, …)` 应为 `require_action(url_t, …)` — 畸形 URL 时 `url_parse` 返回 NULL → 空指针崩溃

### M9 — OTA 可重入 use-after-free
- **文件**: `mico-os/libraries/daemons/ota_server/ota_server.c:435-446`
- **问题**: `ota_server_start` 直接 free 前一线程的全局 context，但前一线程未停止/等待 → 旧线程操作已释放内存

### M10 — 功率中断 ISR 空转
- **文件**: `TC1/user_power.c:55-64`
- **问题**: `n=(spend_ns-past_ns%NS)/NS` 在脉冲稀疏时可达数千 → ISR 空转多秒阻塞所有中断，并写入大量假数据

### M11 — strncpy 不保证 NUL 终止
- **文件**: `TC1/user_wifi.c:179-180`
- **问题**: `strncpy(ap_name/ap_key, x, 32)` 输入≥32 时不加 NUL → 后续 `%s` 越读

---

## 🟡 Minor

| # | 位置 | 问题 |
|---|------|------|
| m1 | `TC1/main.c:115`、`TC1/user_gpio.c:233,235` | 缺 include 导致隐式声名（CI 已报警告） |
| m2 | `TC1/http_server/app_httpd.c:638-653` | `HttpAddTask`：sscanf 失败时用旧残留值校验；`NewTask` 已置 `on_use=true` → 槽位永久占用 |
| m3 | `TC1/http_server/app_httpd.c:669-672` | `HttpDelTask`：sscanf 失败 `time1` 未初始化 → 随机删任务 |
| m4 | `TC1/timed_task/timed_task.c:211` | 空列表时 `GetTaskStr` 返回垃圾 `[X]` |
| m5 | `TC1/ota_server/user_ota.c:8,16` / `app_httpd.c:744` | `ota_progress` float 跨线程无同步 |
| m6 | `TC1/mqtt_server/user_mqtt_client.c:398,505` | `strncpy` 不保证 NUL |
| m7 | `TC1/timed_task/timed_task.c:109-143` | `DelTask` 仅按 `prs_time` 精确匹配 |
| m8 | `mico-os/libraries/daemons/ota_server/ota_server.c:224` | 进度回调 `download_len==0` 除零 |
| m9 | `TC1/ota_server/ota_server.h:35` | `OTA_USE_HTTPS=1` 与应用实际编译不符（死代码副本） |
| m10 | `TC1/main.h:31` | `VERSION "v2.2.0"` 与 `.version`(v1.0.80) 不一致 |
| m11 | `mico-os/makefiles/scripts/map_parse_gcc.py` | 正则转义符 Py3 SyntaxWarning |

---

## 修复批次

| 批 | 内容 | 涉及文件 |
|----|------|----------|
| 1 | 编译警告：补 missing include | `main.c`、`user_gpio.c` |
| 2 | 内存安全：C1-C4、M6、M11 | `user_wifi.c`、`user_mqtt_client.c`、`app_httpd.c` |
| 3 | 定时任务：C5、M4、m2-m4、m7 | `timed_task.c`、`main.c`、`app_httpd.c` |
| 4 | MQTT 控制：M1、M2、m6 | `user_mqtt_client.c` |
| 5 | web_log 线程安全：M5 | `web_log.h`、`web_log.c` |
| 6 | 童锁独立字段：M3 | `main.h`、`app_httpd.c`、`user_gpio.c`、`timed_task.c`、`main.c` |
| 7 | OTA 含 SDK：M7-M9、m8 | `mico-os/.../ota_server.c` |
| 8 | 中断与杂项：M10、m1、m5、m9-m11 | `user_power.c`、`ota_server/ota_server.h`、`main.h`、`map_parse_gcc.py` |
| 9 | 失联回归 + 版本号 + 存量修复 | `main.c`（初始化顺序）、`main.h`（VERSION/COMMIT_HASH 字符串化）、`timed_task.c`（GetTaskStr 堆溢出 89→128）、`app_httpd.c`（HttpAddTask 持久化）、`build.yml`（dev tag 加 run_number） |

## 批9 说明

- **失联回归根因**：`LogMutexInit()` 晚于首个 `tc1_log`（`SetLogRecord` 在未初始化互斥锁上 `mico_rtos_lock_mutex`）→ 挂死。已把 `TaskModuleInit/LogMutexInit` 移到 `mico_system_init` 之后、首个日志之前；`RebuildTaskList/childLockEnabled` 移到版本检查/恢复之后。
- **COMMIT_HASH 字符串化**：`-DCOMMIT_HASH=<hex>` 是裸 token，直接拼接 `"v3.0.0-" COMMIT_HASH` 会编译错误；改双字符串化 `STR(COMMIT_HASH)` + 回退裸 token `local`。
- **GetTaskStr 堆溢出**（存量）：每条任务 sprintf 最长 ~113 字节，按 89/条分配会越界写；改 128/条。
- **HttpAddTask 不持久化**（存量）：新增任务从不写 flash，重启即丢失（RebuildTaskList 暴露）；成功路径补 `mico_system_context_update`。
- **CI dev tag 重复**：每 push 生成相同 `v3.0.1-dev` → 第二次 release 422 失败；dev tag 追加 `github.run_number`。