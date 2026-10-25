# 任务完成报告 — ESP32-S3 智能温控固件优化

日期:2026-02-03 · 目标:ESP32-S3 · 框架:ESP-IDF v6.1.0

## 一、验证状态总览(诚实声明)

| 验证层级 | 状态 | 说明 |
|----------|------|------|
| 源码实现(任务书全部条目 + 事件机制规范 V1.1) | ✅ 完成 | 全部需求已实现 |
| `idf.py build` 编译 | ✅ 通过 | 0 错误 0 警告 |
| 主机单元测试 | ✅ 通过 | 4191 断言,0 失败(ctrl 1050 + LED 状态机 3141,细节见 §四) |
| 首次刷写 + 启动日志抓取(COM4 实物) | ✅ 已做 | 75 s 启动日志;发现 2 个实机缺陷(见 §4.3) |
| 缺陷修复 + 事件重构固件刷写 | ✅ 已做 | BUG 1–4 全部修复并刷写;80 s 复验:单次启动无复位,完整走完启动状态机至 RUNNING(见 §4.3) |
| RGB 状态灯改造(WS2812@GPIO48) | ✅ 刷写验证 | 启动日志确认 `WS2812 RGB LED ready: GPIO=48 brightness=64`;灯语目视待用户确认(见 §4.4) |
| 真实传感器/继电器/断电恢复等实测 | ⚠️ 部分 | **SHT40 已确认在位**(有效采样驱动迟滞加热);继电器安全初始化已见日志;断电恢复/SAFE_LOCKED 冒烟未做 |
| MQTT 连接/联调 | 🟡 连接已建立 | 最新启动日志显示 Wi-Fi(`Xiaomi_35BF`)与 broker 均已连上(`connected to broker`,并完成时间同步);应用层命令/状态往返联调未做 |

**请勿将"编译通过 + 主机测试通过"理解为"已在硬件上验证"。**
上板前建议先用 `idf.py -p <PORT> flash monitor` 做 §五 列出的冒烟检查。

## 二、需求完成情况(对照任务书)

| # | 任务书要求 | 状态 | 实现位置 |
|---|-----------|------|----------|
| 1 | 启动状态机(BOOT_SAFE→…→RUNNING,含 DEGRADED/RECOVERY/SAFE_LOCKED) | ✅ | `main/app_boot.c` |
| 2 | 继电器安全输出优先初始化(先安全电平后使能) | ✅ | `components/relay/relay.c` |
| 3 | NVS 配置读取,仅条件性恢复(绝不无条件擦除) | ✅ | `components/app_cfg/app_cfg.c` |
| 4 | SHT40 2s 采样,CRC/超时处理,无效样本不加热 | ✅ | `components/sensor_sht40/` + `ctrl_logic.c` |
| 5 | 35/37°C 迟滞加热 + 40°C 超温保护 | ✅ | `ctrl_logic.c`(测试 1、6) |
| 6 | 统一 JSON 协议 + 严格校验(类型/范围/一致性/主题匹配) | ✅ | `ctrl_protocol.c`(测试 11-18) |
| 7 | 保护:传感器故障锁、加热超时、冷却、安全锁;优先于手动 | ✅ | `ctrl_logic.c`(测试 3、7、8、9) |
| 8 | 重启循环保护(快速复位→SAFE_LOCKED) | ✅ | `main/app_boot.c`(RTC 内存计数) |
| 9 | 看门狗/健康监控分级升级(10s 停机→安全断开,60s→复位) | ✅ | `main/app_health.c` |
| 10 | SoftAP Web 配网(失败兜底,不阻塞本地控制) | ✅ | `components/wifi_manager/wifi_manager.c` |
| 11 | 主机单元测试 + `idf.py build` | ✅ | `tests/host/`(1038 断言通过) |
| 12 | 文档:详细设计 + MQTT 协议 | ✅ | `docs/DESIGN.md`、`docs/MQTT_PROTOCOL.md` |

## 三、用户确认事项(已按确认值固化)

