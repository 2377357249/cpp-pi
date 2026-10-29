#include <cstdio>
#include <iostream>
#include <string>

#include <pi/application/AgentSessionService.h>
#include <pi/domain/session/Session.h>
#include <pi/infrastructure/llm/MockLlmProvider.h>
#include <pi/infrastructure/session/InMemorySessionStore.h>
#include <pi/infrastructure/tools/LocalToolExecutor.h>

// ---------------------------------------------------------------------------
// CLI REPL: 演示用前端. 正式前端是 TS TUI, 经 `cpp-pi --mode rpc` 接 NDJSON.
// ---------------------------------------------------------------------------
int main() {
   pi::domain::Session           session{"cli-session"};
   pi::infra::MockLlmProvider    llm;
   pi::infra::LocalToolExecutor  tools;
   pi::infra::InMemorySessionStore store;
   pi::app::AgentSessionService  service{
      session, llm, tools, store,
      "You are cpp-pi, a DDD + DCI + trans-dsl rewrite of pi."};

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
