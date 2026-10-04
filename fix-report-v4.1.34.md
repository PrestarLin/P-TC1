# TC1 固件审查修复报告 — v4.1.34

- **审查范围**：全面代码审查（用户认可优先级 N2/N3/N1 → H4-H7 → M10/M11/M12 → H8，共 11 项）
- **修复提交**：`3fd01fd fix: 修复审查报告全部认可问题(N1-N3/H4-H8/M10-M12)`
- **改动规模**：11 个文件，+241 / −122 行
- **CI**：Actions run #155 **success**，Release **v4.1.34**（ota.bin 599980B / all.bin 1959768B）
- 本地未编译固件（按 AGENTS.md，交付产物一律走云构建）

---

## 一、修复明细

### N1 — `get_func_name` 栈外缓冲溢出
- **文件**：`TC1/user_gpio.c`
- **问题**：`static char buffer[32]` + `sprintf`，插座名最长 63 字节，`"Toggle Socket N " + 名字` 必溢出。
- **修复**：缓冲扩到 80，改用 `snprintf(..., sizeof(buffer), ...)`。

### N2 — 恢复出厂两条路径行为不一致
- **文件**：`TC1/main.c`、`TC1/main.h`、`TC1/http_server/app_httpd.c`
- **问题**：`HttpFactoryReset` 只 `memset + version + 插座名`，与 `appRestoreDefault_callback`（按键功能码、夜间模式默认值、上报周期、出厂插座状态等）不一致。
- **修复**：抽取共享函数 `SetFactoryUserDefaults(user_config_t*)`，两条路径共用；`HttpFactoryReset` 的 keep 位（WiFi/MQTT 凭据）恢复放在其后；函数只经参数访问配置，兼容 `mico_system_context_init` 早期回调（全局 `user_config` 尚为 NULL）。

### N3 — MQTT 线程不可重建（配置改完不生效 / DeInit 后永久死亡）
- **文件**：`TC1/mqtt_server/user_mqtt_client.c`
- **修复**：
  - 新增 `volatile bool mqtt_thread_running`；`UserMqttInit` 守卫三分支：运行中→直接返回；已退出→等待旧线程退出（≤70×100ms，其 select 最长 5s）→ `clear + deinit 旧队列` → 全量重建；超时→返回错误下次再试。
  - 线程在连接成功前的 DeInit 检查、重连循环尾部各加 `if (should_exit) goto exit`，不再进入假"连接成功"流程。
  - `exit:` 统一收尾：停 timer（防回调访问已回收队列）、删 event fd（防 fd 关联已销毁队列）、置 `running=false`。线程内原有的 flag 重置行删除（改由 Init 持有）；`running=true` 在 create 前置位，create 失败回退。
  - **终审追加**：`mico_rtos_create_worker_thread` 内部会 `memset` struct 并重建 queue，重 Init 重复调用会破坏常驻 worker 线程 → 加 `static bool mqtt_worker_created` 单例守卫。

### H4 — 上传 OTA 失败后进度卡 0，入口永久占死
- **文件**：`TC1/http_server/app_httpd.c` (`HttpSetOTAFile`)
- **修复**：`malloc` 失败、OTA 分区获取失败显式 `ota_progress=-2`；写 flash 重试耗尽由 `require_noerr_quiet` 改为 `break` 落入公共失败块；`mico_ota_switch_to_new_fw` 失败 `goto ota_failed`——公共失败块（清空被动分区、不切换不重启）带 `ota_failed:` 标签，三条失败路径统一，允许立即重试。

### H5 — 下载 OTA 服务器挂死时线程永久阻塞
- **文件**：`mico-os/libraries/daemons/ota_server/ota_server.c`
- **修复**：
  - `select` 由 NULL（无限）改为 10s 超时；超时经 fall-through 进入 `RECONNECTED` 计数。
  - `RECONNECTED` 处加断点续传看门狗：`download_begin_pos` 有进展即清零计数，连续 **8 次无进展**（约 80s 无数据）→ `OTA_FAIL` + `goto DELETE` 释放 context、退出线程——OTA 入口不再被占死。
  - HTTP 状态码非 200/206 → 记录日志、`OTA_FAIL`、立即 `DELETE`（原逻辑会继续尝试读 body）。

### H6 — 镜像校验形同虚设（无 md5 时恒通过）
- **文件**：`mico-os/.../ota_server.c`
- **问题**：不提供 md5 时 `memcmp(全0, 全0)` 恒真，直接 `switch_to_new_fw`。
- **修复**：`is_md5` 时才校 md5（失败记日志）；**md5 通过与否都必须 `OtaImageHeaderValid()`（MRVL+magic_sig 镜像头，定义于 `TC1/ota_server/user_ota.c`）** 才允许切换，否则 `OTA_FAIL`。

