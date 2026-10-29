#pragma once

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include <folly/json/json.h>

namespace pi::infra {

// ---------------------------------------------------------------------------
// 凭证/模型配置适配器: 复用本机 codex CLI 的配置.
//   key      ~/.codex/auth.json 的 OPENAI_API_KEY, 或环境变量 OPENAI_API_KEY
//   base_url ~/.codex/config.toml 的 base_url, 或 OPENAI_BASE_URL
//   model    ~/.codex/config.toml 的 model,    或 CPP_PI_MODEL / OPENAI_MODEL
// 全部可选: 拿不到就返回 nullopt, 调用方回退 mock.
// ---------------------------------------------------------------------------
struct CodexConfig {
   std::string apiKey;
   std::string baseUrl = "https://api.openai.com/v1";
   std::string model   = "gpt-5.6-sol";

   static auto home() -> std::string {
      if (auto* h = std::getenv("HOME")) return h;
      return {};
   }

   static auto load() -> std::optional<CodexConfig> {
      CodexConfig c;

      if (auto* k = std::getenv("OPENAI_API_KEY")) c.apiKey = k;
      if (c.apiKey.empty()) {
         std::ifstream f{home() + "/.codex/auth.json"};
         if (f) {
            try {
               auto j = folly::parseJson(
                  std::string{std::istreambuf_iterator<char>{f}, {}});
               if (j.count("OPENAI_API_KEY"))
                  c.apiKey = j["OPENAI_API_KEY"].getString();
            } catch (...) {}
         }
      }
      if (c.apiKey.empty()) return std::nullopt;

      if (auto* u = std::getenv("OPENAI_BASE_URL")) c.baseUrl = u;
      else if (auto* u = std::getenv("CODEX_BASE_URL")) c.baseUrl = u;
      if (auto* m = std::getenv("CPP_PI_MODEL")) c.model = m;
      else if (auto* m = std::getenv("OPENAI_MODEL")) c.model = m;

      // config.toml: 平扫 "key = value" 行 (env 优先)
      std::ifstream f{home() + "/.codex/config.toml"};
      for (std::string line; std::getline(f, line);) {
         auto val = tomlValue(line);
         if (!val) continue;
         if (line.find("base_url") == 0 &&
             std::getenv("OPENAI_BASE_URL") == nullptr)
            c.baseUrl = *val;
         if (line.find("model ") == 0 &&
             std::getenv("CPP_PI_MODEL") == nullptr &&
             std::getenv("OPENAI_MODEL") == nullptr)
            c.model = *val;
      }
      // 去尾斜杠, 便于拼接 path
      while (!c.baseUrl.empty() && c.baseUrl.back() == '/') c.baseUrl.pop_back();
      return c;
   }

private:
   // 解析 `key = "value"` 形式的行 -> value
   static auto tomlValue(std::string const& line) -> std::optional<std::string> {
      auto eq = line.find('=');
      if (eq == std::string::npos) return std::nullopt;
      auto v = line.substr(eq + 1);
      auto b = v.find_first_not_of(" \t");
      auto e = v.find_last_not_of(" \t");
      if (b == std::string::npos) return std::nullopt;
      v = v.substr(b, e - b + 1);
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
         v = v.substr(1, v.size() - 2);
      return v.empty() ? std::nullopt : std::optional{std::move(v)};
   }
};

} // namespace pi::infra
