#pragma once

#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

#include <pi/domain/model/Model.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 值对象 + 领域服务: 可用模型目录 (pi model-registry 的 credential-blind
// 快照对应物). 内容由 interface 层装配 (内置静态条目 + models.json),
// domain 只提供查找语义:
//   find(provider,id)     精确匹配;
//   find(term)            "provider/id" 或对目录唯一的裸 id;
//   providers()           出现过的 provider 列表 (保序去重);
//   modelsOf(provider)    该 provider 下的全部模型 (保序).
// ---------------------------------------------------------------------------
struct ModelCatalog {
   std::vector<Model> models;

   auto find(std::string_view provider, std::string_view id) const
      -> Model const* {
      for (auto const& m : models)
         if (m.provider == provider && m.id == id) return &m;
      return nullptr;
   }

   // "provider/id" 精确, 或裸 id 唯一命中; 多 provider 同 id 时裸 id 取首个.
   auto find(std::string_view term) const -> Model const* {
      if (auto const slash = term.find('/'); slash != std::string_view::npos)
         return find(term.substr(0, slash), term.substr(slash + 1));
      return findById(term);
   }

   auto findById(std::string_view id) const -> Model const* {
      for (auto const& m : models)
         if (m.id == id || m.name == id) return &m;
      return nullptr;
   }

   auto providers() const -> std::vector<std::string_view> {
      std::vector<std::string_view> out;
      for (auto const& m : models)
         if (std::ranges::find(out, std::string_view{m.provider}) == out.end())
            out.push_back(m.provider);
      return out;
   }

   auto modelsOf(std::string_view provider) const
      -> std::vector<Model const*> {
      std::vector<Model const*> out;
      for (auto const& m : models)
         if (m.provider == provider) out.push_back(&m);
      return out;
   }
};

} // namespace pi::domain