| 事项 | 确认值 | 固化位置 |
|------|--------|----------|
| 继电器 GPIO | GPIO4,高电平有效 | `Kconfig.projbuild`(`APP_RELAY_GPIO=4`,`APP_RELAY_ACTIVE_HIGH=y`) |
| SHT40 I2C | SDA=GPIO8,SCL=GPIO9 | `Kconfig.projbuild` |
| MQTT Broker | `mqtt://192.168.31.242:1883` | `APP_MQTT_URI_DEFAULT`(NVS 可覆盖) |
| 发现机制 | MQTT Discovery(非 HA 原生) | `devices/{dev}/discovery` retained |
| 主机测试工具链 | 允许安装 ziglang | 已安装(pip,zig 0.16.0) |
| Flash 容量 / 分区布局 | 16MB / 单应用大分区 | `sdkconfig` + `partitions.csv`(app 16320KB) |
| RGB 指示灯 | 板载 WS2812(单线数据)→ GPIO48,亮度 64 | `APP_LED_WS2812_GPIO=48`、`APP_LED_WS2812_BRIGHTNESS=64`;驱动 `espressif/led_strip` 3.1.1(RMT) |

## 四、测试报告

### 4.1 主机单元测试(zig cc 编译,与固件同源 `ctrl_core` 代码)

```
test_ctrl_logic   :  958 assertions passed, 0 failed
test_ctrl_protocol:   92 assertions passed, 0 failed
test_led_pattern  : 3141 assertions passed, 0 failed   (RGB 灯语引擎,对应灯语规范 §7)
合计              : 4191 passed, 0 failed
```

用例覆盖(任务书规则 32 的核心场景):

| 用例 | 验证内容 | 结果 |
|------|----------|------|
| 1 | 迟滞:35 以下开、37 以上关、区间保持 | ✅ |
| 2 | 阈值校验:NaN/Inf/越界/start≥stop 拒绝,原子生效 | ✅ |
| 3 | 传感器故障:无效样本即禁热,连续 3 有效样本恢复 | ✅ |
| 4 | 样本过期(>8s)进入 FAULT_SENSOR | ✅ |
| 5 | 超温 40°C 保护及降温退出 | ✅ |
| 6 | 手动模式强制开/关、与自动切换 | ✅ |
| 7 | 手动过期(`expires_at`)自动退出 | ✅ |
| 8 | 连续加热 30min 超时保护 + 10min 冷却重试 | ✅ |
| 9 | SAFE_LOCKED 全面封锁,满足条件/运维解锁 | ✅ |
| 10 | 从未有过有效样本 → 永不加热 | ✅ |
| 11 | 统一载荷构建与字段顺序/格式(含 `humidity`/`sensor_status`) | ✅ |
| 12 | 主题索引提取(含非法格式) | ✅ |
| 13 | 合法命令解析(四类命令) | ✅ |
| 14 | `sensor` 与主题索引不一致拒绝 | ✅ |
| 15 | 严格类型:bool/NaN/Inf/1e999/字符串拒绝 | ✅ |
| 16 | 阈值规则(成对、范围、start<stop) | ✅ |
| 17 | `heating`↔`relay_status` 一致性 | ✅ |
| 18 | 过期规则:missing/clock_unsynced/expired/too_far/generic | ✅ |
| 19 | 遥测扩展:湿度范围 [0,100] 校验、故障样本 `null`/`fault` 渲染 | ✅ |
| — | `no_action` 空动作拒绝 | ✅ |

测试命令(可复现,三套)见 [README](../README.md)「主机单元测试」。

### 4.2 固件构建

```
idf.py build → Project build complete
app 二进制: 1,023,984 字节(0xF9FF0,约 1000 KB),app 分区 16320KB(0xFF0000)余 94%
bootloader: 0x5280 字节,余 36%
分区表: 自定义 partitions.csv(nvs 24K @0x9000 + factory 16320K @0x10000)
编译告警: 0(全组件 -Werror 通过)
```

### 4.3 实机抓取发现的缺陷(COM4,75 s 启动日志)

首次刷写后的启动日志发现 2 个缺陷,均已在事件机制重构(V1.1)中修复:

| 缺陷 | 现象 | 根因 | 修复 |
|------|------|------|------|
| BUG 1 | Task WDT 约每 10 s 误报,点名 `health` 任务 | `app_health` 每 5 s 检查后才喂狗,喂狗点恰落在 5 s WDT 超时边界上竞争 | 改为 1 s 喂养周期(5×1 s),加狗后立即喂一次(`app_health.c`) |
| BUG 2 | 偶发 `task 1 (CONTROL) made no progress in 5000 ms` | esp-mqtt 客户端任务持有 API 互斥锁做约 10 s 阻塞连接,`enqueue` 同锁被阻塞 | 发布泵任务成为 `esp_mqtt_client_enqueue` 唯一调用者;关闭 SDK 自动重连,单一应用级重连所有者(`mqtt_manager.c`) |

另外修复了 2 个隐性缺陷:`net_wifi` 从未调用 `esp_wifi_connect()`(STA 永不发起连接);
重试循环可能在已连接时误开配网门户 —— 均由新 `wifi_manager` 重写消除。

重构后逐轮刷写验证又发现并修复 2 个缺陷:

| 缺陷 | 现象 | 根因 | 修复 |
|------|------|------|------|
| BUG 3 | 启动即 abort 死循环:`device_manager_init` 返回 `ESP_ERR_INVALID_STATE` | 事件注册用 `esp_event_handler_register` 注册到默认事件循环,而 APP_EVENT 发布到自定义循环 | 改用 `esp_event_handler_register_with(s_loop, ...)`(`event_manager.c`) |
| BUG 4 | 启动即 panic 死循环:`assert failed: esp_netif_create_default_wifi_ap` | `wifi_mgr_start` 已创建 AP netif,`start_portal` 再次创建同名 netif(`WIFI_AP_DEF` 唯一性),`esp_netif_new` 返回 NULL 触发断言 | 复用 `wifi_mgr_start` 创建的 `s_ap_netif`,`start_portal` 仅在为空时创建(`wifi_manager.c`) |

BUG 4 修复同时将配网热点名的 MAC 后缀改回 STA MAC(`THERM-SETUP-A2A0`,与硬件确认时一致)。
修复后 80 s 启动日志验证:单次 POWERON 启动无复位、完整走完启动状态机至 `RUNNING`;
无 BUG 1/BUG 2 特征;配网门户正常启动(`THERM-SETUP-A2A0`/`setup-1234`/192.168.4.1);
MQTT 严格等待 `WIFI_GOT_IP`(T01);10 s 有界 SYNC_STATE 后进入 DEGRADED→RUNNING;
SHT40 实测在位(初始读数短暂无效→FAULT_SENSOR→3 个有效样本→30 s COOLDOWN→恢复后
室温低于 35°C 阈值,继电器按迟滞逻辑进入加热,全程输出路径受 `sensor_valid` 门控)。

### 4.4 RGB 状态灯改造(WS2812 @ GPIO48)与实测

依[《RGB 指示灯状态规范》V1.0](<specs/%23 ESP32-S3 温控设备 RGB 指示灯状态规范.md>) 重构灯语子系统,并将引脚指向 GPIO48 实物刷写验证:

- **架构**:纯 C 灯语引擎 `led_pattern.c`(时序/颜色/优先级解析,无硬件依赖,主机测试 3141 断言)
  + `led_status.c`(FreeRTOS 灯任务 + 事件侧唯一硬件写入者)。优先级 L6>L3>L1>L2>L0>L5>L4,
  时序严格按规范 §2(如 L1 蓝色 200/200 双闪、L6 红色 150/150/150/150/150/900)。
- **硬件后端**:DevKitC-1 板载 RGB 为 **WS2812 单线地址灯**(规范 §5 明确禁用三路独立 GPIO 方案),
  采用 `espressif/led_strip` 3.1.1(RMT 后端,GRB 分量序);亮度 `APP_LED_WS2812_BRIGHTNESS=64`。
  引脚配置 `APP_LED_WS2812_GPIO=48`(-1 时编译为 no-op 桩,LED_STATUS_NOOP)。
  *版本注意:led_strip 2.5.x 在 IDF v6.1 下编译失败(缺 `esp_heap_caps.h`),3.1.0 起官方修复,故锁定 `^3.1.0`。*
