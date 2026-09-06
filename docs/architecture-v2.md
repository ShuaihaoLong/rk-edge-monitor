# RK3588 边缘智能监控终端：V2 架构草案

状态：建议方案，尚未上板验证。依据用户粘贴的重构建议和硬件说明制定。

用户后续调整：优先完成 USB 摄像头视频功能，先准备 WSL2 → ARM64 交叉编译环境；串口阶段后移。摄像头 USB ID 为 0bda:d576（Realtek USB Camera），实际格式仍需板端查询。开发环境以 [交叉编译说明](cross-compile.md) 为准，取代下文最初的板端原生编译建议；原阶段表作为完整功能规划保留，执行顺序改为工程基础 → 视频 → 后续 AI/通信。

## 1. 产品边界

面向嵌入式 Linux C++ 求职，首版交付单路摄像头预览、人员检测事件、STM32 状态采集，以及运行指标和故障恢复。RTSP 作为视频验收出口。检测结果先输出结构化事件，不要求视频叠框。

先完成通信闭环和视频闭环，再集成 AI。MQTT 为后续事件上报出口；广告播放、LVGL、Web 后台、多路摄像头、多模型不进入首版。

首版成功条件：可复现构建、通信异常有测试、视频不会因 AI 变慢持续积压、故障可观测、SIGTERM 可退出、性能有原始记录。1080p30 是待验证目标，不是已经达成的指标。

## 2. 开工前的板卡基线

需要记录：板卡具体型号、内存、系统镜像来源、内核、编译器、CMake、GStreamer 版本；摄像头型号/接口/支持格式；STM32 型号、固件是否可修改、UART 接线及电平。

另外检查 NPU 驱动、librknnrt、RGA、MPP、GStreamer 硬编解码插件是否存在，并分别运行最小示例。Ubuntu 22.04 和 RK3588 型号不能证明这些组件已经可用。不要在验证前升级或替换厂商驱动。

摄像头格式决定视频入口：MJPEG/H.264 需要解码才能推理，原始 YUYV/NV12 需要确认带宽和格式转换。不要预设所有摄像头都输出 NV12 或支持 DMA-BUF。

## 3. 开发与依赖

| 项目 | 首版选择 |
| --- | --- |
| 语言 | C++17；线程采用 std::thread、显式停止和 join，不使用 C++20 才提供的 std::jthread |
| 构建 | CMake + Unix Makefiles，使用 GNU Make 编译，按 target 管理依赖；host 和 rk3588 两组构建配置 |
| 调试 | GDB；主机上对纯逻辑执行 ASan/UBSan，必要时单独执行 TSan |
| 日志/配置 | spdlog + nlohmann/json；配置集中校验，依赖版本固定 |
| 单元测试 | GoogleTest；协议、队列退出、请求超时为重点 |
| 视频 | GStreamer 编排；优先验证厂商 MPP 插件，插件不可用时再决定直接封装 MPP |
| AI | 一个 YOLOv8n 检测模型；板端 C++ RKNN Runtime；RGA 在验证后用于预处理 |
| 服务部署 | systemd，进程异常退出可重启，内部模块异常由模块状态机处理 |

Windows 上的 VSCode 通过 Remote-SSH 直接打开板端工程，板端原生编译和调试。WSL2 用于纯 C++ 测试、脚本和模型转换环境。两处代码通过 Git 的相同提交保持一致，避免依赖自动双向目录同步。

WSL2 主机测试不链接 ARM 专用库。板端真实适配器通过构建选项启用；没有硬件时使用 Mock/伪终端。先不引入交叉编译的 sysroot 维护成本。

记录模型导出参数、输入尺寸/布局/类型、量化数据集摘要、Toolkit 和 Runtime/驱动版本；以兼容性文档与板端示例验证结果选择版本组合，不盲目安装最新版。

## 4. 模块与数据流

