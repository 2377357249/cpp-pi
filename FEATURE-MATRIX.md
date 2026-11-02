# pi → cpp-pi 功能对等矩阵 (功能迁移的单一事实来源)

> 来源: 对 `/Users/dongxu/Desktop/code/pi` 的四路完整审计 (rpc-types/rpc-mode/
> agent-session/session-manager/tools/compaction/export-html/ai 包/skills/
> slash-commands/extensions/mcp/settings)。
> 状态图例: ✅ 已实现 · 🟡 骨架/部分 · ⬜ 未实现 · — 不迁移

## 0. 线协议框架 (jsonl.ts / output-guard / rpc-mode 启停)

| 项 | pi 语义 | 状态 |
|---|---|---|
| 帧格式 | LF 分隔 JSONL; U+2028/2029 不当分隔符; 行尾 \r 剥掉; `JSON.stringify+"\n"` | ✅ |
| stdout 守护 | takeOverStdout: 一切写 stdout 重定向 stderr; 协议帧经 writeRawStdout 串行写, ENOBUFS/EAGAIN 10ms 重试 | 🟡 直接写 cout, 无重试 |
| 命令并发 | 每行 handler 不 await → 并发; session 内部互斥; 响应可与事件交错 | ⬜ 串行 |
| 响应包络 | `{id?,type:"response",command,success,data?}`; 无 data 时键整体省略 | ✅ |
| id 回显 | 有 id 才回显 | ✅ |
| 解析失败 | `{command:"parse",success:false,error:"Failed to parse command: <msg>"}` | 🟡 文案是 "invalid json:" |
| 未知命令 | `error:"Unknown command: <type>"` | 🟡 文案 "unsupported command" |
| handler 异常 | `error:<err.message>` | ✅ |
| extension_ui_response | 入站按 id 解 pending request; 未知 id 丢弃; 无响应帧 | ⬜ |
| extension_error | `{type:"extension_error",extensionPath,event,error}` | ⬜ |
| 退出 | stdin end→0; SIGHUP→129; SIGTERM→143(不 flush) | ⬜ |

## 1. RPC 命令逐条 (rpc-mode.ts)

