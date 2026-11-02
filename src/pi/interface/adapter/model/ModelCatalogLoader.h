#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <folly/json/json.h>

#include <pi/domain/model/ModelCatalog.h>
#include <pi/infra/config/CodexConfig.h>
#include <pi/infra/log/Logging.h>
#include <pi/infra/util/JsonSafe.h>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// 模型目录装配 (pi model-registry 的静态面):
//   1. mock provider 固定条目 (--provider mock / 测试)
//   2. codex 凭证在握时 openai-codex 静态条目 (对齐 pi catalog 的模型 id)
//   3. ~/.pi/agent/models.json 的自定义 providers (pi 文件格式 1:1 子集:
//      baseUrl/api/apiKey/$ENV 插值 + 每模型 reasoning/contextWindow/
//      maxTokens/name; "!command" 形式不执行, 该 provider 标记无凭证)
// ProviderSpec 是建 provider 的最小凭证面; api 未知的 provider 留在目录里
// 但 use() 会失败 —— setModel 报 "No provider available".
// ---------------------------------------------------------------------------
struct ProviderSpec {
   std::string api;       // "openai-responses" | "mock" | 其他(未实现)
   std::string baseUrl;
   std::string apiKey;
};

struct CatalogBundle {
   domain::ModelCatalog                          catalog;
   std::unordered_map<std::string, ProviderSpec> providers;
};

class ModelCatalogLoader {
public:
   static auto load(std::filesystem::path const& agentDir,
                    std::optional<infra::CodexConfig> const& codex)
      -> CatalogBundle {
      CatalogBundle b;
      addMock(b);
      if (codex) addCodex(b, *codex);
      addModelsJson(b, agentDir / "models.json");
      return b;
   }

private:
   // ---------- mock ----------
   static auto addMock(CatalogBundle& b) -> void {
      b.providers["mock"] = ProviderSpec{"mock", "", ""};
      auto mk = [](std::string id, bool reasoning) {
         return domain::Model{id, id, "mock", "mock", "", {"text"},
                              {}, 200000, 8192, reasoning};
      };
      b.catalog.models.push_back(mk("mock-1", true));
      b.catalog.models.push_back(mk("mock-2", true));
      b.catalog.models.push_back(mk("mock-plain", false));
   }

   // ---------- openai-codex 静态目录 (pi catalog/providers/openai-codex.ts) --
   static auto addCodex(CatalogBundle& b,
                        infra::CodexConfig const& c) -> void {
      b.providers["openai-codex"] =
         ProviderSpec{"openai-responses", c.baseUrl, c.apiKey};
      struct Row { char const* id; char const* name; int64_t ctx; };
      static constexpr Row kRows[] = {
         {"gpt-6.1-sol",   "GPT-6.1 Sol",   500000},
         {"gpt-6.1-terra", "GPT-6.1 Terra", 500000},
         {"gpt-6-astra",   "GPT-6 Astra",   272000},
         {"gpt-6-sol",     "GPT-6 Sol",     272000},
         {"gpt-6-luna",    "GPT-6 Luna",    272000},
         {"gpt-5.6-luna",  "GPT-5.6 Luna",  272000},
         {"gpt-5.6-sol",   "GPT-5.6 Sol",   272000},
         {"gpt-5.6-terra", "GPT-5.6 Terra", 272000},
      };
      auto mk = [&](std::string id, std::string name, int64_t ctx) {
         return domain::Model{std::move(id), std::move(name),
                              "openai-responses", "openai-codex", c.baseUrl,
                              {"text"}, {}, ctx, 8192, true};
      };
      // config.toml 里的自定义 model 不在静态表时置顶补入 (启动默认模型)
      bool listed = false;
      for (auto const& r : kRows) listed |= c.model == r.id;
      if (!listed)
         b.catalog.models.push_back(mk(c.model, c.model, 272000));
      for (auto const& r : kRows)
         b.catalog.models.push_back(mk(r.id, r.name, r.ctx));
   }

   // ---------- ~/.pi/agent/models.json ----------
   static auto addModelsJson(CatalogBundle& b,
                             std::filesystem::path const& path) -> void {
      std::ifstream f{path};
      if (!f) return;
      auto pj = infra::parseJsonSafe(
         std::string{std::istreambuf_iterator<char>{f}, {}});
      if (pj.hasError() || !pj->count("providers")) {
         LOG_WARNING(pi::infra::log::logger(), "models.json: 解析失败 {}",
                  path.string());
         return;
      }
      for (auto const& [pid, pv] : (*pj)["providers"].items()) {
         auto const provider = pid.asString();
         ProviderSpec spec;
         spec.api     = str(pv, "api");
         spec.baseUrl = str(pv, "baseUrl");
         spec.apiKey  = resolveKey(str(pv, "apiKey"));
         if (auto const it = b.providers.find(provider);
             it != b.providers.end()) {
            // models.json 覆盖同 id provider 的凭证面 (pi 语义: user 层优先)
            if (!spec.api.empty())     it->second.api     = spec.api;
            if (!spec.baseUrl.empty()) it->second.baseUrl = spec.baseUrl;
            if (!spec.apiKey.empty())  it->second.apiKey  = spec.apiKey;
         } else {
            b.providers[provider] = spec;
         }
         if (!pv.count("models") || !pv["models"].isArray()) continue;
         for (auto const& mv : pv["models"].items()) {
            auto const& m = mv.second;
            domain::Model e;
            e.id            = str(m, "id");
            if (e.id.empty()) continue;
            e.name          = str(m, "name", e.id);
            e.api           = str(m, "api", spec.api);
            e.provider      = provider;
            e.baseUrl       = str(m, "baseUrl", spec.baseUrl);
            e.reasoning     = m.count("reasoning")
                                 ? m["reasoning"].getBool() : false;
            e.contextWindow = m.count("contextWindow")
                                 ? m["contextWindow"].asInt() : 0;
            e.maxTokens     = m.count("maxTokens")
                                 ? m["maxTokens"].asInt() : 0;
            e.input         = {"text"};
            if (m.count("input") && m["input"].isArray())
               for (auto const& iv : m["input"].items())
                  e.input.push_back(iv.second.getString());
            upsert(b.catalog, std::move(e));
         }
      }
   }

   // pi: models.json model 覆盖同 (provider,id) 的内置条目, 否则追加.
   static auto upsert(domain::ModelCatalog& c, domain::Model m) -> void {
      for (auto& e : c.models)
         if (e.provider == m.provider && e.id == m.id) { e = std::move(m); return; }
      c.models.push_back(std::move(m));
   }

   // apiKey 解析: "$ENV" 读环境变量; "!cmd" 不执行 (安全: 不跑任意命令);
   // 其余按字面值.
   static auto resolveKey(std::string const& v) -> std::string {
      if (v.size() > 1 && v.front() == '$')
         if (auto* e = std::getenv(v.c_str() + 1)) return e;
      if (!v.empty() && v.front() == '!') return {};
      return v;
   }

   static auto str(folly::dynamic const& j, char const* k,
                   std::string def = {}) -> std::string {
      auto const it = j.find(k);
      return it != j.items().end() && it->second.isString()
                ? it->second.getString() : std::move(def);
   }
};

} // namespace pi::iface::adapter
