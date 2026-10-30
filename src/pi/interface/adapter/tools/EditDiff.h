#pragma once

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// pi core/tools/edit-diff.ts 1:1 (+ utils/text.ts splitBom)
// 错误用返回值传播 (expected 风格), 不抛异常.
// ---------------------------------------------------------------------------

// ---------- BOM ----------
struct BomSplit { std::string bom; std::string text; };
inline auto splitBom(std::string_view content) -> BomSplit {
   static constexpr std::string_view kBom = "\xEF\xBB\xBF";   // U+FEFF
   if (content.starts_with(kBom))
      return {std::string{kBom}, std::string{content.substr(kBom.size())}};
   return {"", std::string{content}};
}

// ---------- 行尾 ----------
inline auto detectLineEnding(std::string_view content) -> std::string_view {
   auto const crlf = content.find("\r\n");
   auto const lf   = content.find('\n');
   if (lf == std::string_view::npos) return "\n";
   if (crlf == std::string_view::npos) return "\n";
   return crlf < lf ? "\r\n" : "\n";
}

inline auto normalizeToLF(std::string_view text) -> std::string {
   std::string out;
   out.reserve(text.size());
   for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\r') {
         if (i + 1 < text.size() && text[i + 1] == '\n') { out += '\n'; ++i; }
         else out += '\n';
      } else out += text[i];
   }
   return out;
}

inline auto restoreLineEndings(std::string_view text, std::string_view ending)
   -> std::string {
   if (ending != "\r\n") return std::string{text};
   std::string out;
   out.reserve(text.size() * 11 / 10);
   for (char c : text) {
      if (c == '\n') out += "\r\n";
      else out += c;
   }
   return out;
}

// ---------- fuzzy 归一化 ----------
// pi normalizeForFuzzyMatch: NFKC -> 行尾空白剔除 -> 智能引号/破折号/特殊空格
// 归一. NFKC 全量表需 ICU, 这里覆盖 pi 显式列出的映射 + trimEnd —
// 常见场景 (智能引号/dash/NBSP/全角空格) 等价; 罕见 compat 字符是已知近似差.
inline auto normalizeForFuzzyMatch(std::string_view text) -> std::string {
   // 1) 行尾空白剔除
   std::string out;
   out.reserve(text.size());
   size_t pos = 0;
   while (pos <= text.size()) {
      auto nl = text.find('\n', pos);
      auto line = text.substr(pos, nl == std::string_view::npos
                                     ? std::string_view::npos : nl - pos);
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
         line.remove_suffix(1);
      out += line;
      if (nl == std::string_view::npos) break;
      out += '\n';
      pos = nl + 1;
   }

   // 2) UTF-8 序列级替换 (pi 的 unicode 映射)
   struct Rep { std::string_view from; char to; };
   static constexpr Rep kReps[] = {
      {"\xE2\x80\x98", '\''}, {"\xE2\x80\x99", '\''},   // ‘ ’
      {"\xE2\x80\x9A", '\''}, {"\xE2\x80\x9B", '\''},   // ‚ ‛
      {"\xE2\x80\x9C", '"'},  {"\xE2\x80\x9D", '"'},    // “ ”
      {"\xE2\x80\x9E", '"'},  {"\xE2\x80\x9F", '"'},    // „ ‟
      {"\xE2\x80\x90", '-'},  {"\xE2\x80\x91", '-'},    // ‐ ‑
      {"\xE2\x80\x92", '-'},  {"\xE2\x80\x93", '-'},    // ‒ –
      {"\xE2\x80\x94", '-'},  {"\xE2\x80\x95", '-'},    // — ―
      {"\xE2\x88\x92", '-'},                            // −
      {"\xC2\xA0", ' '},                                // NBSP
      {"\xE2\x80\x82", ' '}, {"\xE2\x80\x83", ' '},
      {"\xE2\x80\x84", ' '}, {"\xE2\x80\x85", ' '},
      {"\xE2\x80\x86", ' '}, {"\xE2\x80\x87", ' '},
      {"\xE2\x80\x88", ' '}, {"\xE2\x80\x89", ' '},
      {"\xE2\x80\x8A", ' '},                            // U+2002..U+200A
      {"\xE2\x80\xAF", ' '},                            // U+202F
      {"\xE2\x81\x9F", ' '},                            // U+205F
      {"\xE3\x80\x80", ' '},                            // U+3000
   };
   std::string out2;
   out2.reserve(out.size());
   for (size_t i = 0; i < out.size();) {
      bool hit = false;
      for (auto const& r : kReps) {
         if (std::string_view{out}.substr(i, r.from.size()) == r.from) {
            out2 += r.to;
            i += r.from.size();
            hit = true;
            break;
         }
      }
      if (!hit) out2 += out[i++];
   }
   return out2;
}