| 命令 | pi 语义要点 | 状态 |
|---|---|---|
| prompt | 响应在 disposition 时点发(preflight); "started"=非 streaming 开跑; "queued"=streaming+streamingBehavior(缺→error `"Agent is already processing. Specify streamingBehavior ('steer' or 'followUp') to queue the message."`); "handled"=扩展/input 吞掉; 顺序: queue_update 先于 queued 响应; agent_start 在 started 响应之后; compaction 中 prompt→error `"Cannot submit a prompt while compaction is in progress. Wait for compaction to finish and retry."`; `/`开头→扩展命令; 无模型→`formatNoModelSelectedMessage`; 无 key→`formatNoApiKeyFoundMessage` | 🟡 started 路径对, queued/模板/校验缺 |
| steer | 任何状态都入队(扩展命令文本除外→throw); input handler 可 transform/handled; skill/模板展开始终生效; queue_update 先于响应 | 🟡 只入队, 无展开/校验 |
| follow_up | 同 steer | 🟡 同 |
| abort | abortRetry+abortCompaction+abortBranchSummary+agent.abort; **响应在 agent_settled 之后发**; aborted turn 的 assistant stopReason="aborted" | 🟡 空响应 |
| clear_queue | 快照两队列→清→agent.clearAllQueues→queue_update(空)先于响应 | ✅ |
| new_session | `{parentSession?}`(写进 header 的 parentSession=文件路径); session_before_switch 可 cancel→{cancelled:true}; 旧 agent_settled 先发; session_start{reason:"new"}; 响应在 rebind 后 | 🟡 parentSession/事件/cancel 缺 |
| get_state | 字段对照表见 §2 | 🟡 部分 |
| set_model | getAvailableSnapshot 查 provider+id; miss→`"Model not found: p/m"`; checkAuth 失败→`"No API key for p/m"`; append model_change entry; 思考级 resolved (scoped→settings→global→current→medium) 可能发 thinking_level_changed; model_select 事件; 返回 Model 对象 | ✅ 目录校验+entry; thinking 按能力收敛; 缺 model_select 事件/checkAuth  |
| cycle_model | scoped 或全量可用列表; ≤1 候选→`data:null`(显式null非省略); `(cur+1)%len`; 返回 `{model,thinkingLevel,isScoped}` | ✅ 全目录跨provider轮转+ModelCycleResult形状; 缺 scoped  |
| get_available_models | `{models:Model[]}` 全目录 | ✅ 全目录  |
| set_thinking_level | 不在可用级→clamp; 有效变化→thinking_level_change entry+thinking_level_changed 事件+thinking_level_select; 7级: off/minimal/low/medium/high/xhigh/max | ✅ entry+事件; 缺clamp/thinking_level_select  |
| cycle_thinking_level | !model?.reasoning→`data:null`; else `(i+1)%levels` over getSupportedThinkingLevels (非 reasoning→["off"]; xhigh/max 需 thinkingLevelMap) | ✅ reasoning判null; 缺支持级表  |
| get_available_thinking_levels | 无模型→全7级; 有→getSupportedThinkingLevels | ✅ reasoning→["off"]  |
| set_steering_mode | `agent.steeringMode` + settings 持久化 | 🟡 存内存,默认错 |
| set_follow_up_mode | 同上 | 🟡 同 |
| compact | abort→compaction_start{reason:"manual"}→无模型→noModel error→prepare: 尾已是compaction→"Already compacted"/太小→"Nothing to compact (session too small)"→session_before_compact(cancel→"Compaction cancelled"/{compaction}→采用)→LLM 摘要(summarization_retry_* 事件)→appendCompaction→compaction_end{reason,result,aborted,willRetry}; 失败→compaction_end{errorMessage:"Compaction failed: <msg>"}; 返回 CompactionResult{summary,firstKeptEntryId,tokensBefore,estimatedTokensAfter?,usage?,details?} | 🟡 假实现 |
| set_auto_compaction | settings.compaction.enabled 持久化; 手动 compact 不受 enabled 影响 | 🟡 内存,默认错 |
| set_auto_retry | settings.retry.enabled | 🟡 内存 |
| abort_retry | 只杀 backoff sleep; 有 sleep→auto_retry_end{success:false,finalError:"Retry cancelled"} | 🟡 空 |
| bash | emitUserBash 扩展点(可接管/替换ops)→executeBash: shellCommandPrefix+shellPath settings; 每块→bash_execution_update{id:cmd.id,delta:sanitized}; BashResult{output(sanitized,tail截断),exitCode(cancelled时undefined),cancelled,truncated,fullOutputPath?}; recordBashResult→BashExecutionMessage 入会话(streaming 时延迟到 run 末); **不发 message_*/entry_appended**; 缓冲=尾部2×50KB; >50KB 起 temp 文件 | 🟡 直接 popen, 无净化/截断/事件/消息 |
| abort_bash | 杀全部 bash controller; 部分输出+cancelled:true 仍正常返回 | 🟡 空 |
| get_session_stats | 全量 entry 统计(含被 compact 历史): totalMessages=message 条目数(全角色); tokens=usage entries+compaction/branch_summary usage+toolResult.usage+assistant.usage 求和; cost=usage.cost.total 和; contextUsage{tokens,contextWindow,percent}? | 🟡 只算 assistant usage |
| export_html | 单文件自包含 HTML: template.html/css/js + marked + highlight; SESSION_DATA=base64 JSON {header,entries,leafId,systemPrompt?,tools?,renderedTools?}; 文件名 `pi-session-<base>.html` | 🟡 占位 HTML |
| switch_session | open(path)→校验(文件非法→`"Session file is not a valid pi session"`); teardown 旧(agent_settled 先发)+session_start{reason:"resume"}; {cancelled} | 🟡 载入对; 缺teardown事件/{cancelled} |
| fork | session_before_fork(cancel→{cancelled:true}); 校验 entryId 存在且 position="before"时 type=message+role=user 否则 `"Invalid entry ID for forking"`; targetLeaf=parentId; 新文件=root→leaf 路径(label 重挂); parentSession=旧文件; {text:用户文本,cancelled} | 🟡 SessionFork抽取可用; 缺校验文案/事件 |
| clone | leafId null→`"Cannot clone session: no current entry selected"`; =fork(leafId,position:"at") | 🟡 =fork(leaf,at); 文案对 |
| get_fork_messages | **全部 entry**(全分支)中 type=message && role=user && 非空 → `{entryId,text}[]` | ✅ |
| get_entries | 全部 entry 追加序; since→严格其后(`"Entry not found: <since>"`); {entries,leafId} | 🟡 合成 id |
| get_tree | {entry,children,label?,labelTimestamp?}; roots: parentId null/==id/孤儿; children 按 timestamp 排; label 由 label entry 解析 | 🟡 单链无 label |
| get_last_assistant_text | 投影里最后一个 assistant,跳过 aborted+空content; text块拼接; 空→null | 🟡 未跳过 aborted |
| set_session_name | trim 空→`"Session name cannot be empty"`; appendSessionInfo(清洗[\r\n]+→空+trim); **session_info_changed{name} 事件先于响应** | 🟡 无 entry/事件/校验 |
| get_messages | 投影上下文(含 system/custom/bashExecution/branchSummary/compactionSummary) | 🟡 基础投影已有 |
| get_commands | 扩展命令+prompt模板+skills `{name:skill:name}` 合并; RpcSlashCommand{name,description?,source:"extension"|"prompt"|"skill",sourceInfo{path,source,scope,origin,baseDir?,changelogPath?}} | 🟡 空数组 |

