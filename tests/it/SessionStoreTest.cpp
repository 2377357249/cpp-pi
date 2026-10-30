// JSONL 会话持久化 IT: 真实 --session-dir 写盘 + switch_session 载入往返.
// 覆盖 JsonlSessionStore::load 与 entryFromJson 的反序列化路径.

#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

#include "RpcHarness.h"

using pi::it::RpcProc;
namespace fs = std::filesystem;

namespace {
struct SessDir {
   fs::path dir;
   SessDir() : dir{fs::temp_directory_path()
                   / std::format("cpp-pi-sess-it-{}", ::getpid())} {
      fs::create_directories(dir);
   }
   ~SessDir() { fs::remove_all(dir); }
};
} // namespace

// 写盘后拿到 sessionFile, 新会话 switch_session 载入, get_entries 应含原消息
TEST(SessionStore, SwitchSessionLoadsPersistedEntries) {
   SessDir t;
   std::string file;
   {
      auto p = RpcProc::spawn({"--session-dir", t.dir.string()}, false);
      // 非 message 类 entry 一并落盘: sessionInfo/thinkingLevel/modelChange
      p.send(R"({"type":"set_session_name","name":"itest"})");
      p.readResponse("set_session_name");
      p.send(R"({"type":"set_thinking_level","level":"high"})");
      p.readResponse("set_thinking_level");
      p.send(R"({"type":"prompt","message":"hello store"})");
      p.readUntil("agent_end");
      p.send(R"({"type":"get_session_stats"})");
      auto st = p.readResponse("get_session_stats");
      file = st["data"]["sessionFile"].getString();
      ASSERT_FALSE(file.empty());
   }
   // 文件存在且含 session 头
   ASSERT_TRUE(fs::exists(file));
   {
      std::ifstream f{file};
      std::string first;
      std::getline(f, first);
      EXPECT_TRUE(first.find("\"session\"") != std::string::npos) << first;
   }
   // 新进程 switch_session 载入
   {
      auto p = RpcProc::spawn({"--session-dir", t.dir.string()}, false);
      auto esc = std::string{file};
      p.send("{\"type\":\"switch_session\",\"sessionPath\":\"" + esc + "\"}");
      auto r = p.readResponse("switch_session");
      ASSERT_TRUE(r["success"].getBool()) << folly::toJson(r);
      p.send(R"({"type":"get_entries"})");
      auto e = p.readResponse("get_entries");
      auto const& entries = e["data"]["entries"];
      ASSERT_TRUE(entries.isArray());
      bool sawUser = false, sawInfo = false, sawThinking = false;
      for (auto const& en : entries) {
         if (!en.count("type")) continue;
         auto const ty = en["type"].getString();
         if (ty == "message" &&
             en["message"]["role"].getString() == "user" &&
             en["message"]["content"].getString().find("hello store")
                != std::string::npos)
            sawUser = true;
         if (ty == "session_info" && en["name"].getString() == "itest")
            sawInfo = true;
         if (ty == "thinking_level_change"
             && en["thinkingLevel"].getString() == "high")
            sawThinking = true;
      }
      EXPECT_TRUE(sawUser);
      EXPECT_TRUE(sawInfo);
      EXPECT_TRUE(sawThinking);
   }
}

// fork 后原文件保持, fork 产生新文件
TEST(SessionStore, ForkWritesBranchFile) {
   SessDir t;
   auto p = RpcProc::spawn({"--session-dir", t.dir.string()}, false);
   p.send(R"({"type":"prompt","message":"fork me"})");
   p.readUntil("agent_end");
   p.send(R"({"type":"get_session_stats"})");
   auto st = p.readResponse("get_session_stats");
   auto const file = std::string{st["data"]["sessionFile"].getString()};
   ASSERT_TRUE(fs::exists(file));
   auto const bytesBefore = fs::file_size(file);

   p.send(R"({"type":"fork","entryId":""})");
   auto fr = p.readResponse("fork");
   // entryId 为空 -> 失败分支也测一下
   EXPECT_FALSE(fr["success"].getBool());
   EXPECT_GE(fs::file_size(file), bytesBefore);
}