// ---------- 匹配 ----------
struct FuzzyMatchResult {
   bool        found = false;
   size_t      index = 0;
   size_t      matchLength = 0;
   bool        usedFuzzyMatch = false;
   std::string contentForReplacement;   // 命中时使用的内容视图
};

inline auto fuzzyFindText(std::string const& content, std::string const& oldText)
   -> FuzzyMatchResult {
   if (auto const exact = content.find(oldText); exact != std::string::npos)
      return {true, exact, oldText.size(), false, content};

   auto const fuzzyContent = normalizeForFuzzyMatch(content);
   auto const fuzzyOld     = normalizeForFuzzyMatch(oldText);
   if (auto const i = fuzzyContent.find(fuzzyOld); i != std::string::npos)
      return {true, i, fuzzyOld.size(), true, fuzzyContent};
   return {false, 0, 0, false, content};
}

inline auto countOccurrences(std::string const& content, std::string const& oldText)
   -> size_t {
   auto const fuzzyContent = normalizeForFuzzyMatch(content);
   auto const fuzzyOld     = normalizeForFuzzyMatch(oldText);
   if (fuzzyOld.empty()) return 0;
   size_t n = 0;
   for (size_t i = 0; (i = fuzzyContent.find(fuzzyOld, i)) != std::string::npos;
        i += fuzzyOld.size())
      ++n;
   return n;
}

// ---------- 应用 ----------
struct Edit { std::string oldText; std::string newText; };

struct EditError { std::string message; };
template <class T>
struct EditOr {
   T         value;
   EditError error;
   bool      ok = false;
   static auto success(T v) -> EditOr { return {std::move(v), {}, true}; }
   static auto fail(std::string m) -> EditOr { return {{}, {std::move(m)}, false}; }
};

struct LineSpan { size_t start; size_t end; };
struct TextReplacement { size_t matchIndex; size_t matchLength; std::string newText; };

// pi splitLinesWithEndings: 每行带自己的 \n
inline auto splitLinesWithEndings(std::string_view content)
   -> std::vector<std::string_view> {
   std::vector<std::string_view> lines;
   size_t pos = 0;
   while (pos < content.size()) {
      auto const nl = content.find('\n', pos);
      if (nl == std::string_view::npos) { lines.emplace_back(content.substr(pos)); break; }
      lines.emplace_back(content.substr(pos, nl - pos + 1));
      pos = nl + 1;
   }
   return lines;
}

inline auto getLineSpans(std::string_view content) -> std::vector<LineSpan> {
   std::vector<LineSpan> spans;
   size_t off = 0;
   for (auto const line : splitLinesWithEndings(content)) {
      spans.push_back({off, off + line.size()});
      off += line.size();
   }
   return spans;
}

inline auto applyReplacements(std::string_view content,
                              std::vector<TextReplacement> const& reps,
                              size_t offset = 0) -> std::string {
   std::string result{content};
   for (auto i = reps.size(); i-- > 0;) {
      auto const& rep = reps[i];
      auto const idx = rep.matchIndex - offset;
      result.replace(idx, rep.matchLength, rep.newText);
   }
   return result;
}

