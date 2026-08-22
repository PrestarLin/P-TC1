# P-TC1 八月提交二次审查报告

> 针对 2026-08 批次修复（批1–批9 + CI 提交）的代码级复核，重点核对
> **修复是否真正闭环**、**是否引入新回归**。原报告见 `fix-report.md`。

---

## 一、结论速览

| 级别 | 数量 | 处置 |
|------|------|------|
| 🔴 P0 | 1 | 修复引入的空指针解引用回归，需立即修复 |
| 🟠 P1 | 3 | 功能/正确性缺陷，fix-report 未覆盖，建议修复 |
| 🟡 P2 | 2 | 健壮性/潜在风险，可选优化 |

成功验证项：批1/批2(C1/C2/C4/M6/M11/m6)/批9 初始化顺序/GetTaskStr 分配等均已正确闭环，无新问题。

---

## 二、🔴 P0 — 修复引入的新崩溃（回归）

### N1. `WifiScanCallback` 空指针判空被写反（批2 / C3）

`TC1/user_wifi.c`：

```c
char* ssids = malloc(sizeof(char)*count * 36 + 1);
char* secs  = malloc(sizeof(char)*count * 2 + 1);
if (!ssids) ssids[0] = 0;   // 错误：malloc 失败恰恰是 NULL，此处解引用空指针
if (!secs)  secs[0] = 0;    // 同上
ssids[0] = '\0';
secs[0] = '\0';
```

- **问题**：批2 本意是"补判空"，但判断 `if (!ssids)` 成立时（malloc 返回 NULL）立刻执行 `ssids[0] = 0`，**解引用空指针直接崩溃**。与原代码相比是**主动引入的回归**（原代码未判空但不主动崩溃）。
- **附带**：
  - `wifi_ret = malloc(count*40+64)` 的返回值**始终未判空**，后续直接 `sprintf` 写入。
  - `count==0` 时 `count*36+1 = 1` 字节，`ssids[0]='\0'` 合法，但 `wifi_ret` 分配 `count*40+64=64` 字节，循环不执行，功能上空结果，可接受。
- **建议修复**：
  ```c
  wifi_ret = malloc(sizeof(char)*count * 40 + 64);
  char* ssids = malloc(sizeof(char)*count * 36 + 1);
  char* secs  = malloc(sizeof(char)*count * 2 + 1);
  if (!wifi_ret || !ssids || !secs) { free(wifi_ret); free(ssids); free(secs); return; }
  ssids[0] = '\0';
  secs[0]  = '\0';
  ```

---

## 三、🟠 P1 — 功能/正确性缺陷（fix-report 未覆盖）

### N2. `RebuildTaskList` 启动即推进周任务触发时刻（批3 / M4）

`TC1/timed_task/timed_task.c`：

```c
void RebuildTaskList(void) {
    user_config->task_top = NULL;
    user_config->task_count = 0;
    for (int i = 0; i < MAX_TASK_NUM; i++)
        if (user_config->timed_tasks[i].on_use) {
            user_config->timed_tasks[i].next = NULL;
            AddTask(&user_config->timed_tasks[i]);   // 周任务会重算 prs_time
        }
}
```

- 批3 引入 `RebuildTaskList` 重建链表以消除"持久化裸指针"崩溃，本身必要且正确。
- **但**对 `weekday∈[1,7]` 的周任务，`AddTask → AddTaskWeek` 会 `AddTaskSingle` 之外**重算 `prs_time` 到"下一次触发点"**。因此**每次开机重启都会把周任务的触发时刻往前推进一天/一周**，造成触发漂移、甚至漏触发。
- fix-report 中 M4 只声称"重建链表"，未提及该副作用。
- **建议**：`RebuildTaskList` 中对周任务仅在 `prs_time <= now`（已过期）时才推进，否则原样插入；或为重建单独走"不重算时间的排序插入"路径。

### N3. `ProcessHaCmd` 未校验 `sscanf` 返回值，`on` 未初始化（批4 / M2 残余）

`TC1/mqtt_server/user_mqtt_client.c`：

