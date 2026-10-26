# 任务完成报告 — ESP32-S3 智能温控固件优化

日期:2026-02-03 · 目标:ESP32-S3 · 框架:ESP-IDF v6.1.0

## 一、验证状态总览(诚实声明)

| 验证层级 | 状态 | 说明 |
|----------|------|------|
| 源码实现(任务书全部条目 + 事件机制规范 V1.1) | ✅ 完成 | 全部需求已实现 |
| `idf.py build` 编译 | ✅ 通过 | 0 错误 0 警告 |
| 主机单元测试 | ✅ 通过 | 4269 断言,0 失败(ctrl 1128 + LED 状态机 3141,细节见 §四) |
| 首次刷写 + 启动日志抓取(COM4 实物) | ✅ 已做 | 75 s 启动日志;发现 2 个实机缺陷(见 §4.3) |
| 缺陷修复 + 事件重构固件刷写 | ✅ 已做 | BUG 1–4 全部修复并刷写;80 s 复验:单次启动无复位,完整走完启动状态机至 RUNNING(见 §4.3) |
| RGB 状态灯改造(WS2812@GPIO48) | ✅ 刷写验证 | 启动日志确认 `WS2812 RGB LED ready: GPIO=48 brightness=64`;灯语目视待用户确认(见 §4.4) |
| 风扇调速/测速(PWM@GPIO5 + TACH@GPIO6) | ✅ 刷写 + MQTT E2E | 命令→应答→遥测→堵转事件全链路实测通过(见 §4.6);**风扇本体尚未接线** |
| 真实传感器/继电器/断电恢复等实测 | ⚠️ 部分 | **SHT40 已确认在位**(有效采样驱动迟滞加热);继电器安全初始化已见日志;断电恢复/SAFE_LOCKED 冒烟未做 |
| MQTT 连接/联调 | 🟡 部分联调 | Wi-Fi(`Xiaomi_35BF`)与 broker 已连上;**风扇命令 E2E 已完成**(§4.6);温控命令往返联调未做 |

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
| 11 | 主机单元测试 + `idf.py build` | ✅ | `tests/host/`(4277 断言通过) |
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

风扇接入设计默认值(**自动固化,待用户确认**;用户确认后如需变更改 Kconfig 重刷即可):

| 事项 | 默认值 | 固化位置 |
|------|--------|----------|
| 风扇 PWM / TACH 引脚 | GPIO5 / GPIO6 | `APP_FAN_PWM_GPIO` / `APP_FAN_TACH_GPIO` |
| 控制方式 | 仅手动 MQTT 调速,不与温控自动联动 | 设计决定(避免控制语义歧义) |
| 风扇供电 | 外部 12 V(严禁从开发板取电) | 接线规范 |
| 堵转阈值 / 遥测周期 | 3 s 无脉冲 / 5 s | `CTRL_FAN_STALL_MS`、`temperature_control.c` |
| 占空比持久化 | 否(重启回默认 0%) | 设计决定 |

## 四、测试报告

### 4.1 主机单元测试(zig cc 编译,与固件同源 `ctrl_core` 代码)

```
test_ctrl_logic   :  978 assertions passed, 0 failed
test_ctrl_protocol:  158 assertions passed, 0 failed
test_led_pattern  : 3141 assertions passed, 0 failed   (RGB 灯语引擎,对应灯语规范 §7)
合计              : 4277 passed, 0 failed
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
| 8 | 连续加热 2h 超时保护 + 10min 冷却重试 | ✅ |
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
| 20 | 风扇状态判定:duty=0→`off`(惯性空转 3 s 宽限内)、超宽限持续旋转→`overrun`、rpm>0→`ok`、启动宽限、3 s 阈值→`stall` | ✅ |
| 21 | 风扇载荷构建/回显(`fan/duty/rpm/status`,含越界与缓冲区不足拒绝) | ✅ |
| 22 | 风扇主题索引与 `fan_topic_mismatch` 一致性 | ✅ |
| 23 | 风扇命令解析:`bad_fan`/`bad_duty`/`bad_cmd_id`/`unknown_field` 等严格拒绝 | ✅ |
| — | `no_action` 空动作拒绝 | ✅ |

测试命令(可复现,三套)见 [README](../README.md)「主机单元测试」。

### 4.2 固件构建

```
idf.py build → Project build complete
app 二进制: 1,043,760 字节(0xFED30,约 1019 KB),app 分区 16320KB(0xFF0000)余 94%
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

### 4.6 风扇调速/测速接入(刷写 + MQTT 端到端实测)

- **实现**:`components/fan_ctrl/`(LEDC 25 kHz 10-bit PWM @GPIO5 + PCNT 转速计数 @GPIO6,
  1 s 测速窗口,2 脉冲/转);状态判定 `off/ok/stall` 在纯 C 层 `ctrl_fan_status_eval()`;
  集成于 `temperature_control.c`(遥测 5 s + 变化即发、命令路由、堵转事件)。
  IDF v6.1 的 PCNT API 为 `pcnt_*` 前缀(与 5.x 文档的 `pulse_cnt_*` 不同),
  毛刺滤波经 `pcnt_unit_set_glitch_filter()` 单独配置,已适配。
- **主机测试**:`test_ctrl_logic` +14、`test_ctrl_protocol` +64 断言(状态判定、
  载荷构建、主题索引、命令严格校验),合计 4191 → **4269** 断言全绿。
