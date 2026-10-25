# MQTT 协议使用文档

设备 ID:`s3-thermostat-XXXXXX`(Kconfig 前缀 + STA MAC 后 3 字节)。
下文以 `dev` 代指设备 ID,`n` 代指传感器索引(当前固件为 `0`)。

## 1. 主题总览

| 主题 | 方向 | QoS | Retained | 说明 |
|------|------|-----|----------|------|
| `devices/{dev}/sensors/{n}/telemetry` | 上行 | 0 | 否 | 传感器遥测,每次有效采样一条 |
| `devices/{dev}/sensors/{n}/command` | 下行 | 0/1 | 可 | 控制命令(严格校验) |
| `devices/{dev}/sensors/{n}/command_result` | 上行 | 1 | 否 | 命令执行结果 |
| `devices/{dev}/state` | 上行 | 1 | 是 | 设备整体状态(含保护/原因) |
| `devices/{dev}/config` | 上行 | 1 | 是 | 当前生效配置 |
| `devices/{dev}/availability` | 上行 | 1 | 是 | `online` / `offline`(LWT) |
| `devices/{dev}/discovery` | 上行 | 1 | 是 | 发现/能力声明(幂等) |
| `devices/{dev}/events/device_ready` | 上行 | 1 | 是 | 本地就绪广播(重连后幂等重发) |
| `devices/{dev}/events/system_error` | 上行 | 1 | 是 | 系统错误(如传感器初始化失败) |

**命令主题与遥测主题严格分离**;`command` 的主题尾段索引必须与载荷 `sensor` 字段一致。

## 2. 统一 JSON 载荷

所有传感器相关消息**只使用**如下统一字段集(未知字段会被拒绝):

```json
{
  "sensor": 0,
  "celsius": 35.2,
  "heating": true,
  "relay_status": "heating",
  "start": 35.0,
  "stop": 37.0,
  "mode": "auto"
}
```

| 字段 | 类型 | 约束 |
|------|------|------|
| `sensor` | 整数 | 0..31,必须等于主题尾段索引 |
| `celsius` | 数值 | 有限数,∈ [-40, 125];**bool/NaN/Inf/字符串拒绝** |
| `heating` | bool | 与 `relay_status` 严格一致 |
| `relay_status` | 字符串 | `heating` ↔ `heating:true`;`off` ↔ `heating:false` |
| `start` / `stop` | 数值 | `start < stop`,均 ∈ [-40, 125];必须成对出现 |
| `mode` | 字符串 | `auto` 或 `manual` |

规则:
- JSON 解析失败、非对象、出现未知字段 → 拒绝;
- `heating` 与 `relay_status` 语义必须一致,冲突即拒绝(任何消息都不允许自相矛盾);
- 遥测中的 `start`/`stop` 是**当前生效阈值**(含应用后的回显)。

## 3. 遥测(telemetry)

每次有效采样(2 s 周期)发布,QoS0、不 retained:

```json
{"sensor":0,"celsius":35.2,"heating":true,"relay_status":"heating",
 "start":35.0,"stop":37.0,"mode":"auto"}
```

无效样本(超时/CRC 错/越界)**不产生**遥测,并禁止加热(见 state 的 `sample_valid`)。

## 4. 命令(command)

命令在 `devices/{dev}/sensors/{n}/command` 接收。载荷是统一字段集的**子集**,
命令语义由字段组合推导(不允许出现未定义的组合):

| 命令 | 载荷关键字段 | 语义 |
|------|--------------|------|
| 修改阈值 | `sensor`,`start`,`stop` | 原子应用并持久化 |
| 手动开加热 | `sensor`,`mode:"manual"`,`heating:true`,**必须** `expires_at` | 强制加热至过期/上限 |
| 手动关加热 | `sensor`,`mode:"manual"`,`heating:false` | 立即停止并保持手动关 |
| 恢复自动 | `sensor`,`mode:"auto"` | 回到自动;若处于 SAFE_LOCKED 且已满足条件则显式解锁 |

可选字段:`cmd_id`(字符串,<48 字节,幂等去重)、`expires_at`(Unix 秒,整数)。

### 4.1 校验规则(全部严格)

1. 非 JSON/非对象/未知字段 → `malformed_json` / `not_object` / `unknown_field`;
2. `sensor` 缺失、非整数、越界 → `bad_sensor`;
3. `sensor` 与主题尾段索引不一致 → `sensor_topic_mismatch`;
4. 数值字段 bool/NaN/Inf/字符串 → `bad_thresholds` / `bad_celsius`;
5. 只给 `start` 或只给 `stop` → `thresholds_incomplete`;
6. `start >= stop` → `start_not_below_stop`;越界 → `threshold_out_of_range`;
7. `heating` 与 `relay_status` 不一致 → `heating_relay_mismatch`;
8. `heating` 出现但 `mode != "manual"` → `heating_requires_manual_mode`;
9. 无任何动作字段 → `no_action`;
10. **手动开加热必须携带 `expires_at`** → 否则 `missing_expires_at`;
11. `expires_at` 非整数 → `bad_expires_at`;
12. 时钟未同步(SNTP 未完成或时间 < 2020-09-13)→ `clock_unsynced`
    (含 `expires_at` 的命令一律拒绝,防止过期 retained 命令复活加热);
