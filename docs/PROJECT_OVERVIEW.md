# ESP32-S3 智能温控固件 — 项目描述与业务逻辑文档

## 一、项目概述

### 1.1 项目定位

本项目是一个基于 ESP32-S3 微控制器的**智能温控固件**,用于实现:
- **温湿度采集**:通过 SHT40 传感器实时监测环境温湿度
- **迟滞加热控制**:基于温度阈值的自动/手动加热控制
- **远程管理**:通过 MQTT 协议实现设备远程监控与命令下发
- **完整安全保护**:多层安全机制确保设备和人员安全

### 1.2 技术栈

| 层级 | 技术 |
|------|------|
| 硬件平台 | ESP32-S3 (16MB Flash) |
| 固件框架 | ESP-IDF v6.1+ |
| 编程语言 | C (C2x 标准) |
| 通信协议 | MQTT (QoS 0/1) |
| 传感器 | SHT40 (I2C, 高精度温湿度) |
| 执行器 | 继电器 (加热控制) + 4线 PWM 风扇 (可选) |
| 状态指示 | WS2812 RGB LED (板载) |

### 1.3 核心特性

- **纯 C 控制逻辑层**:控制算法与协议解析无硬件依赖,可在主机环境单元测试
- **事件驱动架构**:自定义事件总线,9 种事件类型,32 条历史记录
- **多层安全保护**:传感器故障锁、超温保护、加热超时、冷却机制、安全锁、重启循环保护
- **降级运行能力**:网络断开时本地控制继续工作
- **严格协议校验**:JSON 载荷严格类型检查、范围验证、一致性校验

---

## 二、系统架构

### 2.1 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│                        main (应用层)                         │
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
│  fan_ctrl        4线 PWM 风扇调速/测速                        │
├─────────────────────────────────────────────────────────────┤
│                   ctrl_core (纯 C 可主机测试层)                │
│  ctrl_logic     控制状态机/迟滞/保护逻辑(无硬件依赖)            │
│  ctrl_protocol  严格 JSON 协议编解码(cJSON)                    │
└─────────────────────────────────────────────────────────────┘
```

**设计原则**:所有可在主机验证的决策逻辑集中在 `ctrl_core`,ESP 层只做 I/O 与调度。

### 2.2 核心组件职责

#### 应用层 (main/)

| 模块 | 职责 |
|------|------|
| `app_boot.c` | 启动状态机管理,11 步初始化序列,RTC 内存重启循环保护 |
| `temperature_control.c` | 传感器采样任务、控制任务、命令执行、MQTT 发布集成 |
| `app_health.c` | 三级健康监控(采样/控制/网络),看门狗喂养,故障升级 |
| `main.c` | 应用入口,设备 ID 生成,启动流程编排 |

#### 驱动/服务层 (components/)

| 组件 | 职责 |
|------|------|
| `sensor_sht40` | SHT40 I2C 驱动,高精度测量,CRC-8 校验,超时处理 |
| `relay` | 继电器 GPIO 唯一所有者,安全优先初始化 |
| `app_cfg` | NVS 配置管理,条件性恢复,逐字段校验 |
| `event_manager` | 自定义事件循环,9 种事件分发,32 条历史 |
| `wifi_manager` | WiFi STA 连接,指数退避重连,SoftAP 配网门户 |
| `mqtt_manager` | MQTT 客户端,LWT 可用性,发布泵,退避重连 |
| `device_manager` | 设备就绪广播,系统错误上报,SNTP 时间同步 |
| `led_status` | RGB 状态灯驱动,事件驱动灯语引擎 |
| `fan_ctrl` | LEDC PWM 输出(25kHz),PCNT 转速计数,堵转检测 |

#### 纯逻辑层 (ctrl_core/)

| 模块 | 职责 |
|------|------|
| `ctrl_logic.c` | 迟滞加热逻辑,保护状态机,风扇状态判定,主机可测 |
| `ctrl_protocol.c` | JSON 协议编解码,严格校验,载荷构建,主机可测 |

---

## 三、硬件配置

### 3.1 默认引脚分配

| 外设 | GPIO | 说明 |
|------|------|------|
| 加热继电器 | GPIO4 | 高电平有效,可配置 |
| SHT40 I2C SDA | GPIO8 | 100 kHz |
| SHT40 I2C SCL | GPIO9 | 100 kHz |
| 风扇 PWM | GPIO5 | LEDC 25 kHz, 10-bit 分辨率 |
| 风扇 TACH | GPIO6 | PCNT 计数,上拉必须接 3.3V |
| RGB LED | GPIO48 | 板载 WS2812,单线数据,RMT 驱动 |

### 3.2 分区布局 (16MB Flash)

| 分区 | 类型 | 偏移 | 大小 | 说明 |
|------|------|------|------|------|
| nvs | data | 0x9000 | 24KB | 配置存储 |
| factory | app | 0x10000 | 16320KB | 应用固件(约 16MB) |

### 3.3 设备标识

- **设备 ID**: `s3-thermostat-XXXXXX` (前缀 + STA MAC 后 3 字节)
- **MQTT client_id**: 与设备 ID 相同
- **主题命名空间**: `devices/{device_id}/`
- **SoftAP 配网**: `THERM-SETUP-XXXX` (仅配网模式可见)

---

## 四、核心业务逻辑

### 4.1 启动状态机

启动流程分为 11 个状态,严格按顺序执行:

```
BOOT_SAFE → INIT_NVS → LOAD_CONFIG → INIT_DRIVERS → INIT_CONTROL
          → INIT_LED → INIT_WIFI → DISCOVERY → INIT_MQTT
          → SYNC_STATE → RUNNING
