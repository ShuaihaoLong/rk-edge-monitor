# STM32 模块

## 职责与入口

STM32 模块通过 UART 接收 DHT11 温湿度数据，并把 MQTT 命令封装为串口帧发送。代码位于 `src/stm32/`，公共协议在 `include/stm32/`，CMake target 为 `rkmon_stm32`，依赖 `rkmon_mqtt` 和 `rkmon_core`。

## 串口协议

默认 `/dev/ttyS9`、115200 8N1、无流控。帧格式为：

```text
AA55 | VERSION:u8 | TYPE:u8 | SEQ:u32le | LEN:u16le | PAYLOAD | CRC32:u32le
```

最大 payload 128 字节，总长度 `14 + LEN`。CRC-32/ISO-HDLC 覆盖 VERSION 到 PAYLOAD，不包含帧头和 CRC。`TYPE=0x10` 温湿度 payload 为四字节：温度整数、小数、湿度整数、小数；当前固件采样约 1.5 秒一次。

解析器支持分片、粘包、噪声、帧头出现在载荷中、坏 CRC 和残帧超时；非法帧逐字节重新同步。串口断开后清空解析状态并按重连间隔重试。

## MQTT 接口

主题前缀为 `rkmon/devices/<device_id>/stm32`：

- `/telemetry`：温湿度 JSON，含 `schema`、序号、数值、原始字节和接收时间。
- `/status`：在线和串口状态，retained，带离线遗嘱。
- `/command`：最多 128 字节的原始下行载荷。
- `/command_result`：发送状态 JSON。
- `/ack`：ACK 序号和原始载荷 JSON。

命令不自动重试，不使用 retained。队列满、载荷过长、数据过期、离线、断开等情况会返回明确拒绝状态。当前固件尚未实现具体控制动作和业务 ACK，因此 `sent` 不能当作执行成功。

## 验证

测试覆盖 CRC 标准向量、实采帧、噪声/分片/坏帧、温湿度范围、PTY 双向传输、MQTT 重连和 retained 命令拒绝。当前实板已验证接收 DHT11 和发布 MQTT；控制动作仍等待固件实现。测试入口为 `tests/stm32_tests.cpp` 和 `tests/stm32_integration_tests.py`。
