# 需求规范原文(归档)

本目录保存需求规范**原文**,内容不随实现文档更新,作为需求依据长期保留。
实现设计见 [DESIGN](../DESIGN.md);协议参考见 [MQTT_PROTOCOL](../MQTT_PROTOCOL.md);
测试与验证见 [REPORT](../REPORT.md)。

| 规范 | 对应实现 |
|------|----------|
| [# ESP32-S3 智能温控固件优化任务书](<%23 ESP32-S3 智能温控固件优化任务书.md>) | 全项目(需求总纲、安全规则 32 条) |
| [# ESP32-S3 温控设备 RGB 指示灯状态规范](<%23 ESP32-S3 温控设备 RGB 指示灯状态规范.md>) | [DESIGN §12](../DESIGN.md)、`components/led_status/` |
| [# ESP32-S3 温控设备事件机制设计规范](<%23 ESP32-S3 温控设备事件机制设计规范.md>) | DESIGN §8、`components/event_manager/` |
| [# MQTT 设备侧与 Python 服务端主题配置](<%23 MQTT 设备侧与 Python 服务端主题配置.md>) | [MQTT_PROTOCOL](../MQTT_PROTOCOL.md) §1(主题总表) |
| [# ESP32-S3 4线PWM风扇接入规范](<%23 ESP32-S3 4线PWM风扇接入规范.md>) | [DESIGN §13](../DESIGN.md)、`components/fan_ctrl/` |

> 链接中 `%23` 为文件名前缀 `#` 的 URL 转义,可直接点击打开。