```

#### 关键状态说明

| 状态 | 行为 | 失败处理 |
|------|------|----------|
| `BOOT_SAFE` | **首先**将继电器配置为安全输出(断开),再做任何其他初始化 | — |
| `INIT_NVS` | NVS 初始化,仅在 `NO_FREE_PAGES`/`NEW_VERSION_FOUND` 时擦除 | DEGRADED |
| `LOAD_CONFIG` | 逐字段读取并校验 NVS 配置,非法值回退默认 | DEGRADED |
| `INIT_DRIVERS` | SHT40 I2C 总线初始化 | DEGRADED(禁用加热,仅联网) |
| `INIT_WIFI` | STA 连接,指数退避+抖动;8 次失败后开 SoftAP | DEGRADED(本地控制继续) |
| `SYNC_STATE` | SNTP 同步(10s 上限),拉取远端配置 | DEGRADED |
| `RUNNING` | 正常运行 | RECOVERY / SAFE_LOCKED |

**其他状态**:
- `DEGRADED`: 局部子系统失败时的降级运行
- `RECOVERY`: 看门狗/健康升级触发后的恢复尝试
- `SAFE_LOCKED`: 安全锁死,加热被强制关闭

### 4.2 继电器安全初始化

**输出安全优先原则**:

1. 先计算"安全电平"(断开 = 逻辑 0)
2. **先写输出锁存,后使能 pad 输出** —— 保证使能瞬间输出就是安全电平,无毛刺
3. 之后任何模块不得直接操作该 GPIO

`relay.c` 是继电器 GPIO 的**唯一所有者**,控制层只能通过 `relay_set()/relay_get()` 访问。

### 4.3 传感器采样 (SHT40)

- **采样周期**: 2 秒
- **测量命令**: 高精度 `0xFD`,读 6 字节(温度 T、湿度 RH、各带 CRC-8)
- **CRC 校验**: 多项式 0x31,失败则本样本标记 `valid=false`
- **超时处理**: 单次读 100 ms 上限
- **温度换算**: `T = -45 + 175 * (raw / 65535)`
- **范围钳制**: `[-40°C, 125°C]`,越界视为无效样本

**关键规则**: 无效样本绝不产生加热决策。

### 4.4 迟滞加热控制

#### 迟滞逻辑

```
温度 < start (35°C) → 请求加热
温度 > stop  (37°C) → 停止加热
start ≤ 温度 ≤ stop → 保持原状态(迟滞区间)
```

#### 阈值校验

- `start < stop`
- 均在 `[-40, 125]` 范围内
- `stop ≤ overtemp (40°C)`
- 均为有限数(NaN/Inf 拒绝)
- 校验失败不改变现有阈值(原子性:两个值同时生效)

#### 运行模式

| 模式 | 行为 |
|------|------|
| `AUTO` | 迟滞逻辑自动控制 |
| `MANUAL_ON` | 强制加热,**必须**携带可验证的 `expires_at`;最长 3 小时 |
| `MANUAL_OFF` | 强制停止 |

### 4.5 保护状态机

保护状态优先级高于一切手动/自动请求:

| 保护状态 | 进入条件 | 退出条件 |
|----------|----------|----------|
| `FAULT_SENSOR` | 无效样本/样本超时(>8s) | 连续 3 个有效样本 |
| `FAULT_OVERTEMP` | 温度 ≥ 40°C | 温度 ≤ stop |
| `FAULT_TIMEOUT` | 连续加热超过 3 小时(任何模式) | 停止加热并冷却 10min 后 |
| `COOLDOWN` | 故障退出后的冷却窗口 | 30s 后 |
| `SAFE_LOCKED` | 显式锁定/紧急停机/重启保护锁死 | 锁定满 30min + 温度 ≤ stop + 有效样本,或运维 `auto` 命令显式解锁 |

**关键规则**:
- 任何保护激活时输出恒为断开
- 手动命令返回 `rejected/refused_by_protection`
- 手动加热到期自动回 `AUTO`
- 所有时间用单调时钟(`esp_timer` 毫秒),仅协议过期判断用 SNTP Unix 时间

### 4.6 重启循环保护

使用 RTC 内存存储重启守护数据,跨复位保留:

```c
RTC_DATA_ATTR struct {
    uint32_t magic;        // 数据有效魔数
    uint32_t boot_count;   // 连续启动计数
    uint32_t healthy_flag; // 非 0 = 已稳定运行 5 分钟
    uint32_t lockout;      // 非 0 = 安全锁定生效
} boot_guard;
```

**逻辑**:
- 每次异常复位递增 `boot_count`
- 启动 300s (5分钟) 稳定运行后置位 `healthy_flag`,下次启动清零计数
- `boot_count ≥ 5` (5分钟内连续快速复位) → `lockout` → 启动即 `SAFE_LOCKED`
- 必须等待 `safe_lock_ms` 或运维通过 MQTT `auto` 命令显式解锁

### 4.7 看门狗与健康监控

三级健康指标:
1. 采样任务进度
2. 控制任务进度
3. MQTT/网络 liveness(仅统计,不作为加热条件)

**升级策略**:
- ESP-IDF Task WDT (5s, 不 panic) 由健康监控任务喂狗
- 任务停滞 **10s** → 一级升级: `temperature_control_emergency_off()` (继电器断开 + `SAFE_LOCKED`)
- 停滞 **60s** → 二级升级: `esp_restart()`, 由重启保护接管
- 看门狗喂养采用 **1s 喂养周期**,避免与 5s 超时窗口边界竞争

### 4.8 风扇控制 (可选)

#### PWM 调速

- **频率**: LEDC 25 kHz (风扇 PWM 规范频率,音频之上无啸叫)
- **分辨率**: 10-bit (0-1023)
- **占空比**: 0-100%
- **GPIO**: GPIO5 (可配置)

#### 转速测量

- **方法**: PCNT 脉冲计数
- **GPIO**: GPIO6 (可配置)
- **窗口**: 1s 定时窗口
- **换算**: `rpm = 脉冲数 × 60 / 2` (2 脉冲/转)
- **消隐**: 硬件毛刺滤波 1μs

#### 状态判定

| 状态 | 条件 | 说明 |
|------|------|------|
| `off` | duty=0 且 rpm=0 (或惯性余转在 3s 宽限内) | 正常停转 |
| `ok` | duty>0 且 rpm>0, 或 duty>0 尚在启动宽限期内 | 正常运转 |
| `stall` | duty>0 且持续 3s 无转速脉冲 | 堵转/未接 |
| `overrun` | duty=0 但持续 3s 以上仍有转速 | PWM 未生效(接线问题) |

**安全语义**:
- 仅手动调速,不与温控联动(避免歧义)
- 占空比不持久化,重启回默认 0%
- 堵转事件边沿触发,只发一次

---

## 五、通信协议 (MQTT)

### 5.1 主题结构

设备订阅 11 个主题,方向、QoS、Retained 严格定义:

| 主题 | 方向 | QoS | Retained | 说明 |
|------|------|-----|----------|------|
| `devices/{dev}/sensors/{n}/telemetry` | 上行 | 0 | 否 | 传感器遥测(温湿度+状态) |
| `devices/{dev}/sensors/{n}/command` | 下行 | 0/1 | 否 | 控制命令(严格校验) |
| `devices/{dev}/sensors/{n}/command_result` | 上行 | 1 | 否 | 命令执行结果 |
| `devices/{dev}/fans/{n}/telemetry` | 上行 | 0 | 否 | 风扇遥测(占空比/转速/状态) |
| `devices/{dev}/fans/{n}/command` | 下行 | 0/1 | 否 | 风扇调速命令 |
| `devices/{dev}/fans/{n}/command_result` | 上行 | 1 | 否 | 风扇命令执行结果 |
| `devices/{dev}/state` | 上行 | 1 | 是 | 设备整体状态(含保护/原因) |
| `devices/{dev}/config` | 上行 | 1 | 是 | 当前生效配置 |
| `devices/{dev}/availability` | 上行 | 1 | 是 | `online` / `offline` (LWT) |
| `devices/{dev}/discovery` | 上行 | 1 | 是 | 发现/能力声明(幂等) |
| `devices/{dev}/events` | 上行 | 1 | 否 | 统一设备事件 |

**关键规则**:
- 命令主题与遥测主题严格分离
- 设备级 `devices/{dev}/command` **不订阅、不支持**
- 主题尾段索引必须与载荷字段一致

### 5.2 统一 JSON 载荷

#### 传感器遥测

```json
{
  "sensor": 0,
  "celsius": 35.2,
  "humidity": 45.5,
  "sensor_status": "ok",
  "heating": true,
  "relay_status": "heating",
  "start": 35.0,
  "stop": 37.0,
  "mode": "auto"
}
```

**字段约束**:
- `sensor`: 整数 0..31, 必须等于主题尾段索引
- `celsius`: 有限数, ∈ [-40, 125]; bool/NaN/Inf/字符串拒绝
- `humidity`: 数值/null, %RH, ∈ [0, 100]; 故障样本为 `null`
- `sensor_status`: `ok` / `fault`
- `heating`: bool, 与 `relay_status` 严格一致
- `relay_status`: `heating` ↔ `heating:true`; `off` ↔ `heating:false`
- `start` / `stop`: 数值, `start < stop`, 均 ∈ [-40, 125]
- `mode`: `auto` 或 `manual`

#### 风扇遥测

```json
{
  "fan": 0,
  "duty": 50,
  "rpm": 1250,
  "status": "ok"
}
```

**字段约束**:
- `fan`: 整数 0..31, 必须等于主题尾段索引
- `duty`: 整数, PWM 占空比 %, ∈ [0, 100]
- `rpm`: 整数, 实测转速(仅遥测)
- `status`: `off` / `ok` / `stall` / `overrun`

### 5.3 命令类型

#### 传感器命令

| 命令 | 载荷关键字段 | 语义 |
|------|--------------|------|
| 修改阈值 | `sensor`, `start`, `stop` | 原子应用并持久化 |
| 手动开加热 | `sensor`, `mode:"manual"`, `heating:true`, **必须** `expires_at` | 强制加热至过期/上限 |
| 手动关加热 | `sensor`, `mode:"manual"`, `heating:false` | 立即停止并保持手动关 |
| 恢复自动 | `sensor`, `mode:"auto"` | 回到自动;若处于 SAFE_LOCKED 且已满足条件则显式解锁 |

**校验规则** (严格):
1. 非 JSON/非对象/未知字段 → 拒绝
2. `sensor` 缺失、非整数、越界 → 拒绝
3. `sensor` 与主题尾段索引不一致 → 拒绝
4. 数值字段 bool/NaN/Inf/字符串 → 拒绝
5. 只给 `start` 或只给 `stop` → 拒绝
6. `start >= stop` → 拒绝
7. `heating` 与 `relay_status` 不一致 → 拒绝
8. 手动开加热必须携带 `expires_at` → 否则拒绝
9. 时钟未同步(SNTP 未完成)→ 拒绝带 `expires_at` 的命令
10. `expires_at <= now` → 拒绝(过期)
11. `cmd_id` 重复 → 不重复执行

#### 风扇命令

```json
{
  "fan": 0,
  "duty": 50,
  "cmd_id": "f-2024-001"
}
```

**校验规则**:
- `fan`: 必填, 整数 0..31
- `duty`: 必填, 整数 0..100
- `cmd_id`: 可选, 字符串 <48 字节
- 出现 `rpm`、`status` 等未定义字段 → 拒绝

### 5.4 命令结果

每条命令必有一条应答 (QoS1, 不 retained):

```json
{
  "sensor": 0,
  "cmd_id": "c-2024-001",
  "type": "set_thresholds",
  "result": "applied",
  "reason": "ok",
  "start": 35.0,
  "stop": 37.0,
  "mode": "auto"
}
```

- `result`: `applied` / `rejected` / `duplicate`
- `reason`: `ok` 或具体拒绝原因
- `type`: `set_thresholds` / `manual_on` / `manual_off` / `auto`
- `start`/`stop`/`mode`: 执行后的**生效值**回显

### 5.5 设备事件

统一发布到 `devices/{dev}/events`, 以 `event` 字段区分类型:

#### device_ready (就绪广播)

```json
{
  "event": "device_ready",
  "device_id": "s3-thermostat-XXXXXX",
  "uptime_s": 1234
}
```

每次 MQTT 重连后幂等重发。

#### system_error (系统错误)

```json
{
  "event": "system_error",
  "device_id": "...",
  "error": 259,
  "source": "sensor"
}
```

当前来源: `sensor` (SHT40 初始化失败)。

#### fan_stall (风扇堵转告警)

```json
{
  "event": "fan_stall",
  "device_id": "...",
  "fan": 0,
  "duty": 50,
  "rpm": 0
}
```

仅在状态**进入** `stall` 瞬间发布一次(边沿触发)。

---

## 六、任务结构

### 6.1 FreeRTOS 任务

| 任务 | 周期/触发 | 职责 | 优先级 |
|------|-----------|------|--------|
| `sensor_task` | 2s | 读 SHT40 → 有界队列(满则丢最旧) | 5 |
| `control_task` | 1s | 取样本→控制逻辑;取命令→严格解析→执行;定时发布 state/config | 6 |
| `health_task` | 1s | 检查进度计数,喂狗/升级 | 12 |
| `led_task` | 事件驱动 | 按 `led_pattern` 引擎驱动板载 WS2812 | 3 |
| MQTT 事件/监督 | 事件驱动 | 发布泵(唯一 `enqueue` 调用者)、退避重连 | 4 |
| WiFi 事件 | 事件驱动 | 连接/断开/配网,只发事件不越权 | 4 |

### 6.2 任务间通信

- **采样队列**: 有界队列,满则丢最旧样本
- **命令队列**: MQTT 回调只把命令复制进有界队列,绝不触碰继电器
- **事件总线**: 自定义事件循环,9 种事件类型
- **发布泵**: 唯一 `esp_mqtt_client_enqueue` 调用者,线程安全

---

## 七、配置管理

### 7.1 NVS 配置

命名空间: `therm`

**字段**:
- `wifi_ssid`: 1..31 字节
- `wifi_pass`: 0 或 8..63 字节
- `mqtt_uri`: 以 `mqtt://` 开头
- `start_c` / `stop_c`: 范围校验 + 迟滞关系校验

