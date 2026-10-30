#pragma once

// trans-dsl-2 的 `#define __loop(...)` 宏会污染 libc++ <regex> 内部的
// `class __loop` — 头文件顺序无关地用 push/undef/pop 隔离.
#pragma push_macro("__loop")
#undef __loop
#include <regex>
#pragma pop_macro("__loop")

#include <string>

#include <folly/Expected.h>

namespace pi::infra {

// ---------------------------------------------------------------------------
// std::regex 构造是抛异常的库边界 — 项目内唯一的 regex try/catch 点.
// 使用方拿 Expected<regex, string> 传播错误.
// ---------------------------------------------------------------------------
inline auto makeRegex(std::string const& pattern, bool ignoreCase = false)
   -> folly::Expected<std::regex, std::string> {
   try {
      return std::regex{pattern, ignoreCase ? std::regex::icase
                                            : std::regex::ECMAScript};
   } catch (std::regex_error const& e) {
      return folly::makeUnexpected(std::string{e.what()});
   }
}

} // namespace pi::infra