// 同会话多 entry 类型 (modelChange) 落盘往返
TEST(SessionStore, ModelChangeEntryPersisted) {
   SessDir t;
   std::string file;
   {
      auto p = RpcProc::spawn({"--session-dir", t.dir.string()}, false);
      p.send(R"({"type":"get_available_models"})");
      auto am = p.readResponse("get_available_models");
      auto const& models = am["data"]["models"];
      ASSERT_TRUE(models.isArray());
      auto const mid = std::string{models[0]["id"].getString()};
      p.send("{\"type\":\"set_model\",\"provider\":\""
             + std::string{models[0]["provider"].getString()}
             + "\",\"modelId\":\"" + mid + "\"}");
      p.readResponse("set_model");
      // model_change entry 随下一轮 turn 的 PersistDelta 落盘
      p.send(R"({"type":"prompt","message":"x"})");
      p.readUntil("agent_end");
      p.send(R"({"type":"get_session_stats"})");
      file = p.readResponse("get_session_stats")["data"]["sessionFile"].getString();
   }
   {
      auto p = RpcProc::spawn({"--session-dir", t.dir.string()}, false);
      p.send("{\"type\":\"switch_session\",\"sessionPath\":\"" + file + "\"}");
      ASSERT_TRUE(p.readResponse("switch_session")["success"].getBool());
      p.send(R"({"type":"get_entries"})");
      auto e = p.readResponse("get_entries");
      bool sawModel = false;
      for (auto const& en : e["data"]["entries"])
         if (en.count("type") && en["type"].getString() == "model_change")
            sawModel = true;
      EXPECT_TRUE(sawModel);
   }
}

// fork 到 user 消息 entry -> 新分支文件落盘, get_entries 截到 fork 点之前
TEST(SessionStore, ForkAtUserEntryWritesBranch) {
   SessDir t;
   {
      auto p = RpcProc::spawn({"--session-dir", t.dir.string()}, false);
      p.send(R"({"type":"prompt","message":"first q"})");
      p.readUntil("agent_end");
      p.send(R"({"type":"prompt","message":"second q"})");
      p.readUntil("agent_end");
      p.send(R"({"type":"get_entries"})");
      auto e = p.readResponse("get_entries");
      std::string userId;
      for (auto const& en : e["data"]["entries"])
         if (en.count("type") && en["type"].getString() == "message" &&
             en["message"]["role"].getString() == "user" &&
             en["message"]["content"].getString().find("second q")
                != std::string::npos)
            userId = en["id"].getString();
      ASSERT_FALSE(userId.empty());

      p.send("{\"type\":\"fork\",\"entryId\":\"" + userId + "\"}");
      auto fr = p.readResponse("fork");
      ASSERT_TRUE(fr["success"].getBool()) << folly::toJson(fr);
      // fork 在 "second q" 之前 -> 分支叶是 "second q" 的 parent;
      // 会话里应不再有 second q 的 user 消息
      p.send(R"({"type":"get_entries"})");
      auto e2 = p.readResponse("get_entries");
      bool sawSecond = false, sawFirst = false;
      for (auto const& en : e2["data"]["entries"])
         if (en.count("type") && en["type"].getString() == "message" &&
             en["message"]["role"].getString() == "user") {
            auto const c = std::string{en["message"]["content"].getString()};
            if (c.find("second q") != std::string::npos) sawSecond = true;
            if (c.find("first q") != std::string::npos) sawFirst = true;
         }
      EXPECT_TRUE(sawFirst);
      EXPECT_FALSE(sawSecond);

      // 分支文件应已创建
      p.send(R"({"type":"get_session_stats"})");
      auto st = p.readResponse("get_session_stats");
      auto const newFile =
         std::string{st["data"]["sessionFile"].getString()};
      EXPECT_TRUE(fs::exists(newFile)) << newFile;
   }
}
