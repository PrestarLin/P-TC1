# P-TC1 待办清单

> 来源：`fix-report-v4.1.34.md` 三章复查（审查报告 Low/Medium 未修项 + 报告遗留 + 挂起决策）
> 基线：v4.1.49（b279589），更新于 2026-10-05
> 已销账：M16、M18、L2、L7、L10、未修项#1、#5（见 fix-report §3.1/3.2）

## 🟠 中危 — 审查 Medium 未修（6 项）

- [ ] **M17 OTA 分发服务器安全**（`ota-server/server.py`）——最优先
  - `:12/:33` `WEBHOOK_SECRET` 默认空 → `verify_webhook` 恒通过，任何人可 POST 伪造 release webhook 推恶意固件（无签名兜底）
  - `:16-30` `branch` 直接拼路径，`/version?branch=../../..` 路径穿越读
  - 附带：单线程 `HTTPServer` 慢客户端卡全部；`download_firmware` 无大小/魔数校验
- [ ] **M9 WiFi 扫描 `wifi_ret` UAF/double-free**（`app_httpd.c:666-674` + `user_wifi.c:146-148`）
  - HTTP 线程 `send_http`/`free` 与 WiFi 线程 `free+wifi_ret` 并发，无锁无快照；UI 轮询扫描可触发
- [ ] **M15 `mqtt_report_freq` 无范围校验**（`app_httpd.c:841`）
  - 负值 → `mico_thread_msleep(1000*freq)` uint32 回绕 ≈49.7 天，功率上报停摆；应 clamp 到合理区间
- [ ] **M19 LED blink timer 自毁 + 跨线程竞态**（`user_gpio.c:369/380-387`）
  - 回调内 `mico_deinit_timer`（UAF 风险）；`StartLedBlink` 定时器线程/主线程并发，`timer_initialized` 无同步
- [ ] **M13 `GetTaskStr` 空列表 1 字节堆越界**（`timed_task.c:402-411`）
  - `task_count==0` → `malloc(2)` 却写 `tmp_str[2]='\0'`；每秒主循环高频触发（大概率无害但确定越界）
- [ ] **M14 `registerMqttEvents` 未初始化 timer**（`user_mqtt_client.c:245-251`）
  - AP 配网模式下 Web 改插座名/设备名 → `mico_start_timer` 未 `init` 的 `timer_handle`（UB）；应加初始化标志或线程存活检查

## 🟢 低危 — 审查 Low 未修（9 项）

- [ ] **L5 MQTT/OTA 等 6 处 `buf_size=97` 装不下满配 101 字节**（`app_httpd.c:812/835/853/1012/1026/1051`）——满配写入 500 静默失败，用户感知较强
- [ ] **L4 插座名空格截断**（`app_httpd.c:250` `%63s` + `index.html:1242` 空格分隔协议）——"Living Room" 存成 "Living"；修需改协议（如 `%d|` 分隔或先 `%d` 再取剩余）顺带修 L3 的 index 未初始化
- [ ] **L3 sscanf 返回值/未初始化变量类**——`app_httpd.c:526` `enableLock`、`:251/:276` `index`、`user_gpio.c:148` `tmp[6]`、`HttpAddTask` weekday/loop_end、`HttpSetNightMode` 时分范围
- [ ] **L8 单引号 JSON 破坏 + `innerHTML` XSS**（`index.html:562` `p()`、`:930`）——名字含 `'`/`\`/`,` 使整个 status 解析失败；建议改 `esc()` 转义 + `textContent`
- [ ] **L1 `WebLog()` malloc 不判空**（`web_log.c:63-67`）——一行 `if (!buff) return;`
- [ ] **L6 `uint8_t key_time` 25.6s 回绕**（`user_gpio.c:410`）+ `:354` 全局死变量
- [ ] **L11 `HttpGetPowerInfo` 裸 GET 5s 阻塞**（`app_httpd.c:539-546`，idx 已初始化）——前端已用 POST 规避，可选
- [ ] **L13 `GetButtonClickConfig` `len += snprintf` 无符号比较模式**（`user_gpio.c:112-116`）——现行不可达，防御性改写
- [ ] **L12 死代码 `TC1/ota_server/ota_server.c`** ——删除或加"未编译副本"注释，防误导审计

## 报告遗留（fix-report §3 未修项）

- [ ] **#2 SDK 内部 `mico_system_context_update` 未加锁**（config_server/easylink/para_storage）——框架代码；低概率偶发丢配置，若要修需评估 SDK 改动面
- [ ] **#3 MQTT 配置保存立即生效**（`app_httpd.c:821`）——`HttpSetMqttConfig` 改为先 `UserMqttDeInit()` 置 exit、再 `UserMqttInit()` 等重建（N3 基础设施已具备，一行级改动）；用户感知价值最高
- [ ] **#6 `get_func_name` 静态缓冲竞态**（`user_gpio.c:34`）——调用方持锁或改用调用者缓冲

## ⚖️ 挂起决策（需你拍板）

- [ ] **分批提交重写**——`3fd01fd`（11 项修复单提交）是否重写为 N/H/M 分批历史；需 force-push + 作废重建 v4.1.34 Release
- [ ] **源码 `VERSION_STRING` 同步**——`TC1/main.h` 仍 `"v4.1.3"`（CI 构建时 sed 覆盖，产物正确）；可一次性同步为当前版本或改 workflow 让 bump bot 一并回写

## 建议顺序

1. **#3 MQTT 配置立即生效**（几行改动，用户可感知）
2. **M17 server.py**（安全面：未认证 webhook 可推全量设备）
3. L5 + L4/L3（HTTP 输入健壮性打包修）
4. 其余按闲余时间
