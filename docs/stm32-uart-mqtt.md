# STM32 串口与 MQTT 接入

应用通过 `/dev/ttyS9` 接收 STM32 的 DHT11 上报，发布到现有 MQTT broker；订阅控制主题后，将 MQTT 原始载荷封装为 `TYPE=0x20` 串口帧发送。串口和 MQTT 各自使用一个线程，独立重连，不影响视频与 AI 服务。

## 与当前 STM32 固件一致的协议

115200 baud、8 数据位、无校验、1 停止位、无流控。所有多字节字段均为小端，无 FLAGS、无帧尾，最大 PAYLOAD 为 **128 字节**。

| 偏移 | 字段 | 字节数 | 定义 |
| --- | --- | --- | --- |
| 0 | HEAD | 2 | `AA 55` |
| 2 | VERSION | 1 | `01` |
| 3 | TYPE | 1 | `10` 温湿度、`20` 控制、`00` ACK |
| 4 | SEQ | 4 | 发送方递增序号 |
| 8 | LEN | 2 | PAYLOAD 长度，0–128 |
| 10 | PAYLOAD | LEN | 消息载荷 |
| 10+LEN | CRC32 | 4 | CRC-32/ISO-HDLC，小端 |

帧总长 `14 + LEN`。CRC 覆盖偏移 2 开始的 `8 + LEN` 字节，不含 HEAD 和 CRC 本身。
CRC 参数：Poly `0x04C11DB7`，反射实现多项式 `0xEDB88320`，Init/XorOut 均为 `0xFFFFFFFF`，RefIn/RefOut 均为 true。
ASCII `123456789` 的结果为 `0xCBF43926`。两端均逐字节计算，不填充到 32 位边界。

`TYPE=0x10` 的载荷必须恰好为四字节：

```text
temp_int:u8 | temp_dec:u8 | humi_int:u8 | humi_dec:u8
```

按小数位表示十分之一解码，例如 `1C 05 2F 00` 表示 28.5 ℃、47.0%RH。小数位必须为 0–9；温度接受 0–50 ℃、湿度接受 0–100%。无效长度、小数位或范围不作为有效采样。JSON 同时保留四个原始字节，便于核对固件编码。

板端实采帧（已作为独立测试向量）：

```text
AA 55 01 10 6A 01 00 00 04 00 1C 05 2F 00 86 9E B1 0A
```

其中 SEQ=362，CRC32=`0x0AB19E86`。解析器支持分片、多帧、噪声和载荷内出现帧头；非法版本、超长或 CRC 错误时逐字节重新同步。残帧超过 `frame_timeout_ms` 后重新搜索帧头，串口重连时清空解析状态。

## 配置

```ini
[stm32]
enabled = true
device = /dev/ttyS9
baud_rate = 115200
frame_timeout_ms = 200
stale_timeout_ms = 5000
reconnect_interval_ms = 2000
```

启用 STM32 要求 `[mqtt] enabled=true`，沿用其 broker、device_id 和状态发布周期。省略 `[stm32]` 时默认关闭。串口打开后设置为 raw 模式并申请独占；运行用户需要串口读写权限，板端 `elf` 已属于 `dialout`。支持波特率 9600/19200/38400/57600/115200/230400，降低波特率时需相应留足整帧超时。

当前固件约每 1.5 秒采样一次，读取失败时不发送。应用连续 5 秒无有效 DHT11 数据便将 `online=false`，但 `serial_connected` 可仍为 true；此时不能区分传感器采样失败、接线中断或 STM32 未运行。

## MQTT 接口

前缀为 `rkmon/devices/<device_id>/stm32`。所有发布和订阅均使用 **QoS 0**，clean session；不提供消息持久化或交付保证。状态 retained，其余非 retained。STM32 桥接连接使用独立 client ID 后缀 `-stm32`，不抢占原有设备状态连接。