inline auto getReplacementLineRange(std::vector<LineSpan> const& lines,
                                    TextReplacement const& rep)
   -> std::pair<size_t, size_t> {
   auto const repStart = rep.matchIndex;
   auto const repEnd   = rep.matchIndex + rep.matchLength;
   size_t startLine = static_cast<size_t>(-1);
   for (size_t i = 0; i < lines.size(); ++i) {
      if (repStart >= lines[i].start && repStart < lines[i].end) {
         startLine = i;
         break;
      }
   }
   if (startLine == static_cast<size_t>(-1)) return {0, 0};
   size_t endLine = startLine;
   while (endLine < lines.size() && lines[endLine].end < repEnd) ++endLine;
   if (endLine >= lines.size()) return {startLine, lines.size()};
   return {startLine, endLine + 1};
}

// pi applyReplacementsPreservingUnchangedLines
inline auto applyReplacementsPreservingUnchangedLines(
   std::string const& originalContent, std::string const& baseContent,
   std::vector<TextReplacement> const& reps) -> std::string {
   auto const originalLines = splitLinesWithEndings(originalContent);
   auto const baseLines     = getLineSpans(baseContent);
   if (originalLines.size() != baseLines.size()) return originalContent;

   struct Group { size_t startLine; size_t endLine;
                  std::vector<TextReplacement> reps; };
   std::vector<Group> groups;
   auto sorted = reps;
   std::ranges::sort(sorted, {}, &TextReplacement::matchIndex);
   for (auto const& rep : sorted) {
      auto const range = getReplacementLineRange(baseLines, rep);
      if (!groups.empty() && range.first < groups.back().endLine) {
         groups.back().endLine = std::max(groups.back().endLine, range.second);
         groups.back().reps.push_back(rep);
         continue;
      }
      groups.push_back({range.first, range.second, {rep}});
   }

   std::string result;
   size_t origIdx = 0;
   for (auto const& g : groups) {
      for (size_t i = origIdx; i < g.startLine && i < originalLines.size(); ++i)
         result += originalLines[i];
      auto const gStart = baseLines[g.startLine].start;
      auto const gEnd   = baseLines[g.endLine - 1].end;
      result += applyReplacements(
         std::string_view{baseContent}.substr(gStart, gEnd - gStart),
         g.reps, gStart);
      origIdx = g.endLine;
   }
   for (size_t i = origIdx; i < originalLines.size(); ++i)
      result += originalLines[i];
   return result;
}

struct AppliedEdits { std::string baseContent; std::string newContent; };