## 2. get_state 字段对照

| 字段 | pi 推导 | 状态 |
|---|---|---|
| model? | agent.state.model 完整 Model(无则省略键) | ✅ 目录实时值 |
| thinkingLevel | agent.state.thinkingLevel | ✅ |
| isStreaming | _isAgentRunActive(含 retry sleep/auto-compact/before-settle) | 🟡 恒false |
| isCompacting | 三个 summarization controller 任一为真 | 🟡 恒false |
| steeringMode/followUpMode | agent.steeringMode/followUpMode; **默认 "one-at-a-time"** | 🟡 默认 "all" 错 |
| sessionFile? | sessionManager.getSessionFile()(内存会话省略) | 🟡 合成名 |
| sessionId | header id (uuidv7) | 🟡 固定串 |
| sessionName? | 最新 session_info entry | 🟡 内存字段 |
| autoCompactionEnabled | settings.compaction.enabled(默认 **true**) | 🟡 默认 false 错 |
| messageCount | 投影消息数(含 system/custom/bashExecution) | 🟡 |
| pendingMessageCount | steering+followUp 队列长 | 🟡 恒0 |

## 3. AgentSessionEvent 全表 (事件名+载荷+触发)

| 事件 | 载荷 | 触发 | 状态 |
|---|---|---|---|
| agent_start | {} | 每次 run/continue 开始(一次 prompt 可有多对) | ✅ |
| turn_start | {} | run 开始+每次 prepareNextTurn 后 | ✅ |
| message_start | {message} | 每条消息(含 user/system/toolResult/queued) | ✅ |
| message_update | {usage,assistantMessageEvent} **无 message 字段**; toolcall_start 变 {id,toolName,contentIndex} | streaming | ✅ IAgentEventSink 实时 |
| message_end | {message} | 配对 start | ✅ |
| tool_execution_start | {toolCallId,toolName,args,parentToolCallId?} | 每次调用前 | ✅ |
| tool_execution_update | +partialResult | 工具回调 | ⬜ |
| tool_execution_end | +result,isError,durationMs | 完成序(并发时乱序完成) | ✅ |
| turn_end | {message,toolResults[]} | 每轮 assistant+其 tool 结果 | ✅ |
| agent_end | {messages,willRetry} | loop 结束; willRetry 会话计算 | 🟡 缺 willRetry |
| agent_settled | {aborted} | run 全链路终态(晚于末次 agent_end) | ⬜ |
| queue_update | {steering[],followUp[]} | 入队/清队列/queued 消息出队(先于其 message_start) | ✅ 部分 |
| compaction_start | {reason:manual\|threshold\|overflow} | | ⬜ |
| compaction_end | {reason,result?,aborted,willRetry,errorMessage?} | | ⬜ |
| entry_appended | {entry} | **仅越带追加**(context_edit/virtual-model/custom)不含普通 message | ⬜ |
| session_info_changed | {name?} | setSessionName | ⬜ |
| thinking_level_changed | {level} | 有效变化时 | ✅ |
| auto_retry_start | {attempt,maxAttempts,delayMs,errorMessage} | | ⬜ |
| auto_retry_end | {success,attempt,finalError?} | | ⬜ |
| summarization_retry_* | scheduled/attempt_start/finished | | ⬜ |
| bash_execution_update | {id?,delta} | bash 命令逐块 | ⬜ |
| session_start/shutdown/session_before_* | 扩展事件 | new/resume/fork | ⬜ |
| extension_ui_request | 8 methods: select/confirm/input/editor/notify/setStatus/setWidget/setTitle/set_editor_text | | ⬜ |