```text
                         App / ServiceManager
                  配置、生命周期、健康状态、指标
                              |
       +----------------------+-----------------------+
       |                      |                       |
 DeviceService          VideoService             AiService
 UART / 协议            GStreamer pipeline       RGA / RKNN
       |                      |                       ^
 状态/设备事件          解码后原始帧 -- 有界队列 --------+
       |                      |
       |                 编码 -> RTSP
       |                                              |
       +------------> 小型控制事件队列 <---------- 检测结果
                              |
                         日志 / 指标
                        后续 MQTT 出口
```

单进程模块化设计，Service 是 C++ 组件，不是独立进程。App 负责组装依赖，模块不依赖全局单例。首版不做通用 RPC、插件系统、线程池或全功能 EventBus。

视频大帧不经过控制事件队列。采集后分流，编码与推理各自消费，推理不作为推流的前置步骤。GStreamer tee 的各分支使用独立 queue；编码分支和 appsink 内部缓存也必须设置边界。

首版推流不叠框。后续如果加入叠框，必须定义检测结果与 frame_id 的匹配、最大可接受结果年龄，以及等待检测是否允许增加视频延迟。

## 5. 线程、缓存和所有权

| 执行上下文 | 责任与约束 |
| --- | --- |
| 主控制线程 | GLib 主循环、pipeline bus 消息、模块状态和退出调度；不得执行耗时推理 |
| 串口 I/O 线程 | poll + 非阻塞 fd，处理收发、部分写入、请求期限和重连；通过 eventfd 等机制唤醒退出 |
| 推理线程 | 从有界队列取帧，预处理、推理、后处理，发布检测结果 |
| GStreamer 内部线程 | 执行采集/编解码/RTSP 数据流；回调保持短小，不再机械地为每个 Service 新建线程 |

线程数是上述应用线程加库内部线程的实测结果，不能宣称固定为三个线程。

AI 输入队列初始容量 2，满时丢弃最旧的待处理帧；用慢消费者测试决定是否调整。串口命令不能套用丢旧策略，队列满时明确拒绝提交。状态通知可合并，关键错误至少保留独立计数并限频记录日志。

Frame 包含 frame_id、像素格式、宽高、各平面的 stride/offset、时间戳及其时钟域、底层 buffer 的 RAII 所有者。跨线程持有 GstBuffer/GstSample 的有效引用；映射指针仅在有效映射期间使用，不能把已 unmap 的裸指针入队。共享 buffer 首版只读。

DetectionResult 包含来源 frame_id、时间戳和检测框；坐标须从模型 letterbox 空间还原到源图像空间。RKNN 输出使用完后按 SDK 要求释放。

队列边界不等于端到端延迟上界：还应检查驱动、编码器、RTSP 和客户端缓存。丢原始帧与丢 H.264 压缩帧不是同一个操作；随意丢压缩帧会破坏参考帧依赖。

首版先实现正确的 buffer 生命周期并记录拷贝次数；DMA-BUF/RGA/MPP/RKNN 间共享内存是否成立，逐段验证。零拷贝不是预先承诺。

## 6. 通信协议与请求语义

优先检查 STM32 现有协议；若固件可以同步修改，设计带 Magic、Version、Type、Session、Seq、Length、Payload、CRC 的版本化协议。

写代码前确定字段宽度、字节序、最大帧长、CRC 参数与覆盖范围，以及心跳、错误码和重启握手。禁止直接发送 C++ struct 内存。首版可以提议 payload 上限 1024 字节，最终依据实际消息确定。

分层为 SerialPort -> FrameDecoder -> ProtocolClient -> DeviceService。FrameDecoder 为不依赖 fd 的纯逻辑，必须处理任意分包、粘包、噪声、CRC 错误和长度攻击，并限制缓存及不完整帧等待时间。

ProtocolClient 维护有容量限制的 pending 请求，以单调时钟判断超时；每个请求只能完成一次。断开时结束旧 pending，重连建立新会话，拒绝旧会话响应，并处理 seq 回绕。

读取状态等幂等操作可配置重试；执行动作类命令必须定义设备端去重/幂等语义后才能自动重试。异步提交不代表可以在 I/O 线程里 future.get()。

## 7. 生命周期与故障边界

