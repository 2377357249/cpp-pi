# cpp-pi 设计文档

> pi（earendil-works/pi, TS monorepo）的 C++ 重写。
> 架构关键词：**前后端分离 · DDD 四层 · DCI（role/context/impl 三层组合） · trans-dsl 事务化编排**。
> 本文档是全部已收敛的设计决策，新会话从这里恢复上下文。

---

## 1. 总体架构：前后端分离

```
┌─────────────────────┐   stdin/stdout    ┌──────────────────────────┐
│  pi TUI (TS, 不变)   │ ◄── NDJSON ────► │  cpp-pi backend (C++20)   │
│  或任意 RPC client   │   JSON lines     │  DDD + DCI + trans-dsl    │
└─────────────────────┘                  └──────────────────────────┘
```

- **协议直接复用 pi 的 RPC mode**：`pi/packages/coding-agent/src/modes/rpc/rpc-types.ts`
  —— commands 从 stdin 进（`prompt`/`steer`/`abort`/`set_model`/`compact`/`bash`/`fork`…），
  responses + streaming events 从 stdout 出。**后端兼容该协议 ⇒ 现有 TUI 零改动接入**。
- 后端二进制即 `cpp-pi`，`cpp-pi --mode rpc` 作为被前端拉起的子进程（与 `pi rpc` 相同用法）。

## 2. 四层（依赖只许向下指：`interface → application → domain → infra`）

| 层 | 内容 | 禁止依赖 |
|---|---|---|
| `interface/` (pi::iface) | 对外消息：`rpc/` NDJSON 协议端、`cli/`；DSL 编排的事务 `trans/`（AgentTurn、SessionLoop、驱动服务）；**出向外部协议的适配器** `adapter/`（LLM provider、工具执行器、session store——它们是"对外的消息"的另一半） | — |
| `application/` (pi::app) | **只放 trans-dsl action**（薄壳：exec/handleEvent 转发到 role） | interface |
| `domain/` (pi::domain) | **业务逻辑所在地**：role（domain/role/）+ context 碎片（domain/context/）+ Service/Impl 合流 + 聚合/值对象/领域服务/端口/协议事件 | application、interface |
| `infra/` (pi::infra) | 全层可用的技术设施：log(quill)/http(libcurl)/process(shell)/config(codex 凭证) | **任何 pi 层**（最底，不能回指，否则成环） |

关键判据：**infra 不认识 domain 类型**——所以实现 domain 端口的适配器不住 infra，住 `interface/adapter/`（端口实现在"对外消息"层，与 rpc/cli 同层不同侧）。

**端口全部回调式签名**（`complete(req, std::function cb)`）：适配器不感知"事件"存在，事件翻译由注入回调完成——适配器连 `Events.h` 都不 include。

## 3. 两种"事件"，各归各位

- **协议事件**（domain/event/Events.h）：事务对外的响应契约。`EV_LLM_DONE` `EV_TOOLS_DONE` `EV_STORE_DONE` `EV_LOAD_DONE` `EV_USER_INPUT` `EV_ABORT` `EV_RETRY_DUE`。
- **传输事件**（infra 内部）：SSE chunk、fd 就绪、stdin 行。infra 内部消化后经回调翻译为协议事件。
- 领域事件（`TurnCompleted` 等，给审计/遥测）以后再加，归 domain。

## 3.4 并发模型（单 loop 线程 + worker 池）

- **`infra::EventLoop`**（`infra/async/`）：唯一的事务/领域线程。`post()` 线程安全，
  `runUntil(done)` 只能由 owner 线程调用——所有 role/context/sink/Session 访问都在
  loop 线程，**领域态零锁**。
- **`infra::ThreadPool`**：收容同步阻塞原语（`curl_easy_perform`/`spawnMerged`/文件
  扫描/backoff 睡眠/会话加载）。适配器把回调 marshal 回 loop：`AsyncLlmProvider`
  （llmPool×1）、`LocalToolExecutor::setAsync`（toolPool×N，真并行 tool calls）、
  `Retryer` 的 `EV_RETRY_DUE` 定时器、RPC `bash`/load 的 pool 任务→`loop.post`。
- **泵送**：`EventQueueCtx::onPost` → `loop.post(pump)`，延迟投递绝不重入
  `handleEvent`；`post()` 只在 loop 线程被调（worker 一律先 marshal）。
  `AgentSessionService::startTurn/startCompact` 返回即挂起，`runTurn` 保留同步
  包装（CLI）。
- **两条事务类型**：`AgentTurn`（run）与 `CompactProc`（压缩）共享同一 impl
  context 树，任一时刻只有一条活跃，`pump` 把队列事件喂给活跃事务。