## 4. 队列/steering 语义

- 两队列: steering(轮中注入: loop 头/prepareNextTurn 后/turn_end 后消费) / followUp(仅 agent 将要停时消费)
- mode: "all"=全出队 | "one-at-a-time"=仅最旧一条; **默认 one-at-a-time**(settings)
- 出队→queue_update 先于该消息 message_start; text 精确匹配移除
- 其他队列: _pendingNextTurnMessages/_pendingCustomMessages(turn_end 刷)/_pendingBashMessages
- 状态: 🟡 前端 deque,轮末排空,无模式区分/无中途注入

## 5. 自动行为

| 行为 | pi 语义 | 状态 |
|---|---|---|
| auto_retry | 末条 assistant 为 retryable error(RETRYABLE_PROVIDER_ERROR_PATTERN 命中且非 overflow) 时: auto_retry_start{attempt,maxRetries=3,delayMs=base2000*2^(n-1) cap 60000}→_omitRecoveryAttempt(context_edit 隐藏失败)→sleep→continue; 成功→auto_retry_end{success:true}; 尽→{success:false,finalError} | ⬜ |
| auto-compaction | threshold: contextTokens>window-16384(reserveTokens) → reason:"threshold"; overflow: isContextOverflow/isRecoverableLength → reason:"overflow"+willRetry+一次恢复预算; keepRecentTokens=20000 切点(绝不切 toolResult) | ⬜ |
| summarization retry | 内部重试同 retry 配置; summarization_retry_* 事件 | ⬜ |

## 6. Session 存储 (JSONL)

- 文件: `~/.pi/agent/sessions/--<cwd编码>--/<ISOts>_<uuidv7>.jsonl`; cwd 编码 `--`+路径去首斜杠+[/\:]→`-`+`--`
- header: `{type:"session",version:3,id,timestamp:ISO,cwd,parentSession?}`; 惰性创建(有 user/assistant 才写)
- entry: `{type,id:8-hex,parentId:null|string,timestamp:ISO}` + 11 种子类型字段
- 子类型: message{message} / thinking_level_change{thinkingLevel} / model_change{provider,modelId} / usage{kind,provider,model,usage,note?} / compaction{summary,firstKeptEntryId,tokensBefore,details?,usage?,fromHook?,systemMessage?} / branch_summary{fromId,summary,details?,usage?,fromHook?} / custom{customType,data?} / custom_message{customType,content,details?,display} / context_edit{targetId,replacement:{content}|null} / label{targetId,label} / session_info{name?}
- 迁移 v1→v2(id/parentId 补链)+v2→v3(hookMessage→custom); 末行无\n修复; 坏行跳过; 非 pi 文件→`"Session file is not a valid pi session"`
- fork/clone: createBranchedSession(leafId) 写新文件仅含 root→leaf 路径(label 重挂)
- 状态: 🟡 Entry 域模型+投影已落 domain; JSONL store 实现中

## 7. 内置工具 (默认启用 read/bash/edit/write; grep/find/ls/powershell 注册未默认)