- **实测(COM4 刷写后 60 s 启动日志)**:`led_status: WS2812 RGB LED ready: GPIO=48 brightness=64`;
  单次 POWERON 无复位,完整走完启动状态机至 `RUNNING`;Wi-Fi 关联 `Xiaomi_35BF` 并获取 IP,
  MQTT 连接 broker 成功、时间同步,35 s 时 `relay -> heating`(对应 L5 绿色慢闪)。
  灯语目视效果待用户确认。

### 4.5 遥测扩展:湿度与传感器状态上报(实机验证)

- **改动**:`ctrl_payload_build()` 新增 `humidity`(%RH,∈ [0,100])与 `sensor_status`(`ok`/`fault`);
  故障样本测量值输出 `null` 而非伪造数字。采样队列携带湿度,遥测由「仅有效样本」改为**每次采样一条**,
  服务端可持续感知传感器状态;discovery 同步声明 `humidity_unit`。
  命令 schema 不变:`humidity`/`sensor_status` 为上报字段,命令携带按 `unknown_field` 拒绝。
- **主机测试**:`test_ctrl_protocol` 80 → 92 断言(新增湿度范围校验、故障样本 `null`/`fault` 渲染、
  故障样本免测量值校验),合计 4191 断言通过。
- **实测(COM4 刷写后 60 s 启动日志)**:遥测载荷
  `{"sensor":0,"celsius":23.10,"humidity":57.20,"sensor_status":"ok","heating":false,...}`
  按 2 s 周期上报;discovery 含 `"humidity_unit":"percent"`;串口温湿度日志正常(≈23.1°C / 57.2%)。
  启动初期(471 ms)一次 `ESP_ERR_INVALID_RESPONSE` 失败采样,MQTT 尚未连接故无故障遥测可见,
  随后自愈;故障遥测路径(`sensor_status":"fault"`,`null` 测量值)由主机测试覆盖。

## 五、遗留事项与上板建议(未验证项)

1. **硬件冒烟清单**(上板后逐项):
   - 上电瞬间继电器是否先处于断开状态(用万用表/指示灯确认无误动作);
   - SHT40 接线后温度读数是否合理(±2°C 内对比参考温度计);
   - 手动断开传感器,确认加热停止且上报 `fault_sensor`;
   - 断开 WiFi,确认本地迟滞控制仍工作;
   - 快速复位 5 次,确认进入 SAFE_LOCKED;
   - 向 broker 发布过期 retained 手动加热命令,确认被拒(`expired`)。
2. `sdkconfig` 中 `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 保持模板默认(3584);
   本设计把重活放在独立任务,尚未在实机确认余量,若上板栈溢出复位可调至 8192。
3. 触点粘连检测:无反馈硬件,`relay_status` 语义为"指令状态"
   (`semantics:"commanded_state"`,已在 discovery 声明);如需真实触点反馈需加检测电路。
4. OTA、TLS/mqtt、多传感器扩展:任务书范围外,未实现。

## 六、交付物清单

```
├── main/                    启动状态机、temperature_control 控制任务、健康监控
├── components/
│   ├── ctrl_core/           纯 C 控制逻辑 + 严格协议(可主机测试)
│   ├── relay/               继电器唯一写入者(安全初始化)
│   ├── sensor_sht40/        SHT40 驱动(CRC/超时)
│   ├── app_cfg/             NVS 配置(条件性恢复)
│   ├── event_manager/       APP_EVENT 统一事件层(9 事件 + 32 条历史)
│   ├── wifi_manager/        STA+退避 / SoftAP 配网(只发事件)
│   ├── mqtt_manager/        MQTT+LWT+发布泵+单一重连所有者
│   ├── device_manager/      DEVICE_READY/SYSTEM_ERROR 广播 + SNTP
│   └── led_status/          事件驱动状态灯
├── tests/host/              主机单元测试(4191 断言)
├── partitions.csv           自定义分区表(16MB 单应用大分区)
├── sdkconfig.defaults       默认配置(flash 16MB + 自定义分区表)
└── docs/
    ├── DESIGN.md            详细设计文档
    ├── MQTT_PROTOCOL.md     MQTT 协议使用文档
    ├── REPORT.md            本报告
    └── specs/               需求规范原文(任务书/RGB 灯语/事件机制/MQTT 主题配置)
```
