#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <pi/interface/trans/AgentSessionService.h>
#include <pi/domain/session/ISessionStore.h>
#include <pi/domain/session/Session.h>
#include <pi/infra/config/CodexConfig.h>
#include <pi/infra/util/Ids.h>
#include <pi/interface/adapter/llm/MockLlmProvider.h>
#include <pi/interface/adapter/llm/OpenAiProvider.h>
#include <pi/interface/adapter/session/InMemorySessionStore.h>
#include <pi/interface/adapter/session/JsonlSessionStore.h>
#include <pi/interface/adapter/tools/LocalToolExecutor.h>
#include <pi/infra/log/Logging.h>
#include <pi/interface/rpc/RpcFrontend.h>

// ---------------------------------------------------------------------------
// 入口: 默认 CLI REPL; `--mode rpc` 起 NDJSON 协议端, 供 pi TUI 接入.
//   --provider openai|mock   默认: 有 codex/openai 凭证则 openai, 否则 mock
//   --no-session             不持久化会话 (InMemorySessionStore)
//   --session-dir <path>     覆盖 ~/.pi/agent 存储根目录
// ---------------------------------------------------------------------------
namespace {

constexpr int64_t kDefaultContextWindow = 200000;
constexpr int64_t kDefaultMaxTokens     = 8192;

} // namespace

int main(int argc, char** argv) {
   bool        rpcMode   = false;
   bool        noSession = false;
   bool        noTools   = false;
   std::string provider  = "auto";
   std::string sessionDir;
   std::vector<std::string> toolsArg, excludeTools;
   auto splitCsv = [](char const* csv) {
      std::vector<std::string> out;
      std::string cur;
      for (char const* p = csv;; ++p) {
         if (*p == ',' || *p == '\0') {
            auto const b = cur.find_first_not_of(' ');
            auto const e = cur.find_last_not_of(' ');
            if (b != std::string::npos)
               out.push_back(cur.substr(b, e - b + 1));
            if (*p == '\0') break;
            cur.clear();
         } else {
            cur += *p;
         }
      }
      return out;
   };
   for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc &&
          std::strcmp(argv[i + 1], "rpc") == 0) {
         rpcMode = true;
      } else if (std::strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
         provider = argv[++i];
      } else if (std::strcmp(argv[i], "--no-session") == 0) {
         noSession = true;
      } else if (std::strcmp(argv[i], "--session-dir") == 0 && i + 1 < argc) {
         sessionDir = argv[++i];
      } else if ((std::strcmp(argv[i], "--tools") == 0 ||
                  std::strcmp(argv[i], "-t") == 0) && i + 1 < argc) {
         toolsArg = splitCsv(argv[++i]);
      } else if ((std::strcmp(argv[i], "--exclude-tools") == 0 ||
                  std::strcmp(argv[i], "-xt") == 0) && i + 1 < argc) {
         excludeTools = splitCsv(argv[++i]);
      } else if (std::strcmp(argv[i], "--no-tools") == 0 ||
                 std::strcmp(argv[i], "-nt") == 0) {
         noTools = true;
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

   // 会话存储: 默认 JSONL 落 ~/.pi/agent (--session-dir 可覆盖); --no-session
   // 走纯内存 (测试/一次性会话).
   std::unique_ptr<pi::domain::ISessionStore> store;
   if (noSession) {
      store = std::make_unique<pi::iface::adapter::InMemorySessionStore>();
   } else {
      auto dir = sessionDir.empty()
         ? std::filesystem::path{std::getenv("HOME")} / ".pi" / "agent"
         : std::filesystem::path{sessionDir};
      store = std::make_unique<pi::iface::adapter::JsonlSessionStore>(dir);
   }

   char cwdBuf[4096];
   std::string cwd = ::getcwd(cwdBuf, sizeof cwdBuf) ? cwdBuf : std::string{};

   pi::domain::Session           session{pi::infra::uuidv7(), cwd};
   pi::iface::adapter::LocalToolExecutor  tools{cwd};
   // pi 默认激活集: read/bash/edit/write; echo 留给 mock 测试通道.
   // --tools 替换默认集 (pi 语义: 普通名替换 DEFAULT_TOOL_NAMES);
   // --exclude-tools 从结果里剔除; --no-tools 全禁.
   std::vector<std::string> active =
      toolsArg.empty()
         ? std::vector<std::string>{"read", "bash", "edit", "write", "echo"}
         : toolsArg;
   for (auto const& x : excludeTools)
      std::erase(active, x);
   if (!noTools) tools.setActiveTools(active);
   pi::iface::AgentSessionService  service{
      session, *llm, tools, *store,
      "You are cpp-pi, a DDD + DCI + trans-dsl rewrite of pi."};

   if (rpcMode) {
      pi::iface::rpc::RpcFrontend frontend{service, session, store.get()};
      if (codex && provider != "mock") {
         frontend.setModel(pi::domain::Model{
            /*id*/ codex->model, /*name*/ codex->model,
            /*api*/ "openai-responses", /*provider*/ "openai-codex",
            /*baseUrl*/ codex->baseUrl, /*input*/ {"text"},
            /*cost*/ {}, /*contextWindow*/ kDefaultContextWindow,
            /*maxTokens*/ kDefaultMaxTokens, /*reasoning*/ true});
      }
      // pi resolveSpawnContext: bash 子进程注入 PI_* 会话环境
      tools.setEnvProvider([&] {
         auto const& m = frontend.model();
         return pi::iface::adapter::ToolSessionEnv{
            session.id(), frontend.sessionFile(),
            m.provider, m.id, frontend.thinkingLevel()};
      });
      return frontend.run();
   }

   tools.setEnvProvider([&] {
      return pi::iface::adapter::ToolSessionEnv{
         session.id(),
         store->fileOf(session),
         codex && provider != "mock" ? std::string{"openai-codex"}
                                     : std::string{"mock"},
         codex && provider != "mock" ? codex->model : std::string{"mock-1"},
         "medium"};
   });

   std::puts("cpp-pi skeleton (DDD + DCI + trans-dsl). /q to quit.");
   for (std::string line;;) {
      std::printf("\ncpp-pi> ");
      std::fflush(stdout);
      if (!std::getline(std::cin, line) || line == "/q") break;
      if (line.empty()) continue;

      auto status = service.runTurn(line);
      std::printf("turn finished, status=%u\n", status);
      auto const msgs = session.messages();
      if (!msgs.empty()) {
         std::printf("assistant: %s\n", msgs.back().text.c_str());
      }
   }
}