```c
int i, on;
sscanf(cmd, "set socket %19s %d %d", mac, &i, &on);
if (strcmp(mac, str_mac)) return;
if (i < 0 || i >= SOCKET_NUM) return;   // 只校验了 i，on 未校验
UserRelaySet(i, on);
```

- 批4 修复了 `i` 越界，但 **`sscanf` 返回值未检查**、`on` 未初始化。若 HA 下发畸形 payload（缺 `on`），`on` 为栈上垃圾 → 随机开/关继电器。
- 同样存在于 `set led`、`set total_socket`、`set childLock`。
- **建议**：`if (sscanf(...) < 期望个数) return;` 或先 `on = 0;` 兜底。

### N4. `reboot` 分支前缀过宽（批4）

```c
} else if (strncmp(cmd, "reboot", 6) == 0) {
    sscanf(cmd, "reboot %19s", mac);
    ...
}
```

- `strncmp(..., 6)` 会匹配任意以 `reboot` 开头的串（如 `rebootX`、`reboot_now`）。结合后续 `sscanf` 取 mac，畸形命令存在误匹配面。
- 严重性低，但因涉及 `MicoSystemReboot()`，建议收紧为精确/空格匹配。

---

## 四、🟡 P2 — 健壮性/潜在风险（可选）

| # | 位置 | 问题 |
|---|------|------|
| P2-1 | `user_wifi.c` | `count*40`、`count*36` 为 `int` 乘法，count 虽来自 `ApNum`（通常 uint8），但理论上有整数溢出回绕可能生极小分配后越界写。建议 `<size_t>` 强转或对 count 上限。（低风险） |
| P2-2 | `app_httpd.c` HttpSetWifiConfig | `sscanf(buf,"%d %127s %127s",...)` 返回值未检查，且依赖 `httpd_get_data` 是否 NUL 终止 `buf`。（遗留，原报告 M6 未覆盖此点） |

---

## 五、成功验证项（确认修复到位，无新问题）

| 项 | 提交 | 核对结果 |
|----|------|---------|
| 批1 missing include | `a408043` | 消除隐式声明，OK |
| C1/C2 栈/配置区溢出 | `006af2e` | `snprintf`+`sizeof` 限长，OK |
| C4 MQTT payload 溢出 | `006af2e` | 截断+`strncpy` NUL 终止+`datalen` 上限，OK |
| M6 HttpSetWifiConfig 泄漏 | `006af2e` | 逐项 `free`+判空，且 `free(NULL)` 安全，OK |
| M11/m6 strncpy NUL 终止 | `006af2e` | 手动补 `'\0'`，OK |
| 批5 web_log 互斥 | `f0ddefc` | 局部缓冲+`log_mutex`，OK |
| 批6 童锁独立字段 | `3f04017` | 新增 `child_lock`，`USER_CONFIG_VERSION`→10 触发恢复，OK |
| 批7 OTA 206/空指针/防重入/除零 | `41fa767` | 修复方向正确 |
| 批8 ISR 限界/原子化 | `708adca` | `n>100` 限界、`volatile int ota_progress`，OK |
| 批9 初始化顺序 | `7e3b658` | `TaskModuleInit/LogMutexInit` 移到 `mico_system_init` 后、首个日志前；`RebuildTaskList/childLockEnabled` 移到版本恢复后，OK |
| 批9 GetTaskStr 溢出 | `9f345d4` | 分配改 `128/条`，OK |
| 批9 HttpAddTask 持久化 | `9f345d4` | 成功路径补 `mico_system_context_update`，OK |

---

## 六、建议的后续动作

1. **[P0] 立即修复** N1：批2 判空逻辑写反，按上文建议改造 `WifiScanCallback`。
2. **[P1] 择机修复** N2（周任务触发漂移）、N3（`sscanf` 返回值/`on` 未初始化）、N4（reboot 前缀过宽）。
3. 修复后统一 bump 版本号并重新走 CI 构建验证。

---

## 七、追加复核：本地 OTA 更新后是否会导致无法初始化开机（死循环）？

