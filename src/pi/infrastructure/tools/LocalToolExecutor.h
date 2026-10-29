#pragma once

#include <vector>

#include <pi/domain/tool/Tool.h>

namespace pi::infra {

// ---------------------------------------------------------------------------
// 本地工具执行器(骨架): 同步实现端口, 之后每个内置工具
// (read/bash/edit/write) 在这里按 pi 的语义逐个实现.
// ---------------------------------------------------------------------------
class LocalToolExecutor : public domain::IToolExecutor {
public:
   void execute(domain::ToolCall const& call, domain::ToolResultHandler done) override {
      if (call.tool == "echo") {
         done({call.id, "echo: " + call.input, false});
      } else {
         done({call.id, "unknown tool: " + call.tool, true});
      }
   }

   auto availableTools() const -> std::vector<domain::Tool> override {
      return {{"echo", "回显输入",
               R"({"type":"object","properties":{"input":{"type":"string"}}})"}};
   }
};

} // namespace pi::infra
