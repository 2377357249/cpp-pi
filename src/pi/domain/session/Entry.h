#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <pi/domain/message/Message.h>

namespace pi::domain {

// ---------------------------------------------------------------------------
// Session 持久化条目 —— pi SessionEntry union 的域内表达.
// (packages/coding-agent/src/core/session-manager.ts)
// JSONL 文件: 首行 SessionHeader, 其后每条 entry {type,id,parentId,timestamp}.
// id = 8-hex 随机; parentId 空 = 根; timestamp = ISO8601 (与消息的 ms 不同!).
// ---------------------------------------------------------------------------

struct EntryBase {
   std::string id;
   std::string parentId;     // "" 表示 null (根)
   std::string timestampIso; // ISO 8601
};

// --- payload 变体 (pi 各 SessionEntry 子类型) ---
struct MessageEntry {          // type: "message"
   Message message;
};
struct ThinkingLevelEntry {    // "thinking_level_change"
   std::string thinkingLevel;
};
struct ModelChangeEntry {      // "model_change"
   std::string provider;
   std::string modelId;
};
struct UsageRecordEntry {      // "usage"
   std::string kind;           // 如 "cache_warm"
   std::string provider;
   std::string model;
   Usage       usage;
   std::string note;
};
struct CompactionEntry {       // "compaction"
   std::string summary;
   std::string firstKeptEntryId;   // == 自身 id 时表示全丢弃
   int64_t     tokensBefore = 0;
   std::string detailsJson;
   Usage       usage;
   bool        hasUsage         = false;
   bool        fromHook         = false;
   Message     systemMessage;      // 压缩时刻重放的 system checkpoint
   bool        hasSystemMessage = false;
};
struct BranchSummaryEntry {    // "branch_summary"
   std::string fromId;         // 旧 leafId, "root" 表示从根分叉
   std::string summary;
   std::string detailsJson;
   Usage       usage;
   bool        hasUsage = false;
   bool        fromHook = false;
};
struct CustomEntry {           // "custom"
   std::string customType;
   std::string dataJson;
};
struct LabelEntry {            // "label"
   std::string targetId;
   std::string label;
   bool        clear = false;  // label == undefined → 清除标签
};
struct SessionInfoEntry {      // "session_info"
   std::string name;
   bool        hasName = false;
};
struct CustomMessageEntry {    // "custom_message"
   std::string customType;
   std::string contentJson;    // string | (Text|Image)[] 的线格式 JSON
   std::string detailsJson;
   bool        display = false;
};
struct ContextEditEntry {      // "context_edit"
   std::string targetId;
   std::string replacementJson; // {content: ...} 或 null
   bool        nullReplacement = true;
};

using EntryPayload = std::variant<
   MessageEntry, ThinkingLevelEntry, ModelChangeEntry, UsageRecordEntry,
   CompactionEntry, BranchSummaryEntry, CustomEntry, LabelEntry,
   SessionInfoEntry, CustomMessageEntry, ContextEditEntry>;

// 完整条目 (type 由 payload 变体推出, 与 pi 字段名一致)
struct Entry : EntryBase {
   EntryPayload payload;
};

inline auto entryType(Entry const& e) -> const char* {
   switch (e.payload.index()) {
      case 0:  return "message";
      case 1:  return "thinking_level_change";
      case 2:  return "model_change";
      case 3:  return "usage";
      case 4:  return "compaction";
      case 5:  return "branch_summary";
      case 6:  return "custom";
      case 7:  return "label";
      case 8:  return "session_info";
      case 9:  return "custom_message";
      case 10: return "context_edit";
   }
   return "custom";
}

} // namespace pi::domain
