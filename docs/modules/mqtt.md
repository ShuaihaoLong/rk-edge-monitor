# MQTT 模块

## 职责与入口

MQTT 模块负责连接本机 Mosquitto、发布设备状态/检测相关消息，以及接收需要转发到 STM32 的命令。代码位于 `src/mqtt/`，CMake target 为 `rkmon_mqtt`；协议配置在 `[mqtt]`。

## 连接和主题

配置包括 broker 地址端口、device_id、role、topic_prefix、发布周期和 keepalive。默认前缀为 `rkmon/devices`，状态主题使用 retained 和离线遗嘱；普通数据和控制消息使用 QoS 0、clean session，不提供持久化交付保证。

设备状态应包含在线、连接和时间信息，订阅者需要结合状态与接收时间判断新鲜度。断线时模块重连；控制命令不能因为连接恢复而无条件重放。

## STM32 桥接

STM32 服务使用独立 client id 后缀 `-stm32`。下行 `/stm32/command` 是原始二进制载荷，不是 JSON 或十六进制文本；发送前限制长度并进行队列容量检查。发送结果和 ACK 使用 JSON 发布，`sent` 只表示写入串口驱动，不表示下位机业务执行成功。

## 故障边界

MQTT 网络故障不能阻塞视频和 AI。结果缓存有容量上限，超过后丢弃最旧的非关键事件；不自动重试非幂等控制命令。TLS、认证、ACL 和公网部署不在当前工程配置中，默认仅面向可信局域网。

## 验证

MQTT 测试覆盖连接、状态、重连、发布周期、订阅和退出；STM32 集成测试覆盖 MQTT 到 PTY 的双向桥接。具体主题和载荷见 [STM32 模块](stm32.md)，测试入口位于 `tests/mqtt_tests.cpp` 和 `tests/stm32_integration_tests.py`。