// pi applyEditsToNormalizedContent — 全匹配原样原文, 反序应用, 错误串一致.
inline auto applyEditsToNormalizedContent(std::string const& normalizedContent,
                                          std::vector<Edit> const& edits,
                                          std::string const& path)
   -> EditOr<AppliedEdits> {
   std::vector<Edit> normalizedEdits;
   normalizedEdits.reserve(edits.size());
   for (auto const& e : edits)
      normalizedEdits.push_back({normalizeToLF(e.oldText), normalizeToLF(e.newText)});

   auto const n = normalizedEdits.size();
   for (size_t i = 0; i < n; ++i) {
      if (normalizedEdits[i].oldText.empty()) {
         if (n == 1)
            return EditOr<AppliedEdits>::fail(
               std::format("oldText must not be empty in {}.", path));
         return EditOr<AppliedEdits>::fail(std::format(
            "edits[{}].oldText must not be empty in {}.", i, path));
      }
   }

   std::vector<FuzzyMatchResult> initial;
   initial.reserve(n);
   for (auto const& e : normalizedEdits)
      initial.push_back(fuzzyFindText(normalizedContent, e.oldText));
   bool const usedFuzzy = std::ranges::any_of(
      initial, [](auto const& m) { return m.usedFuzzyMatch; });
   auto const replacementBase = usedFuzzy
      ? normalizeForFuzzyMatch(normalizedContent) : normalizedContent;

   struct Matched { size_t editIndex; TextReplacement rep; };
   std::vector<Matched> matched;
   for (size_t i = 0; i < n; ++i) {
      auto const& edit = normalizedEdits[i];
      auto const m = fuzzyFindText(replacementBase, edit.oldText);
      if (!m.found) {
         if (n == 1)
            return EditOr<AppliedEdits>::fail(std::format(
               "Could not find the exact text in {}. The old text must match "
               "exactly including all whitespace and newlines.", path));
         return EditOr<AppliedEdits>::fail(std::format(
            "Could not find edits[{}] in {}. The oldText must match exactly "
            "including all whitespace and newlines.", i, path));
      }
      auto const occurrences = countOccurrences(replacementBase, edit.oldText);
      if (occurrences > 1) {
         if (n == 1)
            return EditOr<AppliedEdits>::fail(std::format(
               "Found {} occurrences of the text in {}. The text must be "
               "unique. Please provide more context to make it unique.",
               occurrences, path));
         return EditOr<AppliedEdits>::fail(std::format(
            "Found {} occurrences of edits[{}] in {}. Each oldText must be "
            "unique. Please provide more context to make it unique.",
            occurrences, i, path));
      }
      matched.push_back({i, {m.index, m.matchLength, edit.newText}});
   }

   std::ranges::sort(matched, {}, [](auto const& m) { return m.rep.matchIndex; });
   for (size_t i = 1; i < matched.size(); ++i) {
      auto const& prev = matched[i - 1];
      auto const& cur  = matched[i];
      if (prev.rep.matchIndex + prev.rep.matchLength > cur.rep.matchIndex)
         return EditOr<AppliedEdits>::fail(std::format(
            "edits[{}] and edits[{}] overlap in {}. Merge them into one edit "
            "or target disjoint regions.", prev.editIndex, cur.editIndex, path));
   }

   std::vector<TextReplacement> reps;
   reps.reserve(matched.size());
   for (auto const& m : matched) reps.push_back(m.rep);

   std::string const newContent = usedFuzzy
      ? applyReplacementsPreservingUnchangedLines(
           normalizedContent, replacementBase, reps)
      : applyReplacements(replacementBase, reps);

   if (normalizedContent == newContent) {
      if (n == 1)
         return EditOr<AppliedEdits>::fail(std::format(
            "No changes made to {}. The replacement produced identical "
            "content. This might indicate an issue with special characters "
            "or the text not existing as expected.", path));
      return EditOr<AppliedEdits>::fail(std::format(
         "No changes made to {}. The replacements produced identical content.",
         path));
   }
   return EditOr<AppliedEdits>::success(
      {normalizedContent, std::move(newContent)});
}

// ---------- diff 生成 ----------
struct DiffLine { char tag; std::string text; };   // ' ' | '-' | '+'

