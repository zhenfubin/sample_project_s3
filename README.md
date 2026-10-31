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

## 测试体系与质量门禁

项目采用分层测试策略，覆盖核心控制逻辑、协议解析、组件集成、固件构建和实际硬件运行。测试目标不仅是验证正常流程，还包括边界条件、异常输入、并发行为和故障恢复能力。

### 现有测试

目前已有以下主机侧测试：

- **控制逻辑测试**：验证温度控制逻辑及状态切换。
- **控制协议测试**：验证控制命令和协议解析。
- **LED 模式测试**：验证 LED 状态指示模式。
- **主机编译测试**：使用 Zig C 编译器对可脱离 ESP-IDF 的纯 C 代码进行主机侧编译和测试。

### 测试体系规划

后续测试体系建议分为以下几个层次：

#### 1. 单元测试

持续扩展核心逻辑的测试覆盖率，重点包括：

- 温度阈值边界及滞回控制。
- 时间戳回绕和超时计算。
- 数值溢出及非法参数。
- 控制状态迁移。
- 传感器异常值处理。
- 风扇启动、停止和保护逻辑。

#### 2. 属性测试与模糊测试

针对外部输入和协议解析进行系统性测试：

- MQTT JSON 命令解析。
- 缺失字段、错误字段类型和非法数值。
- 超长字符串及异常编码。
- 随机 JSON 和随机字节流。
- 边界输入下的内存安全和解析稳定性。

#### 3. 组件集成测试

重点验证组件之间的协作：

- MQTT 消息分片与重组。
- 命令队列满载时的处理行为。
- 重复命令、过期命令和命令顺序。
- Wi-Fi 和 MQTT 断线重连。
- NVS 配置保存及读取失败。
- 传感器数据与控制逻辑之间的交互。

#### 4. 并发测试与故障注入

模拟实际运行中可能发生的异常：

- FreeRTOS 任务延迟、阻塞或退出。
- 消息队列耗尽。
- 网络频繁断开和恢复。
- 传感器异常或数据中断。
- 看门狗触发及异常恢复。
- 多个任务同时访问共享状态。

#### 5. 固件构建验证

建立稳定、可重复的构建环境：

- 固定 ESP-IDF 和工具链版本。
- 验证全新环境下的完整构建。
- 检查编译警告和依赖变化。
- 验证分区表及固件配置。
- 确保主机测试和固件构建均通过。

#### 6. 硬件在环测试（HIL）

在真实 ESP32 硬件上验证软件行为：

- 继电器断电和重新上电。
- 温度传感器断线、短路和异常读数。
- 风扇堵转及测速异常。
- Wi-Fi 和 MQTT 连接中断。
- 运行期间掉电和重启。
- 配置持久化及恢复。

#### 7. 长时间稳定性测试

验证系统长期运行能力：

- 连续运行测试。
- 高频重启测试。
- Wi-Fi 和 MQTT 网络抖动测试。
- 多次 NVS 写入和读取。
- 长时间温度控制及风扇调节。

### 高优先级测试范围

建议优先保障以下模块：

| 模块 | 重点验证内容 |
|---|---|
| 温度控制 | 阈值边界、滞回、异常值、状态迁移 |
| MQTT | 分片重组、JSON 解析、非法输入 |
| 命令处理 | 队列满载、去重、超时、执行结果 |
| 配置管理 | NVS 保存失败、配置恢复、一致性 |
| 风扇控制 | 启停保护、测速异常、堵转 |
| 并发与时间 | 任务同步、超时、时间回绕 |
| 网络管理 | 断线重连、连接恢复、异常退出 |

### CI 自动化

建议使用 GitHub Actions 建立持续集成流程：

- 每次 Push 和 Pull Request 自动运行主机侧单元测试。
- Pull Request 自动执行完整固件构建。
- 对关键协议解析模块定期执行模糊测试。
- 定期运行长时间稳定性测试。
- 发布前执行硬件在环测试并记录验收结果。

测试报告应由实际测试执行结果自动生成，避免手动维护断言数量、通过数量等统计信息。

---

## 待开发方向

项目后续改进计划、优先级、实施顺序及验收标准统一维护在：

- [TODO.md](TODO.md)

开发优先级分为：

- **P0：** 安全性、控制可靠性和生产部署相关问题。
- **P1：** 自动化测试、可运维性、OTA 和系统稳定性。
- **P2：** 云端管理、多设备支持和产品化功能。

建议按照 P0 → P1 → P2 的顺序逐步推进。