- **前端**：`RpcFrontend`/`TuiFrontend` 都无 turn 线程——RPC stdin reader jthread
  投递行；TUI 专属 `loopThread_` 驱动 `runUntil`，UI 线程提交经 `loop.post` 进
  `pending_` 队列由 `pumpPending` 排干。RPC `inbox_` 头部把守保序（blocked 时
  abort 系/preempt 置旗堵头部，其余照常分发=pi 单线程语义）；`busy_` 覆盖
  run+排干链+auto-compact 窗口。
- **abort 链路**：`service.abortRequested_` 原子旗标是唯一真值——事务谓词
  （TurnDone）、`ToolRunner` 派发、`BuiltinTools::aborted`（bash spawn 杀进程树）、
  `OpenAiProvider::setAbortCheck`（断 SSE 流）、`Retryer` backoff（50ms 分片醒）
  共用。preempt 命令（compact/new_session/switch/fork/clone）置旗后被新事务起点
  复位；`abort_retry` 只杀 backoff sleep 不动 run。

## 3.5 领域规则回 domain（不漂在 role/action 里）

- `domain/turn/TurnPolicy.h`：turn 是否结束/要不要工具 的统一判定。
- `Session` 聚合自带**持久化游标**：`beginTurn()`/`unpersisted()`/`markPersisted()`——增量记账不再手工放 context。
- `domain/llm/StopReason.h`：领域枚举 + `toWire()`（"stop"/"toolUse"/"error"），codec 只调它。
- role 只留编排 + 发事件；构造 Message 一律走 Session 工厂方法。

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

// 轮次级: pi agent loop 的事务化表达 (已实现)
AgentTurn = Loop<64,
   __with_id(1, __asyn(RequestCompletion))           // WAIT_ON(EV_LLM_DONE)
 , __with_id(2, __asyn(ExecuteTools))                // WAIT_ON(EV_TOOLS_DONE)
 , __with_id(3, __asyn(FinishTurn))
 , __with_id(5, __asyn(PersistDelta))                // WAIT_ON(EV_STORE_DONE)
 , __optional(RetryPending,
      __with_id(6, __asyn(RetryBackoff)))            // WAIT_ON(EV_RETRY_DUE)
 , __until(TurnDone)>;    // finished && !retryInFlight

// 压缩级 (已实现): 两段摘要 → 组 entry → 落盘
CompactProc = __transaction
( __optional(NeedsHistorySummary, __asyn(Summarize)) // WAIT_ON(EV_LLM_DONE)
, __optional(NeedsPrefixSummary,  __asyn(Summarize))
, __asyn(AssembleCompaction), __asyn(PersistDelta) );

// auto-retry: RetryPolicy (domain) 判可重试 (RETRYABLE 模式非 overflow),
// Retryer::begin 发 auto_retry_start + ioPool 分片睡到 EV_RETRY_DUE;
// abort_retry → Retryer::cancel 只杀 sleep. 重试 completion 解出后
// Completer::settle 发 auto_retry_end.

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

- `interface/rpc/`：stdin 行 → `RpcCommand` → 服务命令（prompt→runTurn；abort→`EV_ABORT`/`stop()` 待接）；新增消息 → pi AgentEvent 形状的 streaming 行。
- 命令面照抄 `rpc-types.ts` 全集（见第 8 节迁移表），响应/event 字段与其对齐 ⇒ TS 前端零改动。
- JSON：`folly::dynamic`/`folly::json`（vendored 子集）。
- **事件出口统一走 `IAgentEventSink`**（domain 端口）：role 在事件发生点同步回调，
  前端注入各自实现——`RpcEventSink` 序列化成线帧上 stdout，`TuiEventSink` 更新聊天模型，
  CLI 用 Null。语义 = pi `runAgentLoop`：turn_start/end 按 assistant round 发，
  `message_update` 携带 `assistantStreamEvent`（`text_delta`/`thinking_delta`/`toolcall_*`），
  `thinking_end` 带 `content` 全文而非 `delta`；tool_execution_start 逐个宣布、
  每个 start 恰好配对一份 result（abort 落在派发中时回 "Operation aborted"）。

## 7.5 模型目录 / 思维链 / 会话共享件

- **模型**：`domain/context/ModelCtx` 存活跃 `Model` 值对象 + `thinkingLevel`；
  `domain/model/ModelCatalog`（静态条目 + `~/.pi/agent/models.json` 覆盖合并，
  `interface/adapter/model/ModelCatalogLoader`）。`CompletionRequest` 携带**整个 Model**
  （provider 需要 `reasoning` 等能力标志，不止 id）+ `thinkingLevel`。
- **provider 切换**：`LlmProviderMux`（`interface/adapter/llm/`）按 `model.provider`
  路由到已注册 provider，切模型不重建事务；`AgentSessionService::setModel` 对
  非 reasoning 模型把 thinkingLevel 收敛为 "off"；`cycleModel(dir)` 在**全目录**上
  轮转（跨 provider，pi 语义），≤1 候选返回 null。`--provider mock` 模式下目录
  密封为纯 mock 条目（不读 codex 目录/不注册真 provider，离线保证）。
