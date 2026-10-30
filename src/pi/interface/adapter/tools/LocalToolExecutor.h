#pragma once

#include <chrono>
#include <vector>

#include <folly/json/json.h>

#include <pi/domain/tool/Tool.h>
#include <pi/infra/util/JsonSafe.h>

#include "BuiltinTools.h"

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// 端口实现: pi 内置工具集 (read/bash/edit/write 默认激活;
// grep/find/ls/powershell 注册表中). 同步执行, done 立即回调——
// 语义仍是"回调式端口", 真异步执行器只需换个实现类.
// ---------------------------------------------------------------------------
class LocalToolExecutor : public domain::IToolExecutor {
public:
   explicit LocalToolExecutor(std::string cwd) : tools_{std::move(cwd)} {}

   void setSessionEnv(ToolSessionEnv env) { tools_.setSessionEnv(std::move(env)); }
   void setEnvProvider(std::function<ToolSessionEnv()> f) {
      tools_.setEnvProvider(std::move(f));
   }
   void setAbortCheck(std::function<bool()> f) { tools_.setAbortCheck(std::move(f)); }
   void setShellPath(std::string p) { tools_.setShellPath(std::move(p)); }
   void setActiveTools(std::vector<std::string> names) {
      activeTools_ = std::move(names);
   }

   void execute(domain::ToolCall const& call,
                domain::ToolResultHandler done) override {
      auto const started = std::chrono::steady_clock::now();
      // pi: 未激活的工具调用 -> "Tool <name> not found"
      if (!activeTools_.empty()
          && std::ranges::find(activeTools_, call.tool) == activeTools_.end()
          && call.tool != "echo") {
         done({call.id, call.tool,
               std::format("Tool {} not found", call.tool),
               {}, {}, -1, true});
         return;
      }
      auto const outcome = tools_.execute(call);
      auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::steady_clock::now() - started).count();
      std::string detailsJson;
      if (!outcome.details.isNull()) detailsJson = folly::toJson(outcome.details);
      done({call.id, call.tool, outcome.text, outcome.images,
            std::move(detailsJson), ms, outcome.isError});
   }

   auto availableTools() const -> std::vector<domain::Tool> override {
      auto defs = tools_.definitions();
      if (activeTools_.empty()) return defs;
      defs.push_back({"echo", "回显输入",
                      R"({"type":"object","properties":{"input":{"type":"string"}}})"});
      std::erase_if(defs, [&](domain::Tool const& t) {
         return std::ranges::find(activeTools_, t.name) == activeTools_.end()
                && t.name != "echo";
      });
      return defs;
   }

private:
   BuiltinTools           tools_;
   std::vector<std::string> activeTools_;   // 空 = 全量注册表
};

} // namespace pi::iface::adapter