**条件性恢复**:
- 仅当 `nvs_flash_init` 返回 `NO_FREE_PAGES`/`NEW_VERSION_FOUND` 才擦除 NVS
- 其余错误原样上报,**绝不无条件擦除**
- WiFi 凭据写入后**读回校验**(write-verify)

### 7.2 Kconfig 配置

通过 `menuconfig → App Configuration` 可配置:

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `APP_RELAY_GPIO` | 4 | 继电器 GPIO |
| `APP_RELAY_ACTIVE_HIGH` | y | 高电平有效 |
| `APP_I2C_SDA_GPIO` | 8 | SHT40 SDA |
| `APP_I2C_SCL_GPIO` | 9 | SHT40 SCL |
| `APP_MQTT_URI_DEFAULT` | `mqtt://192.168.31.242:1883` | Broker 地址 |
| `APP_DEVICE_ID_PREFIX` | `s3-thermostat` | 设备 ID 前缀 |
| `APP_LED_WS2812_GPIO` | 48 | RGB LED GPIO |
| `APP_LED_WS2812_BRIGHTNESS` | 64 | LED 亮度 |
| `APP_FAN_PWM_GPIO` | 5 | 风扇 PWM GPIO |
| `APP_FAN_TACH_GPIO` | 6 | 风扇 TACH GPIO |
| `APP_FAN_DEFAULT_DUTY` | 0 | 风扇默认占空比 |

