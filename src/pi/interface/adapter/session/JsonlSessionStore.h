#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <random>
#include <ranges>
#include <string>
#include <unordered_map>

#include <folly/json/json.h>

#include <pi/domain/session/ISessionStore.h>
#include <pi/infra/util/Ids.h>
#include <pi/infra/util/JsonSafe.h>
#include <pi/interface/rpc/JsonCodec.h>

namespace pi::iface::adapter {

namespace fs = std::filesystem;
using infra::nowIso;
using infra::fileStamp;
using infra::uuidv7;

// ---------------------------------------------------------------------------
// JSONL 会话存储: pi SessionManager 文件布局 1:1.
//   ~/.pi/agent/sessions/--<cwd编码>--/<ISOts>_<uuidv7>.jsonl
//   首行 {type:"session",version:3,id,timestamp,cwd,parentSession?}
//   后续每行一条 SessionEntry (append-only).
//   末行无 \n 自动补; 坏行跳过; 非 pi 文件 -> 非法会话错误.
// ---------------------------------------------------------------------------
class JsonlSessionStore : public domain::ISessionStore {
public:
   // agentDir 默认 ~/.pi/agent (与 TS pi 共享目录布局)
   explicit JsonlSessionStore(fs::path agentDir)
      : agentDir_(std::move(agentDir)) {}

   // ---------- 文件命名 ----------
   static auto cwdSlug(std::string_view cwd) -> std::string {
      std::string s{cwd};
      if (s.starts_with('/')) s.erase(0, 1);
      std::ranges::replace_if(
         s, [](char c) { return c == '/' || c == '\\' || c == ':'; }, '-');
      return std::format("--{}--", s);
   }

   auto sessionsDir(std::string_view cwd) const -> fs::path {
      return agentDir_ / "sessions" / cwdSlug(cwd);
   }

   // 惰性建文件: 首次 append 时才落 header (pi flushToFile 语义)
   auto ensureFile(domain::Session const& s) -> fs::path {
      if (auto const it = fileOf_.find(s.id()); it != fileOf_.end())
         return it->second;

      auto const dir  = sessionsDir(s.cwd());
      fs::create_directories(dir);
      auto const path = dir / std::format("{}_{}.jsonl",
                                          fileStamp(), uuidv7());

      folly::dynamic hdr = folly::dynamic::object
         ("type", "session")("version", 3)("id", s.id())
         ("timestamp", nowIso())("cwd", s.cwd());
      if (!s.parentSessionPath().empty())
         hdr["parentSession"] = s.parentSessionPath();

      std::ofstream f{path, std::ios::app};
      f << folly::toJson(hdr) << "\n";
      fileOf_[s.id()] = path;
      return path;
   }

   auto fileOf(domain::Session const& s) const -> std::string override {
      auto const it = fileOf_.find(s.id());
      return it == fileOf_.end() ? std::string{} : it->second.string();
   }

   // ---------- ISessionStore ----------
   void append(domain::Session const&          session,
               std::vector<domain::Entry> const& delta,
               domain::StoreDoneHandler          done) override {
      if (delta.empty()) { done(true); return; }
      std::ofstream f{ensureFile(session), std::ios::app};
      if (!f) { done(false); return; }
      for (auto const& e : delta)
         f << folly::toJson(rpc::entryToJson(e)) << "\n";
      done(static_cast<bool>(f));
   }

   void load(std::string const& sessionPath,
             domain::LoadDoneHandler done) override {
      std::ifstream f{sessionPath};
      if (!f) { done(domain::Session{sessionPath}, {}); return; }

      domain::Session s{sessionPath};
      std::string     line;
      bool            headerSeen = false;

      while (std::getline(f, line)) {
         if (line.empty()) continue;
         auto pj = infra::parseJsonSafe(line);
         if (pj.hasError()) continue;         // 坏行跳过
         auto const& j = *pj;

         if (!headerSeen) {
            if (!j.isObject() || !j.count("type")
                || j["type"].getString() != "session") {
               done(domain::Session{sessionPath}, {});   // 非 pi 会话文件
               return;
            }
            headerSeen = true;
            if (j.count("id"))  s.setId(j["id"].getString());
            if (j.count("cwd")) s.setCwd(j["cwd"].getString());
            if (j.count("parentSession") && !j["parentSession"].isNull())
               s.setParentSessionPath(j["parentSession"].getString());
            continue;
         }
         if (auto e = rpc::entryFromJson(j))
            s.appendLoaded(std::move(*e));
      }

      // 末行无 \n 的半截 JSON 修复: 追加换行
      if (!f.eof() && (f.bad() || f.fail())) {
         if (std::ofstream fix{sessionPath, std::ios::app}) fix << "\n";
      }

      s.setFilePath(sessionPath);
      s.markPersisted();
      fileOf_[s.id()] = sessionPath;
      done(std::move(s), sessionPath);
   }

