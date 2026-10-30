#pragma once

#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// glob 匹配 (fd/globset 语义的子集): * ? [..] {a,b} ** 转义.
// ---------------------------------------------------------------------------
namespace detail {

inline auto globClassMatch(std::string_view& pat, char c) -> bool {
   // [abc] / [a-z] / [!abc]
   if (pat.empty() || pat.front() != '[') return false;
   pat.remove_prefix(1);
   bool negate = false;
   if (!pat.empty() && (pat.front() == '!' || pat.front() == '^')) {
      negate = true; pat.remove_prefix(1);
   }
   bool matched = false;
   bool first = true;
   while (!pat.empty() && (pat.front() != ']' || first)) {
      first = false;
      char lo = pat.front();
      pat.remove_prefix(1);
      if (!pat.empty() && pat.front() == '-' && pat.size() > 1
          && pat[1] != ']') {
         pat.remove_prefix(1);
         char hi = pat.front();
         pat.remove_prefix(1);
         if (c >= lo && c <= hi) matched = true;
      } else if (c == lo) matched = true;
   }
   if (!pat.empty()) pat.remove_prefix(1);   // ']'
   return matched != negate;
}

// 递归 glob 匹配; "**" 跨段, "*" 不跨 '/'.
inline auto globMatch(std::string_view pat, std::string_view str) -> bool {
   while (!pat.empty()) {
      char const p = pat.front();
      if (p == '*') {
         // 处理 ** (可后随 / 或结尾)
         size_t stars = 0;
         while (stars < pat.size() && pat[stars] == '*') ++stars;
         pat.remove_prefix(stars);
         bool const doubleStar = stars >= 2;
         if (pat.empty()) return true;                       // 尾部 ** 或 *
         // "**/" 或 "**" 可匹配 0 段: 先试跳过
         if (doubleStar && globMatch(pat, str)) return true;
         // 单个 *: 消费非 '/' 字符逐个试; 双 *: 任意字符
         for (size_t i = 0; i <= str.size(); ++i) {
            if (globMatch(pat, str.substr(i))) return true;
            if (i == str.size()) break;
            if (!doubleStar && str[i] == '/') return false;
         }
         return false;
      }
      if (str.empty()) return false;
      switch (p) {
         case '?':
            if (str.front() == '/') return false;
            pat.remove_prefix(1); str.remove_prefix(1);
            break;
         case '[':
            if (!globClassMatch(pat, str.front())) return false;
            str.remove_prefix(1);
            break;
         case '\\':
            pat.remove_prefix(1);
            if (pat.empty()) return false;
            if (str.front() != pat.front()) return false;
            pat.remove_prefix(1); str.remove_prefix(1);
            break;
         case '{': {   // {a,b,c} 交替
            auto const close = pat.find('}');
            if (close == std::string_view::npos) {           // 无 } 按字面
               if (str.front() != '{') return false;
               pat.remove_prefix(1); str.remove_prefix(1);
               break;
            }
            auto inner = pat.substr(1, close - 1);
            pat.remove_prefix(close + 1);
            size_t s = 0;
            while (true) {
               auto const comma = inner.find(',', s);
               auto const alt = inner.substr(s, comma == std::string_view::npos
                                               ? comma : comma - s);
               if (globMatch(std::string{alt} + std::string{pat}, str))
                  return true;
               if (comma == std::string_view::npos) return false;
               s = comma + 1;
            }
         }
         default:
            if (str.front() != p) return false;
            pat.remove_prefix(1); str.remove_prefix(1);
      }
   }
   return str.empty();
}

} // namespace detail

inline auto globMatch(std::string_view pattern, std::string_view path) -> bool {
   return detail::globMatch(pattern, path);
}

// ---------------------------------------------------------------------------
// .gitignore 匹配 (rg/fd 行为子集):
//   - 注释/空行跳过; ! 取反; 尾 / 只匹配目录; 含 / 锚定文件所在目录,
//     否则匹配任意深度 basename; 最后一条命中规则生效.
// ---------------------------------------------------------------------------
class GitIgnoreStack {
public:
   struct Rule {
      std::string baseDir;    // .gitignore 所在目录 (相对搜索根, ""=根)
      std::string pattern;    // 去前缀处理后的 glob
      bool        negate = false;
      bool        dirOnly = false;
      bool        anchored = false;   // 模式里含 '/'
   };

   // 把一个目录的 .gitignore 追加进规则栈
   void load(std::string const& gitignorePath, std::string baseDir) {
      std::ifstream f{gitignorePath};
      if (!f) return;
      std::string line;
      while (std::getline(f, line)) {
         if (!line.empty() && line.back() == '\r') line.pop_back();
         if (line.empty() || line.front() == '#') continue;

         Rule r;
         r.baseDir = std::move(baseDir);
         std::string_view pat{line};
         if (pat.starts_with('!')) { r.negate = true; pat.remove_prefix(1); }
         if (pat.ends_with('/'))   { r.dirOnly = true; pat.remove_suffix(1); }
         if (pat.starts_with('/')) pat.remove_prefix(1);
         r.anchored = pat.find('/') != std::string_view::npos;
         r.pattern  = std::string{pat};
         if (!r.pattern.empty()) rules_.push_back(std::move(r));
      }
   }

   // relPath: 相对搜索根, '/' 分隔; isDir: 是否目录
   auto isIgnored(std::string_view relPath, bool isDir) const -> bool {
      bool ignored = false;
      for (auto const& r : rules_) {
         if (r.dirOnly && !isDir) continue;
         std::string_view rel = relPath;
         if (!r.baseDir.empty()) {
            // 规则只作用于其 .gitignore 目录的子树
            if (!relPath.starts_with(r.baseDir)
                || relPath.size() <= r.baseDir.size()
                || relPath[r.baseDir.size()] != '/')
               continue;
            rel = relPath.substr(r.baseDir.size() + 1);
         }
         bool hit;
         if (r.anchored) {
            hit = globMatch(r.pattern, rel);
         } else {
            auto const slash = rel.rfind('/');
            hit = globMatch(r.pattern,
                            slash == std::string_view::npos
                               ? rel : rel.substr(slash + 1));
         }
         if (hit) ignored = !r.negate;
      }
      return ignored;
   }

   auto empty() const -> bool { return rules_.empty(); }

private:
   std::vector<Rule> rules_;
};

} // namespace pi::iface::adapter