### H7 — OTA 启动失败后 context 泄漏，后续启动永远 kGeneralErr
- **文件**：`mico-os/.../ota_server.c`、`TC1/ota_server/user_ota.c`
- **修复**：
  - `ota_server_start` 加 `context_created` 标志：`err != kNoErr` 时仅当 **本次调用** 分配的 context 才释放（运行中任务、已有任务 `kGeneralErr` 路径、`url==NULL` 早退路径均不误删）。
  - `download_url.url` 原 `malloc(strlen(url))` 漏 NUL（`strcpy` 越界写堆）→ `malloc(strlen+1)` + 同步 memset。
  - `host` 由 `strcpy` 改 `strncpy(size-1)` + NUL。
  - `UserOtaStart` 检查 `ota_server_start` 返回值，失败置 `ota_progress=-2`（原先失败无回调，进度永远 0）。

### H8 — `mico_system_context_update` 并发踩踏（seed++/flash 写非线程安全）
- **文件**：`TC1/main.c`、`TC1/main.h` + 全部调用点
- **修复**：`AppContextUpdate()` 经 `context_update_mutex` 串行化；`AppContextUpdateInit()` 在 `application_start` 首行（`mico_system_context_init` 之前）初始化，锁未就绪时直通（早期回调兼容）。TC1 代码内 **51 处** `mico_system_context_update(sys_config)` 全部替换（main/app_httpd/user_gpio/user_wifi/user_mqtt_client/timed_task）。
- **未覆盖**：SDK 内部调用点（easylink、power_daemon 等），无法改动框架代码，见"未修项"。

### M10 — 周任务在北京 00:00–07:59 错位到前一天
- **文件**：`TC1/timed_task/timed_task.c`
- **问题**：设备 `localtime`=UTC，weekday 掩码与任务时刻却按北京时间语义存储，`FindNextMatchTime` 用 UTC 算"今天是周几"，北京凌晨时段两者跨域不一致 → 星期错位一天。
- **修复**：`FindNextMatchTime` 全程北京域（`bj = from + 28800` 计算与比较，返回 `cand - 28800`）；`AddTaskWeek`/`DelFirstTask` 调用点 `hhmm = (prs_time + 28800) % 86400`。

### M11 — `weekday==8` 三义冲突（周三 / 每日哨兵 / 夜灯）
- **文件**：`TC1/timed_task/timed_task.c/.h`、`TC1/main.c`、`TC1/http_server/web/index.html`
- **背景**：bit 掩码 8 = 周三，但旧固件把 8 当"每日"哨兵（`AddTask`/`DelFirst`/`Rebuild` 特判），夜灯任务也写 8；前端 `wk===8` 显示 Daily。`taskSocket` 含 LED（value=7=`SWITCH_LED_ENABLE`），用户可建"周三+LED"任务——哨兵必须让位。
- **修复**：
  - 新增专用标记 `NIGHT_DAILY_WEEKDAY = 127 | (1<<8) = 383`（bit7 是循环标志不可用；bit8 独立）。
  - 删除 `timed_task.c` 三处 `weekday==8` 特判；`CreateNightModeTask` 写 383；`RemoveNightModeTasks` 判 383。
  - **存量迁移**（`RebuildTaskList`，每次开机幂等）：`op==SWITCH_LED_ENABLE && wk==8` **且 prs 北京分钟等于当前 `night_mode_start` 或 `night_mode_end`** 才迁移为 383。时刻条件依据：旧固件改夜灯配置必 `Remove+Create`，故升级残留的夜灯任务时刻必等于最后一次配置——精确命中夜灯任务，**不会误伤**用户手建的周三 LED 任务（时刻不匹配 → 保留 8 = 周三语义）。
  - 前端 `renderTasks`：`wk===127||wk===383 → 'Daily'`；`wk===8` 落入星期列表显示 `Wed`。
- **周期闭环**：开机迁移 → SNTP 同步成功后 `RemoveNightModeTasks`（删 383，含迁移项）+ `CreateNightModeTask`（按当前配置重建）→ 持久化。

### M12 — 循环任务窗口基准漂移（8 小时错位 / 关窗后仍触发）
- **文件**：`TC1/timed_task/timed_task.c/.h`、`TC1/http_server/app_httpd.c`
- **问题**：窗口起点取 `prs_time % 1440`，但 `prs_time` 每轮执行后被重排为"下次触发时间"，起点漂移；窗口比较用 UTC `localtime` 而 `loop_end` 是北京分钟，差 8 小时。
- **修复**：
  - 起点编码进 `weekday` 高位：`bit20-30 = start_min + 1`（`GET_LOOP_START`/`MAKE_LOOP_START`，掩码 0x7FF 覆盖 0–1440）。**+1 是因为 0 保留为"旧任务未编码"哨兵**。
  - `HttpAddTask` 循环分支从 `prs_time` 算北京分钟编码。
  - `ProcessTask` 窗口比较全北京域：`now_min = ((now+28800)%86400)/60`；起点 `start_enc>0 ? start_enc-1 : prs_time%1440`（**legacy 回退 = 原行为**，不引入新错位）。
  - 编码值域核验：`MAKE(1023,1023)|MAKE_START(1439) = 1,510,998,015 < 2^31`，`int` 不溢出；dur/interval（bit0-9/10-19）与 start（bit20-30）无重叠。