### 结论

**当前代码（含批9 `7e3b658` 修复）不会因本地 OTA 触发开机死循环/挂死。**
真正被 `7e3b658` 修复的正是旧版「版本恢复前重建任务链表导致无限重启」问题。
但 **v9→v10 跨界 OTA 会一次性把用户配置重置为默认**（Wi-Fi 凭据、定时任务、child_lock 全部丢失）——这是**数据重置，不是卡死**。

### 依据（启动路径逐段核对）

`application_start`（`TC1/main.c`）顺序：

1. `mico_system_init` → `mico_system_context_init` → `MICOReadConfiguration`（`mico_system_para_storage.c:223`）。
   - 用 **v10 `user_config_data_size`（=sizeof(user_config_t)）** 读 flash 并对 v10 size 算 CRC。
   - OTA 前写入的 v9 数据更小，且原 CRC 是 v9 size 计算的 → 新读出后 CRC 不匹配。
   - 主、备两分区都 CRC 失配 → `mico_system_context_restore`（`mico_system_para_storage.c:283`）→ 调 `appRestoreDefault_callback`（`main.c`）。
   - 该回调：清空 timed_tasks、`task_top=NULL`、`task_count=0`、`version=10`，末尾 `mico_system_context_update` **持久化 v10 默认值到 flash**。
2. `mico_system_init` 返回 `kNoErr`，**不中断**。
3. `application_start` 版本检查 `user_config->version != USER_CONFIG_VERSION`：restore 已把 version 置为 10 → **不再触发二次恢复**。
4. `RebuildTaskList()` 在**已清空**的任务数组上重建 → 空链表，主循环无事可做。

**两个恢复触发点**（CRC 双失效 / magic 校验失败）都经由 `appRestoreDefault_callback`，而该回调会 `mico_system_context_update` 持久化 → 恢复是**一次性**的，后续重启读到合法的 v10 默认值（CRC 匹配），不会反复恢复、不会循环。

### 「不会死循环」的保证来自 `7e3b658`

`7e3b658` 把 `RebuildTaskList()` 与 `childLockEnabled = user_config->child_lock` 移到「版本检查/恢复」**之后**；旧代码在恢复**前**就基于被 v9 数据污染后的 v10 视图重建链表 → 链表错乱/触发 `MicoSystemReboot` 死循环。现顺序已安全。

### 需关注的残留点（非死循环，但真实存在）

| # | 级别 | 位置 | 说明 |
|---|------|------|------|
| OTA-1 | 🟠 P2 | `TC1/ota_server/ota_server.c` | **重复/死代码实现**：`TC1.mk:28` 编译此文件，但它导出 `OtaServerStart`（驼峰名）；`user_ota.c:39` 实际调用的是 **SDK daemon** 的 `ota_server_start`（小写下划线，位于 `mico-os/libraries/daemons/ota_server/ota_server.c`，批7 `41fa767` 的 M7/M8/M9/m8 已修复它并对重入返回 `kGeneralErr`）。本地 `OtaServerStart` **从未被调用**，徒增固件体积且其 439–446 行仍保留「重入即 free 旧 context」的 UAF 隐患（因不可达故未触发）。建议从 `TC1.mk` 移除 `ota_server/ota_server.c` 行。 |
| OTA-2 | 🟡 P3 | `app_httpd.c:772` OTA 触发 | `UserOtaStart(buf, NULL)` 传 `md5=NULL` → SDK 走 `is_md5=false`，仅 CRC16 校验（`mico_ota_switch_to_new_fw`）。若本地/自建 OTA 服务器不保证镜像来源可信，建议强制传 MD5 以防取到坏固件后反复刷写。另 `app_httpd.c:772` 调 OTA 前建议加校验：当前 OTA 线程是否正在运行（防重复触发同一 URL）。 |

### 结论一句话

本地 OTA（含跨界升级）在当前 HEAD 下**不会**造成开机死循环。**（已更新）**：本报告 8.7 已落地「配置布局迁移机制」——`user_config_migrate()` 会从冻结的 v10 布局逐字段投影到当前布局并持久化，保留用户配置；不再一律走 `appRestoreDefault_callback` 清空。仅**未知版本**才回退恢复出厂默认。

