# P-TC1 待办清单

> 来源：`fix-report-v4.1.34.md` 三章复查（审查报告 Low/Medium 未修项 + 报告遗留）
> 基线：v4.1.49（b279589），更新于 2026-10-06
> 已销账：M16、M18、L2、L7、L10、未修项#1、#5（见 fix-report §3.1/3.2）
> 本轮已修并**仅在本地 dev 分支提交（未 push，CI 未跑）**：
> 中危 `4956e99`｜低危 `476c8f6`｜潜在风险+遗留#2 `aaadb32`｜夜间模式即时生效 `7b4cf66`｜遗留#6 `d690e5f`｜遗留#3 `5061886`

## 🟠 中危 — 审查 Medium 未修（6 项）

- [x] **M17 OTA 分发服务器安全**（`ota-server/server.py`）——最优先
  - `:12/:33` `WEBHOOK_SECRET` 默认空 → `verify_webhook` 恒通过，任何人可 POST 伪造 release webhook 推恶意固件（无签名兜底）
  - `:16-30` `branch` 直接拼路径，`/version?branch=../../..` 路径穿越读
  - 附带：单线程 `HTTPServer` 慢客户端卡全部；`download_firmware` 无大小/魔数校验
- [x] **M9 WiFi 扫描 `wifi_ret` UAF/double-free**（`app_httpd.c:666-674` + `user_wifi.c:146-148`）
  - HTTP 线程 `send_http`/`free` 与 WiFi 线程 `free+wifi_ret` 并发，无锁无快照；UI 轮询扫描可触发
- [x] **M15 `mqtt_report_freq` 无范围校验**（`app_httpd.c:841`）
  - 负值 → `mico_thread_msleep(1000*freq)` uint32 回绕 ≈49.7 天，功率上报停摆；应 clamp 到合理区间
- [x] **M19 LED blink timer 自毁 + 跨线程竞态**（`user_gpio.c:369/380-387`）
  - 回调内 `mico_deinit_timer`（UAF 风险）；`StartLedBlink` 定时器线程/主线程并发，`timer_initialized` 无同步
- [x] **M13 `GetTaskStr` 空列表 1 字节堆越界**（`timed_task.c:402-411`）
  - `task_count==0` → `malloc(2)` 却写 `tmp_str[2]='\0'`；每秒主循环高频触发（大概率无害但确定越界）
- [x] **M14 `registerMqttEvents` 未初始化 timer**（`user_mqtt_client.c:245-251`）
  - AP 配网模式下 Web 改插座名/设备名 → `mico_start_timer` 未 `init` 的 `timer_handle`（UB）；应加初始化标志或线程存活检查

## 🟢 低危 — 审查 Low 未修（9 项）

- [x] **L5 MQTT/OTA 等 6 处 `buf_size=97` 装不下满配 101 字节**（`app_httpd.c:812/835/853/1012/1026/1051`）——满配写入 500 静默失败，用户感知较强
- [x] **L4 插座名空格截断**（`app_httpd.c:250` `%63s` + `index.html:1242` 空格分隔协议）——"Living Room" 存成 "Living"；修需改协议（如 `%d|` 分隔或先 `%d` 再取剩余）顺带修 L3 的 index 未初始化
- [x] **L3 sscanf 返回值/未初始化变量类**——`app_httpd.c:526` `enableLock`、`:251/:276` `index`、`user_gpio.c:148` `tmp[6]`、`HttpSetNightMode` 时分范围（`HttpAddTask` 的 weekday/loop_end 已由 `:940` `memset(task,0,sizeof(...))` 保证，未改）
- [x] **L8 单引号 JSON 破坏 + `innerHTML` XSS**（`index.html:562` `p()`、`:930`）——名字含 `'`/`\`/`,` 使整个 status 解析失败；建议改 `esc()` 转义 + `textContent`
- [x] **L1 `WebLog()` malloc 不判空**（`web_log.c:63-67`）——一行 `if (!buff) return;`
- [x] **L6 `uint8_t key_time` 25.6s 回绕**（`user_gpio.c:410`）+ `:354` 全局死变量
- [x] **L11 `HttpGetPowerInfo` 裸 GET 5s 阻塞**（`app_httpd.c:539-546`，idx 已初始化）——前端已用 POST 规避，可选
- [x] **L13 `GetButtonClickConfig` `len += snprintf` 无符号比较模式**（`user_gpio.c:112-116`）——现行不可达，防御性改写
- [x] **L12 死代码 `TC1/ota_server/ota_server.c`** ——该文件已不在仓库中（`git ls-files` 确认）；改为加固遗留的 `ota_server/server.py`：`self.path` 直接拼磁盘路径，加 realpath + 前缀校验，`/../../etc/passwd` 与编码变体均 404

## ⚠️ 潜在风险（本次排查新发现）

- [x] **双分区 CRC「算一次、数据写两次」竞态**（`mico-os/MiCO/system/mico_system_para_storage.c:126-168`）
  - `internal_update_config` 先算 CRC(t0)，随后从**活动 RAM** 分别写 P1(t1)/P2(t2)，窗口≈1-2s（两次扇区擦除）
  - 窗口内任何无锁 RAM 写（`WifiStatusCallback` 写 `reserved`、`recordDailyPCount` 写 p_count、插座状态、SDK `power_daemon.c:125`/`system_misc.c:133` 无锁 `context_update` = N3 家族）→ **两个分区 data≠CRC 同时持久化**
  - 后果：下次开机 `MICOReadConfiguration:281-283` 双分区校验失败 → 恢复出厂（mqtt/任务/名称全清 + 开热点）——低概率、偶发
  - 修法：先 memcpy 快照到临时缓冲，基于快照算 CRC + 写盘；并把互斥下沉进 `mico_system_context_update` 覆盖 SDK 调用点
  - 背景：2026-10-05 排查「静态IP后MQTT丢失」时排除的机制（实际根因是 DHCP_COMPLETED 不派发，已修 `083590b`），但竞态本身仍存在

## 报告遗留（fix-report §3 未修项）

- [x] **#2 SDK 内部 `mico_system_context_update` 未加锁**（config_server/easylink/para_storage）——框架代码；低概率偶发丢配置，已修：把 `para_update_mutex` 下沉进 `internal_update_config`，SDK 各无锁 `context_update` 调用点全部经过它，因此无需逐处加锁
- [x] **#3 MQTT 配置保存立即生效**（`app_httpd.c:821`）——`HttpSetMqttConfig` 改为先 `UserMqttDeInit()` 置 exit、再 `UserMqttInit()` 等重建（N3 基础设施已具备，一行级改动）；用户感知价值最高
- [x] **#6 `get_func_name` 静态缓冲竞态**（`user_gpio.c:34`）——调用方持锁或改用调用者缓冲

## 剩余

- 中危/低危/潜在风险/报告遗留全部清零，无待办项。
- 本轮改动**未在本机编译**（AGENTS.md 约定），验证依赖 push 后 GitHub Actions 构建产物 + 实机回归：MQTT 满配保存、改配置免重启生效、定时任务日志、夜间模式、断电后配置完整性。
