#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <random>
#include <ranges>
#include <unordered_map>

#include <pi/domain/session/Session.h>
#include <pi/domain/session/EntryProjection.h>

namespace pi::domain {

namespace {

auto nowMs() -> int64_t {
   return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ISO 8601 UTC, 毫秒: "2025-12-09T00:52:54.397Z" (pi 条目 timestamp 格式)
auto nowIso() -> std::string {
   auto const tp = std::chrono::system_clock::now();
   auto const s  = std::chrono::floor<std::chrono::seconds>(tp);
   auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp - s)
                      .count();
   return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03}Z", s, ms);
}

// pi generateId(): 8 个小写 hex, 与全表去重 (≤100 次, 兜底 32-hex).
auto makeEntryId(std::vector<Entry> const& entries) -> std::string {
   static thread_local std::mt19937_64 rng{std::random_device{}()};
   auto clash = [&](std::string_view id) {
      return std::ranges::any_of(
         entries, [&](Entry const& e) { return e.id == id; });
   };
   for (int i = 0; i < 100; ++i) {
      auto id = std::format("{:08x}", rng() & 0xffffffffULL);
      if (!clash(id)) return id;
   }
   return std::format("{:016x}{:016x}", rng(), rng());
}

} // namespace

// ---------------------------------------------------------------------------
// 追加
// ---------------------------------------------------------------------------
auto Session::appendEntry(Entry e) -> Entry const& {
   if (e.id.empty())           e.id           = makeEntryId(entries_);
   if (e.timestampIso.empty()) e.timestampIso = nowIso();
   if (e.parentId.empty() && !leafId_.empty()) e.parentId = leafId_;

   leafId_ = e.id;
   return entries_.emplace_back(std::move(e));
}

auto Session::appendLoaded(Entry e) -> Entry const& {
   leafId_ = e.id;
   return entries_.emplace_back(std::move(e));
}

auto Session::appendUser(std::string text) -> void {
   Message m{Role::User, std::move(text), {}, {}};
   m.timestampMs = nowMs();
   appendEntry(Entry{EntryBase{}, MessageEntry{std::move(m)}});
}

auto Session::appendAssistant(Message msg) -> void {
   msg.role = Role::Assistant;
   if (msg.timestampMs == 0) msg.timestampMs = nowMs();
   appendEntry(Entry{EntryBase{}, MessageEntry{std::move(msg)}});
}

auto Session::appendToolResults(std::vector<ToolResult> const& results) -> void {
   for (auto const& r : results) {
      Message m{Role::Tool, r.content, {}, r.toolCallId, r.toolName, r.isError};
      m.images      = r.images;
      m.detailsJson = r.detailsJson;
      m.durationMs  = r.durationMs;
      m.timestampMs = nowMs();
      appendEntry(Entry{EntryBase{}, MessageEntry{std::move(m)}});
   }
}

// ---------------------------------------------------------------------------
// 树
// ---------------------------------------------------------------------------
auto Session::findEntry(std::string const& id) const -> Entry const* {
   auto const it = std::ranges::find_if(
      entries_, [&](Entry const& e) { return e.id == id; });
   return it == entries_.end() ? nullptr : std::addressof(*it);
}

auto Session::branchEntries() const -> std::vector<Entry> {
   std::vector<Entry> path;
   for (std::string cur = leafId_; !cur.empty();) {
      auto const* e = findEntry(cur);
      if (!e) break;
      path.push_back(*e);
      cur = e->parentId;
   }
   return {path.rbegin(), path.rend()};
}

auto Session::setLeaf(std::string const& entryId) -> bool {
   if (!findEntry(entryId)) return false;
   leafId_ = entryId;
   return true;
}

// ---------------------------------------------------------------------------
// 投影: pi sessionEntryToContextMessages + buildContextEntries + context_edit
// (entryContextMessages / isoToMs 在 EntryProjection.h)
// ---------------------------------------------------------------------------
auto Session::messages() const -> std::vector<Message> {
   auto const path = branchEntries();

   // context_edit: 每个 targetId 最新一条生效
   std::unordered_map<std::string, ContextEditEntry const*> edits;
   for (auto const& e : path) {
      if (auto const* ce = std::get_if<ContextEditEntry>(&e.payload))
         edits[ce->targetId] = ce;
   }

   // 最新 compaction: [compaction] + firstKeptEntryId 起(非 system message)
   // + compaction 之后全部 (pi buildContextEntries)
   std::optional<size_t> ci;
   for (size_t i = path.size(); i-- > 0;) {
      if (std::holds_alternative<CompactionEntry>(path[i].payload)) {
         ci = i;
         break;
      }
   }

   std::vector<Entry> effective;
   if (ci) {
      auto const& c = std::get<CompactionEntry>(path[*ci].payload);
      effective.push_back(path[*ci]);

      bool inKept = c.firstKeptEntryId.empty();
      for (size_t i = 0; i < *ci; ++i) {
         if (!inKept && path[i].id == c.firstKeptEntryId) inKept = true;
         if (!inKept) continue;
         if (auto const* me = std::get_if<MessageEntry>(&path[i].payload);
             me && me->message.role == Role::System)
            continue;
         effective.push_back(path[i]);
      }
      for (size_t i = *ci + 1; i < path.size(); ++i)
         effective.push_back(path[i]);
   } else {
      effective = path;
   }

   std::vector<Message> out;
   for (auto const& e : effective) {
      if (auto const it = edits.find(e.id); it != edits.end()) {
         if (it->second->nullReplacement) continue;   // 整体剔除
      }
      entryContextMessages(e, out, isoToMs(e.timestampIso));
   }
   return out;
}

} // namespace pi::domain
