#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include <pi/interface/trans/AgentSessionService.h>
#include <pi/domain/session/Session.h>
#include <pi/infra/config/CodexConfig.h>
#include <pi/interface/adapter/llm/MockLlmProvider.h>
#include <pi/interface/adapter/llm/OpenAiProvider.h>
#include <pi/interface/adapter/session/InMemorySessionStore.h>
#include <pi/interface/adapter/tools/LocalToolExecutor.h>
#include <pi/infra/log/Logging.h>
#include <pi/interface/rpc/RpcFrontend.h>

// ---------------------------------------------------------------------------
// 入口: 默认 CLI REPL; `--mode rpc` 起 NDJSON 协议端, 供 pi TUI 接入.
//   --provider openai|mock   默认: 有 codex/openai 凭证则 openai, 否则 mock
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
   bool rpcMode = false;
   std::string provider = "auto";
   for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc &&
          std::strcmp(argv[i + 1], "rpc") == 0) {
         rpcMode = true;
      } else if (std::strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
         provider = argv[++i];
      }
   }

   pi::infra::log::init();

   auto codex = pi::infra::CodexConfig::load();
   std::unique_ptr<pi::domain::ILlmProvider> llm;
   if (provider != "mock" && codex) {
      llm = std::make_unique<pi::iface::adapter::OpenAiProvider>(
         codex->apiKey, codex->baseUrl, codex->model);
      LOG_INFO(pi::infra::log::logger(), "provider: openai model={} base={}",
               codex->model, codex->baseUrl);
   } else {
      llm = std::make_unique<pi::iface::adapter::MockLlmProvider>();
      LOG_INFO(pi::infra::log::logger(), "provider: mock");
   }

   pi::domain::Session           session{"cpp-pi-session"};
   pi::iface::adapter::LocalToolExecutor  tools;
   pi::iface::adapter::InMemorySessionStore store;
   pi::iface::AgentSessionService  service{
      session, *llm, tools, store,
      "You are cpp-pi, a DDD + DCI + trans-dsl rewrite of pi."};

   if (rpcMode) {
      pi::iface::rpc::RpcFrontend frontend{service, session};
      return frontend.run();
   }

   std::puts("cpp-pi skeleton (DDD + DCI + trans-dsl). /q to quit.");
   for (std::string line;;) {
      std::printf("\ncpp-pi> ");
      std::fflush(stdout);
      if (!std::getline(std::cin, line) || line == "/q") break;
      if (line.empty()) continue;

      auto status = service.runTurn(line);
      std::printf("turn finished, status=%u\n", status);
      auto const& msgs = session.messages();
      if (!msgs.empty()) {
         std::printf("assistant: %s\n", msgs.back().text.c_str());
      }
   }
}