---

## 八、网络层

### 8.1 WiFi (wifi_manager)

- **STA 模式**: 连接配置的 SSID/密码
- **断线重连**: 指数退避 + 随机抖动(避免上电风暴)
- **配网门户**: 连续 8 次失败后开启 SoftAP
  - SSID: `THERM-SETUP-XXXX` (MAC 后 2 字节)
  - 密码: `setup-1234`
  - HTTP 服务: `GET /` 配置表单, `POST /save` 写入配置后复位
- **关键特性**: WiFi 断开不影响本地控制(控制任务不依赖网络)

### 8.2 MQTT (mqtt_manager)

- **LWT**: 断线由 broker 发布 retained `offline`;连接成功发布 retained `online`
- **重连**: 关闭 SDK 自动重连,由监督任务做**退避+抖动**重连
- **发布**: 使用 `esp_mqtt_client_enqueue`(线程安全)
- **SNTP**: 连接后启动,10s 内同步成功则启用 `expires_at` 校验
- **安全**: 同步前**拒绝**带强制加热语义的命令(`clock_unsynced`)

---

## 九、RGB 状态灯

### 9.1 灯语引擎

三层拆分,**只有 LED 任务触碰 LED 硬件**:

| 层 | 文件 | 职责 |
|----|------|------|
| 灯语引擎(纯 C) | `led_pattern.c/h` | 状态→颜色/闪烁时序转换、优先级解析;无硬件依赖,主机测试 3141 断言 |
| 灯任务 | `led_status.c` | FreeRTOS 任务消费引擎输出,定时翻转;事件侧写入者 |
| 硬件后端 | `led_status.c` (led_strip RMT) | 板载 WS2812 单线地址灯,GRB 分量序,亮度 64 |

