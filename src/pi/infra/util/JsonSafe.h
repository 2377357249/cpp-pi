#pragma once

#include <exception>
#include <string>
#include <string_view>

#include <folly/Expected.h>
#include <folly/json/json.h>

namespace pi::infra {

// ---------------------------------------------------------------------------
// 非抛异常 JSON 解析: 错误经 Expected<dynamic, std::string> 传播.
// 唯一的 try/catch 在本函数内 —— folly::parseJson 的契约是抛
// json::parse_error, 此处是库边界适配点, 把异常收敛成返回值;
// 项目其余代码一律走 Expected/optional.
// ---------------------------------------------------------------------------
inline auto parseJsonSafe(std::string_view s)
   -> folly::Expected<folly::dynamic, std::string> {
   try {
      return folly::parseJson(s);
   } catch (std::exception const& e) {
      return folly::makeUnexpected(std::string{e.what()});
   } catch (...) {
      return folly::makeUnexpected(std::string{"json parse error"});
   }
}

inline auto jsonOr(std::string_view s, folly::dynamic fallback)
   -> folly::dynamic {
   auto p = parseJsonSafe(s);
   return p ? std::move(*p) : std::move(fallback);
}

} // namespace pi::infra