- **实机验证(COM4 刷写后)**:
  1. 启动日志:`风扇PWM已初始化: GPIO5, 25000Hz, 10-bit`、`风扇测速已初始化: GPIO6,
     2脉冲/转, 1s窗口`、订阅 `fans/0/command`,discovery 含 `fans` 能力段;
  2. **MQTT E2E**(自研原始 socket 客户端,发布 + 订阅同时抓包):
     - 发布 `{"fan":0,"duty":50,"cmd_id":"e2e-fan-1"}` → 应答
       `{"fan":0,"result":"applied","err_code":"ok","cmd_id":"e2e-fan-1"}`;
     - 遥测立即回显 `{"fan":0,"duty":50,"rpm":0,"status":"ok"}`;
     - 3 s 后(风扇本体未接线,rpm 恒 0)正确判定堵转:边沿事件
       `{"event":"fan_stall","device_id":"...","fan":0,"duty":50,"rpm":0}` 发布一次,
       遥测转为 `"status":"stall"` 且持续周期上报、事件不重发;
     - discovery retained 载荷确认含 `fans:[{"index":0,"pwm_freq_hz":25000,...}]`
       与 `fan_telemetry/fan_command/fan_command_result` 主题映射。
  - 结论:**调速、测速框架、堵转在线监测、MQTT 协议全链路已实机验证**;
    实际转速测量(RPM>0)待风扇本体接线后确认。

### 4.7 风扇失控空转(overrun)状态修复

- **问题**(实机反馈):风扇满速旋转,遥测仍报 `{"fan":0,"duty":0,"rpm":840,"status":"off"}`
  ——duty=0 与持续 rpm>0 矛盾。原判定 `duty==0 → off` 不看 rpm,把持续异常旋转
  掩盖成"正常停转";硬件层成因是 PWM 信号未生效(风扇满速证明 PWM 线未接/接错或
  实为 3 线风扇),TACH 测速链路正常。
- **改动**:`ctrl_fan_status_eval()` 新增第 4 种状态 `overrun`(duty=0 但持续 3 s
  (`CTRL_FAN_SPINDOWN_MS`)以上 rpm>0,PWM 未生效);停机惯性余转 3 s 宽限内仍报
  `off`。`fan_ctrl` 新增 `fan_ctrl_off_spin_ms()` 统计 duty=0 持续旋转时长,
  占空比变化即清零;`temperature_control.c` 增加 `overrun` 边沿 ERROR 日志
  (不发事件,协议 `status` 枚举扩为 `off/ok/stall/overrun`,**Python 服务端需
  适配新枚举值**)。
- **主机测试**:`test_ctrl_logic` +6、`test_ctrl_protocol` +2 断言(惯性宽限边界、
  overrun 判定边界、载荷 `"status":"overrun"` 渲染),合计 4269 → **4277** 断言全绿。
- **硬件排查指引**(软件无法代替正确接线):duty=0 仍满速旋转时检查
  ① PWM 线(蓝)是否接 GPIO5;② 风扇是否为 4 线(3 线风扇仅测速无调速);
  ③ PWM 输入悬空时部分风扇默认全速。详见风扇接入规范 §5。

### 4.8 加热时长上限调整

- **需求**:连续加热上限(`heat_max_ms`)与手动加热上限(`manual_max_ms`)
  均调整为 **2 小时**(`2*60*60*1000` ms)。
- **改动**:`ctrl_set_defaults()` 默认值 30min/15min → 2h/2h;
  手动加热到期仍优雅回落 AUTO + 冷却(不计 `FAULT_TIMEOUT`);
  连续加热满 2h 触发 `FAULT_TIMEOUT`,解除路径不变
  (温度达 `stop` 或 `timeout_retry_ms=10min` 后转冷却 30s)。
- **文档**:DESIGN、MQTT_PROTOCOL(`manual_max_s` 示例 900 → 7200)同步更新。

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
4. **风扇实物验证**(接线见 [风扇接入规范](specs/%23%20ESP32-S3%204线PWM风扇接入规范.md)):
   - 按规范接线(12 V 外部供电 + 共地;TACH 10 kΩ 上拉**必须 3.3 V**);
   - 发布 `{"fan":0,"duty":50}` 确认风扇起转,遥测 `rpm>0`、`status:"ok"`;
   - 堵转告警路径已实机验证(未接线场景),接线后确认 `stall` 消失、恢复时无重复事件;
   - **duty=0 时风扇不得持续满速旋转**:若仍满速,查 PWM 线(蓝)/风扇类型
     (见规范 §5),此时遥测报 `overrun` 而非 `off`。
5. OTA、TLS/mqtt、多传感器扩展:任务书范围外,未实现。

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
│   ├── fan_ctrl/            4 线 PWM 风扇(LEDC 调速 + PCNT 测速 + 堵转检测)
│   └── led_status/          事件驱动状态灯
├── tests/host/              主机单元测试(4277 断言)
├── docs/specs/              需求规范原文 + 4 线 PWM 风扇接入规范
├── partitions.csv           自定义分区表(16MB 单应用大分区)
├── sdkconfig.defaults       默认配置(flash 16MB + 自定义分区表)
└── docs/
    ├── DESIGN.md            详细设计文档
    ├── MQTT_PROTOCOL.md     MQTT 协议使用文档
    ├── REPORT.md            本报告
    └── specs/               需求规范原文(任务书/RGB 灯语/事件机制/MQTT 主题配置)
```