### 9.2 优先级

| 优先级 | 状态 | 颜色/时序 |
|--------|------|-----------|
| L6 (最高) | 故障 | 红色 150/150/150/150/150/900 |
| L3 | 全断(无 WiFi/MQTT/传感器) | 黄色 |
| L1 | WiFi 连接中 | 蓝色 200/200 双闪 |
| L2 | 待 MQTT | 紫色 |
| L0 | 初始化 | 蓝色 |
| L5 | 加热中 | 绿色慢闪 |
| L4 (最低) | 就绪 | 绿色常亮 |

---

## 十、测试策略

### 10.1 主机单元测试

**工具链**: `zig cc` (zig 0.16.0)

**测试套件**:
- `test_ctrl_logic`: 控制逻辑测试 (978 断言)
- `test_ctrl_protocol`: 协议解析测试 (158 断言)
- `test_led_pattern`: LED 灯语引擎测试 (3141 断言)
- **合计**: 4277 断言, 0 失败

**核心覆盖**:
- 迟滞逻辑(开/关/保持)
- 阈值校验(NaN/Inf/越界/start≥stop)
- 传感器故障(无效样本/样本过期/恢复)
- 超温保护及退出
- 手动模式(强制开/关/过期)
- 加热超时保护及冷却重试
- SAFE_LOCKED 全面封锁及解锁
- 统一载荷构建与字段校验
- 主题索引提取与匹配
- 严格类型检查(bool/NaN/Inf/字符串拒绝)
- 过期规则(missing/clock_unsynced/expired/too_far)
- 风扇状态判定(off/ok/stall/overrun)
- 风扇载荷构建/回显
- 风扇命令严格校验

