# ESP32-S3 智能温控固件 — 详细设计文档

## 1. 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                        main (app 层)                         │
│  app_boot           启动状态机 / RTC 重启保护                  │
│  temperature_control 采样任务 + 控制任务 + 命令执行             │
│  app_health         健康监控 / 看门狗喂养 / 升级处理            │
├─────────────────────────────────────────────────────────────┤
│                     components (驱动/服务层)                   │
│  sensor_sht40    SHT40 I2C 驱动(超时+CRC)                    │
│  relay           继电器 GPIO 唯一写入者                        │
│  app_cfg         NVS 配置(条件性恢复)                         │
│  event_manager   APP_EVENT 统一事件层(9 事件+32 条历史)        │
│  wifi_manager    WiFi STA + SoftAP 配网(只发事件)             │
│  mqtt_manager    MQTT 客户端 + 发布泵 + 单一重连所有者          │
│  device_manager  DEVICE_READY/SYSTEM_ERROR 广播 + SNTP        │
│  led_status      事件驱动状态灯(板载 WS2812,GPIO48)          │
├─────────────────────────────────────────────────────────────┤
│                   ctrl_core (纯 C 可主机测试层)                │
│  ctrl_logic     控制状态机/迟滞/保护逻辑(无硬件依赖)            │
│  ctrl_protocol  严格 JSON 协议编解码(cJSON)                    │
└─────────────────────────────────────────────────────────────┘
```

设计原则:**所有可在主机验证的决策逻辑集中在 `ctrl_core`**,ESP 层只做 I/O 与调度。
控制逻辑与协议解析均为纯 C,无 FreeRTOS/ESP-IDF 头文件依赖,主机单元测试直接编译同源代码。

## 2. 启动状态机

状态序列(正常路径):

```
BOOT_SAFE → INIT_NVS → LOAD_CONFIG → INIT_DRIVERS → INIT_CONTROL
          → INIT_LED → INIT_WIFI → DISCOVERY → INIT_MQTT
          → SYNC_STATE → RUNNING
```

| 状态 | 行为 | 失败去向 |
|------|------|----------|
| `BOOT_SAFE` | **先**把继电器配置为安全输出(断开),再做任何其他初始化 | — |
| `INIT_NVS` | `nvs_flash_init`;仅在 `ESP_ERR_NVS_NO_FREE_PAGES` / `NEW_VERSION_FOUND` 时擦除 | DEGRADED(用默认配置) |
| `LOAD_CONFIG` | 逐字段读取并校验 NVS;非法字段回退默认值 | DEGRADED |
| `INIT_DRIVERS` | SHT40 I2C 总线初始化 | DEGRADED(禁用加热,仅联网上报) |
| `INIT_CONTROL` | 控制状态机初始化(默认 AUTO 模式) | — |
| `INIT_WIFI` | STA 连接,指数退避+抖动;8 次失败后开 SoftAP 配网页 | DEGRADED(本地控制继续) |
| `DISCOVERY` | SoftAP 检查(非阻塞) | — |
| `INIT_MQTT` | MQTT 连接 + LWT;断网不阻塞控制 | DEGRADED |
| `SYNC_STATE` | SNTP 同步(10s 上限);拉取远端配置(失败用本地) | DEGRADED |
| `RUNNING` | 正常运行 | RECOVERY / SAFE_LOCKED |

其他状态:
- **DEGRADED**:局部子系统失败时的降级运行;继电器保持安全,本地控制(若有传感器)继续。
- **RECOVERY**:看门狗/健康升级触发后的恢复尝试。
- **SAFE_LOCKED**:安全锁死,加热被强制关闭,需满足解锁条件(见 §6)。

状态迁移逐条打串口日志(初始化进度 `[n/11]: 中文说明 (boot state -> 状态名)`),
并记录复位原因(esp_reset_reason)。

## 3. 继电器安全初始化

**输出安全优先原则**:`app_boot` 在 `BOOT_SAFE` 阶段调用 `relay_init_safe()`,该函数:

1. 先计算"安全电平"(断开 = 逻辑 0;若 `APP_RELAY_ACTIVE_HIGH=n` 则物理电平取反);
2. **先写输出锁存,后使能 pad 输出**——保证使能瞬间输出就是安全电平,无毛刺;
3. 之后任何模块不得直接操作该 GPIO。

`relay.c` 是继电器 GPIO 的**唯一所有者**;控制层只能通过 `relay_set()/relay_get()`
访问;`temperature_control.c` 中 `apply_output()` 是正常路径唯一写入点,
紧急路径 `temperature_control_emergency_off()` 同样走 relay 模块。

GPIO4、高电平有效(用户确认),由 `menuconfig → App Configuration` 可改。

## 4. 传感器采样(SHT40)

- I2C 引脚:SDA=GPIO8、SCL=GPIO9(用户确认),100 kHz;
- 采样周期 2 s(`sample_period_ms=2000`);
- 高精度测量命令 `0xFD`,读 6 字节:温度 T、湿度 RH、各带 CRC-8(多项式 0x31);
- CRC 失败或 I2C 超时(单次读 100 ms 上限)→ 本样本标记 `valid=false`;
- 连续失败由控制层计入 `invalid_streak`;**无效样本绝不产生加热决策**;
- 温度与湿度均上报遥测;无效样本也上报(`sensor_status":"fault"`,测量值 `null`),
  载荷格式见 [MQTT_PROTOCOL](MQTT_PROTOCOL.md) §3。