// 行级 LCS diff (jsdiff diffLines 等价的最小编辑序列)
inline auto diffLines(std::string_view oldC, std::string_view newC)
   -> std::vector<DiffLine> {
   auto const a = splitLinesWithEndings(oldC);
   auto const b = splitLinesWithEndings(newC);
   auto const n = a.size(), m = b.size();
   if (n == 0 && m == 0) return {};

   // 大文件防 O(nm) 爆内存: 超过阈值退化为公共前后缀
   constexpr size_t kLcsCap = 4000;
   std::vector<DiffLine> out;
   if (n > kLcsCap || m > kLcsCap) {
      size_t pre = 0;
      while (pre < n && pre < m && a[pre] == b[pre]) ++pre;
      size_t suf = 0;
      while (suf < n - pre && suf < m - pre
             && a[n - 1 - suf] == b[m - 1 - suf]) ++suf;
      for (size_t i = 0; i < pre; ++i)   out.push_back({' ', std::string{a[i]}});
      for (size_t i = pre; i < n - suf; ++i) out.push_back({'-', std::string{a[i]}});
      for (size_t i = pre; i < m - suf; ++i) out.push_back({'+', std::string{b[i]}});
      for (size_t i = n - suf; i < n; ++i)   out.push_back({' ', std::string{a[i]}});
      return out;
   }

   // LCS DP (uint32 压缩行)
   std::vector<uint32_t> dp((n + 1) * (m + 1), 0);
   auto at = [&](size_t i, size_t j) -> uint32_t& { return dp[i * (m + 1) + j]; };
   for (auto i = n; i-- > 0;)
      for (auto j = m; j-- > 0;)
         at(i, j) = a[i] == b[j] ? at(i + 1, j + 1) + 1
                                 : std::max(at(i + 1, j), at(i, j + 1));
   size_t i = 0, j = 0;
   while (i < n && j < m) {
      if (a[i] == b[j]) { out.push_back({' ', std::string{a[i]}}); ++i; ++j; }
      else if (at(i + 1, j) >= at(i, j + 1)) { out.push_back({'-', std::string{a[i]}}); ++i; }
      else { out.push_back({'+', std::string{b[j]}}); ++j; }
   }
   while (i < n) out.push_back({'-', std::string{a[i++]}});
   while (j < m) out.push_back({'+', std::string{b[j++]}});
   return out;
}

inline auto stripTrailingNl(std::string_view s) -> std::string {
   auto r = std::string{s};
   if (!r.empty() && r.back() == '\n') r.pop_back();
   return r;
}

struct DiffStringResult { std::string diff; std::optional<size_t> firstChangedLine; };

// pi generateDiffString (jsdiff diffLines + 带行号上下文展示, context=4)
inline auto generateDiffString(std::string const& oldContent,
                               std::string const& newContent,
                               size_t contextLines = 4) -> DiffStringResult {
   // 先切成 (added|removed|unchanged) 段
   struct Part { char kind; std::vector<std::string> lines; };
   std::vector<Part> parts;
   for (auto const& d : diffLines(oldContent, newContent)) {
      auto const kind = d.tag == '+' ? '+' : (d.tag == '-' ? '-' : ' ');
      if (!parts.empty() && parts.back().kind == kind)
         parts.back().lines.push_back(stripTrailingNl(d.text));
      else parts.push_back({kind, {stripTrailingNl(d.text)}});
   }

   auto const oldLines = splitLinesWithEndings(oldContent).size();
   auto const newLines = splitLinesWithEndings(newContent).size();
   auto const width = std::to_string(std::max(oldLines, newLines)).size();

   size_t oldNum = 1, newNum = 1;
   bool   lastWasChange = false;
   std::optional<size_t> firstChangedLine;
   std::vector<std::string> output;
   auto pad = [&](size_t num) {
      auto s = std::to_string(num);
      return std::string(width - s.size(), ' ') + s;
   };
   auto push = [&](char tag, size_t num, std::string const& l) {
      output.push_back(std::format("{}{} {}", tag, pad(num), l));
   };

   for (size_t i = 0; i < parts.size(); ++i) {
      auto const& p = parts[i];
      if (p.kind != ' ') {
         if (!firstChangedLine) firstChangedLine = newNum;
         for (auto const& l : p.lines) {
            if (p.kind == '+') { push('+', newNum, l); ++newNum; }
            else               { push('-', oldNum, l); ++oldNum; }
         }
         lastWasChange = true;
         continue;
      }
      // 上下文段
      bool const nextIsChange = i + 1 < parts.size() && parts[i + 1].kind != ' ';
      bool const lead = lastWasChange, trail = nextIsChange;
      auto const& raw = p.lines;
      auto emit = [&](size_t count) {
         for (size_t k = 0; k < count; ++k) {
            push(' ', oldNum, raw[k]);
            ++oldNum; ++newNum;
         }
      };
      auto skip = [&](size_t count) {
         output.push_back(" " + std::string(width, ' ') + " ...");
         oldNum += count; newNum += count;
      };
      if (lead && trail) {
         if (raw.size() <= contextLines * 2) emit(raw.size());
         else {
            emit(contextLines);
            skip(raw.size() - contextLines * 2);
            for (size_t k = raw.size() - contextLines; k < raw.size(); ++k) {
               push(' ', oldNum, raw[k]); ++oldNum; ++newNum;
            }
         }
      } else if (lead) {
         if (raw.size() > contextLines) {
            emit(contextLines);
            skip(raw.size() - contextLines);
         } else emit(raw.size());
      } else if (trail) {
         auto const skipped = raw.size() > contextLines
                              ? raw.size() - contextLines : 0;
         if (skipped > 0) skip(skipped);
         for (size_t k = skipped; k < raw.size(); ++k) {
            push(' ', oldNum, raw[k]); ++oldNum; ++newNum;
         }
      } else { oldNum += raw.size(); newNum += raw.size(); }
      lastWasChange = false;
   }

   std::string diff;
   for (auto const& l : output) { diff += l; diff += '\n'; }
   if (!diff.empty()) diff.pop_back();
   return {std::move(diff), firstChangedLine};
}