   // ---------- fork/clone: 新文件只含 root->leaf 路径 ----------
   auto writeBranch(domain::Session const& src,
                    std::string_view       targetLeafId) -> std::string override {
      std::vector<domain::Entry> path;
      for (std::string cur{targetLeafId}; !cur.empty();) {
         auto const* e = src.findEntry(cur);
         if (!e) break;
         path.push_back(*e);
         cur = e->parentId;
      }

      domain::Session branch{uuidv7(), src.cwd()};
      auto const srcFile = fileOf(src);
      branch.setParentSessionPath(srcFile.empty()
                                     ? src.id() + ".jsonl"
                                     : srcFile);
      auto const outPath = ensureFile(branch);

      std::ofstream f{outPath, std::ios::app};
      if (!f) return {};
      std::string parent;
      for (auto const& e : path | std::views::reverse) {
         auto copy     = e;
         copy.parentId = parent;            // 新文件内重链
         parent = copy.id;
         f << folly::toJson(rpc::entryToJson(copy)) << "\n";
      }
      return f ? outPath.string() : std::string{};
   }

   // ---------- /resume: 列 cwd 目录下的历史会话 (mtime 新→旧) ----------
   auto list(std::string_view cwd) const
      -> std::vector<domain::SessionListItem> override {
      std::vector<domain::SessionListItem> out;
      auto const dir = sessionsDir(cwd);
      std::error_code ec;
      if (!fs::exists(dir, ec)) return out;
      for (auto const& de : fs::directory_iterator(dir, ec)) {
         if (!de.is_regular_file() || de.path().extension() != ".jsonl")
            continue;
         auto item = scan(de.path());
         if (!item.sessionId.empty()) out.push_back(std::move(item));
      }
      std::ranges::sort(out, {}, [](auto const& i) { return -i.mtimeMs; });
      return out;
   }

   // 快速扫一个会话文件: header id/timestamp + 首条 user + 最新名字.
   static auto scan(fs::path const& path) -> domain::SessionListItem {
      domain::SessionListItem item;
      item.path = path.string();
      std::error_code ec;
      item.mtimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
         fs::last_write_time(path, ec).time_since_epoch()).count();
      std::ifstream f{path};
      for (std::string line; std::getline(f, line);) {
         if (line.empty()) continue;
         auto pj = infra::parseJsonSafe(line);
         if (pj.hasError() || !pj->isObject() || !pj->count("type"))
            continue;
         auto const& j = *pj;
         auto const    t = j["type"].getString();
         if (t == "session") {
            if (j.count("id"))        item.sessionId = j["id"].getString();
            if (j.count("timestamp")) item.timestamp =
               j["timestamp"].getString();
         } else if (t == "session_info" && j.count("name")) {
            item.name = j["name"].getString();
         } else if (t == "message" && item.firstUserText.empty()
                    && j.count("message")) {
            auto const& m = j["message"];
            if (m.count("role") && m["role"].getString() == "user") {
               if (m.count("content") && m["content"].isString())
                  item.firstUserText = m["content"].getString();
               else if (m.count("content") && m["content"].isArray())
                  for (auto const& c : m["content"].items())
                     if (c.second.count("type")
                         && c.second["type"].getString() == "text"
                         && c.second.count("text")) {
                        item.firstUserText = c.second["text"].getString();
                        break;
                     }
            }
         }
         if (!item.sessionId.empty() && !item.firstUserText.empty()
             && !item.name.empty())
            break;   // 元数据齐了提前收工
      }
      return item;
   }

private:
   fs::path                                agentDir_;
   std::unordered_map<std::string, fs::path> fileOf_;
};

} // namespace pi::iface::adapter