温度换算(数据手册公式):
```
T = -45 + 175 * (raw / 65535)
```
合理范围钳制:`min_c=-40 .. max_c=125`,越界视为无效样本。

## 5. 控制逻辑(ctrl_logic,纯 C)

### 5.1 迟滞加热
- `celsius < start`(35°C)→ 请求加热;
- `celsius > stop`(37°C)→ 停止加热;
- 介于两者之间 → 保持原状态(迟滞区间)。

### 5.2 阈值校验(应用前)
`start < stop`、均在 `[-40, 125]`、`stop ≤ overtemp(40)`、均为有限数(NaN/Inf 拒绝)。
校验失败不改变现有阈值(原子性:两个值同时生效)。

### 5.3 模式
| 模式 | 行为 |
|------|------|
| `AUTO` | 迟滞逻辑自动控制 |
| `MANUAL_ON` | 强制加热,**必须**携带可验证的 `expires_at`;最长 `manual_max_ms=2h` |
| `MANUAL_OFF` | 强制停止 |

### 5.4 保护状态(优先级高于一切手动/自动请求)

| 保护 | 进入条件 | 退出条件 |
|------|----------|----------|
| `FAULT_SENSOR` | 无效样本/样本超时(`sensor_stale_ms=8s`) | 连续 3 个有效样本(`sensor_recover_samples`) |
| `FAULT_OVERTEMP` | `celsius ≥ overtemp(40)` | 温度 ≤ `stop` |
| `FAULT_TIMEOUT` | 连续加热超过 `heat_max_ms=2h`(任何模式) | 停止加热并冷却 `timeout_retry_ms=10min` 后 |
| `COOLDOWN` | 故障退出后的冷却窗口 | `cooldown_ms=30s` 后 |
| `SAFE_LOCKED` | 显式锁定/紧急停机/重启保护锁死 | 锁定满 `safe_lock_ms=30min` + 温度 ≤ `stop` + 有效样本,或运维 `auto` 命令显式解锁 |

**任何保护激活时输出恒为断开**,手动命令返回 `rejected/refused_by_protection`。
`manual_deadline_ms` 到期自动回 `AUTO`。所有时间用单调时钟(`esp_timer` 毫秒),
仅协议过期判断用 SNTP Unix 时间。

**保护状态串口日志**:启动时打印初始保护状态;此后任何保护状态变更在控制任务
以 1 s 轮询边沿检测并打印(`保护状态进入/解除: 旧 -> 新`,进入故障/安全锁用 E、
冷却用 W、解除用 I),覆盖 tick 自动迁移、命令触发与紧急停机全部路径。

### 5.5 采样有效性
- `sample_valid=false` 时:禁止加热,已有加热立即停止;
- 无有效样本从未发生 → 永不加热(测试用例 10 验证)。

## 6. 重启循环保护(app_boot,RTC 内存)

```c
RTC_DATA_ATTR struct { magic, boot_count, healthy_flag, lockout } boot_guard;
```

- 每次**异常复位**(SW reset 除外的非预期路径)递增 `boot_count`;
- `healthy_flag`:启动 300 s(5 分钟)稳定运行后置位,下次启动清零计数;
- `boot_count ≥ 5`(5 分钟内连续快速复位)→ `lockout` → 启动即 `SAFE_LOCKED`,
  必须等待 `safe_lock_ms` 或运维通过 MQTT `auto` 命令显式解锁(解锁同时清空重启保护);
- 正常上电/深睡唤醒/未知复位会重置计数器。

## 7. 看门狗与健康监控(app_health)

三级健康指标(每个任务定期上报进度计数):
1. 采样任务进度;
2. 控制任务进度;
3. MQTT/网络 liveness(仅统计,不作为加热条件)。

- ESP-IDF Task WDT(5 s,不 panic)由健康监控任务喂狗;**只有进度计数前进才喂狗**;
- 任务停滞 **10 s** → 一级升级:`temperature_control_emergency_off()`(继电器断开 + `SAFE_LOCKED`);
- 停滞 **60 s** → 二级升级:`esp_restart()`,由重启保护接管;
- 故障有界处理,不无限重试同一失败路径;
- 看门狗喂养采用 **1 s 喂养周期**(每个周期结束喂狗),避免与 5 s 超时窗口边界竞争
  (修复初版固件中喂狗点恰好落在超时边界导致误报 WDT 的缺陷)。