### 10.2 固件构建

```powershell
idf.py set-target esp32s3
idf.py build
```

**验证**:
- 编译: 0 错误 0 警告
- 二进制大小: 约 1019 KB (app 分区 16320KB, 余 94%)

### 10.3 实机验证

**已验证**:
- 启动状态机完整走完至 RUNNING
- SHT40 在位(有效采样驱动迟滞加热)
- 继电器安全初始化
- WiFi 连接与 MQTT 连接
- 风扇命令端到端(命令→应答→遥测→堵转事件)
- RGB 状态灯初始化

**待验证**:
- 断电恢复/SAFE_LOCKED 冒烟
- 真实传感器/继电器长时间运行
- 温控命令往返联调

---

## 十一、安全机制总结

### 11.1 多层保护

1. **继电器安全初始化**: 上电即断开,无毛刺
2. **传感器故障锁**: 无效样本立即禁热
3. **超温保护**: 40°C 硬限制
4. **加热超时**: 连续加热 3 小时强制停止
5. **冷却机制**: 故障后 30s 冷却窗口
6. **安全锁**: 显式锁定/紧急停机/重启保护
7. **重启循环保护**: 5 分钟内连续 5 次复位 → 锁定 30 分钟
8. **看门狗**: 10s 停滞 → 紧急停机,60s → 复位