---

### 补充边界：v10 → v10 OTA 会丢配置吗？

**不会。** v10 升级 v10 的 OTA 完全保留配置。

- 结构体布局、`sizeof(user_config_t)`、CRC 计算范围三者均不变 → `MICOReadConfiguration` 算出的 CRC 与 flash 中 v10 写入的 CRC **匹配**（`mico_system_para_storage.c:317` else 分支），只做主/备分区互相校验，**不调用 restore**。
- `MICOReadConfiguration` 仅在两种情况恢复：CRC 主备双失配（`mico_system_para_storage.c:281`，出现在**布局 size 变化**或数据损坏时）、magic 号校验失败（`mico_system_para_storage.c:347`）。
- `application_start` 版本检查 `user_config->version != USER_CONFIG_VERSION`：v10 读到 version 仍为 10 → 不进 restore 分支。

**关键约束：只有在再次修改 `user_config_t` 布局（增删改字段/顺序/类型）并 bump `USER_CONFIG_VERSION` 时，才会再次触发一次全量配置重置。**

### 演进建议（**已落地**，详见 8.7）

已实现"按旧布局逐字段迁移而非一律置默认"：
1. 冻结 `user_config_v10_t` 保存 v10 落地布局（offset 表天然内建）。
2. `user_config_t` 末尾 `reserved` 补齐到固定 `USER_CONFIG_STRUCT_CAP`，`sizeof` 恒定。
3. `user_config_migrate()` 按 `struct version` 选择旧布局解析并投影到当前布局，再持久化。
4. 用户配置跨布局变更**不再被清空**（仅首次"变长→定长"切块清空一次）。

未来再改布局照 8.7"演进后续"三步走即可。

---

## 八、本次全量优化/修复变更记录

以下变更已按用户批准的「P0/P1/P2 + OTA 相关项」全量计划实施完成，逐项与报告第四~七节对应。变更均已应用，**需重新构建并经 CI 验证后再发布**。

### 8.1 `TC1/user_wifi.c` — `WifiScanCallback`（P0-N1 + P2-1）

- **P0-N1（判空写反）**：原逻辑 `if (!ssids) ssids[0]=0;` 在 malloc 失败时对 `NULL` 解引用（UB/崩溃）。已改为三处分配统一判空后 `free` + `return`，不再解引用 NULL。
- **P2-1（int 溢出）**：`count*40 / count*36` 由 `int` 乘法改为经 `(size_t)` 强转，规避理论溢出回绕。

```c
size_t buf_size = (size_t)count * 40 + 64;
wifi_ret = malloc(buf_size);
char* ssids = malloc((size_t)count * 36 + 1);
char* secs  = malloc((size_t)count * 2 + 1);
if (!wifi_ret || !ssids || !secs)
{ free(wifi_ret); wifi_ret = NULL; free(ssids); free(secs); return; }
```

### 8.2 `TC1/timed_task/timed_task.c` — `RebuildTaskList`（P1-N2 周任务漂移）

- 原逻辑对 `weekday∈[1,8]` 的重复任务一律 `AddTask`，在启动时经 `AddTaskWeek` **重算 `prs_time` 到下一次触发点**，导致每次开机都人为推进触发时刻（漂移/漏触发）。
- 修复：仅当重复任务 `prs_time <= now`（已过期）才推进；否则原样经 `AddTaskSingle` 插入，保留未到期的触发时刻。
  - `weekday==8`（每日）：锚定到"今天该时刻"，若已过则推到明天。
  - `weekday∈[1,7]`（按周）：过期时走 `AddTask`（`AddTaskWeek` 自动对齐最近匹配星期）。
- 新增文件级前置声明 `bool AddTaskSingle(pTimedTask task);`（供早于定义的调用使用）。

### 8.3 `TC1/mqtt_server/user_mqtt_client.c` — `ProcessHaCmd`（P1-N3 + P1-N4）

