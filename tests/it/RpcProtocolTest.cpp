#include <filesystem>
#include <gtest/gtest.h>

#include "RpcHarness.h"

using pi::it::RpcHarness;

// ---------------------------------------------------------------------------
// 用例结构对齐 pi packages/coding-agent/test/rpc.test.ts +
// rpc-prompt-response-semantics.test.ts + rpc-jsonl.test.ts.
// 粒度: 外部输入 NDJSON 命令 -> stdout 输出帧字段.
// mock LLM 行为: 首轮 completion 请求 {echo, input=prompt}; 第二轮产出
// "工具返回: echo: <prompt>". 单 prompt 消息序: user / asst(toolCall)
// / toolResult / asst(text) 共 4 条.
// ---------------------------------------------------------------------------

// == rpc.test.ts: "should get state" ======================================
TEST(RpcMode, ShouldGetState) {
   auto frames = RpcHarness::run(R"({"type":"get_state"})" "\n");
   auto d = RpcHarness::response(frames, "get_state")["data"];
   ASSERT_TRUE(d.isObject());
   EXPECT_TRUE(d.count("model"));
   EXPECT_EQ(d["model"]["provider"].getString(), "mock");
   EXPECT_EQ(d["model"]["id"].getString(), "mock-1");
   EXPECT_FALSE(d["isStreaming"].getBool());
   EXPECT_EQ(d["messageCount"].getInt(), 0);
}

// == rpc-prompt-response-semantics: started disposition + 事件序 =========
TEST(RpcPromptSemantics, EmitsStartedResponseAndAgentEvents) {
   auto frames = RpcHarness::run(
      R"({"id":"b2","type":"prompt","message":"Hello"})" "\n");

   // 恰好一条 prompt 响应, disposition=started (pi: preflight 成功)
   auto resps = RpcHarness::ofType(frames, "response");
   ASSERT_EQ(resps.size(), 1u);
   EXPECT_EQ(resps[0]["id"].getString(), "b2");
   EXPECT_EQ(resps[0]["command"].getString(), "prompt");
   EXPECT_TRUE(resps[0]["success"].getBool());
   EXPECT_EQ(resps[0]["data"]["disposition"].getString(), "started");

   // 事件流: agent_start -> turn_start -> message_start/end x4
   //        -> turn_end -> agent_end
   EXPECT_EQ(RpcHarness::ofType(frames, "agent_start").size(), 1u);
   EXPECT_EQ(RpcHarness::ofType(frames, "turn_start").size(), 1u);
   auto msgEnds = RpcHarness::ofType(frames, "message_end");
   ASSERT_EQ(msgEnds.size(), 4u);
   EXPECT_EQ(msgEnds[0]["message"]["role"].getString(), "user");
   EXPECT_EQ(msgEnds[3]["message"]["role"].getString(), "assistant");
   EXPECT_EQ(msgEnds[3]["message"]["stopReason"].getString(), "stop");

   auto turnEnds = RpcHarness::ofType(frames, "turn_end");
   ASSERT_EQ(turnEnds.size(), 1u);
   EXPECT_EQ(turnEnds[0]["toolResults"].size(), 1u);
   auto agentEnds = RpcHarness::ofType(frames, "agent_end");
   ASSERT_EQ(agentEnds.size(), 1u);
   EXPECT_EQ(agentEnds[0]["messages"].size(), 4u);
}

// == rpc.test.ts: "should set and get thinking level" ====================
TEST(RpcMode, ShouldSetAndGetThinkingLevel) {
   auto frames = RpcHarness::run(
      R"({"type":"set_thinking_level","level":"high"})" "\n"
      R"({"type":"get_state"})" "\n");
   EXPECT_TRUE(RpcHarness::response(frames, "set_thinking_level")
                  ["success"].getBool());
   EXPECT_EQ(RpcHarness::response(frames, "get_state")
                ["data"]["thinkingLevel"].getString(), "high");
}

// == rpc.test.ts: "should cycle thinking level" ==========================
TEST(RpcMode, ShouldCycleThinkingLevel) {
   auto frames = RpcHarness::run(
      R"({"type":"cycle_thinking_level"})" "\n"
      R"({"type":"get_state"})" "\n");
   auto cycled = RpcHarness::response(frames, "cycle_thinking_level");
   ASSERT_TRUE(cycled["success"].getBool());
   auto level = cycled["data"]["level"].getString();
   EXPECT_NE(level, "off");   // 与初始 off 不同
   EXPECT_EQ(RpcHarness::response(frames, "get_state")
                ["data"]["thinkingLevel"].getString(), level);
}

