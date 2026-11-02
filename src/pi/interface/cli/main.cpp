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
#include <pi/infra/async/EventLoop.h>
#include <pi/infra/async/ThreadPool.h>
#include <pi/infra/config/CodexConfig.h>
#include <pi/infra/util/Ids.h>
#include <pi/interface/adapter/llm/AsyncLlmProvider.h>
#include <pi/interface/adapter/llm/LlmProviderMux.h>
#include <pi/interface/adapter/llm/MockLlmProvider.h>
#include <pi/interface/adapter/llm/OpenAiProvider.h>
#include <pi/interface/adapter/model/ModelCatalogLoader.h>
#include <pi/interface/adapter/session/InMemorySessionStore.h>
#include <pi/interface/adapter/session/JsonlSessionStore.h>
#include <pi/interface/adapter/tools/LocalToolExecutor.h>
#include <pi/infra/log/Logging.h>
#include <pi/interface/rpc/RpcFrontend.h>
#include <pi/interface/tui/TuiFrontend.h>

// ---------------------------------------------------------------------------
// 入口: 默认 CLI REPL; `--mode rpc` 起 NDJSON 协议端, 供 pi TUI 接入;
//   `--mode tui` 起 FTXUI 全屏界面 (纯 C++ 前端, 无需 TS 端).
//   --provider openai|mock   默认: 有 codex/openai 凭证则 openai, 否则 mock
//   --no-session             不持久化会话 (InMemorySessionStore)
//   --session-dir <path>     覆盖 ~/.pi/agent 存储根目录
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
   bool        rpcMode   = false;
   bool        tuiMode   = false;
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
      if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
         rpcMode = std::strcmp(argv[i + 1], "rpc") == 0;
         tuiMode = std::strcmp(argv[i + 1], "tui") == 0;
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

   // TUI 独占终端: 日志只留文件 sink (stderr 会打花全屏界面)
   pi::infra::log::init("logs", /*console=*/!tuiMode);

   // ---------- provider mux + 模型目录 ----------
   // mux 是 service 持有的稳定 ILlmProvider 门面; setModel 跨 provider 时
   // 经 providerChange 钩子切转发目标. provider 工厂按 ProviderSpec.api:
   //   "mock"→MockLlmProvider, "openai-responses"→OpenAiProvider;
   //   其他 api 无实现 → use() false → set_model 报 "No provider available".
   auto codex = pi::infra::CodexConfig::load();
   auto const agentDir = sessionDir.empty()
      ? std::filesystem::path{std::getenv("HOME")} / ".pi" / "agent"
      : std::filesystem::path{sessionDir};
   // --provider mock = 显式离线: 目录只含 mock 条目, codex provider 不进 mux
   // (测试密封: mock 模式下任何路径都不许摸到真网络).
   auto bundle = pi::iface::adapter::ModelCatalogLoader::load(
      agentDir, provider == "mock" ? std::nullopt : codex);

   pi::iface::adapter::LlmProviderMux mux;
   std::vector<pi::iface::adapter::OpenAiProvider*> oaProviders;
   for (auto const& [pid, spec] : bundle.providers) {
      if (provider == "mock" && pid != "mock") continue;   // 离线密封
      if (pid == "mock") {
         mux.add(pid, std::make_unique<pi::iface::adapter::MockLlmProvider>());
      } else if (spec.api == "openai-responses" && !spec.apiKey.empty()) {
         auto oa = std::make_unique<pi::iface::adapter::OpenAiProvider>(
                      spec.apiKey, spec.baseUrl, /*model*/ "", pid);
         oaProviders.push_back(oa.get());
         mux.add(pid, std::move(oa));
      }
   }
   if (provider == "mock" || !mux.has("openai-codex")) {
      mux.use("mock");
      LOG_INFO(pi::infra::log::logger(), "provider: mock");
   } else {
      mux.use("openai-codex");
      LOG_INFO(pi::infra::log::logger(), "provider: openai-codex base={}",
               codex->baseUrl);
   }

   // 会话存储: 默认 JSONL 落 agentDir (--session-dir 可覆盖); --no-session
   // 走纯内存 (测试/一次性会话).
   std::unique_ptr<pi::domain::ISessionStore> store;
   if (noSession) {
      store = std::make_unique<pi::iface::adapter::InMemorySessionStore>();
   } else {
      store = std::make_unique<pi::iface::adapter::JsonlSessionStore>(agentDir);
   }

   char cwdBuf[4096];
   std::string cwd = ::getcwd(cwdBuf, sizeof cwdBuf) ? cwdBuf : std::string{};

   // ---------- 异步装配: 单 loop 线程 + 两个 worker 池 ----------
   // loop 最先声明 (最后析构): worker 池在 mux/store/tools 之后声明,
   // 先于它们析构 join —— 不会 post 进已销毁对象.
   //   llmPool: 跑同步 curl 的专用线程 (一次只有一个 completion 在飞);
   //   toolPool: 跑 bash 子进程/文件扫描, 支持并行 tool calls.
   pi::infra::EventLoop loop;
   constexpr size_t     kLlmWorkers  = 1;
   constexpr size_t     kToolWorkers = 4;

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

   pi::infra::ThreadPool llmPool{kLlmWorkers}, toolPool{kToolWorkers};
   tools.setAsync(loop, toolPool);
   pi::iface::adapter::AsyncLlmProvider asyncLlm{mux, loop, llmPool};
   pi::iface::AgentSessionService  service{
      session, asyncLlm, tools, *store,
      "You are cpp-pi, a DDD + DCI + trans-dsl rewrite of pi.",
      loop, toolPool};

   // abort 链路: service 自持旗标 -> 工具派发检查/bash spawn 杀进程树/
   // provider 断 SSE 流, 三方共用同一个原子量.
   tools.setAbortCheck([&service] { return service.aborted(); });
   for (auto* oa : oaProviders)
      oa->setAbortCheck([&service] { return service.aborted(); });

   // 目录 + provider 切换钩子 + 初始模型 (catalog 必含; mock 场景退 mock-1)
   service.setCatalog(std::move(bundle.catalog));
   service.setProviderChangeHandler(
      [&mux](std::string_view pid) { return mux.use(pid); });
   auto const defaultProvider =
      (provider != "mock" && mux.has("openai-codex"))
         ? std::string{"openai-codex"} : std::string{"mock"};
   auto const defaultModel =
      defaultProvider == "openai-codex" ? codex->model : std::string{"mock-1"};
   if (auto err = service.setModel(defaultProvider, defaultModel))
      LOG_WARNING(pi::infra::log::logger(), "set_model: {}", *err);

   // pi resolveSpawnContext: bash 子进程注入 PI_* 会话环境 (跟随当前模型)
   tools.setEnvProvider([&] {
      auto const& m = service.model();
      return pi::iface::adapter::ToolSessionEnv{
         session.id(), store->fileOf(session),
         m.provider, m.id, service.thinkingLevel()};
   });

   if (tuiMode) {
      pi::iface::tui::TuiFrontend frontend{service, session, *store,
                                           loop, toolPool};
      return frontend.run();
   }

   if (rpcMode) {
      pi::iface::rpc::RpcFrontend frontend{service, session, loop, toolPool,
                                           store.get()};
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
      auto const msgs = session.messages();
      if (!msgs.empty()) {
         std::printf("assistant: %s\n", msgs.back().text.c_str());
      }
   }
}