## 8. 网络层(事件驱动)

### 8.1 WiFi(wifi_manager)
- STA 模式连接配置的 SSID/密码;
- 断线重连:**指数退避 + 随机抖动**(避免上电风暴);单次连接等待有上限;
- 连续 8 次失败 → 开启 SoftAP 配网:
  - SSID:`THERM-SETUP-XXXX`(MAC 后 2 字节),密码 `setup-1234`;
  - 内置 HTTP 服务:`GET /` 配置表单,`POST /save` 写入 WiFi/MQTT 配置(写后读校验)后复位;
- **WiFi 断开不影响本地控制**(控制任务不依赖网络)。

### 8.2 MQTT(mqtt_manager)
- esp-mqtt 组件(Component Registry `espressif/mqtt`);
- LWT:断线由 broker 发布 retained `offline`;连接成功发布 retained `online`
  (幂等的发现/可用性公告,QoS1);
- 关闭 SDK 自动重连,由监督任务做**退避+抖动**重连;
- MQTT 回调**只把命令复制进有界队列**,绝不触碰继电器;
- 发布用 `esp_mqtt_client_enqueue`(线程安全);
- SNTP:连接后启动,10 s 内同步成功则启用 `expires_at` 校验;
  同步前**拒绝**带强制加热语义的命令(`clock_unsynced`)。

## 9. 配置管理(app_cfg)

NVS 命名空间 `therm`,字段逐个读取并校验,非法值回退默认:
- `wifi_ssid`(1..31)、`wifi_pass`(0 或 8..63)、`mqtt_uri`(以 `mqtt://` 开头);
- `start_c`/`stop_c`(范围校验 + 迟滞关系校验);
- **条件性恢复**:仅当 `nvs_flash_init` 返回 `NO_FREE_PAGES`/`NEW_VERSION_FOUND`
  才擦除 NVS;其余错误原样上报,**绝不无条件擦除**;
- WiFi 凭据写入后**读回校验**(write-verify),失败返回错误;
- 阈值修改立即持久化。

Kconfig(`menuconfig → App Configuration`):`APP_RELAY_GPIO=4`、
`APP_RELAY_ACTIVE_HIGH=y`、`APP_I2C_SDA_GPIO=8`、`APP_I2C_SCL_GPIO=9`、
`APP_MQTT_URI_DEFAULT=mqtt://192.168.31.242:1883`、`APP_DEVICE_ID_PREFIX=s3-thermostat`。

## 10. 任务结构

| 任务 | 周期/触发 | 职责 |
|------|-----------|------|
| `sensor_task` | 2 s | 读 SHT40 → 有界队列(满则丢最旧) |
| `control_task` | 1 s | 取样本→`ctrl_on_sample`;取命令→严格解析→执行;`ctrl_tick`;`apply_output()`(唯一继电器写入点);定时发布 state/config |
| `health_task` | 1 s | 检查进度计数,喂狗/升级 |
| `led_task` | 事件驱动 | `led_status`:按 `led_pattern` 引擎驱动板载 WS2812;事件侧唯一 LED 硬件写入者 |
| MQTT 事件/监督 | 事件驱动 | `mqtt_manager`:发布泵(唯一 `enqueue` 调用者)、退避重连(单一所有者) |
| WiFi 事件 | 事件驱动 | `wifi_manager`:连接/断开/配网,只发事件不越权 |
| APP_EVENT 事件循环 | 事件驱动 | `event_manager`:自定义事件循环,9 事件分发 + 32 条历史 |

## 11. 构建与测试

- 目标芯片 ESP32-S3,IDF v6.1;构建/烧录命令见 [README](../README.md)「构建与烧录」;
- Flash 16MB,自定义分区表 `partitions.csv`(nvs 24KB + factory 16320KB 单应用大分区),
  布局速查见 README「分区布局」;`sdkconfig.defaults` 固化 flash 容量与分区表选择;
- 依赖(`idf_component.yml`,Component Registry):`espressif/cjson`、`espressif/mqtt`
  (IDF v6.x 起二者不再随 IDF 树分发)、`espressif/led_strip` ^3.1.0
  (WS2812 驱动;2.5.x 在 IDF v6.1 编译失败,3.1.0 起修复,实装 3.1.1);
- 主机单元测试(`tests/host/`,`zig cc` 同源编译 `ctrl_logic`/`ctrl_protocol`/`led_pattern` + cJSON):
  命令见 [README](../README.md)「主机单元测试」,结果与用例覆盖见 [REPORT](REPORT.md) §4.1。

