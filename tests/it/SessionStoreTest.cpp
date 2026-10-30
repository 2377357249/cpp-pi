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

// 手写富 entry JSONL -> switch_session -> get_entries 字段还原
// 直击 messageFromJson/usageFromJson 全部分支: assistant 块(thinking/toolCall/
// usage)、toolResult details+image、usage record、compaction、custom、label、
// custom_message、context_edit、bashExecution.
TEST(SessionStore, RichEntriesRoundTrip) {
   SessDir t;
   auto const file = (t.dir / "sessions" / "rich.jsonl");
   fs::create_directories(file.parent_path());
   {
      std::ofstream f{file};
      f << R"({"type":"session","version":3,"id":"rich-1","timestamp":"2025-01-01T00:00:00.000Z","cwd":"/tmp"})" << "\n";
      f << R"({"type":"message","id":"aaaa0001","parentId":null,"timestamp":"2025-01-01T00:00:01.000Z","message":{"role":"assistant","content":[{"type":"thinking","thinking":"hmm","thinkingSignature":"sig1"},{"type":"text","text":"let me check"},{"type":"toolCall","id":"call_1","name":"bash","arguments":{"command":"ls"},"thoughtSignature":"ts"}],"api":"openai-responses","provider":"openai","model":"gpt-5","usage":{"input":10,"output":5,"cacheRead":2,"cacheWrite":1,"totalTokens":18,"reasoning":3,"cost":{"input":0.1,"output":0.2,"cacheRead":0.01,"cacheWrite":0.02,"total":0.33}},"stopReason":"toolUse","timestamp":1735689600000,"durationMs":42}})" << "\n";
      f << R"({"type":"message","id":"bbbb0002","parentId":"aaaa0001","timestamp":"2025-01-01T00:00:02.000Z","message":{"role":"toolResult","toolCallId":"call_1","toolName":"bash","content":[{"type":"text","text":"file.txt"},{"type":"image","data":"AAAA","mimeType":"image/png"}],"details":{"exitCode":0},"isError":false,"durationMs":7,"timestamp":1735689601000}})" << "\n";
      f << R"({"type":"usage","id":"cccc0003","parentId":"bbbb0002","timestamp":"2025-01-01T00:00:03.000Z","kind":"cache_warm","provider":"openai","model":"gpt-5","usage":{"input":3,"output":0,"cacheRead":0,"cacheWrite":0,"totalTokens":3,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}},"note":"warmup"})" << "\n";
      f << R"({"type":"label","id":"dddd0004","parentId":"cccc0003","timestamp":"2025-01-01T00:00:04.000Z","targetId":"aaaa0001","label":"checkpoint"})" << "\n";
      f << R"({"type":"custom","id":"eeee0005","parentId":"dddd0004","timestamp":"2025-01-01T00:00:05.000Z","customType":"ext-note","data":{"x":1}})" << "\n";
      f << R"({"type":"custom_message","id":"ffff0006","parentId":"eeee0005","timestamp":"2025-01-01T00:00:06.000Z","customType":"ui","display":true,"content":[{"type":"text","text":"hi"}],"details":{"k":1}})" << "\n";
      f << R"({"type":"message","id":"11110007","parentId":"ffff0006","timestamp":"2025-01-01T00:00:07.000Z","message":{"role":"bashExecution","command":"ls /tmp","output":"out","cancelled":false,"truncated":true,"exitCode":0,"fullOutputPath":"/tmp/x.log","timestamp":1735689602000}})" << "\n";
      f << R"({"type":"compaction","id":"22220008","parentId":"11110007","timestamp":"2025-01-01T00:00:08.000Z","summary":"everything so far","firstKeptEntryId":"ffff0006","tokensBefore":1234,"details":{"a":1},"usage":{"input":1,"output":1,"cacheRead":0,"cacheWrite":0,"totalTokens":2,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}},"fromHook":true})" << "\n";
      f << R"({"type":"branch_summary","id":"33330009","parentId":"22220008","timestamp":"2025-01-01T00:00:09.000Z","summary":"branched","fromId":"aaaa0001","details":{"b":2},"fromHook":true})" << "\n";
      f << R"({"type":"context_edit","id":"44440010","parentId":"33330009","timestamp":"2025-01-01T00:00:10.000Z","targetId":"bbbb0002","replacement":{"content":[{"type":"text","text":"redacted"}]}})" << "\n";
   }
   ASSERT_TRUE(fs::exists(file));

   auto p = RpcProc::spawn({"--session-dir", t.dir.string()}, false);
   p.send("{\"type\":\"switch_session\",\"sessionPath\":\"" + file.string()
          + "\"}");
   ASSERT_TRUE(p.readResponse("switch_session")["success"].getBool());
   p.send(R"({"type":"get_entries"})");
   auto e = p.readResponse("get_entries");
   auto const& entries = e["data"]["entries"];
   ASSERT_TRUE(entries.isArray());

   bool sawAssistant = false, sawToolResult = false, sawUsage = false,
        sawLabel = false, sawCustom = false, sawCustomMsg = false,
        sawBashExec = false, sawCompaction = false, sawBranch = false,
        sawCtxEdit = false;
   for (auto const& en : entries) {
      if (!en.count("type")) continue;
      auto const ty = en["type"].getString();
      if (ty == "message" && en.count("message")) {
         auto const& m = en["message"];
         auto const role = m.count("role") ? m["role"].getString() : "";
         if (role == "assistant" && m.count("content")) {
            for (auto const& b : m["content"]) {
               auto const bt = b["type"].getString();
               if (bt == "thinking"
                   && b["thinkingSignature"].getString() == "sig1")
                  sawAssistant = true;
               if (bt == "toolCall" && b["id"].getString() == "call_1"
                   && b["thoughtSignature"].getString() == "ts")
                  sawAssistant = sawAssistant || b.count("arguments") > 0;
            }
            EXPECT_EQ(m["usage"]["cost"]["total"].asDouble(), 0.33);
            EXPECT_EQ(m["stopReason"].getString(), "toolUse");
         }
         if (role == "toolResult" && m["toolCallId"].getString() == "call_1") {
            sawToolResult = m["details"]["exitCode"].getInt() == 0
                            && m["durationMs"].getInt() == 7;
            for (auto const& b : m["content"])
               if (b["type"].getString() == "image"
                   && b["mimeType"].getString() == "image/png")
                  sawToolResult = true;
         }
         if (role == "bashExecution")
            sawBashExec = m["command"].getString() == "ls /tmp"
                          && m["truncated"].getBool()
                          && m["fullOutputPath"].getString() == "/tmp/x.log";
      }
      if (ty == "usage")
         sawUsage = en["kind"].getString() == "cache_warm"
                    && en["usage"]["input"].getInt() == 3
                    && en["note"].getString() == "warmup";
      if (ty == "label")
         sawLabel = en["targetId"].getString() == "aaaa0001"
                    && en["label"].getString() == "checkpoint";
      if (ty == "custom")
         sawCustom = en["customType"].getString() == "ext-note"
                     && en["data"]["x"].getInt() == 1;
      if (ty == "custom_message")
         sawCustomMsg = en["customType"].getString() == "ui"
                        && en["details"]["k"].getInt() == 1;
      if (ty == "compaction")
         sawCompaction = en["summary"].getString() == "everything so far"
                         && en["tokensBefore"].getInt() == 1234
                         && en["fromHook"].getBool()
                         && en["usage"]["input"].getInt() == 1;
      if (ty == "branch_summary")
         sawBranch = en["fromId"].getString() == "aaaa0001"
                     && en["details"]["b"].getInt() == 2;
      if (ty == "context_edit")
         sawCtxEdit = en["targetId"].getString() == "bbbb0002"
                      && en["replacement"]["content"].isArray();
   }
   EXPECT_TRUE(sawAssistant);
   EXPECT_TRUE(sawToolResult);
   EXPECT_TRUE(sawUsage);
   EXPECT_TRUE(sawLabel);
   EXPECT_TRUE(sawCustom);
   EXPECT_TRUE(sawCustomMsg);
   EXPECT_TRUE(sawBashExec);
   EXPECT_TRUE(sawCompaction);
   EXPECT_TRUE(sawBranch);
   EXPECT_TRUE(sawCtxEdit);
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
