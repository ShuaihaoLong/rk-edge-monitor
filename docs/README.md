# 文档入口

本文档以当前源码、配置和脚本为准。少量阶段验证记录仍保留在 `docs/` 中，但它们描述的是特定时间点，不能替代当前代码。

## 快速入口

| 目标 | 文档 |
| --- | --- |
| 理解整体进程和数据流 | [当前架构](architecture.md) |
| 构建、测试、交叉编译 | [开发与构建](development.md) |
| 修改 INI 和运行参数 | [配置参考](configuration.md) |
| 部署、服务、端口、故障处理 | [运行与运维](operations.md) |
| Core 生命周期、队列和日志 | [Core 中间层](modules/core.md) |
| V4L2 相机采集 | [Camera](modules/camera.md) |
| GStreamer 解码、OSD、编码和 RTSP | [Media](modules/media.md) |
| RKNN/RGA 推理和检测事件 | [AI](modules/ai.md) |
| MQTT 状态和消息接口 | [MQTT](modules/mqtt.md) |
| STM32 串口协议和 MQTT 桥接 | [STM32](modules/stm32.md) |
| MediaMTX 录像、索引和回放 | [Recording](modules/recording.md) |

## 当前边界

工程包含两类运行单元：

- `rkmon`：C++17 主程序，负责配置、服务生命周期、相机、视频处理、AI、MQTT 和 STM32。
- 板端配套进程：MediaMTX、`rkmon-recording` Python 服务、Nginx、Mosquitto，由 systemd 管理。

`src/recording` 和 `web/` 不属于根 CMake 的 C++ 编译目标。录像 Python 服务通过部署包安装，由 systemd 直接运行；网页由 Nginx 提供。

## 文档维护规则

- 代码入口、配置字段和服务行为变化时，同步更新对应模块文档。
- 测试命令和验证边界写入对应模块文档，不把一次性测量当作永久性能保证。
- 根目录 [AGENTS.md](../AGENTS.md) 是给开发工具使用的仓库约束；用户操作说明放在本文档体系。
- 文档中的 IP、用户名和路径仅表示当前开发板示例，不应当视为通用凭据。