### 11.2 协议安全

1. **严格类型检查**: bool/NaN/Inf/字符串拒绝
2. **范围验证**: 温度、湿度、阈值范围校验
3. **一致性校验**: `heating` 与 `relay_status` 必须一致
4. **主题匹配**: 载荷索引必须与主题索引一致
5. **时钟同步**: SNTP 未同步前拒绝带 `expires_at` 的命令
6. **过期检查**: `expires_at` 必须 > now 且 < now+24h
7. **命令去重**: `cmd_id` 重复不重复执行
8. **保护优先**: 任何保护激活时手动加热请求被拒

### 11.3 运行安全

1. **降级运行**: 网络断开时本地控制继续
2. **唯一写入者**: 继电器 GPIO 只有一个模块可写
3. **有界队列**: 满则丢最旧,不阻塞
4. **故障有界**: 不无限重试同一失败路径
5. **幂等发布**: discovery/availability 幂等重发

---

## 十二、运维注意事项

### 12.1 MQTT 最佳实践

1. **避免 retained 命令**: 命令主题请勿发布 retained 强制加热
2. **携带 cmd_id**: 为命令携带 `cmd_id`,重发安全
3. **QoS1**: 重要命令使用 QoS1
4. **保护优先**: 任何保护激活时手动加热请求都会被拒并回报原因