// == rpc.test.ts: "should get available thinking levels" =================
TEST(RpcMode, ShouldGetAvailableThinkingLevels) {
   auto frames = RpcHarness::run(
      R"({"type":"get_available_thinking_levels"})" "\n"
      R"({"type":"cycle_thinking_level"})" "\n");

   auto levels = RpcHarness::response(frames, "get_available_thinking_levels")
                    ["data"]["levels"];
   ASSERT_GT(levels.size(), 0u);

   // 当前 level 必须在可选列表里
   auto cur = RpcHarness::response(frames, "cycle_thinking_level")
                 ["data"]["level"].getString();
   bool found = false;
   for (auto const& l : levels)
      if (l.getString() == cur) found = true;
   EXPECT_TRUE(found);
}

// == rpc.test.ts: "should get available models" ==========================
TEST(RpcMode, ShouldGetAvailableModels) {
   auto frames = RpcHarness::run(R"({"type":"get_available_models"})" "\n");
   auto models = RpcHarness::response(frames, "get_available_models")
                    ["data"]["models"];
   ASSERT_GT(models.size(), 0u);
   for (auto const& m : models) {
      EXPECT_TRUE(m.count("provider"));
      EXPECT_TRUE(m.count("id"));
      EXPECT_GT(m["contextWindow"].getInt(), 0);
      EXPECT_TRUE(m["reasoning"].isBool());
   }
}

// == rpc.test.ts: "should get session stats" =============================
TEST(RpcMode, ShouldGetSessionStats) {
   auto frames = RpcHarness::run(
      R"({"type":"prompt","message":"Hello"})" "\n"
      R"({"type":"get_session_stats"})" "\n");
   auto d = RpcHarness::response(frames, "get_session_stats")["data"];
   ASSERT_TRUE(d.isObject());
   EXPECT_TRUE(d.count("sessionFile"));
   EXPECT_TRUE(d.count("sessionId"));
   EXPECT_GE(d["userMessages"].getInt(), 1);
   EXPECT_GE(d["assistantMessages"].getInt(), 1);
   EXPECT_EQ(d["toolCalls"].getInt(), 1);
   EXPECT_EQ(d["toolResults"].getInt(), 1);
   EXPECT_EQ(d["totalMessages"].getInt(), 4);
}

// == rpc.test.ts: "should create new session" ============================
TEST(RpcMode, ShouldCreateNewSession) {
   auto frames = RpcHarness::run(
      R"({"type":"prompt","message":"Hello"})" "\n"
      R"({"type":"new_session"})" "\n"
      R"({"type":"get_state"})" "\n");
   auto resp = RpcHarness::response(frames, "new_session");
   EXPECT_TRUE(resp["success"].getBool());
   EXPECT_FALSE(resp["data"]["cancelled"].getBool());
   EXPECT_EQ(RpcHarness::response(frames, "get_state")
                ["data"]["messageCount"].getInt(), 0);
}

// == rpc.test.ts: "should export to HTML" ================================
TEST(RpcMode, ShouldExportToHtml) {
   auto path = std::string{"/tmp/cpp-pi-it-export-"}
             + std::to_string(::getpid()) + ".html";
   auto frames = RpcHarness::run(
      R"({"type":"prompt","message":"Hello"})" "\n"
      R"({"type":"export_html","outputPath":")" + path + "\"}\n");

   auto resp = RpcHarness::response(frames, "export_html");
   ASSERT_TRUE(resp["success"].getBool());
   EXPECT_EQ(resp["data"]["path"].getString(), path);
   EXPECT_TRUE(std::filesystem::exists(path));
   std::filesystem::remove(path);
}

// == rpc.test.ts: "should get last assistant text" =======================
TEST(RpcMode, ShouldGetLastAssistantText) {
   auto frames = RpcHarness::run(
      R"({"type":"get_last_assistant_text"})" "\n"
      R"({"type":"prompt","message":"test123"})" "\n"
      R"({"type":"get_last_assistant_text"})" "\n");

   auto resps = frames;
   int n = 0;
   for (auto const& f : resps) {
      if (f["type"].getString() == "response" &&
          f["command"].getString() == "get_last_assistant_text") {
         ++n;
         if (n == 1) EXPECT_TRUE(f["data"]["text"].isNull());      // 初始 null
         if (n == 2) {
            EXPECT_TRUE(f["data"]["text"].isString());
            EXPECT_EQ(f["data"]["text"].getString(), "工具返回: echo: test123");
         }
      }
   }
   EXPECT_EQ(n, 2);
}

