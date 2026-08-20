# P-TC1 固件更新报告

## 版本 v3.0.0 — 全面 Bug 修复与 CI 构建链路打通

---

## 一、CI 云构建链路（GitHub Actions）

之前的 P-TC1 没有可用的云构建，每次需在本地 Windows 用 MiCoder IDE 编译。本次打通了 `ubuntu-latest` 上的完整 CI 流水线。

| 问题 | 修复 |
|------|------|
| 官方 ARM GCC 5.4 下载链接已 404 | 改用 Launchpad 镜像 `launchpadlibrarian.net` |
| 工具链 tarball 解压后目录层级不对 | 修正 `mv` 路径，使 `arm-none-eabi-gcc` 位于预期位置 |
| 缺少 Linux64 宿主工具（dash, cat, echo 等） | 创建符号链接到 `/usr/bin/` |
| SDK 路径大小写：`Security/TLS` vs `security/TLS` | Linux 大小写敏感导致 TLS 匹配失败，误引入 wolfSSL 与 mocSSL 冲突 |
| GITHUB_TOKEN 默认无 release 权限 | 增加 `permissions: contents: write` |
| upload-artifact@v3 已弃用 | 升级到 @v4，gh-release 升级到 @v2 |
| Python 2 → 3 脚本转换 | `map_parse_gcc.py` 等 3 个构建脚本 |

---

## 二、批修复（8 批，25+ 个 Bug）

### 批1 — 编译警告
消除 CI 报告的 3 处 `implicit-function-declaration` 警告（真实缺陷：隐式声明假定返回 `int`，与真函数签名不匹配）

- `main.c` — 补 `#include "mqtt_server/user_mqtt_client.h"`
- `user_gpio.c` — 补 `#include "http_server/app_httpd.h"`

### 批2 — 内存安全（Critical）
| Bug | 严重性 | 问题 | 修复 |
|-----|--------|------|------|
| C1 | 🔴 Critical | `WifiConnect` 中 `strcpy(wifi_key)` 可从 HTTP 接收 127 字节写入 64 字节栈缓冲区 → 栈溢出 | `snprintf` 限长 |
| C2 | 🔴 Critical | 同上，`strcpy` 写入配置区 `ssid[32]`/`user_key[64]` 越界 | `snprintf` 限长 |
| C3 | 🔴 Critical | `WifiScanCallback` 堆溢出（分配 `count*32` 但写入 `'%s'` 最多 36 字节）+ 越界读 + 0 条处理错误 | 重新计算分配大小、修正步进、保护 `count==0` |
| C4 | 🔴 Critical | MQTT 接收 payload 无上限 → broker 可发 >2048 字节堆溢出 | 截断 + NUL 终止 |
| M6 | 🟠 Major | `HttpSetWifiConfig` 泄漏 256 字节/次 + 空指针崩溃 | 补 free + 逐项判空 |
| M11 | 🟠 Major | `strncpy(ap_name, n=32)` 输入≥32 时不 NUL 终止 | 手动补 `'\0'` |

### 批3 — 定时任务模块
| Bug | 严重性 | 问题 | 修复 |
|-----|--------|------|------|
| C5 | 🔴 Critical | 链表 `task_top` 由主循环线程与 HTTP 线程并发访问，无任何锁 → 链表损坏/硬崩溃 | 添加 `task_mutex` 互斥锁 |
| M4 | 🟠 Major | 任务增删从未持久化 → 重启后全部丢失；`task_top` 裸指针被写进 flash → 重启后解引用裸指针崩溃 | 启动时 `RebuildTaskList()` 重建链表；增删后调用 `mico_system_context_update` |
| m2 | 🟡 Minor | `HttpAddTask` 失败后 `on_use` 仍为 true → 槽位永久占用（64 次后无法添加） | 失败时 `task->on_use = false` |
| m3 | 🟡 Minor | `HttpDelTask` sscanf 失败时 `time1` 未初始化 | 初始化为 0，检查返回值 |
| m4 | 🟡 Minor | 空列表时 `GetTaskStr` 返回垃圾 `[X]` | 返回 `[]` |

### 批4 — MQTT 控制修复
| Bug | 严重性 | 问题 | 修复 |
|-----|--------|------|------|
| M1 | 🟠 Major | `ProcessHaCmd` 用 `strcmp(cmd,"set socket")` 匹配完整串，但 HA 下发的是 `"set socket <MAC> n on"` → **五条命令分支全是死代码，HA 无法控制插座** | 改为 `strncmp` 前缀匹配 |
| M2 | 🟠 Major | `sscanf` 无宽度写入 `mac[20]`；socket 索引未校验 → 修复 M1 后立即越界 | 加 `%19s` 宽度限制 + `SOCKET_NUM` 校验 |

### 批5 — web_log 线程安全
| Bug | 严重性 | 问题 | 修复 |
|-----|--------|------|------|
| M5 | 🟠 Major | `web_log` 宏用全局 `LOG_TMP`/`LOG_NOW`，多线程并发 → 指针互相覆盖（泄漏）、写坏他人缓冲区；malloc 未判空 | 改用局部变量；`SetLogRecord`/`GetLogRecord` 加互斥锁 |