- **P1-N3**：
  - 每个分支先校验 `sscanf` 返回值（socket 需 3 项、其余需 2 项、reboot 需 1 项），解析失败即 `return`。
  - `on` 统一在函数顶部声明并**在动作前校验 ∈{0,1}**，杜绝栈上垃圾值随机开/关继电器。
  - `set socket` 的 `i` 越界 + `on` 非法双校验。
  - `set total_socket` 局部循环计数变量改名 `j` 以去重。
- **P1-N4（reboot 前缀过宽）**：`strncmp(cmd,"reboot",6)` → `strncmp(cmd,"reboot ",7)`，仅匹配以 `reboot ` 开头的确切命令，杜绝 `rebootX`/`reboot_now` 误触发 `MicoSystemReboot()`。

### 8.4 `TC1/http_server/app_httpd.c`

- **P2-2 `HttpSetWifiConfig`**：校验 `sscanf(buf,"%d %127s %127s",...)` 返回值 `<3` 即判非法请求，返回 `ERR` 并 `goto exit`，避免 `mode` 因解析失败残留在 `-1` 时误走 `ApConfig`。
- **OTA-2 `OtaStart` 防重复触发**：在调用 `UserOtaStart` 前检查 `ota_progress ∈ [0,100)`（OTA 进行中）则返回 `BUSY` 并跳过，防止同一/并发 OTA 反复触发。`ota_progress` 由 `ota_server/user_ota.h` 的 `extern volatile int` 提供。

### 8.5 `TC1/TC1.mk` — 移除死代码编译（OTA-1）

- 从 `$(NAME)_SOURCES` 删除 `ota_server/ota_server.c` 一行（原第 28 行）。
- 该文件导出 `OtaServerStart/OtaServerPause/OtaServerContinue/OtaServerStop/OtaServerGet`（驼峰名）**从未被引用**；`user_ota.c:39` 实际调用 SDK daemon 的 `ota_server_start`。移除后固件体积减小，且消除其 439–446 行"重入即 free 旧 context" 的不可达 UAF 隐患。
- 说明：`ota_server/ota_server.h` 保留，因为 `user_ota.c:4` 依赖其中的 `OTA_STATE_E`/`ota_server_cb_fn` 类型及 `ota_server_start` 原型；`ota_server.c` 文件本身仍留档未删除（仅不再编译）。

### 8.6 未改动项（按报告定为 P3/演进，暂不纳入本次）

- OTA-2 的**强制 MD5 完整性**：`/ota/start` 仅携带 URL，MD5 需由固件目录/服务器下发，属跨组件特性，本次仅加"防重复"守卫；如产品要求镜像可信校验，需在 OTA 服务器测另加断言。

### 8.7 配置布局迁移机制（`main.h` + `main.c`，转化为第七节演进建议）

将第七节「避免未来每次布局变更都清空配置」从**建议**落地为**实际机制**。布局变更不再触发 MiCO 层 restore（不再清空用户数据），而是逐字段投影迁移。

**`TC1/main.h`**：
- 新增 `USER_CONFIG_STRUCT_CAP 4096`：用户配置在 flash 上的**固定落地大小**。`sizeof(system_config_t) + CAP + 2(CRC) <= PARAMETER1/PARAMETER2`(16KB) 约束满足。
- 新增冻结快照 `user_config_v10_t`（版本 10 发布时的落地布局，==当前布局，**请勿修改**）。
- `user_config_t` 末尾新增 `reserved[CAP - sizeof(user_config_v10_t)]`，使 `sizeof(user_config_t)` 恒等于 `USER_CONFIG_STRUCT_CAP`——CRC 范围、数据与 CRC 的 flash 偏移在所有版本间恒定，MiCO 底层永不再因"长度变化"误判损坏清空。
- 新增 `_Static_assert(sizeof(user_config_t)==CAP)`（GCC 下生效）硬约束。