- **思维链**：provider 产生 `ThinkingStart/Delta/End` 流事件 → `thinking_start/
  thinking_delta/thinking_end` 线帧 + 持久化 `thinking` content block。mock provider
  在 `model.reasoning && level!="off"` 时发确定性 thinking；OpenAiProvider 走
  Responses SSE（`httpPostJsonStream`），`reasoning_summary_text_*`/`reasoning_text_*`
  翻译成 thinking 流，`reasoning.effort` 映射 thinkingLevel，encrypted reasoning
  项回放（`store:false` 端点要求）。
- **会话共享件**（RPC/TUI 复用，避免各抄一份）：`domain/session/SessionFork`
  （fork/clone 分支语义）、`interface/trans/CompactRunner`（manual/auto compaction
  全流程+回调）、`interface/session/SessionExport`（HTML 导出）、
  `ISessionStore::list(cwd)`（`/resume` 会话枚举，JsonlSessionStore 扫目录头条目）。

## 8. pi → cpp-pi 功能迁移路线

| pi package/特性 | cpp-pi 落点 | 优先级 |
|---|---|---|
| `agent` agent-loop | `interface/trans/AgentTurn` proc + domain roles | P0 骨架已立 |
| `coding-agent` RPC mode | `interface/rpc` NDJSON 协议 | P0 已对齐(30 IT) |
| LLM provider (openai/responses) | `interface/adapter/llm/OpenAiProvider` + `infra/http` | P0 已通真模型 |
| 内置工具 read/bash/edit/write | `interface/adapter/tools` + `ToolCalls` proc | P1 |
| `ai` 各 provider(anthropic/openai/…) | `interface/adapter/llm/*Provider`（HTTP+SSE） | P1 |
| session 持久化/fork | `ISessionStore`+JSONL adapter, `fork` 命令 | P1 |
| compaction | `Compact` proc | P2 |
| OAuth/API key 登录 | `Auth` proc + `ICredentialStore` | P2 |
| permission/approval | `PermissionGate` proc + `EV_PERMISSION_RSP` | P2 |
| MCP | `interface/adapter/mcp` client | P3 |
| skills/extensions/slash-commands | domain/application 资源加载 | P3 |
| `tui` | **不迁移 pi TS 版**; 纯 C++ TUI `interface/tui/`（vendored FTXUI，`ChatModel`+`TuiEventSink` 复用 `IAgentEventSink` 实时渲染；slash 命令面/模型·thinking·session 选择器/Ctrl+P 切模型·Shift-Tab thinking·Ctrl-T 思维链显隐·Ctrl-O 工具输出展开；footer=模型/thinking/queued/tokens/ctx%） | ✅ 功能对齐（PTY 冒烟验证） |
| `durable` | `JournalCtx` 事件重放 | P4 |
| telemetry | 监听器体系 | P4 |

## 9. 三方依赖（不手搓, 全部 vendor 进 third_party/）

| 依赖 | 位置 | 用途 |
|---|---|---|
| **trans-dsl-2** | `../trans-dsl-2`（兄弟目录, `-DTDSL_ROOT` 覆盖） | 事务 DSL |
| **folly**(子集) | `third_party/folly` + `third_party/folly-config` | `dynamic`/`json`；后续 EventBase/Synchronized |
| fmt | `third_party/fmt` | folly 依赖 |
| glog | `third_party/glog` | folly 依赖 |
| fast_float | `third_party/fast_float` | folly Conv 浮点解析（libc++ 无 float from_chars） |
| boost-headers(按需子集) | `third_party/boost-headers` | folly 头文件闭包（~38MB, 非全量 179MB） |
| quill | `third_party/quill` | 异步日志: stderr console + 文件每分钟滚动×30 备份 ≈ 保留 30 分钟 |
| libcurl | macOS 系统库（非 brew） | `infra/http` 当前 HTTP 后端 |
| **ftxui** | `third_party/ftxui`（v6.1.9 vendored） | `--mode tui` 全屏界面 |
| **proxygen** | 待 vendor | HTTPS+SSE 流式/并发，替换 libcurl |

- 前端协议走 stdin/stdout NDJSON，**不需要 socket 库**。
- 网络只服务于 LLM provider / MCP——HTTP 细节关在 `infra/http`，domain 协议细节关在 `interface/adapter`。
- `folly-config/folly/folly-config.h` 是手写的 macOS 静态配置（对应 folly 的 cmake 模板，裁剪到最小面）。
- **禁止 `brew install` 作为构建前置**（见 AGENTS.md）。

## 10. 构建

```bash
cmake -B build -DTDSL_ROOT=/path/to/trans-dsl-2 && cmake --build build
./build/cpp-pi            # CLI REPL (演示)
./build/cpp-pi --mode rpc # RPC 后端 (前端接入)
```

C++20（folly 要求 C++20；trans-dsl-2 在 C++20 下编译验证通过）。