// == rpc.test.ts: "should execute bash command" ==========================
TEST(RpcMode, ShouldExecuteBashCommand) {
   auto frames = RpcHarness::run(
      R"({"type":"bash","command":"echo hello"})" "\n");
   auto d = RpcHarness::response(frames, "bash")["data"];
   ASSERT_TRUE(d.isObject());
   EXPECT_EQ(d["output"].getString(), "hello\n");
   EXPECT_EQ(d["exitCode"].getInt(), 0);
   EXPECT_FALSE(d["cancelled"].getBool());
   EXPECT_FALSE(d["truncated"].getBool());
}

// == rpc.test.ts: "should set and get session name" ======================
TEST(RpcMode, ShouldSetAndGetSessionName) {
   auto frames = RpcHarness::run(
      R"({"type":"set_session_name","name":"my-test-session"})" "\n"
      R"({"type":"get_state"})" "\n");
   EXPECT_TRUE(RpcHarness::response(frames, "set_session_name")
                  ["success"].getBool());
   EXPECT_EQ(RpcHarness::response(frames, "get_state")
                ["data"]["sessionName"].getString(), "my-test-session");
}

// == rpc.test.ts: "should get session entries with since cursor" =========
TEST(RpcMode, ShouldGetSessionEntriesWithSinceCursor) {
   auto frames = RpcHarness::run(
      R"({"type":"prompt","message":"ok"})" "\n"
      R"({"type":"get_entries"})" "\n");

   auto d = RpcHarness::response(frames, "get_entries")["data"];
   ASSERT_TRUE(d.isObject());
   auto const& entries = d["entries"];
   ASSERT_GE(entries.size(), 2u);   // user + assistant (含 tool 消息共 4)
   for (auto const& e : entries) EXPECT_TRUE(e.count("id"));
   EXPECT_EQ(d["leafId"].getString(),
             entries[entries.size() - 1]["id"].getString());

   // since 游标: 只要首个 entry 之后的
   auto frames2 = RpcHarness::run(
      R"({"type":"prompt","message":"ok"})" "\n"
      R"({"type":"get_entries","since":")"
         + entries[0]["id"].getString() + "\"}\n");
   auto d2 = RpcHarness::response(frames2, "get_entries")["data"];
   ASSERT_EQ(d2["entries"].size(), entries.size() - 1);
   EXPECT_EQ(d2["entries"][0]["id"].getString(),
             entries[1]["id"].getString());
   EXPECT_EQ(d2["leafId"].getString(), d["leafId"].getString());
}

// == rpc.test.ts: 未知 since id 是错误响应 ===============================
TEST(RpcMode, UnknownSinceIdIsErrorResponse) {
   auto frames = RpcHarness::run(
      R"({"type":"prompt","message":"ok"})" "\n"
      R"({"type":"get_entries","since":"nonexistent-id"})" "\n");
   auto resp = RpcHarness::response(frames, "get_entries");
   EXPECT_FALSE(resp["success"].getBool());
   EXPECT_EQ(resp["error"].getString(), "Entry not found");
}

// == rpc.test.ts: "should get session tree" ==============================
TEST(RpcMode, ShouldGetSessionTree) {
   auto frames = RpcHarness::run(
      R"({"type":"prompt","message":"ok"})" "\n"
      R"({"type":"get_entries"})" "\n"
      R"({"type":"get_tree"})" "\n");

   auto entries = RpcHarness::response(frames, "get_entries")
                     ["data"]["entries"];
   auto leafId  = RpcHarness::response(frames, "get_entries")
                     ["data"]["leafId"].getString();
   auto tree    = RpcHarness::response(frames, "get_tree")["data"]["tree"];
   auto treeLeaf = RpcHarness::response(frames, "get_tree")
                      ["data"]["leafId"].getString();
   EXPECT_EQ(treeLeaf, leafId);

   // 单根, 链式下钻 id 序与 entries 一致
   ASSERT_EQ(tree.size(), 1u);
   std::vector<std::string> chainIds;
   auto node = tree[0];
   while (true) {
      chainIds.push_back(node["entry"]["id"].getString());
      auto next = node["children"].size() ? folly::dynamic{node["children"][0]}
                                        : folly::dynamic{};
      ASSERT_TRUE(node["children"].size() <= 1u);
      if (next.isNull()) break;
      node = std::move(next);
   }
   ASSERT_EQ(chainIds.size(), entries.size());
   for (size_t i = 0; i < entries.size(); ++i)
      EXPECT_EQ(chainIds[i], entries[i]["id"].getString());
}