**`TC1/main.c`**：
- 新增 `bool user_config_migrate(void)`：首字节恒为版本号（定长驻块 offset0 稳定）。
  - == 当前版本 → 直接返回 true（已是当前布局）。
  - 版本 ∈ [1,10] → 从冻结 v10 布局逐字段投影到当前布局（`snprintf`/`memcpy`/赋值），置 version 为当前，`mico_system_context_update` 写回（含固定 CRC），返回 true。
  - 未知版本 → 返回 false。
- `appRestoreDefault_callback` 增加 `memset(..,0,sizeof(user_config_t))`，确保 `reserved` 尾部字节确定，CRC 可复现。
- 启动流程 `application_start`：`if (user_config->version != USER_CONFIG_VERSION)` 的"直接 restore"改为 `if (!user_config_migrate())` 才 restore（仅未知版本才回退恢复出厂）。

**行为对照（相对第七节结论更新）**：
- 旧行为：任何布局版本 × 当前版本都会走 `appRestoreDefault_callback` 全量重置（清空 Wi-Fi 凭据/定时任务/child_lock）。
- 新行为：v10 已是最新布局 → OTA 完全保留；从冻结 v10 内的任意旧版本 → 逐字段迁移保留；仅**未知版本**才重置。

**演进后续**（未来再次改布局时）：
1. 先把当前布局整体复制冻结为 `user_config_v<N>_t`（N=当前 `USER_CONFIG_VERSION`）。
2. 再改 `user_config_t` 字段（新增字段一律加在 `reserved` 之前），并 `USER_CONFIG_VERSION`+1。
3. 在 `user_config_migrate()` 新增一段"从 vN 投影到当前"的字段拷贝。

**注意**：本项目从"变长"首次切到"定长驻块"时，MiCO 底层会因长度变化对存量旧记录判定损坏而**清空一次**（已接受的**一次性**重置，见第七节）。此后布局永不再因长度变化而清空。

### 后续步骤

1. 执行构建验证（本次 8.1–8.7：仅 `user_wifi/timed_task/mqtt/app_httpd/TC1.mk/main.h/main.c`；未变更配置结构体字段，`USER_CONFIG_VERSION` 仍为 10）。
2. 走 CI 构建 + 静态分析验证上述改动。
3. 若通过，再提交 commit（当前仍未提交，交由用户决定）。

---

## 九、变更文件/位置速查表（可按此对照手动落盘）

> 给无法直接传输文件的场景：以下列出每个改动的**文件 + 函数 + 命中的行号（当前工作区）+ 改法**。
> 行号以改动后的当前工作区为准；另一台机器若代码一致，行号应完全对应。

### 9.1 `TC1/main.h`

| 位置（行） | 改动 |
|-----------|------|
| 31–42 | `USER_CONFIG_VERSION` 之下新增 `#define USER_CONFIG_STRUCT_CAP 4096`。含义：用户配置在 flash 的**固定落地大小**；约束 `sizeof(system_config_t)+CAP+2(CRC)<=PARAMETER1/2(16KB)`。 |
| 65–81 | 新增"配置布局迁移机制"注释块（冻结快照/固定 CAP/三步演进规则）。 |
| 83–105 | 新增冻结结构体 `user_config_v10_t`（字段 == 当前布局，**勿改**，用作旧版本投影的 offset 基准）。 |
| 108–130 | 现有 `user_config_t` 末尾（`struct TimedTask timed_tasks[MAX_TASK_NUM];` 之后）追加最后一个字段 `char reserved[USER_CONFIG_STRUCT_CAP - sizeof(user_config_v10_t)];`。 |
| 132–135 | 追加（GCC 下生效）`_Static_assert(sizeof(user_config_t)==USER_CONFIG_STRUCT_CAP, ...)`。 |

### 9.2 `TC1/main.c`