13. `expires_at <= now` → `expired`;超过 now+24h → `expires_too_far`;
14. 其他命令携带 `expires_at` 时同样在**执行时刻**复查过期(排队延迟防护);
15. `cmd_id` 重复 → `duplicate`(不重复执行);
16. 保护状态优先:手动开加热被保护拒绝 → `rejected/refused_by_protection`。

### 4.2 示例

修改阈值:
```json
{"sensor":0,"start":35.0,"stop":37.0,"cmd_id":"c-2024-001"}
```

手动开加热 10 分钟(必须给过期时间):
```json
{"sensor":0,"mode":"manual","heating":true,
 "expires_at":1735689600,"cmd_id":"c-2024-002"}
```

手动关加热:
```json
{"sensor":0,"mode":"manual","heating":false,"cmd_id":"c-2024-003"}
```

恢复自动(运维解锁 SAFE_LOCKED):
```json
{"sensor":0,"mode":"auto","cmd_id":"c-2024-004"}
```

## 5. 命令结果(command_result)

每条命令必有一条应答(QoS1、不 retained):

```json
{"sensor":0,"cmd_id":"c-2024-001","type":"set_thresholds",
 "result":"applied","reason":"ok","start":35.0,"stop":37.0,"mode":"auto"}
```

- `result`:`applied` / `rejected` / `duplicate`;
- `reason`:`ok` 或具体拒绝原因(与 §4.1 错误码一致);
- `type`:`set_thresholds` / `manual_on` / `manual_off` / `auto`(解析失败时为 `unknown`);
- `start`/`stop`/`mode` 是执行后的**生效值**回显。

## 6. 设备状态(state,retained)

```json
{"sensor":0,"celsius":35.2,"heating":true,"relay_status":"heating",
 "start":35.0,"stop":37.0,"mode":"auto","protection":"none",
 "sample_valid":true,"uptime_s":1234,"rssi":-55,"reason":"periodic"}
```

- `protection`:`none|cooldown|fault_sensor|fault_overtemp|fault_timeout|safe_locked`;
- `reason`:`periodic`(30 s 周期)/ `command` / `reconnect` / `output_change` 等变更原因;
- `sample_valid`:最近样本是否有效。

## 7. 配置(config,retained)

```json
{"sensor":0,"start":35.0,"stop":37.0,"overtemp":40.0,
 "sample_period_s":2.0,"sensor_stale_s":8.0,"manual_max_s":900}
```

## 8. 可用性(availability,retained)

- 连接成功:发布 `online`(QoS1);
- 断线/停机:LWT 由 broker 发布 `offline`(QoS1);
- 消息为纯文本,不是 JSON。

## 9. 发现(discovery,retained)

每次 MQTT 重连后幂等发布,声明设备能力、传感器驱动、继电器语义
(`"semantics":"commanded_state"`——**继电器语义为"指令状态"而非触点反馈**,
无触点反馈硬件,故无法检测触点粘连)以及全部主题名。

## 10. 事件(events,retained QoS1)

- `devices/{dev}/events/device_ready`:
  `{"device_id":"s3-thermostat-XXXXXX","uptime_s":<自上电秒数>}`
  本地初始化完成即视为就绪(不依赖网络);MQTT 每次重连后幂等重发。
- `devices/{dev}/events/system_error`:
  `{"device_id":"...","error":<esp_err_t 码>,"source":"<模块名>"}`
  当前来源:`sensor`(SHT40 初始化失败,设备进入传感器故障安全态)。
  错误在 MQTT 连接前产生时先缓存,连上后补发(只保留最新一条)。

## 11. 运维注意事项

1. **retained 命令风险**:命令主题请勿发布 retained 强制加热;固件已用
   `expires_at` + SNTP 校验兜底(时钟未同步时拒绝),但最佳实践仍是 QoS1 非 retained;
2. **时钟依赖**:上电后 SNTP 同步前,带 `expires_at` 的命令被拒(`clock_unsynced`);
3. **幂等**:为命令携带 `cmd_id`,重发安全;
4. **保护优先**:任何保护激活时手动加热请求都会被拒并回报原因,不是静默失败;
5. **本地自治**:WiFi/MQTT 全断时,控制逻辑照常运行,仅失去远程观测/控制。
