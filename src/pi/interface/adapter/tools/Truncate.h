#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// pi core/tools/truncate.ts 1:1 — 行限 2000 / 字节限 50KB, 谁先谁生效.
// ---------------------------------------------------------------------------
inline constexpr int64_t kDefaultMaxLines     = 2000;
inline constexpr int64_t kDefaultMaxBytes     = 50 * 1024;
inline constexpr int64_t kGrepMaxLineLength   = 500;
inline constexpr int64_t kStructuredMaxBytes  = 1024 * 1024;  // bash structuredContent.output

struct TruncationResult {
   std::string content;
   bool        truncated      = false;
   // "lines" | "bytes" | "" (未截断)
   std::string truncatedBy;
   int64_t     totalLines     = 0;
   int64_t     totalBytes     = 0;
   int64_t     outputLines    = 0;
   int64_t     outputBytes    = 0;
   bool        lastLinePartial       = false;
   bool        firstLineExceedsLimit = false;
   int64_t     maxLines = kDefaultMaxLines;
   int64_t     maxBytes = kDefaultMaxBytes;
};

struct TruncationOptions {
   int64_t maxLines = kDefaultMaxLines;
   int64_t maxBytes = kDefaultMaxBytes;
};

// pi splitLinesForCounting: split("\n"), 结尾 \n 产生的空尾巴丢掉
inline auto splitLinesForCounting(std::string_view content)
   -> std::vector<std::string_view> {
   if (content.empty()) return {};
   std::vector<std::string_view> lines;
   size_t pos = 0;
   while (pos <= content.size()) {
      auto const nl = content.find('\n', pos);
      if (nl == std::string_view::npos) { lines.emplace_back(content.substr(pos)); break; }
      lines.emplace_back(content.substr(pos, nl - pos));
      pos = nl + 1;
   }
   if (!lines.empty() && lines.back().empty() && content.back() == '\n')
      lines.pop_back();
   return lines;
}

// pi formatSize: <1KB "NB" / <1MB "N.NKB" / "N.NMB"
inline auto formatSize(int64_t bytes) -> std::string {
   if (bytes < 1024) return std::format("{}B", bytes);
   if (bytes < 1024 * 1024) return std::format("{:.1f}KB", bytes / 1024.0);
   return std::format("{:.1f}MB", bytes / (1024.0 * 1024.0));
}

// pi truncateHead: 保头部完整行; 首行超字节限 -> 空 + firstLineExceedsLimit.
inline auto truncateHead(std::string_view content,
                         TruncationOptions const& opt = {}) -> TruncationResult {
   auto const lines      = splitLinesForCounting(content);
   auto const totalBytes = static_cast<int64_t>(content.size());
   auto const totalLines = static_cast<int64_t>(lines.size());

   TruncationResult r;
   r.totalLines = totalLines;
   r.totalBytes = totalBytes;
   r.maxLines   = opt.maxLines;
   r.maxBytes   = opt.maxBytes;

   if (totalLines <= opt.maxLines && totalBytes <= opt.maxBytes) {
      r.content     = std::string{content};
      r.outputLines = totalLines;
      r.outputBytes = totalBytes;
      return r;
   }

   if (!lines.empty()
       && static_cast<int64_t>(lines.front().size()) > opt.maxBytes) {
      r.truncated = true;
      r.truncatedBy = "bytes";
      r.firstLineExceedsLimit = true;
      return r;
   }

   std::string out;
   int64_t     outBytes = 0;
   std::string truncatedBy = "lines";
   int64_t     kept = 0;
   for (size_t i = 0; i < lines.size() && static_cast<int64_t>(i) < opt.maxLines; ++i) {
      auto const lineBytes = static_cast<int64_t>(lines[i].size()) + (i > 0 ? 1 : 0);
      if (outBytes + lineBytes > opt.maxBytes) { truncatedBy = "bytes"; break; }
      if (!out.empty()) out += '\n';
      out += lines[i];
      outBytes += lineBytes;
      ++kept;
   }
   if (kept >= opt.maxLines && outBytes <= opt.maxBytes) truncatedBy = "lines";

   r.content      = std::move(out);
   r.truncated    = true;
   r.truncatedBy  = truncatedBy;
   r.outputLines  = kept;
   r.outputBytes  = static_cast<int64_t>(r.content.size());
   return r;
}

