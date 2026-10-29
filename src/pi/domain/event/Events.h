#pragma once

#include <event/concept/EventId.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 事务协议事件: 事务对外部世界的"响应契约". 事务协议事件 ID 由 domain 层定义;
// 基础设施通过注入的回调间接触发, 不 include 本文件.
// ---------------------------------------------------------------------------
enum AgentEvent : EV_NS::EventId {
   EV_LLM_DONE   = 1,  // 一次 LLM completion 完成, 结果在 TurnStateCtx
   EV_TOOLS_DONE = 2,  // 本轮全部工具结果收齐, 在 TurnStateCtx
   EV_USER_INPUT = 3,  // 用户输入到达 (SessionLoop 用)
   EV_STORE_DONE = 4,  // 一批消息已落盘
   EV_LOAD_DONE  = 5,  // 历史加载完成
   EV_ABORT      = 6,  // 用户中止
};

// 消息体: 真实数据经 context 碎片传递, 这些仅作信标
struct LlmDoneMsg    { char _{}; };
struct ToolsDoneMsg  { char _{}; };
struct UserInputMsg  { char _{}; };
struct StoreDoneMsg  { char _{}; };
struct LoadDoneMsg   { char _{}; };
struct AbortMsg      { char _{}; };

} // namespace pi::domain