---

## 二、实施过程中终审发现并修正的问题

| # | 问题 | 处置 |
|---|------|------|
| 1 | `mico_rtos_create_worker_thread` 内部 `memset` struct——重 Init 重复创建会摧毁常驻 worker | `mqtt_worker_created` 单例守卫（N3） |
| 2 | 初版 `MAKE_LOOP_START` 掩码误写 0x3FF（1023 < 1440） | 改为 `0x7FF` |
| 3 | 初版迁移条件 `op==7 && wk==8` 会把**新固件上用户建的周三 LED 任务**开机改判为每日（前端合法功能路径回归） | 收紧为"夜灯配置时刻匹配"才迁移（M11） |
| 4 | `context_created` 若放在 `require_action(url,...)` 之前，已有任务运行时的 `kGeneralErr` 会释放**运行中** context | 标志置于 malloc 成功之后 |
| 5 | daemons 文件替换时 python 闭包绑定旧 `data`，仅最后一次 `replace` 生效 | 重跑全部替换并 assert 计数 |

## 三、未修项（超出认可范围，仅记录）

1. **`GET_LOOP_DURATION` 与循环标志 bit7 重叠（既有）**：`dur & 0x3FF` 含 bit7 → 取出的 duration 恒为 `dur|128`（默认 5m → 133m）；`loop_interval` 变量实际未被消费。属既有宏设计缺陷，修改会波及存量任务解码，本次未动。
2. **SDK 内部 `mico_system_context_update` 调用点**（easylink/power_daemon 等）未纳入 `AppContextUpdate` 锁——框架代码，改动风险大于收益。
3. **MQTT 配置修改需等线程重连/重 Init 生效**（既有行为；本次修复后保存配置触发的 `UserMqttInit` 已能真正重建线程，缩短了生效路径）。
4. **`TC1/ota_server/ota_server.c`** 为 `mico-os` daemons 副本的死代码（`TC1.mk` 不编译），未同步修改。
5. 降级中的首次连接失败即回调 `OTA_FAIL`（随后仍重试）为既有表现，未改。
6. `get_func_name` 静态缓冲的多线程竞态（内容可能串扰，但不再有内存风险）——既有。

## 四、行为变化（升级须知）

| 变化 | 说明 |
|------|------|
| 夜灯任务标识 8 → 383 | 开机自动迁移存量夜灯任务（时刻匹配当前配置）；Web 列表仍显示 **Daily**，行为不变 |
| **周三 + LED 任务** | `wk=8` 现按**周三**执行/显示（旧固件把它当日哨兵每日执行——修复）；仅"时刻恰好=夜灯配置"的存量任务会被迁移 |
| 出厂插座名 | Web 恢复出厂的 `Socket %d` 统一为 `插座-%d`（与配置损坏恢复一致） |
| 循环任务起点 | 新建/重新保存的循环任务带精确起点；**存量**未编码任务走 legacy 回退（=旧行为），要获得正确窗口需重新保存一次 |
| OTA 校验收紧 | 无 md5 也强制镜像头校验；md5 错/头错/状态码非 2xx → 明确 FAIL 且可立即重试 |
| MQTT 配置保存 | 触发线程回收重建，最坏等待旧线程退出约 7s 内完成 |

## 五、验证

- [x] 全量 `git diff` 逐文件终审（含改动函数控制流/作用域/goto 合法性）
- [x] 8 个改动 C 文件括号平衡检查；include 连通性（`main.h`→`timed_task.h`、`user_gpio.h`、`NIGHT_DAILY_WEEKDAY` 可见性、`OtaImageHeaderValid` 声明/定义签名一致）
- [x] 残留检查：TC1 内 `mico_system_context_update(sys_config)` 0 处、`weekday==8` 特判仅剩迁移条件 1 处、前端 `wk===8` 判断 0 处
- [x] M12 宏单元测试（编码/解码/legacy/383/值域溢出）
- [x] GitHub Actions run #155 **编译通过**，Release **v4.1.34** 产物齐全
- 本地未做固件编译（AGENTS.md 规定交付一律云构建）；MiCO 框架行为（timer stop on 非运行态、mutex init 时序）已对照 `mico-os` 源码人工核验