// pi generateUnifiedPatch: jsdiff createTwoFilesPatch FILE_HEADERS_ONLY,
// context=4 -> "Index: path\n===...\n--- path\n+++ path\n@@ -a,b +c,d @@\n..."
inline auto generateUnifiedPatch(std::string const& path,
                                 std::string const& oldContent,
                                 std::string const& newContent,
                                 size_t contextLines = 4) -> std::string {
   auto const diffs = diffLines(oldContent, newContent);
   std::string out = "Index: " + path + "\n"
                   + "===================================================================\n"
                   + "--- " + path + "\n"
                   + "+++ " + path + "\n";

   // 1) 收集变更段 (连续的非 ' ' 行)
   struct Span { size_t start; size_t end; };
   std::vector<Span> changes;
   for (size_t i = 0; i < diffs.size();) {
      if (diffs[i].tag == ' ') { ++i; continue; }
      size_t s = i;
      while (i < diffs.size() && diffs[i].tag != ' ') ++i;
      changes.push_back({s, i});
   }
   if (changes.empty()) return out;

   // 2) 中间上下文 ≤ 2*contextLines 的变更合并成一个 hunk
   std::vector<Span> hunks{changes.front()};
   for (size_t i = 1; i < changes.size(); ++i) {
      if (changes[i].start - hunks.back().end <= contextLines * 2)
         hunks.back().end = changes[i].end;
      else hunks.push_back(changes[i]);
   }

   // 3) 每 hunk 前后扩 contextLines, 发射行号头 + 行体
   for (auto const& h : hunks) {
      size_t hunkStart = h.start > contextLines ? h.start - contextLines : 0;
      size_t hunkEnd   = std::min(h.end + contextLines, diffs.size());

      size_t hOldStart = 1, hNewStart = 1;
      for (size_t k = 0; k < hunkStart; ++k) {
         if (diffs[k].tag != '+') ++hOldStart;
         if (diffs[k].tag != '-') ++hNewStart;
      }
      size_t hOld = 0, hNew = 0;
      for (size_t k = hunkStart; k < hunkEnd; ++k) {
         if (diffs[k].tag != '+') ++hOld;
         if (diffs[k].tag != '-') ++hNew;
      }
      out += std::format("@@ -{},{} +{},{} @@\n", hOldStart, hOld, hNewStart, hNew);
      for (size_t k = hunkStart; k < hunkEnd; ++k) {
         auto const& d = diffs[k];
         auto text = stripTrailingNl(d.text);
         if (d.text.empty() || d.text.back() != '\n') {
            // 文件末尾无换行: 标记 "\ No newline at end of file"
            out += d.tag;
            out += text;
            out += "\n\\ No newline at end of file\n";
         } else {
            out += d.tag;
            out += text;
            out += '\n';
         }
      }
   }
   return out;
}

} // namespace pi::iface::adapter