| 后缀 | 方向 | 内容 |
| --- | --- | --- |
| `/telemetry` | 应用 → broker | 温湿度 JSON，仅有新读数时发布 |
| `/status` | 应用 → broker | 在线/串口状态 JSON，retained，含离线遗嘱 |
| `/command` | broker → 应用 | **原始二进制载荷**，最多 128 字节 |
| `/command_result` | 应用 → broker | 串口发送结果 JSON |
| `/ack` | 应用 → broker | STM32 ACK 的序号和原始载荷 JSON |

温湿度示例：

```json
{"schema":1,"sequence":362,"temperature_c":28.5,"humidity_percent":47.0,"raw":[28,5,47,0],"received_at_ms":1789804800000}
```

时间戳是 RK3588 接收时间。订阅方应结合 `/status` 和 `received_at_ms` 判断新鲜度；遗嘱时间是连接时生成的，不能当作实际断线时刻。断网期间仅保留最新读数，重连后只发布仍未过期的读数。

控制主题内容不是 JSON，也不是十六进制文本。安装了 Mosquitto 命令行工具的客户端可这样发布三个字节（仅演示传输格式，不定义继电器或 LED 操作）：

```bash
printf '\x01\x00\xff' | mosquitto_pub -h 192.168.100.11 \
  -t rkmon/devices/rk3588-center/stm32/command -q 0 -s
```

不要为命令设置 retained。应用拒绝订阅时重放的 retained 命令；MQTT 3.1.1 对在线订阅者转发实时消息时会清除 RETAIN 标志，因此应用不能识别所有被发布者设置 retained 的实时命令。

发送结果示例：

```json
{"schema":1,"sequence":0,"state":"sent","updated_at_ms":1789804800000}
```

`sequence` 是 RK3588 分配的串口序号；`sent` **仅表示整帧已写入串口驱动，不表示 STM32 执行成功**。其他状态：

- `rejected_retained`：拒绝 retained 重放。
- `rejected_length`：载荷超过 128 字节。
- `rejected_offline`：下位机数据过期或串口断开。
- `rejected_busy`：16 条待发送命令队列已满。
- `rejected_disconnected`：MQTT 连接断开时取消尚未取出的命令。
- `unknown`：串口写入中出错或超时，可能已发送部分内容。

不自动重试控制命令。断网时最多缓存最近 32 条发送结果/ACK 事件，超限丢弃最旧事件；这不是审计日志。

当前固件 `Process_Master_Command()` 尚未执行控制操作或生成 ACK，且 `Protocol_Send_Frame()` 对所有帧独立递增 SEQ。因此应用不按 ACK 帧头序号确认命令，而将其原样发布：

```json
{"schema":1,"sequence":999,"payload_hex":"02ff","received_at_ms":1789804800000}
```

后续固件定义具体 opcode 和 ACK 载荷后，再增加业务命令映射与执行结果确认。ACK 应明确携带原请求 SEQ 或使用可指定 SEQ 的应答发送接口。

## 验证与部署

```bash
bash script/build.sh --target host --test
bash script/build.sh --target rk3588
bash script/deploy.sh
```

部署继续使用现有入口，安装包自动包含新的应用程序和 INI，无新增运行库依赖。

`stm32_protocol` 验证 CRC 标准向量、STM32 实采帧、残帧超时、坏帧恢复和温湿度解码。
`stm32_integration` 使用正式应用、PTY 和本地 MQTT 测试端验证双向传输、原始 ACK 转发、retained 拒绝、数据过期、串口及 MQTT 重连和正常退出；测试需要创建本机 socket 与伪终端。

MQTT 封装依据 [MQTT 3.1.1 标准](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)。

2026-09-19 板端验证：以独立设备 ID `stm32-validation` 和临时配置运行新应用，
通过真实 `/dev/ttyS9` 收到序号 760–764 的五条 DHT11 数据，在现有 Mosquitto 上由独立订阅端收到，
读数为 28.0 ℃、47.0%RH，接收间隔约 1500 ms。SIGTERM 正常退出并发布离线状态，
随后清除测试设备 retained 状态；未替换 `/opt/rkmon` 正式服务。
实机未验证控制动作或执行 ACK，因为当前固件尚未实现这两项业务；下行组帧和 ACK 转发通过 PTY 集成测试验证。
