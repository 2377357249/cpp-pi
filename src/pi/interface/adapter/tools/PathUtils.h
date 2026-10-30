#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace pi::iface::adapter {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// pi utils/paths.ts + core/tools/path-utils.ts 1:1
// ---------------------------------------------------------------------------

inline auto homeDir() -> std::string {
   if (auto const* h = std::getenv("HOME"); h && *h) return h;
   return {};
}

// unicode 空白变体 -> 空格 (pi UNICODE_SPACES):
// U+00A0 U+2000..U+200A U+202F U+205F U+3000 (UTF-8 序列精确匹配)
inline auto normalizeUnicodeSpaces(std::string_view s) -> std::string {
   static constexpr std::string_view kSeqs[] = {
      "\xC2\xA0",                                  // U+00A0
      "\xE2\x80\x80", "\xE2\x80\x81", "\xE2\x80\x82", "\xE2\x80\x83",
      "\xE2\x80\x84", "\xE2\x80\x85", "\xE2\x80\x86", "\xE2\x80\x87",
      "\xE2\x80\x88", "\xE2\x80\x89", "\xE2\x80\x8A", // U+2000..U+200A
      "\xE2\x80\xAF",                              // U+202F
      "\xE2\x81\x9F",                              // U+205F
      "\xE3\x80\x80",                              // U+3000
   };
   std::string out;
   out.reserve(s.size());
   for (size_t i = 0; i < s.size();) {
      bool replaced = false;
      for (auto const seq : kSeqs) {
         if (s.substr(i, seq.size()) == seq) {
            out += ' ';
            i += seq.size();
            replaced = true;
            break;
         }
      }
      if (!replaced) out += s[i++];
   }
   return out;
}

// pi normalizePath: trim? -> unicode spaces -> strip @ -> ~ 展开 -> file://
// (本端口固定 normalizeUnicodeSpaces + stripAtPrefix, 与 pi 调用点一致)
inline auto normalizePath(std::string_view input, bool stripAtPrefix) -> std::string {
   auto s = normalizeUnicodeSpaces(input);
   if (stripAtPrefix && !s.empty() && s.front() == '@') s.erase(0, 1);

   auto const& home = homeDir();
   if (s == "~" && !home.empty()) return home;
   if (s.starts_with("~/") && !home.empty()) return home + s.substr(1);

   if (s.starts_with("file://")) {
      // file:///abs/path -> /abs/path (percent-decode 精简版)
      std::string u = s.substr(7);
      std::string dec;
      dec.reserve(u.size());
      for (size_t i = 0; i < u.size();) {
         if (u[i] == '%' && i + 2 < u.size()) {
            auto hex = [&](char c) -> int {
               if (c >= '0' && c <= '9') return c - '0';
               if (c >= 'a' && c <= 'f') return c - 'a' + 10;
               if (c >= 'A' && c <= 'F') return c - 'A' + 10;
               return -1;
            };
            int hi = hex(u[i + 1]), lo = hex(u[i + 2]);
            if (hi >= 0 && lo >= 0) { dec += static_cast<char>(hi * 16 + lo); i += 3; continue; }
         }
         dec += u[i++];
      }
      return dec;
   }
   return s;
}

// pi resolvePath: 绝对路径规范化, 否则 join(cwd) 后 resolve (./.. 折叠)
inline auto resolvePath(std::string_view input, std::string const& baseDir,
                        bool stripAtPrefix = true) -> std::string {
   auto const normalized     = normalizePath(input, stripAtPrefix);
   auto const normalizedBase = normalizePath(baseDir, false);
   fs::path const p{normalized};
   fs::path const resolved = p.is_absolute()
      ? p.lexically_normal()
      : (fs::path{normalizedBase} / p).lexically_normal();
   return resolved.string();
}

inline auto resolveToCwd(std::string_view p, std::string const& cwd) -> std::string {
   return resolvePath(p, cwd, true);
}

inline auto fileExists(std::string const& p) -> bool {
   std::error_code ec;
   return fs::exists(p, ec);
}

inline auto tryMacOSScreenshotPath(std::string p) -> std::string {
   // " 8.30.45 AM.png" -> " 8.30.45<NNBSP>AM.png"  (窄不换行空格)
   static constexpr std::string_view kNnbsp = "\xE2\x80\xAF";
   for (size_t i = 0; i + 4 <= p.size(); ++i) {
      if (p[i] != ' ') continue;
      auto tail = p.substr(i + 1);
      if (tail.starts_with("AM.") || tail.starts_with("PM.")) {
         p.replace(i, 1, kNnbsp);
         i += kNnbsp.size();
      }
   }
   return p;
}

inline auto tryCurlyQuoteVariant(std::string p) -> std::string {
   static constexpr std::string_view kRsquo = "\xE2\x80\x99";   // U+2019
   for (size_t i = 0; (i = p.find('\'', i)) != std::string::npos;)
      p.replace(i, 1, kRsquo), i += kRsquo.size();
   return p;
}

// pi resolveReadPath: 依次试原始 -> AM/PM -> NFD(近似: macOS fs 已透明处理,
// 用 std::filesystem::exists 命中即算) -> curly -> 组合.
// 注: NFD 归一化在 APFS/HFS+ 层透明, C++ 侧只需拼存在性探测.
inline auto resolveReadPath(std::string_view filePath, std::string const& cwd)
   -> std::string {
   auto const resolved = resolveToCwd(filePath, cwd);
   if (fileExists(resolved)) return resolved;

   auto const amPm = tryMacOSScreenshotPath(resolved);
   if (amPm != resolved && fileExists(amPm)) return amPm;

   auto const curly = tryCurlyQuoteVariant(resolved);
   if (curly != resolved && fileExists(curly)) return curly;

   auto const curlyAmPm = tryMacOSScreenshotPath(curly);
   if (curlyAmPm != resolved && fileExists(curlyAmPm)) return curlyAmPm;

   return resolved;
}

} // namespace pi::iface::adapter
