# ESP32-S3 智能温控固件

SHT40 温湿度采集 + 继电器迟滞加热控制 + MQTT 远程管理的温控固件,
具备完整安全保护(传感器故障锁、超温、加热超时、冷却、安全锁、重启循环保护)。

## 文档导航

| 文档 | 内容 |
|------|------|
| [docs/DESIGN.md](docs/DESIGN.md) | 详细设计:架构分层、启动状态机、安全保护、任务结构、RGB 灯语 |
| [docs/MQTT_PROTOCOL.md](docs/MQTT_PROTOCOL.md) | MQTT 协议:主题总表(唯一权威)、载荷格式、命令校验、事件语义 |
| [docs/REPORT.md](docs/REPORT.md) | 测试与验证报告:主机测试、实机抓取、缺陷记录、遗留事项 |
| [docs/specs/](docs/specs/) | 需求规范原文归档:任务书、RGB 灯语、事件机制、MQTT 主题配置 |

## 本机设备标识与 MQTT 身份

固件在 MQTT 上**没有单独的"设备名称"字段,身份统一为设备 ID(= MQTT client_id)**。

| 标识 | 值 | 说明 |
|------|----|------|
| 设备 ID(device_id) | `s3-thermostat-83A2A0` | `<前缀>-<STA MAC 后 3 字节>`,每块板随 MAC 不同 |
| MQTT client_id | `s3-thermostat-83A2A0` | 与设备 ID 相同,broker 侧以此识别本机 |
| 主题命名空间 | `devices/s3-thermostat-83A2A0/` | 所有 MQTT 主题的本机前缀 |
| SoftAP 配网名称 | `THERM-SETUP-XXXX` | 仅配网模式可见(XXXX = MAC 后 2 字节) |

- 前缀 `s3-thermostat` 可在 `menuconfig → App Configuration → Device ID prefix` 修改;
- 查询实际值:上电串口日志 `app_main: device_id=...` 一行,或订阅
  `devices/+/discovery` 查看 retained 的 `device_id` 字段。

### MQTT 主题

本机 11 个主题(`{n}` = 传感器/风扇索引,当前均为 `0`)的方向、QoS/Retained、载荷格式
与命令语义以 [docs/MQTT_PROTOCOL.md](docs/MQTT_PROTOCOL.md) §1 为唯一权威。
命令唯一入口 `devices/s3-thermostat-83A2A0/sensors/0/command`(温控)与
`devices/s3-thermostat-83A2A0/fans/0/command`(风扇调速);
设备级 `devices/{id}/command` **不订阅、不支持**,向其发布无任何效果。

## 默认硬件配置(menuconfig → App Configuration 可改)

| 外设 | 配置 |
|------|------|
| 加热继电器 | GPIO4,高电平有效 |
| SHT40 I2C | SDA=GPIO8,SCL=GPIO9 |
| 风扇 PWM 调速 | GPIO5,LEDC 25 kHz(接线见 [风扇接入规范](docs/specs/%23%20ESP32-S3%204线PWM风扇接入规范.md)) |
| 风扇 TACH 测速 | GPIO6,PCNT 计数(**上拉电阻必须接 3.3 V,严禁 5 V**) |
| MQTT Broker | `mqtt://192.168.31.242:1883` |

## 分区布局(自定义,[partitions.csv](partitions.csv),16MB flash)

| 分区 | 类型 | 偏移 | 大小 |
|------|------|------|------|
| `nvs` | data | 0x9000 | 24KB |
| `factory` | app | 0x10000 | **16320KB(约 16MB)** |

应用分区约 16MB(原默认 1MB 的 16 倍);flash 容量与布局可在
`menuconfig → Serial flasher config` / `Partition Table` 调整,
`partitions.csv` 亦可按需修改后重新 `idf.py build`。

## 构建与烧录(ESP-IDF v6.1+)

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

首次构建会经 Component Registry 拉取 `espressif/cjson` 与 `espressif/mqtt`
(IDF v6.x 起不再随 IDF 分发)。

## 配网

设备先按配置的 WiFi 连接;连续 8 次失败后开启 SoftAP:
SSID `THERM-SETUP-XXXX`(MAC 后 2 字节),密码 `setup-1234`,
连接后访问 `http://192.168.4.1/` 填写 WiFi 与 broker 地址,保存后自动重启。

## 主机单元测试(无需硬件)

需 `pip install ziglang`;与固件同源编译 `ctrl_core` / `led_pattern` 三套测试:

```powershell
python -m ziglang cc -std=c2x -Wall -Wextra -Icomponents/ctrl_core/include -Itests/host `
  tests/host/test_ctrl_logic.c components/ctrl_core/ctrl_logic.c -o test_ctrl_logic.exe
python -m ziglang cc -std=c2x -Wall -Wextra -Icomponents/ctrl_core/include -Itests/host `
  -Imanaged_components/espressif__cjson/cJSON tests/host/test_ctrl_protocol.c `
  components/ctrl_core/ctrl_protocol.c components/ctrl_core/ctrl_logic.c `
  managed_components/espressif__cjson/cJSON/cJSON.c -o test_ctrl_protocol.exe -lm
python -m ziglang cc -std=c2x -Wall -Wextra -Icomponents/led_status/include -Itests/host `
  tests/host/test_led_pattern.c components/led_status/led_pattern.c -o test_led_pattern.exe
.\test_ctrl_logic.exe; .\test_ctrl_protocol.exe; .\test_led_pattern.exe
```

测试结果与用例覆盖见 [docs/REPORT.md](docs/REPORT.md) §4.1。