// UTF-8 安全地从尾部截到 maxBytes (pi truncateStringToBytesFromEnd)
inline auto truncateStringToBytesFromEnd(std::string_view str, int64_t maxBytes)
   -> std::string_view {
   if (static_cast<int64_t>(str.size()) <= maxBytes) return str;
   size_t start = str.size() - static_cast<size_t>(maxBytes);
   while (start < str.size()
          && (static_cast<unsigned char>(str[start]) & 0xc0) == 0x80)
      ++start;
   return str.substr(start);
}

// pi truncateTail: 保尾部完整行; 末行超字节限 -> 取该行尾部 (partial).
inline auto truncateTail(std::string_view content,
                         TruncationOptions const& opt = {}) -> TruncationResult {
   auto const lines      = splitLinesForCounting(content);
   auto const totalBytes = static_cast<int64_t>(content.size());
   auto const totalLines = static_cast<int64_t>(lines.size());

   TruncationResult r;
   r.totalLines = totalLines;
   r.totalBytes = totalBytes;
   r.maxLines   = opt.maxLines;
   r.maxBytes   = opt.maxBytes;

   if (totalLines <= opt.maxLines && totalBytes <= opt.maxBytes) {
      r.content     = std::string{content};
      r.outputLines = totalLines;
      r.outputBytes = totalBytes;
      return r;
   }

   std::vector<std::string_view> kept;
   int64_t outBytes = 0;
   std::string truncatedBy = "lines";
   bool    partial = false;

   for (auto i = lines.size(); i-- > 0 && static_cast<int64_t>(kept.size()) < opt.maxLines;) {
      auto const lineBytes = static_cast<int64_t>(lines[i].size()) + (kept.empty() ? 0 : 1);
      if (outBytes + lineBytes > opt.maxBytes) {
         truncatedBy = "bytes";
         if (kept.empty()) {
            auto t = truncateStringToBytesFromEnd(lines[i], opt.maxBytes);
            kept.push_back(t);
            outBytes = static_cast<int64_t>(t.size());
            partial = true;
         }
         break;
      }
      kept.push_back(lines[i]);
      outBytes += lineBytes;
   }
   if (static_cast<int64_t>(kept.size()) >= opt.maxLines && outBytes <= opt.maxBytes)
      truncatedBy = "lines";

   std::string out;
   for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
      if (!out.empty()) out += '\n';
      out += *it;
   }
   r.content         = std::move(out);
   r.truncated       = true;
   r.truncatedBy     = truncatedBy;
   r.outputLines     = static_cast<int64_t>(kept.size());
   r.outputBytes     = static_cast<int64_t>(r.content.size());
   r.lastLinePartial = partial;
   return r;
}

// pi truncateLine: 单行超 maxChars -> "... [truncated]"
inline auto truncateLine(std::string_view line, int64_t maxChars = kGrepMaxLineLength)
   -> std::pair<std::string, bool> {
   if (static_cast<int64_t>(line.size()) <= maxChars)
      return {std::string{line}, false};
   return {std::string{line.substr(0, static_cast<size_t>(maxChars))}
           + "... [truncated]", true};
}

struct MiddleTruncationResult {
   std::string content;
   bool        truncated    = false;
   int64_t     removedChars = 0;
   int64_t     totalBytes   = 0;
   int64_t     totalLines   = 0;
};

// pi truncateMiddle: 头尾各留一半, 中间 "...N chars truncated..." (UTF-8 边界)
inline auto truncateMiddle(std::string_view content, int64_t maxBytes)
   -> MiddleTruncationResult {
   MiddleTruncationResult r;
   r.totalBytes = static_cast<int64_t>(content.size());
   r.totalLines = static_cast<int64_t>(splitLinesForCounting(content).size());
   if (r.totalBytes <= maxBytes) { r.content = std::string{content}; return r; }

   auto isBoundary = [&](size_t i) {
      return i >= content.size()
             || (static_cast<unsigned char>(content[i]) & 0xc0) != 0x80;
   };
   auto headEnd = static_cast<size_t>(maxBytes / 2);
   while (headEnd > 0 && !isBoundary(headEnd)) --headEnd;
   auto tailStart = content.size() - static_cast<size_t>(maxBytes - maxBytes / 2);
   while (tailStart < content.size() && !isBoundary(tailStart)) ++tailStart;

   auto const removed = static_cast<int64_t>(tailStart - headEnd);
   r.content = std::string{content.substr(0, headEnd)}
               + std::format("…{} chars truncated…", removed)
               + std::string{content.substr(tailStart)};
   r.truncated    = true;
   r.removedChars = removed;
   return r;
}

} // namespace pi::iface::adapter