### 12.2 硬件排查

1. **上电瞬间**: 确认继电器先处于断开状态
2. **传感器读数**: 对比参考温度计,±2°C 内为正常
3. **手动断开传感器**: 确认加热停止且上报 `fault_sensor`
4. **断开 WiFi**: 确认本地迟滞控制仍工作
5. **快速复位 5 次**: 确认进入 SAFE_LOCKED
6. **风扇堵转**: 检查接线,PWM 线是否接 GPIO5,风扇是否为 4 线

### 12.3 日志监控

- **启动进度**: `[n/11]: 中文说明 (boot state -> 状态名)`
- **保护状态**: `保护状态进入/解除: 旧 -> 新`
- **命令执行**: 每条命令的 `command_result`
- **事件**: `device_ready` / `system_error` / `fan_stall`

---

## 十三、项目文件结构

```
sample_project_s3/
├── main/                          # 应用层
│   ├── main.c                     # 应用入口
│   ├── app_boot.c/h               # 启动状态机
│   ├── app_health.c/h             # 健康监控
│   └── temperature_control.c/h    # 温控集成
├── components/                    # 驱动/服务层
│   ├── app_cfg/                   # NVS 配置管理
│   ├── ctrl_core/                 # 纯 C 控制逻辑(主机可测)
│   │   ├── ctrl_logic.c/h         # 控制状态机/保护逻辑
│   │   ├── ctrl_protocol.c/h      # JSON 协议编解码
│   │   └── ctrl_types.h           # 类型定义
│   ├── device_manager/            # 设备管理/SNTP
│   ├── event_manager/             # 事件总线
│   ├── fan_ctrl/                  # 风扇调速/测速
│   ├── led_status/                # RGB 状态灯
│   │   ├── led_status.c           # 灯任务+硬件后端
│   │   └── led_pattern.c          # 灯语引擎(纯 C)
│   ├── mqtt_manager/              # MQTT 客户端
│   ├── relay/                     # 继电器驱动
│   ├── sensor_sht40/              # SHT40 驱动
│   └── wifi_manager/              # WiFi 管理
├── tests/                         # 测试
│   └── host/                      # 主机单元测试
├── docs/                          # 文档
│   ├── DESIGN.md                  # 详细设计
│   ├── MQTT_PROTOCOL.md           # MQTT 协议
│   ├── REPORT.md                  # 测试报告
│   └── specs/                     # 需求规范
├── CMakeLists.txt                 # 构建配置
├── partitions.csv                 # 分区表
├── sdkconfig.defaults             # 默认配置
└── README.md                      # 项目说明
```

---

## 十四、总结

本项目是一个**工业级智能温控固件**,具备以下核心优势:

1. **安全可靠**: 多层安全保护机制,确保设备和人员安全
2. **可测试性**: 纯 C 控制逻辑层,主机单元测试覆盖 4277 断言
3. **可维护性**: 清晰的分层架构,模块化设计
4. **可观测性**: 完整的日志、事件、遥测上报
5. **灵活性**: 丰富的 Kconfig 配置,支持多种硬件配置
6. **鲁棒性**: 降级运行能力,网络断开时本地控制继续

固件已通过编译、主机测试、部分实机验证,可用于进一步的产品化开发。