// == rpc.test.ts: "should handle manual compaction" ======================
TEST(RpcMode, ShouldHandleManualCompaction) {
   auto frames = RpcHarness::run(
      R"({"type":"prompt","message":"Say hello"})" "\n"
      R"({"type":"compact"})" "\n");
   auto d = RpcHarness::response(frames, "compact")["data"];
   ASSERT_TRUE(d.isObject());
   EXPECT_TRUE(d["summary"].isString());
   EXPECT_GT(d["tokensBefore"].getInt(), 0);
}

// == rpc-prompt-response-semantics: steer/follow_up -> queued + queue_update
TEST(RpcPromptSemantics, SteerAndFollowUpAreQueued) {
   auto frames = RpcHarness::run(
      R"({"type":"steer","message":"A"})" "\n"
      R"({"type":"follow_up","message":"B"})" "\n");

   for (auto c : {"steer", "follow_up"}) {
      auto resp = RpcHarness::response(frames, c);
      EXPECT_TRUE(resp["success"].getBool());
      EXPECT_EQ(resp["data"]["disposition"].getString(), "queued");
   }
   // queue_update 帧: steering=["A"], followUp=["B"] (后者先空后有)
   auto qu = RpcHarness::ofType(frames, "queue_update");
   ASSERT_GE(qu.size(), 2u);
   EXPECT_EQ(qu[0]["steering"].size(), 1u);
   EXPECT_EQ(qu[0]["steering"][0].getString(), "A");
   EXPECT_EQ(qu[1]["followUp"].size(), 1u);
   EXPECT_EQ(qu[1]["followUp"][0].getString(), "B");
}

// == rpc-prompt-response-semantics: 队列在 prompt 完成后排干 ==============
TEST(RpcPromptSemantics, QueuedMessagesDrainAfterRun) {
   auto frames = RpcHarness::run(
      R"({"type":"steer","message":"steer-msg"})" "\n"
      R"({"type":"follow_up","message":"follow-msg"})" "\n"
      R"({"type":"prompt","message":"go"})" "\n");

   // prompt 自己 + steering + followUp 各一轮 run -> 3 个 agent_end
   EXPECT_EQ(RpcHarness::ofType(frames, "agent_end").size(), 3u);
   // 末态队列已空
   auto qu = RpcHarness::ofType(frames, "queue_update");
   EXPECT_EQ(qu.back()["steering"].size(), 0u);
   EXPECT_EQ(qu.back()["followUp"].size(), 0u);
}

// == rpc-prompt-response-semantics: clear_queue 返回并清空 ===============
TEST(RpcPromptSemantics, ClearQueueReturnsAndClears) {
   auto frames = RpcHarness::run(
      R"({"type":"steer","message":"Change direction"})" "\n"
      R"({"type":"follow_up","message":"Summarize when finished"})" "\n"
      R"({"type":"clear_queue"})" "\n");

   auto d = RpcHarness::response(frames, "clear_queue")["data"];
   ASSERT_TRUE(d.isObject());
   ASSERT_EQ(d["steering"].size(), 1u);
   EXPECT_EQ(d["steering"][0].getString(), "Change direction");
   ASSERT_EQ(d["followUp"].size(), 1u);
   EXPECT_EQ(d["followUp"][0].getString(), "Summarize when finished");
}

// == 5868-rpc-unknown-command-id: 未知命令也回 id ========================
TEST(RpcMode, UnknownCommandEchoesId) {
   auto frames = RpcHarness::run(
      R"({"id":"u1","type":"totally_made_up"})" "\n");
   auto resp = RpcHarness::response(frames, "totally_made_up");
   EXPECT_FALSE(resp["success"].getBool());
   EXPECT_EQ(resp["id"].getString(), "u1");
   EXPECT_EQ(resp["error"].getString(), "unsupported command");
}

// == rpc-jsonl.test.ts: 行内 U+2028/U+2029 不被当换行 ====================
TEST(RpcJsonl, UnicodeLineSeparatorsInsidePayload) {
   // U+2028 LINE SEPARATOR 直接出现在 JSON 字符串值里
   auto frames = RpcHarness::run(
      "{\"type\":\"prompt\",\"message\":\"a\xE2\x80\xA8"
      "b\xE2\x80\xA9" "c\"}\n");
   auto ends = RpcHarness::ofType(frames, "message_end");
   ASSERT_GE(ends.size(), 1u);
   auto content = ends[0]["message"]["content"].getString();
   EXPECT_TRUE(content.find("a\xE2\x80\xA8" "b\xE2\x80\xA9" "c")
               != std::string::npos);
}