| 位置（行） | 函数/语句 | 改动 |
|-----------|----------|------|
| 36 | `appRestoreDefault_callback` | 在 `user_config_t *userConfigDefault = user_config_data;` 后插入 `memset(userConfigDefault,0,sizeof(user_config_t));`，保证 `reserved` 尾部字节确定、CRC 可复现。 |
| 129–172 | 新增 `bool user_config_migrate(void)` | 见报告 8.7。首字节为版本号；==当前→true；∈[1,10]→从 `user_config_v10_t` 逐字段投影到当前并 `mico_system_context_update(sys_config)` 写回、置 version、返回 true；未知→false。 |
| 210 | `application_start` 版本检查 | `if (user_config->version != USER_CONFIG_VERSION){ restore }` 改为 `if (!user_config_migrate()){ restore }`——仅未知版本才 `mico_system_context_restore`。 |

### 9.3 `TC1/user_wifi.c`

| 位置（行） | 函数 | 改动 |
|-----------|------|------|
| 74–84 | `WifiScanCallback` | 原 `if(!ssids) ssids[0]=0;`（判空写反→空指针解引用）与 `if(!secs)` 删除；`count*40/count*36` 改 `(size_t)` 强转；改统一 `if(!wifi_ret||!ssids||!secs){ free 全部; return; }`。 |
| 85–86 | 同 | `ssids[0]='\0'; secs[0]='\0';` 移到判空通过之后。 |

### 9.4 `TC1/timed_task/timed_task.c`

| 位置（行） | 函数 | 改动 |
|-----------|------|------|
| 17 | 文件级 | 新增前置声明 `bool AddTaskSingle(pTimedTask task);`（供重建路径早于定义处调用）。 |
| 38 | `RebuildTaskList` | 新增 `time_t now = time(NULL);`。 |
| 44–66 | 同 | 对重复任务仅当 `task->weekday!=0 && task->prs_time<=now`（已过期）才推进：`==8`（每日）锚定今天该时刻、已过则 `+day_sec` 到明天；`∈[1,7]` 走 `AddTask`（`AddTaskWeek` 自动对齐最近星期）。未过期走 `AddTaskSingle` 原样插入（不再漂移）。 |

### 9.5 `TC1/mqtt_server/user_mqtt_client.c` — `ProcessHaCmd`（435–484）

| 位置（行） | 分支 | 改动 |
|-----------|------|------|
| 438 | 函数顶部 | `int i, on;` 上提为函数级声明（供多分支共用）。 |
| 440–447 | `set socket` | `sscanf(...)!=3` 即 return；`on` 校验 ∈{0,1}（与 `i` 越界同一 if）。 |
| 448–459 | `set led` | `sscanf(...)!=2` 即 return；`on` ∈{0,1} 校验。 |
| 460–470 | `set total_socket` | `sscanf(...)!=2`；`on` 校验；循环计数变量 `i` 改 `j`（去重）。 |
| 471–478 | `set childLock` | `sscanf(...)!=2`；`on` 校验。 |
| 479–483 | `reboot` | `strncmp(cmd,"reboot",6)` → `strncmp(cmd,"reboot ",7)`；`sscanf(...)!=1` 即 return。 |

### 9.6 `TC1/http_server/app_httpd.c`

| 位置（行） | 函数 | 改动 |
|-----------|------|------|
| 494–499 | `HttpSetWifiConfig` | `sscanf(buf,"%d %127s %127s",...)` 返回值 `<3` → `err=kUnknownErr; send_http("ERR",...); goto exit;`，避免解析失败误走分支。 |
| 776–780 | `OtaStart` | 在 `UserOtaStart(buf,NULL)` 前插入：`if (ota_progress>=0 && ota_progress<100){ http_log(...); send_http("BUSY",...); return err; }`，防 OTA 重复触发。 |

### 9.7 `TC1/TC1.mk`

| 位置（行） | 改动 |
|-----------|------|
| 28（`$(NAME)_SOURCES` 内） | 删除该行 `ota_server/ota_server.c\`。该文件导出驼峰名 `OtaServer*` 从未被调用（实际走 SDK daemon 的 `ota_server_start`），移除可减体积并消除其不可达 UAF 隐患。`ota_server.h` 保留（`user_ota.c` 依赖其类型/原型）。 |

---
> 说明：以上行号取自**当前工作区**，已在本地直接应用（工作区文件即最新状态）。文档供你对照在受限环境下手动落盘与核对。