# MQTT 设备侧与 Python 服务端主题配置

设备 ID：`s3-thermostat-83A2A0`  
传感器索引：`0`

## 1. 设备侧（ESP32-S3）

### 订阅

| 主题 | 用途 |
|---|---|
| `devices/s3-thermostat-83A2A0/sensors/0/command` | 接收控制命令 |

### 发布

| 主题 | QoS | Retained | 用途 |
|---|---:|---|---|
| `sensors/0/telemetry`（完整主题需加设备前缀） | 0 | 否 | 温湿度遥测，每 2 秒 |
| `sensors/0/command_result`（完整主题需加设备前缀） | 1 | 否 | 命令执行结果 |
| `state`（完整主题需加设备前缀） | 1 | 是 | 整体状态 |
| `config`（完整主题需加设备前缀） | 1 | 是 | 当前生效配置 |
| `availability`（完整主题需加设备前缀） | 1 | 是 | 在线状态及 LWT |
| `discovery`（完整主题需加设备前缀） | 1 | 是 | 设备能力声明 |
| `events`（完整主题需加设备前缀） | 1 | 建议否 | 就绪、系统错误等事件 |

## 2. Python 服务端（FastAPI）

### 订阅

订阅设备发布的全部上行主题：

- `devices/s3-thermostat-83A2A0/sensors/0/telemetry`
- `devices/s3-thermostat-83A2A0/sensors/0/command_result`
- `devices/s3-thermostat-83A2A0/state`
- `devices/s3-thermostat-83A2A0/config`
- `devices/s3-thermostat-83A2A0/availability`
- `devices/s3-thermostat-83A2A0/discovery`
- `devices/s3-thermostat-83A2A0/events`

### 发布

| 主题 | QoS | Retained | 用途 |
|---|---:|---|---|
| `devices/s3-thermostat-83A2A0/sensors/0/command` | 1（建议） | 否 | 下发控制命令 |

## 3. 约定

- 设备和 Python 服务端分别创建并维护自己的 MQTT 客户端。
- MQTT 主题通常不需要在 Broker 中预先创建。
- 禁止使用设备级主题 `devices/{id}/command`；所有控制命令统一发送至传感器命令主题。
- 命令应携带唯一 ID；设备需处理 QoS 1 的重复投递，服务端根据 ID 匹配命令结果。
- `events` 如果用于一次性事件，建议关闭 Retained，避免收到旧事件。
