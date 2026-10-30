#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <pi/infra/util/Ids.h>

#include "Truncate.h"

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// pi core/tools/output-accumulator.ts 1:1 — 滚动尾部 2×maxBytes + 超限落
// 临时文件. 简化: 合并流已到齐, UTF-8 分段边界由 truncateTail 保证.
// ---------------------------------------------------------------------------
class OutputAccumulator {
public:
   struct Snapshot {
      std::string              content;
      TruncationResult         truncation;
      std::optional<std::string> fullOutputPath;
   };

   explicit OutputAccumulator(std::string_view tempFilePrefix = "pi-bash",
                              int64_t maxLines = kDefaultMaxLines,
                              int64_t maxBytes = kDefaultMaxBytes)
      : maxLines_{maxLines}, maxBytes_{maxBytes},
        maxRollingBytes_{maxBytes * 2 > 1 ? maxBytes * 2 : 1},
        tempFilePrefix_{tempFilePrefix} {}

   void append(std::string_view data) {
      if (finished_) return;
      totalBytes_ += static_cast<int64_t>(data.size());
      for (char c : data) {
         if (c == '\n') { ++totalLines_; hasOpenLine_ = false; }
         else hasOpenLine_ = true;
      }
      if (tempStream_ || shouldUseTempFile()) {
         ensureTempFile();
         if (tempStream_) tempStream_->write(data.data(), data.size());
      } else {
         rawChunks_ += data;
      }
      tail_ += data;
      if (static_cast<int64_t>(tail_.size()) > maxRollingBytes_)
         tail_ = std::string{
            truncateStringToBytesFromEnd(tail_, maxRollingBytes_)};
   }

   void finish() {
      finished_ = true;
      if (shouldUseTempFile()) ensureTempFile();
   }

   auto snapshot(bool persistIfTruncated = false) -> Snapshot {
      auto trunc = truncateTail(tail_, {maxLines_, maxBytes_});
      auto const truncated = totalLineCount() > maxLines_
                             || totalBytes_ > maxBytes_;
      if (truncated) {
         trunc.truncated = true;
         if (trunc.truncatedBy.empty())
            trunc.truncatedBy = totalBytes_ > maxBytes_ ? "bytes" : "lines";
      }
      trunc.totalLines = totalLineCount();
      trunc.totalBytes = totalBytes_;
      if (persistIfTruncated && trunc.truncated) ensureTempFile();
      return {trunc.content, trunc,
              tempFilePath_.empty()
                 ? std::optional<std::string>{} : std::optional{tempFilePath_}};
   }

   void closeTempFile() { if (tempStream_) tempStream_->close(); }

   auto lastLineBytes() const -> int64_t {
      auto const nl = tail_.rfind('\n');
      return static_cast<int64_t>(
         nl == std::string::npos ? tail_.size() : tail_.size() - nl - 1);
   }

   auto fullOutputPath() const -> std::string const& { return tempFilePath_; }

   // structuredContent.output (≤1MB 中段截断); 未落 temp 时 rawChunks_ 即全量
   auto readFullOutput(int64_t maxBytes = kStructuredMaxBytes)
      -> std::pair<std::string, bool> {
      std::string full;
      if (tempFilePath_.empty()) full = rawChunks_;
      else {
         std::ifstream f{tempFilePath_, std::ios::binary};
         full = {std::istreambuf_iterator<char>{f},
                 std::istreambuf_iterator<char>{}};
      }
      if (static_cast<int64_t>(full.size()) <= maxBytes)
         return {std::move(full), false};
      auto mid = truncateMiddle(full, maxBytes);
      return {std::move(mid.content), true};
   }

private:
   auto totalLineCount() const -> int64_t {
      return totalLines_ + (hasOpenLine_ ? 1 : 0);
   }
   auto shouldUseTempFile() const -> bool {
      return totalBytes_ > maxBytes_;
   }
   void ensureTempFile() {
      if (tempStream_) return;
      auto const dir = std::filesystem::temp_directory_path();
      tempFilePath_ = (dir / std::format("{}-{}.log", tempFilePrefix_,
                                         infra::randomHex(8))).string();
      tempStream_.emplace(tempFilePath_, std::ios::binary | std::ios::app);
      if (tempStream_ && !rawChunks_.empty())
         tempStream_->write(rawChunks_.data(), rawChunks_.size());
   }

   int64_t              maxLines_;
   int64_t              maxBytes_;
   int64_t              maxRollingBytes_;
   std::string          tempFilePrefix_;
   std::string          rawChunks_;
   std::string          tail_;
   int64_t              totalBytes_  = 0;
   int64_t              totalLines_  = 0;
   bool                 hasOpenLine_ = false;
   bool                 finished_    = false;
   std::string          tempFilePath_;
   std::optional<std::ofstream> tempStream_;
};

} // namespace pi::iface::adapter
