# cpp-pi 工程规则

本文件是已确认的硬约束，改动前先读 [DESIGN.md](DESIGN.md) 恢复完整设计上下文。

## 依赖铁律（不可违反）

1. **四层，依赖只许向下指**：
   `interface → application → domain → infra`。
   - `interface/`（pi::iface）：对外消息（rpc/cli/adapter）+ DSL 编排的事务（trans/）。
     出向外部协议（LLM HTTP、工具进程、存储）的适配器也在这里。
   - `application/`（pi::app）：**只放 trans-dsl action**，薄壳，转发到 role。
   - `domain/`（pi::domain）：role（业务逻辑）+ context（状态）+ 聚合/值对象/
     领域服务/端口/协议事件。role 间用 USE_ROLE/IMPL_ROLE 互找。
   - `infra/`（pi::infra）：**不依赖任何 pi 层**的技术设施（log/http/shell/config），
     所有层可用。
2. **协议事件归 domain**（`domain/event/Events.h`）；传输事件留在 infra；
   端口适配器经注入回调翻译事件，不反向依赖。
3. domain 不许 include application/interface；infra 不许 include 任何 pi 层
   （否则会成环——要 domain 类型的适配器一律放 `interface/adapter/`）。

## 三方库

4. **不手搓**：JSON 用 `folly::dynamic`/`folly::json`；HTTP 先走
   `infra/http`（libcurl，macOS 系统库），proxygen 留给 SSE/并发；
   **日志用 quill**（`infra/log/Logging.h`，stderr console +
   文件每分钟滚动 × 30 备份 ≈ 保留 30 分钟）。
5. **全部 vendor**：三方库一律进 `third_party/`（git clone/submodule/FetchContent），
   **禁止 `brew install` 等全局安装**作为构建前置条件。
6. **C++20**（folly 要求；trans-dsl-2 在 C++20 下已验证）。
   - **用高特性**：`std::format`/`std::ranges`/`std::filesystem`/`std::optional`/
     `std::variant`/`std::chrono`；禁 `snprintf`/`gmtime`/手写循环找元素等低层写法。
   - **禁魔法数**：所有裸字面量换具名 `constexpr`。
   - **禁 try/catch**：错误一律用返回值传播（`folly::Expected`/`std::optional`/
     `bool`）。唯一例外是库边界适配 `infra/util/JsonSafe.h`（folly `parseJson`
     只抛异常——项目代码统一走 `infra::parseJsonSafe`/`jsonOr`）。

## 架构

7. **前后端分离**：前端复用 pi TS TUI，协议 = pi 的 NDJSON RPC（`rpc-types.ts`），
   stdin 收 commands / stdout 出 responses+events。
   **stdout 只许出协议帧**——日志一律走 stderr。
8. **DCI 三层**：role = 业务逻辑（具体类，单实现不劈接口）；context = 纯状态碎片；
   `XxxService` 只绑 role，`XxxContext` 继承碎片，`XxxImpl : XxxService, XxxContext`
   用 `IMPL_ROLE` 补 context 槽位。**不用 `IMPL_ROLE_WITH_VAR`**（impl IS-A context）。
   role/context/service/impl 全在 `domain/`。
9. **领域规则回 domain**：不变式和判定（turn 是否结束、消息怎么构造、
   增量怎么算）放聚合/领域服务（TurnPolicy、Session 的持久化游标、
   StopReason）；role 只留编排和发事件；**action 永远是薄壳**。
10. **异步统一走事件**：LLM/工具/DB 读写都 `WAIT_ON(EV_*)`，DB 不做事务外 flush。
    `WAIT_ON` handler 不可捕获（`sizeof==1`），共享态经 `toRole`/`ROLE` 拿。

## trans-dsl 备忘

- `__loop` 只跑 2 趟（1 次重跑）；动态轮数直接实例化 `Loop<N,...>`
  （本版 `__loop_max`/`__forever` 引用了未定义的 `Loop_`，悬空）。
- `__listeners(...)` 宏自带分号，必须先起别名再传给 `__bind_listener`。
- `__until(pred)` 退出返回环境状态（默认 SUCCESS）。
- 结构确定的并行用 `__concurrent`；动态数量（LLM 返回任意 tool calls）用
  聚合 action：内部逐个发起、计数收齐后 post 单个事件。
- `settle` 类操作不要 `std::move` 事务中间态——谓词可能还要读。

## 测试/覆盖率

11. **集成测试用 gtest**（vendored `third_party/googletest`），粒度 =
    外部输入 NDJSON -> stdout 输出帧字段，进程级黑盒
    （`tests/it/RpcHarness.h` spawn `cpp-pi --mode rpc --provider mock`）。
    **测试必须密封**：spawn 一律带 `--provider mock`，不许打真网络。
12. **用例对齐 pi 原仓库测试套件**：
    `rpc.test.ts` / `rpc-prompt-response-semantics.test.ts` / `rpc-jsonl.test.ts`
    的每条 case 在本仓 `tests/it/` 有对应 TEST。改协议行为先改测试。
13. **覆盖率 ≥80% 行覆盖**（clang source-based coverage，只计 `src/`，
    外部 IO 适配器 OpenAiProvider/HttpClient/CodexConfig 除外——
    它们由真 LLM 冒烟覆盖）：`tools/coverage.sh` 出报告并做门禁。

## 构建/验证

```bash
cmake -B build -DTDSL_ROOT=../trans-dsl-2 && cmake --build build
./build/cpp-pi-it                     # 集成测试 (30 cases, 全 mock 密封)
./tools/coverage.sh                   # 覆盖率门禁 (当前 ~94% 行)
printf 'x\n/q\n' | ./build/cpp-pi --provider mock   # 冒烟(离线)
printf 'x\n/q\n' | ./build/cpp-pi    # 有 codex 凭证时打真模型
printf '{"type":"prompt","message":"hi"}\n' | ./build/cpp-pi --mode rpc --provider mock
```
