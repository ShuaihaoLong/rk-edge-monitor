# rkmon 中间层

本次保留现有 `include/core` 路径和 C++20 配置；公共命名空间为 `rkmon::core`，日志使用 `rkmon::log`。编译目标为 `rkmon_core`（别名 `rkmon::core`）和应用 `rkmon`。

## 构建

```bash
bash script/build.sh --target host --jobs 4 --test
bash script/build.sh --target rk3588 --jobs 4
```

主程序目前只是初始化/退出验证入口，尚未注册摄像头或 GLib 事件循环，启动后会正常退出。现有 GStreamer smoke 示例仍使用 `script/smoke-build.sh` 独立构建。

`.clangd` 改为读取主工程 `build/rk3588/compile_commands.json`。主机和 ARM64 使用不同构建目录，不能混用缓存。第三方 spdlog 使用现有子模块提交，不修改或升级源码；静态编译并使用随附 fmt。

## 队列：BoundedQueue<T>

- `push(T)`：阻塞等待空位，关闭后返回 false。
- `try_push(T)`：不等待空位，返回 accepted/full/closed。
- `try_push(T, OverflowPolicy::drop_oldest)`：视频分支使用，满时替换最旧待处理帧，返回 dropped_oldest。
- `pop()`：等待新数据；关闭且排空后返回 nullopt。
- `try_pop()`：不等待新数据。
- `close(CloseMode::drain)`：默认停止入队并排空；`discard` 清空待处理数据。两者都唤醒等待者，可重复调用。

容量必须大于 0，类型需要 noexcept 移动构造。支持 unique_ptr 等仅移动类型；按值提交时，失败也会消费调用方移入对象的所有权。队列关闭后不能重新打开，服务重启时创建新实例。

队列本身不拥有线程；销毁前调用方必须停止并 join 所有使用者。size/dropped 是独立时刻的监控读数，不应先 size 再 push 以判断成功。try_ 接口仍需获取互斥锁，并非无锁或硬实时。

## 控制邮箱：Mailbox<Event>

事件类型由业务层定义，可采用 std::variant；中间层不依赖 Video/AI 类型。try_post 永不丢弃旧命令，满时明确返回 full。既支持阻塞 receive，也支持事件循环中的 try_receive。

可传入唤醒回调，但它只能通知接收方，必须线程安全、短小。回调在锁外执行；收到通知后接收方排空邮箱。GLib/eventfd 的具体适配留在应用层，回调引用的事件循环资源必须存活到所有生产者结束。

通知异常不改变已经成功入队的结果，而是计入 notification_failures；运行时应有健康检查/定时兜底。退出请求通过 IService::request_stop 直接发出，不依赖邮箱剩余容量。状态合并和请求超时属于具体业务策略，本层不自动实现。

## 服务管理

IService 将停止拆成 request_stop 和 join；旧 stop 接口作为二者的便捷组合保留，具体服务需实现这两个新接口。running/name 保留，health 提供默认快照，需要降级/故障原因的模块自行覆盖并同步状态。

ServiceManager 仅由主控制线程调用，按依赖顺序 add，顺序启动；失败或异常会把失败模块也纳入回滚。停止先逆序通知全部服务，再逆序 join。重复启动已运行组、重复停止均不会重复调用模块；析构自动停止。

服务名字非空且唯一；运行期间不能增加服务。服务必须支持启动到一半时停止，join 必须结束所有资源访问后才能返回。具体服务单独使用时，其析构函数也应调用 stop，基类析构不能代替派生类的线程回收。

管理器不会自动重试或杀死卡住的线程。第三方 API 超时、重连退避、健康快照同步由相应服务实现。禁止业务线程调用管理器 stop_all，避免自 join。

## 日志

`rkmon::log::init(Options)` 在启动工作线程之前调用，`shutdown()` 在全部线程结束之后调用。默认同步输出控制台和轮转文件；日志级别、文件大小、备份数可配置。默认格式含时间、rkmon、级别和线程 ID。默认 warn 及以上刷新文件。

`get()` 返回共享 logger，模块可缓存；未初始化会抛异常。初始化失败不发布部分 logger，重复初始化报错。shutdown 可重复调用，但不是强制撤销所有已缓存的共享指针，调用前必须停止业务线程。

新代码使用 RKMON_INFO/WARN/ERROR 或 get()->info，保留 REM_* 作为草稿兼容别名。当前不实现异步日志、业务级限频或复杂后端抽象。

## 验证范围

CTest 使用无额外下载依赖的轻量断言程序：
- queue：溢出/关闭/丢旧策略、移动所有权、阻塞线程唤醒和多生产者多消费者无丢失重复。
- mailbox：满队列拒绝、关闭通知、锁外唤醒回调及通知异常计数。
- service：依赖顺序、两阶段停止、启动失败/异常回滚、重复启停、析构回收真实线程。
- logger：初始化错误、多线程写入、文件轮转、级别过滤及退出刷新。

ARM64 构建生成同一测试程序，但不在 WSL 上自动运行。尚不包含硬件、GStreamer 主循环、设备重连或长时间稳定性测试。
