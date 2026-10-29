#pragma once

#include <vector>

#include <folly/json/json.h>

#include <pi/domain/tool/Tool.h>
#include <pi/infra/process/Shell.h>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// 本地工具执行器(骨架): 同步实现端口, 之后每个内置工具
// (read/edit/write) 在这里按 pi 的语义逐个实现.
// ---------------------------------------------------------------------------
class LocalToolExecutor : public domain::IToolExecutor {
public:
   void execute(domain::ToolCall const& call, domain::ToolResultHandler done) override {
      if (call.tool == "echo") {
         done({call.id, "echo", "echo: " + jsonArg(call.input, "input"),
               {}, {}, -1, false});
         return;
      }
      if (call.tool == "bash") {
         done({call.id, "bash", runBash(jsonArg(call.input, "command")),
               {}, {}, -1, false});
         return;
      }
      done({call.id, call.tool, "unknown tool: " + call.tool,
            {}, {}, -1, true});
   }

   auto availableTools() const -> std::vector<domain::Tool> override {
      return {
         {"echo", "回显输入",
          R"({"type":"object","properties":{"input":{"type":"string"}}})"},
         {"bash", "执行 shell 命令",
          R"({"type":"object","properties":{"command":{"type":"string"}}})"},
      };
   }

private:
   // 工具入参是 JSON 串: 取 field 字段; 解不出就原样用 (模型可能给裸串).
   static auto jsonArg(std::string const& input, std::string const& field)
      -> std::string {
      try {
         auto j = folly::parseJson(input);
         if (j.count(field) && j[field].isString()) return j[field].getString();
      } catch (...) {}
      return input;
   }

   // 截断到 8KB (infra::runShell 默认窗口).
   static auto runBash(std::string const& cmd) -> std::string {
      auto r = infra::runShell(cmd);
      if (r.exitCode != 0) r.output += "\n[exit " + std::to_string(r.exitCode) + "]";
      return r.output;
   }
};

} // namespace pi::iface::adapter