### 批6 — 童锁独立字段
| Bug | 严重性 | 问题 | 修复 |
|-----|--------|------|------|
| M3 | 🟠 Major | `user_config->user[0]` 同时存童锁状态和按钮 0 功能编码 → 相互破坏 | 新增 `child_lock` 字段，`USER_CONFIG_VERSION` 升级到 10 |

### 批7 — OTA SDK 修复
| Bug | 严重性 | 问题 | 修复 |
|-----|--------|------|------|
| M7 | 🟠 Major | 断点续传要求 HTTP 200，但 Range 请求返回 206 → 续传全部失败 | 同时接受 200 和 206 |
| M8 | 🟠 Major | `require_action(url,…)` 应为 `url_t` → 畸形 URL 空指针崩溃 | 修正为 `url_t` |
| M9 | 🟠 Major | 重入：`ota_server_start` 直接 free 旧线程仍在使用中的 context → UAF | 已运行时不释放，返回 `kGeneralErr` |
| m8 | 🟡 Minor | 进度回调 `download_len==0` 时除零 → NaN | 除零保护 |

### 批8 — 余项
| Bug | 严重性 | 问题 | 修复 |
|-----|--------|------|------|
| M10 | 🟠 Major | 功率中断 ISR 中 `n` 可达数千 → ISR 空转多秒阻塞所有中断 | 限界 100 |
| m5 | 🟡 Minor | `ota_progress` float 跨线程撕裂读 | 改为 `volatile int` |
| m9 | 🟡 Minor | `OTA_USE_HTTPS=1` 与应用实际不匹配 | 改为 0 与 SDK 一致 |
| m11 | 🟡 Minor | `map_parse_gcc.py` 正则 Py3 SyntaxWarning | 改用 raw string |

### 批9 — 失联回归 + 版本号 + 存量修复
| Bug | 严重性 | 问题 | 修复 |
|-----|--------|------|------|
| 回归 | 🔴 Critical | `LogMutexInit()` 晚于首个 `tc1_log` → `SetLogRecord` 在未初始化互斥锁上 `mico_rtos_lock_mutex` → 开机挂死、设备失联 | `TaskModuleInit/LogMutexInit` 移到 `mico_system_init` 之后、首个日志之前 |
| 回归 | 🔴 Critical | `RebuildTaskList()`/`childLockEnabled` 在版本检查/恢复**之前**执行 → 旧 v9 配置（新结构体插入了 `child_lock`）字段错位，在垃圾数据上重建链表（可能触发 REBOOT_SYSTEM 无限重启） | 移到版本恢复之后 |
| 版本 | 🟠 Major | `-DCOMMIT_HASH=<hex>` 是裸 token，`"v3.0.0-" COMMIT_HASH` 直接拼接 → **CI 编译错误** | 双字符串化 `STR(COMMIT_HASH)`，本地回退裸 token `local` |
| 存量 | 🟠 Major | `GetTaskStr` 按 `task_count*89+2` 分配，每条 sprintf 最长 ~113 字节 → **堆溢出** | 改 `128/条`（含余量） |
| 存量 | 🟠 Major | `HttpAddTask` 新增任务从不写 flash → 重启即丢失（批3 的 RebuildTaskList 使其暴露） | 成功路径补 `mico_system_context_update` |
| CI | 🟡 Minor | 每 push dev 生成相同 `v3.0.1-dev` tag → 连续推送时 release 步骤 422 失败 | dev tag 追加 `github.run_number` |

---

## 三、版本号

- `main.h`：`v2.2.0` → **`v3.0.0`**
- `.version`：`v1.0.79` → **`v3.0.0`**

---

## 四、提交历史（dev 分支，自原始 fork 起）

```
9f345d4 批9: COMMIT_HASH字符串化 + GetTaskStr堆溢出 + 任务持久化 + CI tag唯一
cc7a65a 版本号改用 EXTERNAL_MiCO_GLOBAL_DEFINES 传入
7e3b658 修复初始化顺序导致OTA后失联 + 版本号加commit缩写
708adca 批8: 余项修复 + 版本号 v3.0.0
41fa767 批7: OTA SDK 修复
3f04017 批6: 童锁独立字段
f0ddefc 批5: web_log 线程安全
bad3aa9 批4: MQTT 控制修复
df245df 批3: 定时任务锁 + 持久化
006af2e 批2: 内存安全
a408043 批1: 补 missing include
3638189 Grant contents: write to GITHUB_TOKEN
642d1cd 修复 TLS 大小写敏感
fee53e2 补 daemons/ota_server 组件依赖
9e98684 修正工具链解压目录
3f09d17 ARM GCC 改用 Launchpad 镜像
c095c78 升级 Actions 版本
fa17876 CI 转 Python3 + Linux64 工具
26c1f4d 改用 ubuntu-latest
1530dd1 修复严重 BUG 和内存安全
…（更早的历史）
```

---

## 五、CI 验证

每次推送 dev 分支自动触发 CI 构建，生成 `v3.0.1-dev-<run_number>` 预发布（含 `ota.bin` / `all.bin` / `ota` 完整包）。批9 提交 `9f345d4` 的 CI 构建已通过，固件内嵌版本号实测为 `v3.0.0-9f345d4`（已从 `ota.bin` 二进制中验证）。验证通过后合入 master 即可发布正式版。