## 12. RGB 状态灯(led_status,依[《RGB 指示灯状态规范》V1.0](<specs/%23 ESP32-S3 温控设备 RGB 指示灯状态规范.md>))

三层拆分,**只有 LED 任务触碰 LED 硬件**(规范 §6):

| 层 | 文件 | 职责 |
|----|------|------|
| 灯语引擎(纯 C) | `led_pattern.c/h` | 状态→颜色/闪烁时序转换、优先级解析;无硬件依赖,主机测试 3141 断言 |
| 灯任务 | `led_status.c` | FreeRTOS 任务消费引擎输出,定时翻转;事件侧写入者 |
| 硬件后端 | `led_status.c`(led_strip RMT) | 板载 WS2812 单线地址灯,GRB 分量序,亮度 64 |

- **状态输入**:WiFi(三态:CONNECTING/GOT_IP/DOWN)、MQTT(连接/断开)、传感器
  (OK/FAULT,UNKNOWN 起始)、设备 ready、`control_active`(迟滞加热中);
- **优先级**(规范 §4):L6 故障红 > L3 全断黄 > L1 WiFi 连接中蓝 > L2 待 MQTT 紫
  > L0 初始化蓝 > L5 加热绿闪 > L4 就绪绿常亮;
- **硬件配置**:`APP_LED_WS2812_GPIO=48`(DevKitC-1 板载 RGB 数据脚)、
  `APP_LED_WS2812_BRIGHTNESS=64`;GPIO 配 -1 编译为 no-op 桩(`LED_STATUS_NOOP`),
  便于移植到无 RGB 灯板型;
- **合规**:规范 §5 要求 WS2812 用专用驱动(禁三路独立 GPIO 色彩方案),
  故采用 `espressif/led_strip` 3.1.1(RMT 后端)。

## 13. 风扇调速与测速(fan_ctrl,4 线 PWM 风扇)

依[《4 线 PWM 风扇接入规范》](<specs/%23 ESP32-S3 4线PWM风扇接入规范.md>)实现,
接线见规范 §3(TACH 上拉必须接 3.3 V,**严禁 5 V**)。分层与温控控制回路一致:

| 层 | 文件 | 职责 |
|----|------|------|
| 状态判定(纯 C) | `ctrl_logic.c` `ctrl_fan_status_eval()` | duty/rpm/持续时间→`off/ok/stall/overrun`,主机测试覆盖 |
| 协议(纯 C) | `ctrl_protocol.c` | 风扇命令解析/主题索引/遥测与应答载荷构建 |
| 硬件驱动 | `fan_ctrl.c/h` | LEDC PWM 输出 + PCNT 转速计数,1 s 测速窗口 |
| 集成 | `temperature_control.c` | 遥测节流、命令路由、堵转事件上报、失控空转告警 |

- **PWM 调速**:LEDC 25 kHz(风扇 PWM 规范频率)、10-bit 分辨率
  (`APP_FAN_PWM_GPIO=5`),占空比 0..100%;25 kHz 在音频之上,无啸叫;
- **转速测量**:PCNT 脉冲计数(`APP_FAN_TACH_GPIO=6`,2 脉冲/转,开漏输出
  需 10 kΩ 上拉至 3.3 V),1 s 定时窗口取计数换算
  `rpm = 脉冲数 × 60 / 2`,硬件消隐滤波 1 μs;
- **堵转判定**(`CTRL_FAN_STALL_MS=3000`):占空比 >0 持续 3 s 无脉冲 →
  `stall`(边沿触发 `fan_stall` 事件);启动 3 s 宽限期内 `rpm=0` 仍报 `ok`;
- **停机惯性与失控空转**(`CTRL_FAN_SPINDOWN_MS=3000`):duty=0 时 `rpm>0`
  在 3 s 停机惯性宽限内仍报 `off`;宽限期后仍持续旋转 → `overrun`
  (PWM 未生效——接线/风扇类型问题,边沿 ERROR 日志,不谎报 `off`);
- **安全语义**:仅手动调速(不与温控联动,避免歧义);占空比不持久化,
  重启回 Kconfig 初始值(默认 0%);风扇未启用(引脚 -1)时驱动不初始化、
  无遥测,命令应答 `pwm_disabled`;
- **MQTT**:独立命名空间 `fans/0/*`(见 [MQTT_PROTOCOL](MQTT_PROTOCOL.md)
  §2.1/§3.1/§4.3),遥测 5 s 周期 + 变化即发,堵转事件走统一 `events` 主题;
- **API 兼容**:IDF v6.1 PCNT 驱动为 `pcnt_*` 前缀
  (头文件仍为 `driver/pulse_cnt.h`),毛刺滤波经
  `pcnt_unit_set_glitch_filter()` 单独配置。
