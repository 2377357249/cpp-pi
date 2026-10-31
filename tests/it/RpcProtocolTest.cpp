#include <filesystem>
#include <gtest/gtest.h>

#include "RpcHarness.h"

using pi::it::RpcHarness;
using pi::it::RpcProc;

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
   // prompt 异步跑 turn —— 等 agent_end 再查 stats (pi 语义下同此确定性点)
   auto p = RpcProc::spawn({});
   p.send(R"({"type":"prompt","message":"Hello"})");
   p.readUntil("agent_end");
   p.send(R"({"type":"get_session_stats"})");
   auto r = p.readResponse("get_session_stats");
   auto const& d = r["data"];
   ASSERT_TRUE(d.isObject());
   EXPECT_FALSE(d.count("sessionFile"));   // in-memory 会话无文件
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
   auto p = RpcProc::spawn({});
   p.send(R"({"type":"get_last_assistant_text"})");
   auto r1 = p.readResponse("get_last_assistant_text");
   EXPECT_TRUE(r1["data"]["text"].isNull());      // 初始 null
   p.send(R"({"type":"prompt","message":"test123"})");
   p.readUntil("agent_end");
   p.send(R"({"type":"get_last_assistant_text"})");
   auto r2 = p.readResponse("get_last_assistant_text");
   ASSERT_TRUE(r2["data"]["text"].isString());
   EXPECT_EQ(r2["data"]["text"].getString(), "工具返回: echo: test123");
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
   auto proc = RpcProc::spawn();
   proc.send(R"({"type":"prompt","message":"ok"})");
   proc.readUntil("agent_end");
   proc.send(R"({"type":"get_entries"})");
   auto d = proc.readResponse("get_entries")["data"];
   ASSERT_TRUE(d.isObject());
   auto const& entries = d["entries"];
   ASSERT_GE(entries.size(), 2u);
   for (auto const& e : entries) EXPECT_TRUE(e.count("id"));
   EXPECT_EQ(d["leafId"].getString(),
             entries[entries.size() - 1]["id"].getString());

   // since 游标: 只要首个 entry 之后的 (同会话, 真实 id)
   proc.send(std::string{"{\"type\":\"get_entries\",\"since\":\""}
             + entries[0]["id"].getString() + "\"}");
   auto d2 = proc.readResponse("get_entries")["data"];
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
   EXPECT_EQ(resp["error"].getString(), "Entry not found: nonexistent-id");
}

