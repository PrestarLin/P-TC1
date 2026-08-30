# P-TC1 - 斐讯TC1智能排插第三方固件

> 感谢原作者 [zogodo](https://github.com/zogodo/zTC1) 无私分享的代码

基于斐讯TC1 A1智能排插的第三方固件，无需服务器即可实现完整的智能控制功能。

![web](./doc/Phicomm_TC1.png)

## 功能特性

### 已实现功能

- [x] 按键控制总开关
- [x] 独立控制每个插口通断
- [x] Web实时显示功率和功耗
- [x] 添加定时任务控制插口通断
- [x] **倒计时功能** - 设置倒计时后自动执行指定操作
- [x] **循环开关功能** - 开启多长时间后关闭多长时间，支持小时/分钟/秒单位设置
- [x] OTA在线升级
- [x] 本地文件升级
- [x] 通过MQTT接入HomeAssistant
- [x] 自定义设备和插座名称
- [x] 自定义按键单击/长按功能
- [x] 童锁功能（防止误操作）
- [x] 电源指示灯开关控制
- [x] 可配置MQTT数据上报频率

### HomeAssistant集成

- 总耗电量传感器
- 今日耗电量传感器
- 昨日电量传感器
- 所有插座独立控制
- 总开关控制
- 童锁开关

## Web界面

<img src="doc/IMG_0863.png"><img src="doc/1.png"><img src="doc/IMG_0887.png">

## HomeAssistant接入效果

<img src="doc/IMG_0888.png">

## 硬件要求

**仅支持A1版本**，A2版本不支持。

### 区分硬件版本

硬件版本在外包装底部标注：
![hardware_version](./doc/hardware_version.png)

如无包装，可拆开分辨：
- 左侧为不支持的A2版本
- 右侧为支持的A1版本

![a1_a2](./doc/a1_a2.png)

## 快速开始

### 1. 首次刷机

固件启动后会创建热点 `TC1-AP-XXXXXX`（XXXXXX为MAC地址后六位），连接后访问：
- Web界面：`http://192.168.0.1`

### 2. 烧录方法

**软件准备：**
- [FlashPlus烧录工具](https://github.com/a2633063/zTC1/blob/master/README/FlashPlus_v1.0.10.msi.zip)

**硬件准备：**
- SWD接口的JLink烧录器（约20元）
- 仅需连接：GND、SWDIO、SWCLK、VCC

**烧录步骤：**
1. 使用T9梅花螺丝刀拆开排插
2. 连接烧录器到TC1的SWD触点
3. 打开FlashPlus软件
4. 选择Chip: 88MW30X
5. 选择Interface: JLINK
6. Flash: External Flash → QSPI → Other
7. Flash Address填 `0`
8. Erase Mode选择 `Erase all flash`
9. 勾选 `Verify after programming`
10. 选择下载的 `all.bin` 文件
11. 点击Start开始烧录

**注意事项：**
- 烧录时请勿连接220V电源
- 烧录完成后重新上电即可使用
- 首次烧录需使用烧录器，后续可OTA升级

### 3. 编译固件

**环境要求：**
- Windows系统
- Python 2.7

**安装步骤：**

```bash
# 1. 安装Python 2.7并添加到Path
# 默认安装路径需添加：C:\Python27 和 C:\Python27\Scripts

# 2. 安装mico-cube
python -m pip install mico-cube-1.0.0.tar.gz

# 3. 下载并解压Micoder（不要有中文路径）
# 下载地址：http://firmware.mxchip.com/MiCoder_v1.3_Win32:64.zip

# 4. 配置Micoder路径
mico config --global MICODER <micoder路径>/MiCoder

# 5. 编译固件
# 切换到项目根目录执行
./build.sh
```

**常见问题：**
- pip安装mico-cube失败时，使用完整路径安装：
  ```bash
  python -m pip install mico-cube-1.0.0.tar.gz
  ```

### 4. OTA升级

从V0.2版本开始支持OTA在线升级：
- 在Web界面「固件升级」页面输入OTA地址
- 或上传本地固件文件进行升级

## 计划实现功能

- [ ] 更丰富的功率统计图表
- [ ] 多语言支持（英语、日语）
- [ ] 蓝牙Mesh组网
- [ ] 语音助手集成（天猫精灵、小爱同学）
- [ ] 用电习惯分析
- [ ] 异常用电告警
- [ ] 固件自动备份与回滚

## 项目结构

```
P-TC1/
├── TC1/                    # 主程序
│   ├── http_server/        # Web服务器
│   │   └── web/            # 前端界面
│   ├── mqtt_server/        # MQTT服务
│   ├── ota_server/         # OTA升级服务
│   ├── time_server/        # 时间服务
│   └── timed_task/         # 定时任务模块
├── mico-os/                # MiCO操作系统
├── doc/                    # 文档和图片
└── build.sh                # 编译脚本
```

## 致谢

- [zogodo](https://github.com/zogodo/zTC1) - 原始代码作者
- [a2633063](https://github.com/a2633063/zTC1) - 烧录教程提供者
- 所有贡献者和测试人员

## 许可证

本项目基于原作者代码开发，仅供学习交流使用。

## 支持与反馈

如遇到问题，请在GitHub Issues中反馈。
