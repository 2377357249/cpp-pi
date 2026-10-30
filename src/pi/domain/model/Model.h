#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pi::domain {

// pi ModelCost (catalog 费率, USD / 百万 token; 可能带 tiers, 后续补)
struct ModelCost {
   double input = 0, output = 0, cacheRead = 0, cacheWrite = 0;
};

// 值对象: 一个可用模型条目 (pi BaseModel+Model<chat>).
// 线格式字段与 packages/ai types.ts 对齐.
struct Model {
   std::string id;
   std::string name;
   std::string api;          // "openai-responses" | "anthropic-messages" | ...
   std::string provider;     // ProviderId
   std::string baseUrl;
   std::vector<std::string> input;   // 模态: "text" | "image"
   ModelCost cost;
   int64_t     contextWindow = 0;
   int64_t     maxTokens     = 0;
   bool        reasoning     = false;
   // thinkingLevelMap / headers / compat / promptCache / samplingParams 后续补
};

} // namespace pi::domain