// == rpc-jsonl.test.ts: CRLF 分隔输入 ====================================
TEST(RpcJsonl, CrlfDelimitedInput) {
   auto frames = RpcHarness::run(
      "{\"type\":\"get_state\"}\r\n{\"type\":\"get_messages\"}\r\n");
   EXPECT_TRUE(RpcHarness::response(frames, "get_state")["success"].getBool());
   EXPECT_TRUE(RpcHarness::response(frames, "get_messages")["success"].getBool());
}

// == rpc-jsonl.test.ts: 末行无换行也处理 =================================
TEST(RpcJsonl, FinalLineWithoutTrailingLf) {
   auto frames = RpcHarness::run("{\"type\":\"get_state\"}");   // 无 \n
   EXPECT_TRUE(RpcHarness::response(frames, "get_state")["success"].getBool());
}

// == 错误路径 =============================================================
TEST(RpcMode, MalformedJsonIsErrorResponse) {
   auto frames = RpcHarness::run("this is not json\n");
   ASSERT_EQ(frames.size(), 1u);
   EXPECT_EQ(frames[0]["type"].getString(), "response");
   EXPECT_FALSE(frames[0]["success"].getBool());
   EXPECT_TRUE(frames[0]["error"].getString().find("invalid json") !=
               std::string::npos);
}

TEST(RpcMode, MissingTypeIsErrorResponse) {
   auto frames = RpcHarness::run(R"({"foo":1})" "\n");
   auto resp = RpcHarness::response(frames, "unknown");
   ASSERT_TRUE(resp.isObject());
   EXPECT_FALSE(resp["success"].getBool());
}

TEST(RpcMode, AbortVariantsAreAcknowledged) {
   auto frames = RpcHarness::run(
      R"({"type":"abort"})" "\n"
      R"({"type":"abort_bash"})" "\n"
      R"({"type":"abort_retry"})" "\n");
   for (auto c : {"abort", "abort_bash", "abort_retry"})
      EXPECT_TRUE(RpcHarness::response(frames, c)["success"].getBool()) << c;
}

TEST(RpcMode, UnsupportedCommandsAreErrorResponses) {
   auto frames = RpcHarness::run(
      R"({"type":"fork","entryId":"e-0"})" "\n"
      R"({"type":"switch_session","sessionPath":"/tmp/x"})" "\n"
      R"({"type":"get_fork_messages"})" "\n");
   for (auto c : {"fork", "switch_session", "get_fork_messages"}) {
      auto resp = RpcHarness::response(frames, c);
      EXPECT_FALSE(resp["success"].getBool()) << c;
      EXPECT_EQ(resp["error"].getString(), "unsupported command") << c;
   }
}

// == 工具路径变体 (mock 前缀路由): bash 工具 + 未知工具错误分支 ============
TEST(RpcMode, BashToolCallViaPrompt) {
   auto frames = RpcHarness::run(
      R"({"type":"prompt","message":"bash:echo tool-bash"})" "\n");
   auto tr = RpcHarness::ofType(frames, "message_end");
   // toolResult 是第 3 条消息
   auto const& m = tr[2]["message"];
   EXPECT_EQ(m["role"].getString(), "toolResult");
   EXPECT_EQ(m["toolName"].getString(), "bash");
   EXPECT_EQ(m["content"][0]["text"].getString(), "tool-bash\n");
   EXPECT_FALSE(m["isError"].getBool());
}

TEST(RpcMode, UnknownToolCallIsErrorResult) {
   auto frames = RpcHarness::run(
      R"({"type":"prompt","message":"unknown:x"})" "\n");
   auto tr = RpcHarness::ofType(frames, "message_end");
   auto const& m = tr[2]["message"];
   EXPECT_EQ(m["role"].getString(), "toolResult");
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_TRUE(m["content"][0]["text"].getString()
                  .find("unknown tool") != std::string::npos);
}

// == CLI 模式 =============================================================
TEST(RpcMode, CliModeProcessesQueue) {
   auto base = std::string{"/tmp/cpp-pi-it-cli-"} + std::to_string(::getpid());
   auto cmd  = std::string{"printf 'hello\\n/q\\n' | "} + CPP_PI_BIN
             + " --provider mock > " + base + ".out 2>/dev/null";
   ASSERT_EQ(std::system(cmd.c_str()), 0);

   std::ifstream f{base + ".out"};
   std::string out{std::istreambuf_iterator<char>{f}, {}};
   EXPECT_TRUE(out.find("echo: hello") != std::string::npos);
   EXPECT_TRUE(out.find("工具返回") != std::string::npos);
}