状态至少包含 Stopped、Starting、Running、Degraded、Stopping、Failed。启动失败需要回滚已启动模块；stop 可重复调用。

依赖顺序关闭：停止新任务、停止生产并唤醒等待、按策略取消或消费队列、join 应用线程、将 pipeline 置为 NULL 并结束库任务、释放上下文和 fd。具体释放次序以引用依赖为准，禁止线程还在使用资源时先销毁资源。

信号通过安全方式转交控制循环；不要在原始信号处理函数里调用日志、锁和复杂 stop 逻辑。

摄像头断开后重建 pipeline，重试有退避和上限；推理连续失败进入降级并保持视频出口可用；STM32 离线明确更新设备状态。系统不应因为断网持续积压事件。

线程捕获异常并报告模块失败，不能静默退出；进程崩溃由 systemd 恢复。第三方调用永久阻塞无法靠 std::thread 安全强杀，先确认 API 超时/取消能力；必要时由进程级 watchdog 恢复，而不是 detach 后销毁资源。

## 8. 目录边界

```text
app/                    # 组装、入口、主循环
src/core/               # 有界队列、生命周期、公共错误类型
src/device/             # 串口、解码器、请求管理、设备状态
src/video/              # GStreamer、帧所有权、RTSP
src/ai/                 # 预处理、RKNN、检测后处理
src/observability/      # 日志、统计、健康快照
include/edge/           # 模块公开接口
tests/                  # 单元测试、伪终端集成测试
configs/                # 无凭据的示例配置
scripts/                # 板卡检查、部署、性能采集
deploy/                 # systemd unit
docs/                   # 架构、协议、测试结果
CMakeLists.txt
```

目录随实际功能建立，不先创建没有行为的抽象。共享接口放业务需要的最小内容，第三方句柄封装在对应适配器内。

## 9. 实施顺序及验收

| 阶段 | 工作 | 完成依据 |
| --- | --- | --- |
| P0 | 板卡基线与独立能力验证 | 串口回环、摄像头取帧、硬编码、官方 RKNN 示例有输出记录；阻塞项明确 |
| P1 | 最小 C++ 工程 | WSL2 核心测试和板端构建通过；配置错误可诊断；正常启动和 SIGTERM 退出 |
| P2 | 串口通信闭环 | 伪终端测试分包/噪声/超时/断连；实板 request-response；重复响应不重复完成请求 |
| P3 | 视频闭环 | 摄像头到 RTSP 可播放；缓存有限；摄像头异常有状态和恢复记录 |
| P4 | AI 分支 | 检测结果正确关联原帧；人为减慢 AI 后视频继续运行，队列不超过上限 |
| P5 | 故障注入与测量 | UART/摄像头断连恢复、退出测试；记录 RSS、FPS、丢帧、延迟分位数和长稳结果 |
| P6 | 可选出口 | MQTT/简单 UI，仅在主链路验收后添加 |

可靠性和测量从 P1 开始贯穿开发，P5 是集中验收。长稳按短跑、数小时、24h、72h 逐级推进，失败保留日志和运行条件。

指标必须说明测量口径：采集、推理、编码 FPS 分开；应用内延迟使用同一单调时钟；camera 到客户端实际显示延迟用可验证的画面计时/同步测量方法，不拿服务端发送耗时代替。UART RTT 与单向 STM32 到 Linux 延迟分开；后者需要时钟同步或外部仪器。

## 10. 官方参考

- GStreamer tee 分支与线程隔离：https://gstreamer.freedesktop.org/documentation/coreelements/tee.html
- GStreamer queue 容量与丢帧策略：https://gstreamer.freedesktop.org/documentation/coreelements/queue.html
- GStreamer appsink 缓存和应用接口：https://gstreamer.freedesktop.org/documentation/app/appsink.html
- Rockchip RKNN Toolkit2 与 Runtime：https://github.com/airockchip/rknn-toolkit2

官方在线文档可能比 Ubuntu 22.04 板端组件更新，配置属性以板端 gst-inspect-1.0 和实际 SDK 版本为准。
