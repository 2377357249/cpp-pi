# cpp-pi 设计文档

> pi（earendil-works/pi, TS monorepo）的 C++ 重写。
> 架构关键词：**前后端分离 · DDD 四层 · DCI（role/context/impl 三层组合） · trans-dsl 事务化编排**。
> 本文档是全部已收敛的设计决策，新会话从这里恢复上下文。

---

## 1. 总体架构：前后端分离

```
┌─────────────────────┐   stdin/stdout    ┌──────────────────────────┐
│  pi TUI (TS, 不变)   │ ◄── NDJSON ────► │  cpp-pi backend (C++17)   │
│  或任意 RPC client   │   JSON lines     │  DDD + DCI + trans-dsl    │
└─────────────────────┘                  └──────────────────────────┘
```

- **协议直接复用 pi 的 RPC mode**：`pi/packages/coding-agent/src/modes/rpc/rpc-types.ts`
  —— commands 从 stdin 进（`prompt`/`steer`/`abort`/`set_model`/`compact`/`bash`/`fork`…），
  responses + streaming events 从 stdout 出。**后端兼容该协议 ⇒ 现有 TUI 零改动接入**。
- 后端二进制即 `cpp-pi`，`cpp-pi --mode rpc` 作为被前端拉起的子进程（与 `pi rpc` 相同用法）。

## 2. DDD 四层（依赖只许向下指：底层依赖高层，高层不感知底层）

```
presentation ─► application ─► domain
                    ▲             ▲
                    └── infrastructure ┘   (实现 domain/app 定义的端口)
```

| 层 | 内容 | 禁止依赖 |
|---|---|---|
| `domain/` | Message/ToolCall/Tool/Completion/Model 值对象; **Session 聚合根**(append-only 消息历史); 端口 `ILlmProvider` `IToolExecutor` `ISessionStore` | 一切（纯 C++） |
| `application/` | trans-dsl 事务与 action、context 碎片、role、service/impl、协议事件、RPC 命令分发 | infra、presentation |
| `infrastructure/` | 端口实现：HTTP LLM provider、本地工具执行器、JSONL store、MCP client | —（最底层） |
| `presentation/` | RPC 前端（NDJSON ↔ 协议事件）；CLI REPL | infra 具体类 |

**端口全部回调式签名**（`complete(req, std::function cb)`）：infra 实现不感知"事件"存在，事件翻译由注入回调完成——infra 连 `Events.h` 都不 include。

## 3. 两种"事件"，各归各位

- **协议事件**（application/Events.h）：事务对外的响应契约。`EV_LLM_DONE` `EV_TOOLS_DONE` `EV_STORE_DONE` `EV_LOAD_DONE` `EV_USER_INPUT` `EV_ABORT`。只有应用层知道 ID。
- **传输事件**（infrastructure 内部）：SSE chunk、fd 就绪、stdin 行。infra 内部消化后经回调翻译为协议事件。
- 领域事件（`TurnCompleted` 等，给审计/遥测）以后再加，归 domain。

## 4. DCI 三层组合（每个 proc 一套三元组）

```
TransactionInfo ──toRole──► XxxImpl ──继承──┬── XxxService  (role 树: 只绑 role)
   (框架绑定, updateUserContext)             └── XxxContext  (context 树: 只绑状态碎片)
```

- **context 碎片**：纯状态载体，粒度按职责切（`EventQueueCtx` 队列+`post()`、`TurnStateCtx` lastCompletion/pendingToolResults、`IoCtx`…）。`XxxContext` 继承若干碎片。
- **role**：具体业务类，业务逻辑写在 role 方法里；`USE_ROLE(XxxCtx)` 声明依赖的 context 碎片；异步回调里 `ROLE(XxxCtx).pub()` 发事件——**USE_ROLE 的存在理由：回调触发时手里没有 `TransactionInfo`**。role 持有哑 IO 对象引用（provider/store），构造注入。
- **XxxService**：只继承各 role，`IMPL_ROLE` 填角色槽位；不碰 context。
- **XxxImpl** `: XxxService, private XxxContext`：`IMPL_ROLE` 补 context 槽位（impl IS-A context，无需 `IMPL_ROLE_WITH_VAR`）。
- role 接口（DEFINE_ROLE 抽象层）**暂不引入**——出现第二个需运行时互换的实现再抽。
- 收益：Service↔Context 解耦（测试可配 TestContext）；role 按需 USE 碎片；role 间互访走 `ROLE(X)` 同一 this 虚分发。

## 5. trans-dsl 事务/状态机清单