| 工具 | 关键语义 | 状态 |
|---|---|---|
| read | offset/limit; 截断 2000行/50KB(head); 图片探测(4100B嗅探)+转码+resize; 无行号; notices `[Showing lines A-B of T. Use offset=B+1 to continue.]`; errors `ENOENT...`/`Offset N is beyond end of file (M lines total)` | ✅ (resize/非视觉 note 未做) |
| bash(工具) | schema 精确; spawn /bin/bash -c(或 sh); env+PI_* 注入; timeout 校验(≤0→`Invalid timeout: must be a finite number of seconds`; >2147483.647→`Invalid timeout: maximum is...`); tail 截断 2000行/50KB; temp `pi-bash-<hex>.log`; stdout+stderr 合并; `(no output)`; 非零→`Command exited with code N`+isError; structuredContent{output,truncated,fullOutputPath,exitCode,wallTimeMs?} 1MB 上限 | ✅ (structuredContent 不进 wire, codemode 专用) |
| edit | edits[] 精确匹配→NFKC+trailing-space+dash/space 归一化 fuzzy; 多匹配/重叠/空 oldText 错误串精确; BOM/换行符保留; 反序应用; details{diff,patch,firstChangedLine?}; file-mutation-queue | ✅ (file-mutation-queue 未做) |
| write | 覆盖+递归建目录; `Successfully wrote to <path>` | ✅ |
| grep | 外调 `rg --no-config --json --line-number --color=never --hidden` +ignore-case/fixed-strings/glob; context 自己实现; limit默认100; 输出 `path:N: line`/`path-N- ctx`; notices | ✅ (原生 std::regex+遍历实现, 语义对齐) |
| find | 外调 `fd`; `--glob --color=never --hidden --max-results`; `/`→`**/`规则; notices | ✅ (原生遍历, 目录匹配带 `/` 后缀) |
| ls | 字母序(case-insensitive)+`/`目录后缀; limit 500; notices | ✅ |
| powershell | Windows only(非 win 执行时报 `The powershell tool is only available on Windows.`) | ✅ |
| 通用 | execute(toolCallId,params,signal,onUpdate)→{content,details,structuredContent?,usage?,isError?}; 抛错→isError+err.message; constrainedSampling json_schema:prefer; 嵌套调用上限 256 calls/8KB args/32KB total/500char err; 校验错误串 `Tool <name> not found`/`Operation aborted`/`Tool execution was blocked`/`Tool call "<name>" was not executed: the response hit the output token limit...` | 🟡 (嵌套上限/onUpdate 未做) |

## 8. LLM provider/API 面

- API: openai-completions/openai-responses/azure-openai-responses/openai-codex-responses/anthropic-messages/bedrock-converse-stream/google-generative-ai/google-vertex/pi-messages/mistral-conversations + openrouter-images + 分类器 API
- provider 静态表 ~30 个 (env key 映射见审计D §6)
- 流式归一化: text_*/thinking_*/toolcall_*/done/error → AssistantMessageEventStream
- Anthropic stop 映射: end_turn→stop, max_tokens→length, tool_use→toolUse, refusal→error, pause_turn/stop_sequence→stop, sensitive→error
- 状态: ✅ openai-responses SSE流式+reasoning翻译; ⬜ anthropic/openai-completions

## 9. 模型目录/注册表

- Model 字段全量; getAvailableSnapshot = auth 过滤; models.json `~/.pi/agent/models.json` 用户目录覆盖/合并; models-store.json 远程缓存; remote catalog `pi.dev/api/models/providers/{id}?types=chat,image,classifier` 4h 刷新 ETag; virtual-model pi-virtual; 初始选择链: scoped→saved default→preferred→first→none
- 状态: 🟡 静态目录+models.json解析+请求级model/thinkingLevel+provider mux; 缺远程目录/virtual-model/scoped

## 10. 认证

- `~/.pi/agent/auth.json` `Record<provider,{type:"api_key",key?|env?|cmd?}|{type:"oauth",access,refresh,expires}>`; 0600/0700; resolveConfigValue 支持 env/cmd 间接
- 解析序: explicit override→stored(临期<5min 刷新)→ambient env; env key 全表(审计D §6)
- OAuth: Anthropic PKCE localhost:53692; Meta device; Copilot; Cloudflare
- 状态: 🟡 codex auth.json 读取 (路径不同源)

## 11. Skills / slash / prompts / 扩展 / MCP

