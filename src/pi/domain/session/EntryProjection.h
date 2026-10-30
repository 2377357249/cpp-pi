#pragma once

#include <cstdio>
#include <ctime>
#include <string_view>
#include <vector>

#include <pi/domain/session/Entry.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// 单条 entry -> 上下文消息 (pi sessionEntryToContextMessages).
// 供 Session::messages() 与 compaction 的逐条投影共用.
// ---------------------------------------------------------------------------

// ISO8601 -> ms (秒级即可; 解析失败给 0)
inline auto isoToMs(std::string_view iso) -> int64_t {
   std::tm tm{};
   if (::sscanf(std::string{iso}.c_str(), "%d-%d-%dT%d:%d:%d",
                &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 6)
      return 0;
   tm.tm_year -= 1900;
   tm.tm_mon  -= 1;
   return static_cast<int64_t>(::timegm(&tm)) * 1000;
}

inline void entryContextMessages(Entry const& e, std::vector<Message>& out,
                                 int64_t entryTs) {
   switch (e.payload.index()) {
      case 0: {   // message
         auto m = std::get<MessageEntry>(e.payload).message;
         if (m.timestampMs == 0) m.timestampMs = entryTs;
         out.push_back(std::move(m));
         break;
      }
      case 4: {   // compaction -> [systemMessage?, compactionSummary]
         auto const& c = std::get<CompactionEntry>(e.payload);
         if (c.hasSystemMessage) {
            auto s = c.systemMessage;
            s.role = Role::System;
            if (s.timestampMs == 0) s.timestampMs = entryTs;
            out.push_back(std::move(s));
         }
         Message m{Role::CompactionSummary, {}, {}, {}};
         m.summary      = c.summary;
         m.tokensBefore = c.tokensBefore;
         m.timestampMs  = entryTs;
         out.push_back(std::move(m));
         break;
      }
      case 5: {   // branch_summary -> branchSummary (summary 空则跳过)
         auto const& b = std::get<BranchSummaryEntry>(e.payload);
         if (b.summary.empty()) break;
         Message m{Role::BranchSummary, {}, {}, {}};
         m.summary     = b.summary;
         m.fromId      = b.fromId;
         m.timestampMs = entryTs;
         out.push_back(std::move(m));
         break;
      }
      case 9: {   // custom_message -> Custom
         auto const& c = std::get<CustomMessageEntry>(e.payload);
         Message m{Role::Custom, {}, {}, {}};
         m.customType  = c.customType;
         m.contentJson = c.contentJson;
         m.detailsJson = c.detailsJson;
         m.display     = c.display;
         m.timestampMs = entryTs;
         out.push_back(std::move(m));
         break;
      }
      default: break;   // 其余 entry 不产生上下文消息
   }
}

} // namespace pi::domain
