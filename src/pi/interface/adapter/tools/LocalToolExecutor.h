#pragma once

#include <chrono>
#include <vector>

#include <folly/json/json.h>

#include <pi/domain/tool/Tool.h>
#include <pi/infra/async/EventLoop.h>
#include <pi/infra/async/ThreadPool.h>
#include <pi/infra/util/JsonSafe.h>

#include "BuiltinTools.h"

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// 端口实现: pi 内置工具集 (read/bash/edit/write 默认激活;
// grep/find/ls/powershell 注册表中). 两种执行模式:
//   同步 (未 setAsync): execute 内联跑完立即 done —— 测试/一次性调用.
//   异步 (setAsync):   env 快照在调用方 (loop) 线程解析 (provider 读
//     ModelCtx, 跨线程会碰领域态), 阻塞体 (bash 子进程/文件扫描) 丢进
//     pool worker, done 经 EventLoop marshal 回 loop 线程 ——
//     ToolRunner 的 EV_TOOLS_DONE 聚合由此获得真并行.
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

   void setAsync(infra::EventLoop& loop, infra::ThreadPool& pool) {
      loop_ = &loop;
      pool_ = &pool;
   }

   void execute(domain::ToolCall const& call,
                domain::ToolResultHandler done) override {
      // pi: 未激活的工具调用 -> "Tool <name> not found" (同步应答)
      if (!activeTools_.empty()
          && std::ranges::find(activeTools_, call.tool) == activeTools_.end()
          && call.tool != "echo") {
         done({call.id, call.tool,
               std::format("Tool {} not found", call.tool),
               {}, {}, -1, true});
         return;
      }
      // env 快照必须在 loop 线程解析
      auto const env = tools_.resolveEnv();
      if (pool_ == nullptr) {
         done(run(call, env));
         return;
      }
      auto& pool = *pool_;
      auto& loop = *loop_;
      pool.submit([this, call, env, &loop,
                   done = std::move(done)]() mutable {
         auto outcome = run(call, env);
         loop.post([done = std::move(done),
                    r = std::move(outcome)]() mutable {
            done(std::move(r));
         });
      });
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
   auto run(domain::ToolCall const& call, ToolSessionEnv const& env)
      -> domain::ToolResult {
      auto const started = std::chrono::steady_clock::now();
      auto const outcome = tools_.execute(call, env);
      auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::steady_clock::now() - started).count();
      std::string detailsJson;
      if (!outcome.details.isNull()) detailsJson = folly::toJson(outcome.details);
      return {call.id, call.tool, outcome.text, outcome.images,
              std::move(detailsJson), ms, outcome.isError};
   }

   BuiltinTools           tools_;
   std::vector<std::string> activeTools_;   // 空 = 全量注册表
   infra::EventLoop*      loop_ = nullptr;  // setAsync 后非空
   infra::ThreadPool*     pool_ = nullptr;
};

} // namespace pi::iface::adapter