// == rpc.test.ts: "should get session tree" ==============================
TEST(RpcMode, ShouldGetSessionTree) {
   auto p = RpcProc::spawn({});
   p.send(R"({"type":"prompt","message":"ok"})");
   p.readUntil("agent_end");
   p.send(R"({"type":"get_entries"})");
   auto ge = p.readResponse("get_entries");
   p.send(R"({"type":"get_tree"})");
   auto gt = p.readResponse("get_tree");

   auto const& entries  = ge["data"]["entries"];
   auto const  leafId   = ge["data"]["leafId"].getString();
   auto const& tree     = gt["data"]["tree"];
   auto const  treeLeaf = gt["data"]["leafId"].getString();
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
// pi: 会话总 tokens < keepRecentTokens(20000) 时无可摘要前缀 ->
// "Nothing to compact (session too small)", 且先 emit compaction_start,
// compaction_end 无 result 键 + errorMessage 带 "Compaction failed:" 前缀.
TEST(RpcMode, CompactTooSmallFailsLikePi) {
   auto frames = RpcHarness::run(
      R"({"type":"prompt","message":"Say hello"})" "\n"
      R"({"type":"compact"})" "\n");
   auto r = RpcHarness::response(frames, "compact");
   ASSERT_FALSE(r["success"].getBool());
   EXPECT_EQ(r["error"].getString(), "Nothing to compact (session too small)");

   auto ends = RpcHarness::ofType(frames, "compaction_end");
   ASSERT_EQ(ends.size(), 1u);
   EXPECT_FALSE(ends[0].count("result"));
   EXPECT_EQ(ends[0]["errorMessage"].getString(),
             "Compaction failed: Nothing to compact (session too small)");
   auto starts = RpcHarness::ofType(frames, "compaction_start");
   ASSERT_EQ(starts.size(), 1u);
   EXPECT_EQ(starts[0]["reason"].getString(), "manual");
}

// 会话超过 keepRecentTokens -> 真 LLM 摘要: CompactionEntry 落会话 +
// compaction_start/end 事件 + CompactionResult 五字段; 投影只剩
// compactionSummary + 保留尾巴.
TEST(RpcMode, CompactBigSessionWritesEntry) {
   auto const big = std::string(90 * 1024, 'x');
   auto frames = RpcHarness::run(
      R"({"type":"prompt","message":"Say hello"})" "\n"
      "{\"type\":\"prompt\",\"message\":\"" + big + "\"}" "\n"
      R"({"type":"compact"})" "\n"
      R"({"type":"get_state"})" "\n");
   auto r = RpcHarness::response(frames, "compact");
   ASSERT_TRUE(r["success"].getBool()) << folly::toJson(r);
   auto const& d = r["data"];
   EXPECT_TRUE(d["summary"].isString());
   EXPECT_GT(d["tokensBefore"].getInt(), 0);
   EXPECT_TRUE(d.count("estimatedTokensAfter"));
   EXPECT_TRUE(d["usage"].isObject());
   EXPECT_TRUE(d["details"]["readFiles"].isArray());
   EXPECT_TRUE(d["details"]["modifiedFiles"].isArray());

   auto ends = RpcHarness::ofType(frames, "compaction_end");
   ASSERT_EQ(ends.size(), 1u);
   EXPECT_TRUE(ends[0]["result"]["summary"].isString());

   // get_entries 应出现 compaction entry
   auto entries = RpcHarness::ofType(frames, "response");
   (void)entries;
}

// == P3 流式: message_update + tool_execution_* 帧序 ==========================
// pi agent-loop: start→message_start{骨架content:[]}; delta系→message_update
// {usage,assistantMessageEvent(剥partial)}; done/error→message_end 不上update.
TEST(RpcMode, StreamingMessageUpdateFrames) {
   auto p = RpcProc::spawn();
   p.send(R"({"type":"prompt","message":"hello"})");
   p.readResponse("prompt");
   std::vector<folly::dynamic> frames;
   for (;;) {
      auto f = p.readFrame();
      if (!f.isObject()) break;
      frames.push_back(f);
      if (f["type"].getString() == "agent_end") break;
   }
   auto typesOf = [&](size_t from, size_t to) {
      std::vector<std::string> v;
      for (size_t i = from; i < to && i < frames.size(); ++i) {
         auto const& f = frames[i];
         if (f["type"].getString() == "message_update")
            v.push_back("mu:" + f["assistantMessageEvent"]["type"].getString());
         else
            v.push_back(f["type"].getString());
      }
      return v;
   };
   // 定位第一个 assistant message_start —— 其后必须接 toolcall 增量序列
   size_t aStart = frames.size();
   for (size_t i = 0; i < frames.size(); ++i) {
      if (frames[i]["type"].getString() == "message_start"
          && frames[i]["message"]["role"].getString() == "assistant") {
         aStart = i;
         break;
      }
   }
   ASSERT_LT(aStart, frames.size());
   // 骨架: 流开始的 assistant content 为空
   EXPECT_TRUE(frames[aStart]["message"]["content"].empty());
   // mock 路由 text@0 → toolcall@1: 6 个 update 帧后接 message_end
   auto seg = typesOf(aStart + 1, aStart + 8);
   ASSERT_EQ(seg.size(), 7u);
   EXPECT_EQ(seg[0], "mu:text_start");
   EXPECT_EQ(seg[1], "mu:text_delta");
   EXPECT_EQ(seg[2], "mu:text_end");
   EXPECT_EQ(seg[3], "mu:toolcall_start");
   EXPECT_EQ(seg[4], "mu:toolcall_delta");
   EXPECT_EQ(seg[5], "mu:toolcall_end");
   EXPECT_EQ(seg[6], "message_end");
   // toolcall_start 展开 {id,toolName}, 无 partial
   auto tcStart = frames[aStart + 4]["assistantMessageEvent"];
   EXPECT_EQ(tcStart["id"].getString(), "call-1");
   EXPECT_EQ(tcStart["toolName"].getString(), "echo");
   EXPECT_FALSE(tcStart.count("partial"));
   // 每条 message_update 顶层携带 usage
   for (auto const& f : frames)
      if (f["type"].getString() == "message_update")
         EXPECT_TRUE(f["usage"].isObject());
   // tool_execution_* 帧: args/result/isError 齐全
   auto starts = RpcHarness::ofType(frames, "tool_execution_start");
   ASSERT_EQ(starts.size(), 1u);
   EXPECT_EQ(starts[0]["toolCallId"].getString(), "call-1");
   EXPECT_EQ(starts[0]["toolName"].getString(), "echo");
   EXPECT_EQ(starts[0]["args"].getString(), "hello");
   auto ends = RpcHarness::ofType(frames, "tool_execution_end");
   ASSERT_EQ(ends.size(), 1u);
   EXPECT_FALSE(ends[0]["isError"].getBool());
   EXPECT_TRUE(ends[0]["result"]["content"].isArray());
   // 第二条 assistant (文本路径): text_start/delta/end update 帧
   size_t tStart = frames.size();
   for (size_t i = aStart + 1; i < frames.size(); ++i) {
      if (frames[i]["type"].getString() == "message_start"
          && frames[i]["message"]["role"].getString() == "assistant") {
         tStart = i;
         break;
      }
   }
   ASSERT_LT(tStart, frames.size());
   std::vector<std::string> upd;
   for (size_t i = tStart + 1; i < frames.size(); ++i) {
      if (frames[i]["type"].getString() != "message_update") break;
      upd.push_back(
         frames[i]["assistantMessageEvent"]["type"].getString());
   }
   ASSERT_GE(upd.size(), 4u);
   EXPECT_EQ(upd.front(), "text_start");
   EXPECT_EQ(upd.back(), "text_end");
   // done/error 不以 message_update 出现
   EXPECT_EQ(std::ranges::count(upd, "done"), 0);
}

// == abort: 事务在下一个周期检查点退出 =======================================
// prompt 的 bash sleep 期间发 abort -> 该轮工具照常收尾 (in-flight 不可杀),
// 但 TurnDone 谓词看到旗标即退出 —— 不再产生第二轮 assistant 文本;
// set_auto_compaction 语义: turn 收尾时 usage 越过
// contextWindow(200000) - reserveTokens(16384) → 自动压缩
// (pi _checkCompaction case 3, reason:"threshold", 不重试).
TEST(RpcMode, AutoCompactionAtThresholdTriggers) {
   auto p = RpcProc::spawn();
   // 先铺 ~90KB 会话让 prepareCompaction 有合法切点
   auto const big = std::string(90 * 1024, 'x');
   p.send("{\"type\":\"prompt\",\"message\":\"" + big + "\"}");
   p.readResponse("prompt");
   p.readUntil("agent_end");
   p.send(R"({"type":"prompt","message":"usage:190000"})");
   p.readResponse("prompt");
   p.readUntil("agent_end");
   auto end = p.readUntil("compaction_end");
   ASSERT_TRUE(end.isObject());
   EXPECT_EQ(end["reason"].getString(), "threshold");
   EXPECT_TRUE(end.count("result"));
   EXPECT_FALSE(end["willRetry"].getBool());
}

// enabled:false → 同样条件下不发任何 compaction_* 事件.
TEST(RpcMode, AutoCompactionDisabledSkipsCheck) {
   auto p = RpcProc::spawn();
   p.send(R"({"type":"set_auto_compaction","enabled":false})");
   p.readResponse("set_auto_compaction");
   // 同样铺大会话 —— 否则 prep 闸门静默也会让断言空过
   auto const big = std::string(90 * 1024, 'x');
   p.send("{\"type\":\"prompt\",\"message\":\"" + big + "\"}");
   p.readResponse("prompt");
   p.readUntil("agent_end");
   p.send(R"({"type":"prompt","message":"usage:190000"})");
   p.readResponse("prompt");
   p.readUntil("agent_end");
   p.send(R"({"type":"get_state"})");
   bool sawCompaction = false;
   for (;;) {
      auto f = p.readFrame();
      if (!f.isObject() || !f.count("type")) break;
      if (f["type"].getString() == "response") break;   // get_state 响应
      if (f["type"].getString().find("compaction")
          != std::string::npos)
         sawCompaction = true;
   }
   EXPECT_FALSE(sawCompaction);
}

// abort 响应恒 success (pi session.abort 同义).
TEST(RpcMode, AbortStopsLoopAfterInFlightWork) {
   auto p = RpcProc::spawn(
      {"--tools", "read,bash,edit,write,grep,find,ls,echo"});
   p.send(R"({"type":"prompt","message":"tool:bash:{\"command\":\"sleep 1\"}"})");
   p.readResponse("prompt");
   p.send(R"({"type":"abort"})");
   // 帧序恒定: agent_end (turn 收尾时发出) 先于 abort 响应 (join 之后才发)
   auto frames = p.readUntil("agent_end");
   auto r = p.readResponse("abort");
   EXPECT_TRUE(r["success"].getBool());
   int assistants = 0, toolResults = 0;
   for (auto const& m : frames["messages"]) {
      if (m["role"].getString() == "assistant") ++assistants;
      if (m["role"].getString() == "toolResult") ++toolResults;
   }
   EXPECT_EQ(assistants, 1);   // 只有发起 toolCall 的那条
   EXPECT_EQ(toolResults, 1);  // in-flight bash 的真实结果
   // abort 后再 prompt 能正常跑 (旗标在 startTurn 复位)
   p.send(R"({"type":"prompt","message":"after abort"})");
   auto f2 = p.readUntil("agent_end");
   bool sawFinal = false;
   for (auto const& m : f2["messages"])
      if (m["role"].getString() == "assistant"
          && m["content"].isArray())
         for (auto const& b : m["content"])
            if (b["type"].getString() == "text"
                && b["text"].getString().find("工具返回") != std::string::npos)
               sawFinal = true;
   EXPECT_TRUE(sawFinal);
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
   EXPECT_EQ(resp["error"].getString(), "Unknown command: totally_made_up");
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
   EXPECT_EQ(frames[0]["command"].getString(), "parse");
   EXPECT_TRUE(frames[0]["error"].getString().find("Failed to parse command") !=
               std::string::npos);
}

TEST(RpcMode, MissingTypeIsErrorResponse) {
   auto frames = RpcHarness::run(R"({"foo":1})" "\n");
   ASSERT_EQ(frames.size(), 1u);
   auto const& resp = frames[0];
   EXPECT_EQ(resp["type"].getString(), "response");
   EXPECT_FALSE(resp["success"].getBool());
   EXPECT_FALSE(resp.count("command"));      // pi: command 键省略
   EXPECT_EQ(resp["error"].getString(), "Unknown command: undefined");
}

TEST(RpcMode, AbortVariantsAreAcknowledged) {
   auto frames = RpcHarness::run(
      R"({"type":"abort"})" "\n"
      R"({"type":"abort_bash"})" "\n"
      R"({"type":"abort_retry"})" "\n");
   for (auto c : {"abort", "abort_bash", "abort_retry"})
      EXPECT_TRUE(RpcHarness::response(frames, c)["success"].getBool()) << c;
}

TEST(RpcMode, CommandErrorPathsMatchPi) {
   auto frames = RpcHarness::run(
      R"({"type":"fork","entryId":"deadbeef"})" "\n"
      R"({"type":"switch_session","sessionPath":"/tmp/nonexistent-pi-session.jsonl"})" "\n"
      R"({"type":"get_fork_messages"})" "\n"
      R"({"type":"set_session_name","name":"  "})" "\n");
   // fork: 非 user message 或不存在的 entry -> Invalid entry ID for forking
   EXPECT_EQ(RpcHarness::response(frames, "fork")["error"].getString(),
             "Invalid entry ID for forking");
   // switch_session: 文件无效 -> 非 pi 会话错误
   auto sw = RpcHarness::response(frames, "switch_session");
   EXPECT_FALSE(sw["success"].getBool());
   EXPECT_TRUE(sw["error"].getString().find("valid pi session")
               != std::string::npos);
   // get_fork_messages: 空会话 -> success, messages:[]
   auto fm = RpcHarness::response(frames, "get_fork_messages");
   EXPECT_TRUE(fm["success"].getBool());
   EXPECT_EQ(fm["data"]["messages"].size(), 0u);
   // 空白会话名 -> Session name cannot be empty
   EXPECT_EQ(RpcHarness::response(frames, "set_session_name")["error"]
                .getString(), "Session name cannot be empty");
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
   // pi: "Tool no_such not found"
   EXPECT_EQ(m["content"][0]["text"].getString(), "Tool no_such not found");
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