```cpp
// 会话级(常驻, 整个对话生命周期一个事务)
SessionLoop = __transaction
( __asyn(LoadHistory)                                // WAIT_ON(EV_LOAD_DONE)
, __forever( __asyn(AwaitUserInput)                  // WAIT_ON(EV_USER_INPUT)
           , __procedure(AgentTurn)
           , __optional(ContextOverflow, __procedure(Compact)))
);

// 轮次级: pi agent loop 的事务化表达
AgentTurn = __procedure
( __loop_max(64,
      __with_id(1, __asyn(RequestCompletion))        // WAIT_ON(EV_LLM_DONE)
    , __with_id(2, __asyn(ExecuteTools))             // WAIT_ON(EV_TOOLS_DONE)
    , __with_id(5, __asyn(PersistDelta))             // WAIT_ON(EV_STORE_DONE)
    , __until(TurnDone))                              // 无 toolCalls → 本轮结束
, __time_guard(TURN_TIMER, TURN_TIMEOUT_MS)
, __finally(__sync(FlushTurnStats))
);

// 工具调用级(每 call 一个 proc, pi 最有特色处)
ToolCalls = __procedure( PermissionGate → ExecTool → Report );
//   PermissionGate = __optional(NeedsApproval, __asyn(AskUser))  // WAIT_ON(EV_PERMISSION_RSP)
//   ExecTool       = __asyn(RunTool), __on_fail(MarkToolError), __redo_if(Retryable)

// 其余 proc: Interrupt(stop/EV_ABORT) · Compact · Auth(OAuth device flow)
//   Subagent(__procedure 内嵌独立 impl 的 AgentTurn) · RetryProvider(__redo_if) 
```

要点：
- `__loop` 只允许 1 次重跑（2 趟）；动态轮数用 `__loop_max(n)` 或 `__forever`；`__until(pred)` 退出返回环境状态（默认 SUCCESS）。
- 结构确定的编排用 `__concurrent`/`__procedure`；**动态数量**（LLM 返回任意个 tool call）用聚合 action：单 action 内逐个发起、计数器等齐 post 一个 `EV_TOOLS_DONE`。
- `WAIT_ON` 的 handler 必须是空类/无捕获 lambda（`sizeof==1`），共享态一律 `trans.toRole<XxxImpl>()` 取。
- 监听器 `__bind_listener` + `ObservedActionIdRegistry` = telemetry/footer 雏形，编译期零开销。

## 6. 持久化

| 数据 | 方式 |
|---|---|
| 会话历史 | `ISessionStore`（domain 端口，回调式）→ `JsonlSessionStore`（infra）：append-only JSONL，对齐 `~/.pi/sessions/*.jsonl`；**以 `EV_STORE_DONE`/`EV_LOAD_DONE` 进事务**（`PersistDelta`/`LoadHistory` action），失败用 `__on_fail`/`__redo_if` 降级；原子写 tmp+rename |
| 事件 journal | `JournalCtx` 碎片在 `post()` 单点 tee 事件流 → 崩溃重放 `handleEvent`（event sourcing，对应 pi `durable` 包）— 后置 |
| settings/凭证 | `ISettingsStore`/`ICredentialStore` 小端口，文件 0600 + 原子写 |

原则：Session 聚合决定"什么该落盘"，context 只管事件流，两通道互不穿越。

## 7. RPC 边界（前后端协议适配层）

- `presentation/rpc/`：stdin 行 → `RpcCommand` → 应用服务命令（prompt→`EV_USER_INPUT` 喂事务；abort→`EV_ABORT`/`stop()`）；事务监听器/context 状态 → streaming events 行。
- 命令面照抄 `rpc-types.ts` 全集（见第 8 节迁移表），响应/event 字段与其对齐 ⇒ TS 前端零改动。
- JSON 解析：nlohmann/json（单头）。

## 8. pi → cpp-pi 功能迁移路线

| pi package/特性 | cpp-pi 落点 | 优先级 |
|---|---|---|
| `agent` agent-loop | `AgentTurn` proc + context/roles | P0 骨架已立 |
| `coding-agent` RPC mode | `presentation/rpc` NDJSON 协议 | P0 前端接入前提 |
| 内置工具 read/bash/edit/write | `infra/tools` + `ToolCalls` proc | P1 |
| `ai` 各 provider(anthropic/openai/…) | `infra/llm/*Provider`（HTTP+SSE） | P1 |
| session 持久化/fork | `ISessionStore`+JSONL, `fork` 命令 | P1 |
| compaction | `Compact` proc | P2 |
| OAuth/API key 登录 | `Auth` proc + `ICredentialStore` | P2 |
| permission/approval | `PermissionGate` proc + `EV_PERMISSION_RSP` | P2 |
| MCP | `infra/mcp` client | P3 |
| skills/extensions/slash-commands | application 资源加载 | P3 |
| `tui` | **不迁移——前端保持 TS 版不变** | — |
| `durable` | `JournalCtx` 事件重放 | P4 |
| telemetry | 监听器体系 | P4 |

## 9. 三方依赖（不手搓）

| 依赖 | 用途 |
|---|---|
| **trans-dsl-2** | 事务 DSL（本地源码，`-DTDSL_ROOT`，默认 `../trans-dsl-2`） |
| **folly** | `dynamic`/`json`（JSON）、`EventBase`（infra 异步运行时）、`Synchronized`（跨线程向事件队列投递） |
| **proxygen** | HTTPS client + SSE 流式（LLM provider 适配器；长在 folly 上的 Meta HTTP 栈） |

- 前端协议走 stdin/stdout NDJSON，**不需要 socket 库**。
- 网络只服务于 LLM provider / MCP——全部关在 infra 适配器内。

## 10. 构建

```bash
cmake -B build -DTDSL_ROOT=/path/to/trans-dsl-2 && cmake --build build
./build/cpp-pi            # CLI REPL (演示)
./build/cpp-pi --mode rpc # RPC 后端 (前端接入)
```

C++17（trans-dsl-2 的基准）。
