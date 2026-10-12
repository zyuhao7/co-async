# steps — 阶梯索引

`step1.cpp` … `step24.cpp` 是 co_async 逐步长出来的过程：**每一步只比上一步多一样能力**，用来配合教学视频对照阅读。

## 构建运行

不需要 CMake，单文件直接编（`step1`–`step7` 自包含，`step11`–`step24` 共享 `steps/co_async/*.hpp`）：

```bash
g++ -std=c++20 -O0 -g -Isteps steps/step16.cpp -o /tmp/step16
/tmp/step16
```

`steps/run.sh` 是一行的编译+运行示例。更多运行注意点见仓库根 [README](../README.md#构建与运行-steps-阶梯)。

> `step8`、`step9`、`step10` **不存在**，阶梯从 `step7` 直接跳到 `step11`。

## 主线：五个阶段

**协程原语（1–4）→ 调度与组合（5–7）→ 事件循环（11–18）→ 网络与 HTTP（19–22）→ 服务端（23–24）**

两次**结构性跃迁**：

- **step7**：定时器从 `std::priority_queue`（裸句柄、不可摘）换成**侵入式可摘红黑树**。
- **step18**：手摆 `timerLoop`/`epollLoop` 换成 **`AsyncLoop` 封装 + `run_task`**。

## 每步新增 / 卡点

| 步 | 新增能力 | 上一步卡在哪 |
|---|---|---|
| step1 | 裸 `Task`/`Promise`/`RepeatAwaiter`，`co_await` 一个 `RepeatAwaitable` | 无调度器，不能 `co_await` 别的任务 |
| step2 | `PreviousAwaiter`：任务可 `co_await` 上一个派生的任务 | 仍无定时器与 I/O |
| step3 | `return_value(int)`，`Task(Task&&)=delete` | 任务没有返回值、没有所有权纪律 |
| step4 | `Promise<T>` 模板 + `Promise<void>` 特化 | step3 把类型写死成 `int` |
| step5 | 调度器 `Loop`：`addTask`/`addTimer`/`runAll` + `co_sleep` | 任务能睡了，但定时器只是堆里的一堆条目 |
| step6 | `NonVoidHelper`/`Uninitialized` + 组合子 `when_all`/`when_any` | 有组合了，但 `when_any` 析构输家后定时器留悬挂句柄 |
| step7 | 侵入式 `RbTree` 定时器 + `sleep_until`/`sleep_for` | step6 的释放后使用（输家留在树里、要摘就摘） |
| step11 | 裸 `epoll_create`/`epoll_wait` + 非阻塞 `read` | 第一次非阻塞 I/O，但没和协程接起来 |
| step12 | `EpollFilePromise`/`EpollLoop::addListener`/`tryRun` + `wait_file` | step11 手搓 read 循环；不能注销、不能限时 |
| step13 | `removeListener`、`tryRun(timeout)`、promise 析构注销 | step12 不能注销、不能限时等待 |
| step14 | `checkError` 带 `source_location`；分块读 | step13 单字节读、吞吐差 |
| step15 | `EpollFilePromise : Promise<uint32_t>`，回传事件掩码 | step13/14 的 `Promise<void>` 拿不到「发生了什么事件」 |
| step16 | 抽出 `epoll_loop.hpp`：`AsyncFile` + `read_file`/`write_file` | step15 的 epoll 全写在 .cpp 里、裸 fd 无主 |
| step17 | `limit_timeout` + 终端原始模式 + 可玩的游戏 | step16 的读会**死等**输入，没有截止时间 |
| step18 | `AsyncLoop`（装 timer+epoll）+ `run_task` | step17 手摆两个 loop、手搓顶层句柄 |
| step19 | `socket.hpp`：`ip_address`/`socket_address`/`create_tcp_client`/`socketConnect` | step18 只有 fd 与文件 I/O |
| step20 | `stream_base`/`stream`/`stdio`：`IStream`/`OStream` + `getline`/`getchar` | step19 只有裸字节，没有行/字段语义 |
| step21 | `HTTPHeaders`/`HTTPRequest`/`HTTPResponse` | step20 没有 HTTP 报文语义 |
| step22 | 同 HTTP 层，专练**二进制 body** 落盘 | step21 未验证二进制（`\0` 等）搬运 |
| step23 | 服务端：`create_tcp_server`/`socket_listen`/`socket_accept` + 固定响应 | step19–22 只会当客户端 |
| step24 | accept 循环 + `spawn_task` 每连接一协程 + 流式 `getchar` | step23 只 accept 一个连接就退出 |

## 概念索引：概念 → 首次出现

| 概念 | 步骤 |
|---|---|
| `Task`/`Promise`/`coroutine_handle` | step1 |
| 对称转移、`PreviousAwaiter` | step2 |
| `Uninitialized`/`NonVoidHelper` | step6 |
| `when_all` / `when_any` | step6 |
| 可摘红黑树定时器 | step7 |
| `co_sleep` / `sleep_until` | step5 / step7 |
| `epoll` + `EpollFilePromise`/`Awaiter` | step12 |
| 事件掩码回传（`Promise<uint32_t>`） | step15 |
| `AsyncFile` RAII 包装 | step16 |
| `read_file` / `write_file` | step16 |
| `limit_timeout` | step17 |
| `AsyncLoop` | step18 |
| 套接字 / IP / 地址解析 | step19 |
| 非阻塞 connect（`EINPROGRESS` + `SO_ERROR`） | step19 |
| `IStream`/`OStream`/`StreamBuf` 流抽象 | step20 |
| HTTP 请求/响应 | step21 |
| 服务端 `listen`/`accept` | step23 |
| `spawn_task`（detach 的即发即忘任务） | step24 |

## 注意事项

- **`step6` 运行会 SIGSEGV，这是有意为之的对照**：它的 `when_any` 析构输家后 `std::priority_queue` 定时器仍留悬挂句柄 —— 正是 step7 要解决的问题。
- **`step17` 必须在真 tty 里运行**（`w`/`a`/`s`/`d` 移动、`q` 退出）；退出后终端异常可 `stty sane`。
- **`step23`/`step24`** 是本地服务端，分别监听 `127.0.0.1:8080` / `:8081`，用 `curl` 验证。
- **`step21`/`step22`** 访问外部服务 `142857.red:8080`，该地址不可用属环境问题。
