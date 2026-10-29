# cpp-pi 工程规则

本文件是已确认的硬约束，改动前先读 [DESIGN.md](DESIGN.md) 恢复完整设计上下文。

## 依赖铁律（不可违反）

1. **依赖只许向下指**：`infrastructure → application → domain ← presentation`。
   高层绝不 #include 底层头文件。端口契约（回调式签名 + 协议事件）定义在高层。
2. **协议事件归应用层**（`application/Events.h`）；传输事件归 infrastructure；
   领域事件归 domain。infra 适配器通过注入回调翻译事件，不 include Events.h。
3. **domain 零依赖**：纯 C++，不 include trans-dsl、folly、任何 infra/app 头文件。

## 三方库

4. **不手搓**：JSON 用 `folly::dynamic`/`folly::json`；HTTP/SSE 用 proxygen；
   异步运行时用 `folly::EventBase`；**日志用 quill**（`pi/log/Logging.h`，
   stderr console + 文件每分钟滚动 × 30 备份 ≈ 保留 30 分钟）。
   不引入 simdjson/nlohmann/curl 等替代品。
5. **全部 vendor**：三方库一律进 `third_party/`（git clone/submodule/FetchContent），
   **禁止 `brew install` 等全局安装**作为构建前置条件。当前已 vendor：
   folly(子集)/fmt/glog/fast_float/boost-headers(按需)/quill。
6. **C++20**（folly 要求；trans-dsl-2 在 C++20 下已验证）。

## 架构

7. **前后端分离**：前端复用 pi TS TUI，协议 = pi 的 NDJSON RPC（`rpc-types.ts`），
   stdin 收 commands / stdout 出 responses+events。后端 socket 库不需要。
   **stdout 只许出协议帧**——日志一律走 `pi/log`（console sink = stderr）。
8. **DCI 三层**：role = 业务逻辑（具体类，单实现不劈接口）；context = 纯状态碎片；
   `XxxService` 只绑 role，`XxxContext` 继承碎片，`XxxImpl : XxxService, XxxContext`
   用 `IMPL_ROLE` 补 context 槽位。**不用 `IMPL_ROLE_WITH_VAR`**（impl IS-A context）。
9. **异步统一走事件**：LLM/工具/DB 读写都 `WAIT_ON(EV_*)`，DB 不做事务外 flush。
   `WAIT_ON` handler 不可捕获（`sizeof==1`），共享态经 `toRole`/`ROLE` 拿。

## trans-dsl 备忘

- `__loop` 只跑 2 趟（1 次重跑）；动态轮数直接实例化 `Loop<N,...>`
  （本版 `__loop_max`/`__forever` 引用了未定义的 `Loop_`，悬空）。
- `__listeners(...)` 宏自带分号，必须先起别名再传给 `__bind_listener`。
- `__until(pred)` 退出返回环境状态（默认 SUCCESS）。
- 结构确定的并行用 `__concurrent`；动态数量（LLM 返回任意 tool calls）用
  聚合 action：内部逐个发起、计数收齐后 post 单个事件。
- `settle` 类操作不要 `std::move` 事务中间态——谓词可能还要读。

## 构建/验证

```bash
cmake -B build -DTDSL_ROOT=../trans-dsl-2 && cmake --build build
printf 'x\n/q\n' | ./build/cpp-pi   # 冒烟: 应看到两轮 action #1/#2/#5 后退出
```