- skills: `~/.pi/agent/skills/`+`.pi/skills/`+`--skills`; SKILL.md frontmatter(name/description 必须/disable-model-invocation); 校验≤64 `[a-z0-9-]+`; `<available_skills>` XML 注入 prompt; `/skill:name`
- 内置 slash 23 个: settings/model/tree/thinking/scoped-models/export/import/share/bug/copy/name/session/changelog/hotkeys/fork/clone/trust/login/logout/new/compact/resume/reload/quit
- prompt 模板: `*.md` frontmatter description/argument-hint; `$1..$N $@ $ARGUMENTS ${N:-d} ${@:-d} ${@:N} ${@:N:L}` 引号感知
- 扩展: `.pi/extensions/`+`~/.pi/agent/extensions/`+packages; UI 8 method + extension_error
- MCP: `~/.pi/agent/mcp.json`+`.pi/mcp.json`; stdio/http(streamable); `mcp__<server>__<tool>`; exposure codemode|deferred|direct|hidden; OAuth dcr/cimd
- system prompt 分节: preamble+tools+rules+docs+addendum+project_context+skills+cwd+custom
- 状态: ⬜ 全部

## 12. compaction 细节 (prompts 原文见审计C §5)

- 触发: 手动/threshold/overflow; settings{enabled:true,reserveTokens:16384,keepRecentTokens:20000}
- prompt: SUMMARIZATION_SYSTEM_PROMPT + SUMMARIZATION_PROMPT(##Goal/Constraints/Progress/Done/In-Progress/Blocked/Key-Decisions/Next-Steps/Critical-Context) / UPDATE(包裹 previous-summary) / TURN_PREFIX
- serializeConversation: `[User]:`/`[Assistant]:`/`[Assistant tool calls]: name(k=v)`/`[Tool result]:`(2000字截断`[... N more characters truncated]`); thinking 省略
- entry: compaction{summary,firstKeptEntryId(=自身id则全弃),tokensBefore,details?,usage?,fromHook?,systemMessage?}; systemMessage=全部 system 折叠 checkpoint
- branch summary: navigateTree 触发; BRANCH_SUMMARY_PROMPT+preamble; BranchSummaryEntry{fromId(旧leaf|"root"),summary,...}
- 状态: ⬜

## 13. 消息线格式 (packages/ai types.ts)

- 已在 domain Message v2 对齐: 角色 8 个/Usage/StopReason 7 值/内容块 4 种/custom 字段
- textSignature=TextSignatureV1 JSON; thinkingSignature; thoughtSignature(Google); argumentsError; nestedCalls; DeferredHandle
- 状态: ✅ 域模型; 🟡 codec 未覆盖 nestedCalls/deferred

## 14. 其他 pi 功能区(审计覆盖但未展开, 逐点迁移时补读)

- settings.json 全 schema — RPC 可见项已列; `~/.pi/agent/settings.json` 读写
- export_from_file/serializeSessionBranch(线性化导出)
- project trust(trust-manager 保护文件表: mcp.json 等)
- package-manager/skills 安装源; telemetry; bug-report; crash-log; cache-warmer; keybindings; themes
- durable 包(事件重放) — cpp-pi 侧对应 JournalCtx(后置)
- evals/chord/codemode 沙箱(QuickJS)/llama/tool-search 扩展本体

## 迁移批次 (执行顺序)

1. **P1a 内置工具** read/bash/edit/write/grep/find/ls + truncate/fileMutationQueue/输出净化 — 按 §7 审计实现
2. **P1b 会话存储** JSONL store+文件布局+migrations → get_entries/get_tree/fork/clone/get_fork_messages/switch_session/new_session(parentSession) 全真 — **进行中**
3. **P1c 命令语义补齐** prompt disposition/排队语义/默认模式修正/error 文案/model 目录(单 provider)/set_model 校验/session_info/thinking_level/model_change entries+事件
4. **P2a 事件完备** tool_execution_*/agent_settled/willRetry/entry_appended/bash_execution_update/message_update(流式)
5. **P2b abort 链路** EV_ABORT→stop()+aborted stopReason+响应延后
6. **P2c compact 真实现** prepare+prompt+entry+事件+auto-threshold
7. **P3 流式+provider 生态** SSE(anthropic/openai-completions)/usage/cost/thinkingLevelMap/auth.json/env 全表
8. **P4 skills/slash/prompts/extensions/MCP** 长尾部
9. **P5 durable/telemetry/trust/exports 等**
