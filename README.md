# ESP32-S3 智能温控固件

SHT40 温湿度采集 + 继电器迟滞加热控制 + MQTT 远程管理的温控固件,
具备完整安全保护(传感器故障锁、超温、加热超时、冷却、安全锁、重启循环保护)。

- 详细设计:[docs/DESIGN.md](docs/DESIGN.md)
- MQTT 协议:[docs/MQTT_PROTOCOL.md](docs/MQTT_PROTOCOL.md)
- 任务完成报告(含测试报告与上板建议):[REPORT.md](REPORT.md)

## 默认硬件配置(menuconfig → App Configuration 可改)

| 外设 | 配置 |
|------|------|
| 加热继电器 | GPIO4,高电平有效 |
| SHT40 I2C | SDA=GPIO8,SCL=GPIO9 |
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

```powershell
python -m ziglang cc -std=c2x -Icomponents/ctrl_core/include -Itests/host `
  tests/host/test_ctrl_logic.c components/ctrl_core/ctrl_logic.c -o test_ctrl_logic.exe
python -m ziglang cc -std=c2x -Icomponents/ctrl_core/include -Itests/host `
  -Imanaged_components/espressif__cjson/cJSON tests/host/test_ctrl_protocol.c `
  components/ctrl_core/ctrl_protocol.c components/ctrl_core/ctrl_logic.c `
  managed_components/espressif__cjson/cJSON/cJSON.c -o test_ctrl_protocol.exe -lm
.\test_ctrl_logic.exe
.\test_ctrl_protocol.exe
```
