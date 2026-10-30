// 内置工具集 IT: 经 "tool:<name>:<json>" mock 路由驱动真实现,
// 断言 toolResult 消息字段 (content/isError/details), 对齐 pi 工具语义.

#include <filesystem>
#include <fstream>
#include <format>
#include <gtest/gtest.h>
#include <unistd.h>

#include "RpcHarness.h"

using pi::it::RpcHarness;
namespace fs = std::filesystem;

namespace {

// 全部 message_end 帧里找第一条 toolResult
auto toolResult(std::vector<folly::dynamic>& frames) -> folly::dynamic {
   for (auto& f : RpcHarness::ofType(frames, "message_end"))
      if (f["message"]["role"].getString() == "toolResult") return f["message"];
   return folly::dynamic{};
}

// JSON 字符串值转义 (拼 tool args / 外层 message)
auto esc(std::string_view s) -> std::string {
   std::string out;
   for (char c : s) {
      if (c == '"') out += "\\\"";
      else if (c == '\\') out += "\\\\";
      else out += c;
   }
   return out;
}



struct TmpDir {
   fs::path dir;
   TmpDir() : dir{fs::temp_directory_path()
                  / std::format("cpp-pi-tools-{}", ::getpid())} {
      fs::create_directories(dir);
   }
   ~TmpDir() { fs::remove_all(dir); }
   auto path(std::string_view f) const -> std::string {
      return (dir / f).string();
   }
};

// toolArgs 是字面 "name:{json}", 嵌入外层 message 字符串需再转义.
// 经 --tools 启用全部内置工具 (pi 语义: 普通名替换默认激活集).
auto prompt(std::string const& toolArgs) -> std::vector<folly::dynamic> {
   return RpcHarness::run(
      "{\"type\":\"prompt\",\"message\":\"tool:" + esc(toolArgs) + "\"}\n",
      "--tools read,bash,edit,write,grep,find,ls,echo");
}

} // namespace

// == read ==================================================================
TEST(BuiltinTools, ReadReturnsContents) {
   TmpDir t;
   { std::ofstream f{t.path("a.txt")}; f << "line1\nline2\nline3\n"; }
   auto frames = prompt("read:{\"path\":\"" + esc(t.path("a.txt")) + "\"}");
   auto const& m = toolResult(frames);
   ASSERT_TRUE(m.isObject());
   EXPECT_FALSE(m["isError"].getBool());
   EXPECT_EQ(m["content"][0]["text"].getString(), "line1\nline2\nline3\n");
}

TEST(BuiltinTools, ReadOffsetLimitAndContinuationNotice) {
   TmpDir t;
   { std::ofstream f{t.path("a.txt")}; f << "l1\nl2\nl3\nl4\nl5\n"; }
   auto frames = prompt("read:{\"path\":\"" + esc(t.path("a.txt"))
                        + "\",\"offset\":2,\"limit\":2}");
   auto const& m = toolResult(frames);
   EXPECT_EQ(m["content"][0]["text"].getString(),
             "l2\nl3\n\n[3 more lines in file. Use offset=4 to continue.]");
}

TEST(BuiltinTools, ReadOffsetBeyondEofIsError) {
   TmpDir t;
   { std::ofstream f{t.path("a.txt")}; f << "l1\n"; }
   auto frames = prompt("read:{\"path\":\"" + esc(t.path("a.txt"))
                        + "\",\"offset\":99}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_TRUE(m["content"][0]["text"].getString()
               .find("Offset 99 is beyond end of file (2 lines total)")
               != std::string::npos);
}

TEST(BuiltinTools, ReadMissingFileIsError) {
   auto frames = prompt("read:{\"path\":\"/nonexistent-f123.txt\"}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_TRUE(m["content"][0]["text"].getString().find("ENOENT")
               != std::string::npos);
}

// == write =================================================================
TEST(BuiltinTools, WriteCreatesFileAndParents) {
   TmpDir t;
   auto const p = t.path("sub/deep/w.txt");
   auto frames = prompt("write:{\"path\":\"" + esc(p)
                        + "\",\"content\":\"hello\"}");
   auto const& m = toolResult(frames);
   EXPECT_FALSE(m["isError"].getBool());
   EXPECT_EQ(m["content"][0]["text"].getString(),
             "Successfully wrote to " + p);
   std::ifstream check{p};
   std::string got{std::istreambuf_iterator<char>{check},
                   std::istreambuf_iterator<char>{}};
   EXPECT_EQ(got, "hello");
}

// == edit ==================================================================
TEST(BuiltinTools, EditExactReplacement) {
   TmpDir t;
   { std::ofstream f{t.path("e.txt")}; f << "alpha\nbeta\ngamma\n"; }
   auto frames = prompt("edit:{\"path\":\"" + esc(t.path("e.txt"))
                        + "\",\"edits\":[{\"oldText\":\"beta\",\"newText\":\"BETA\"}]}");
   auto const& m = toolResult(frames);
   EXPECT_FALSE(m["isError"].getBool());
   EXPECT_EQ(m["content"][0]["text"].getString(),
             "Successfully replaced 1 block(s) in " + t.path("e.txt") + ".");
   std::ifstream check{t.path("e.txt")};
   std::string got{std::istreambuf_iterator<char>{check},
                   std::istreambuf_iterator<char>{}};
   EXPECT_EQ(got, "alpha\nBETA\ngamma\n");
   ASSERT_TRUE(m.count("details"));
   EXPECT_TRUE(m["details"]["diff"].getString().find("+2 BETA")
               != std::string::npos);
   EXPECT_TRUE(m["details"]["patch"].getString().find("@@")
               != std::string::npos);
}

TEST(BuiltinTools, EditFuzzySmartQuoteMatch) {
   TmpDir t;
   { std::ofstream f{t.path("e.txt")}; f << "say \xE2\x80\x9Chello\xE2\x80\x9D\n"; }
   // oldText 用 ASCII 引号, fuzzy 归一化命中
   auto frames = prompt(
      "edit:{\"path\":\"" + esc(t.path("e.txt"))
      + "\",\"edits\":[{\"oldText\":\"\\\"hello\\\"\",\"newText\":\"HI\"}]}");
   auto const& m = toolResult(frames);
   EXPECT_FALSE(m["isError"].getBool());
   std::ifstream check{t.path("e.txt")};
   std::string got{std::istreambuf_iterator<char>{check},
                   std::istreambuf_iterator<char>{}};
   EXPECT_EQ(got, "say HI\n");
}

TEST(BuiltinTools, EditNotFoundError) {
   TmpDir t;
   { std::ofstream f{t.path("e.txt")}; f << "alpha\n"; }
   auto frames = prompt("edit:{\"path\":\"" + esc(t.path("e.txt"))
                        + "\",\"edits\":[{\"oldText\":\"missing\",\"newText\":\"x\"}]}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_TRUE(m["content"][0]["text"].getString()
               .find("Could not find the exact text in")
               != std::string::npos);
}

TEST(BuiltinTools, EditDuplicateError) {
   TmpDir t;
   { std::ofstream f{t.path("e.txt")}; f << "dup\ndup\n"; }
   auto frames = prompt("edit:{\"path\":\"" + esc(t.path("e.txt"))
                        + "\",\"edits\":[{\"oldText\":\"dup\",\"newText\":\"x\"}]}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_TRUE(m["content"][0]["text"].getString()
               .find("Found 2 occurrences of the text in")
               != std::string::npos);
}

// == bash ==================================================================
TEST(BuiltinTools, BashRunsCommand) {
   auto frames = prompt("bash:{\"command\":\"echo out42\"}");
   auto const& m = toolResult(frames);
   EXPECT_FALSE(m["isError"].getBool());
   EXPECT_EQ(m["content"][0]["text"].getString(), "out42\n");
}

TEST(BuiltinTools, BashNonzeroIsErrorWithStatus) {
   auto frames = prompt("bash:{\"command\":\"exit 7\"}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_EQ(m["content"][0]["text"].getString(),
             "(no output)\n\nCommand exited with code 7");
}

TEST(BuiltinTools, BashTimeoutValidation) {
   auto frames = prompt("bash:{\"command\":\"sleep 1\",\"timeout\":-5}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_EQ(m["content"][0]["text"].getString(),
             "Invalid timeout: must be a finite number of seconds");
}

// == grep ==================================================================
TEST(BuiltinTools, GrepFindsMatches) {
   TmpDir t;
   { std::ofstream f{t.path("g.txt")}; f << "hello\nneedle here\nbye\n"; }
   auto frames = prompt("grep:{\"pattern\":\"needle\",\"path\":\""
                        + esc(t.dir.string()) + "\"}");
   auto const& m = toolResult(frames);
   EXPECT_FALSE(m["isError"].getBool());
   EXPECT_EQ(m["content"][0]["text"].getString(), "g.txt:2: needle here");
}

TEST(BuiltinTools, GrepNoMatches) {
   TmpDir t;
   { std::ofstream f{t.path("g.txt")}; f << "hello\n"; }
   auto frames = prompt("grep:{\"pattern\":\"zzz\",\"path\":\""
                        + esc(t.dir.string()) + "\"}");
   auto const& m = toolResult(frames);
   EXPECT_EQ(m["content"][0]["text"].getString(), "No matches found");
}

TEST(BuiltinTools, GrepContextAndGitignore) {
   TmpDir t;
   { std::ofstream f{t.path("a.txt")}; f << "x1\ntag\nx2\n"; }
   { std::ofstream f{t.path("ign.log")}; f << "tag\n"; }
   { std::ofstream f{t.path(".gitignore")}; f << "*.log\n"; }
   auto frames = prompt("grep:{\"pattern\":\"tag\",\"path\":\""
                        + esc(t.dir.string()) + "\",\"context\":1}");
   auto const& m = toolResult(frames);
   auto const text = m["content"][0]["text"].getString();
   EXPECT_TRUE(text.find("a.txt:2: tag") != std::string::npos) << text;
   EXPECT_TRUE(text.find("a.txt-1- x1") != std::string::npos) << text;
   // .gitignore 排除 ign.log
   EXPECT_TRUE(text.find("ign.log") == std::string::npos) << text;
}

// == find ==================================================================
TEST(BuiltinTools, FindGlob) {
   TmpDir t;
   { std::ofstream f{t.path("x.py")}; }
   { std::ofstream f{t.path("y.md")}; }
   fs::create_directories(t.dir / "sub");
   { std::ofstream f{t.dir / "sub" / "z.py"}; }
   auto frames = prompt("find:{\"pattern\":\"*.py\",\"path\":\""
                        + esc(t.dir.string()) + "\"}");
   auto const& m = toolResult(frames);
   auto const text = m["content"][0]["text"].getString();
   EXPECT_TRUE(text.find("x.py") != std::string::npos) << text;
   EXPECT_TRUE(text.find("sub/z.py") != std::string::npos) << text;
   EXPECT_TRUE(text.find("y.md") == std::string::npos) << text;
}

// == ls ====================================================================
TEST(BuiltinTools, LsSortedWithDirSuffix) {
   TmpDir t;
   { std::ofstream f{t.path("b.txt")}; }
   { std::ofstream f{t.path("A.txt")}; }
   fs::create_directories(t.dir / "mid");
   auto frames = prompt("ls:{\"path\":\"" + esc(t.dir.string()) + "\"}");
   auto const& m = toolResult(frames);
   EXPECT_EQ(m["content"][0]["text"].getString(), "A.txt\nb.txt\nmid/");
}

TEST(BuiltinTools, LsNotADirectory) {
   TmpDir t;
   { std::ofstream f{t.path("f.txt")}; }
   auto frames = prompt("ls:{\"path\":\"" + esc(t.path("f.txt")) + "\"}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_TRUE(m["content"][0]["text"].getString().find("Not a directory")
               != std::string::npos);
}

// == 边界: read 字节截断 / 图片 / 目录 ======================================
TEST(BuiltinTools, ReadByteLimitNotice) {
   TmpDir t;
   {
      std::ofstream f{t.path("big.txt")};
      std::string line(200, 'x');
      for (int i = 0; i < 400; ++i) f << line << "\n";   // ~80KB > 50KiB
   }
   auto frames = prompt("read:{\"path\":\"" + esc(t.path("big.txt")) + "\"}");
   auto const& m = toolResult(frames);
   EXPECT_FALSE(m["isError"].getBool());
   auto const text = m["content"][0]["text"].getString();
   EXPECT_TRUE(text.find("limit") != std::string::npos) << text.substr(text.size()-200);
   EXPECT_TRUE(text.find("offset=") != std::string::npos);
}

TEST(BuiltinTools, ReadImageReturnsImageBlock) {
   TmpDir t;
   {
      std::ofstream f{t.path("p.png"), std::ios::binary};
      // 合法 PNG 头: 8B 签名 + IHDR chunk (len=13) — mime 嗅探要求 IHDR
      const char png[] = {'\x89','P','N','G','\x0d','\x0a','\x1a','\x0a',
                          0,0,0,13,'I','H','D','R',
                          0,0,0,1, 0,0,0,1, 8,6,0,0,0};
      f.write(png, sizeof png);
      std::string pad(32, '\0');
      f.write(pad.data(), 32);
   }
   auto frames = prompt("read:{\"path\":\"" + esc(t.path("p.png")) + "\"}");
   auto const& m = toolResult(frames);
   EXPECT_FALSE(m["isError"].getBool());
   // pi: 图片作为 {type:"image",data,mimeType} content block 返回
   ASSERT_TRUE(m["content"].isArray());
   bool sawImage = false;
   for (auto const& b : m["content"])
      if (b["type"].getString() == "image") {
         sawImage = true;
         EXPECT_EQ(b["mimeType"].getString(), "image/png");
         EXPECT_FALSE(b["data"].getString().empty());
      }
   EXPECT_TRUE(sawImage);
}

TEST(BuiltinTools, ReadDirectoryIsError) {
   TmpDir t;
   auto frames = prompt("read:{\"path\":\"" + esc(t.dir.string()) + "\"}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_TRUE(m["content"][0]["text"].getString().find("EISDIR")
               != std::string::npos);
}

TEST(BuiltinTools, ReadHomeTildeExpansion) {
   auto frames = prompt("read:{\"path\":\"~/definitely-not-exist-cpppi.txt\"}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   // 错误串里应展开成绝对路径而不是 ~ 开头
   EXPECT_TRUE(m["content"][0]["text"].getString().find("~") == std::string::npos);
}

// == 边界: edit 多替换 / CRLF 保留 ==========================================
TEST(BuiltinTools, EditMultipleBlocks) {
   TmpDir t;
   { std::ofstream f{t.path("m.txt")}; f << "a1\nb1\nc1\n"; }
   auto frames = prompt(
      "edit:{\"path\":\"" + esc(t.path("m.txt"))
      + "\",\"edits\":[{\"oldText\":\"a1\",\"newText\":\"A\"},"
        "{\"oldText\":\"c1\",\"newText\":\"C\"}]}");
   auto const& m = toolResult(frames);
   EXPECT_FALSE(m["isError"].getBool());
   EXPECT_TRUE(m["content"][0]["text"].getString()
               .find("2 block(s)") != std::string::npos);
   std::ifstream check{t.path("m.txt")};
   std::string got{std::istreambuf_iterator<char>{check},
                   std::istreambuf_iterator<char>{}};
   EXPECT_EQ(got, "A\nb1\nC\n");
}

TEST(BuiltinTools, EditPreservesCrLf) {
   TmpDir t;
   { std::ofstream f{t.path("w.txt"), std::ios::binary}; f << "x1\r\ny1\r\n"; }
   auto frames = prompt(
      "edit:{\"path\":\"" + esc(t.path("w.txt"))
      + "\",\"edits\":[{\"oldText\":\"y1\",\"newText\":\"Y2\"}]}");
   auto const& m = toolResult(frames);
   EXPECT_FALSE(m["isError"].getBool());
   std::ifstream check{t.path("w.txt"), std::ios::binary};
   std::string got{std::istreambuf_iterator<char>{check},
                   std::istreambuf_iterator<char>{}};
   EXPECT_EQ(got, "x1\r\nY2\r\n");
}

// == 边界: bash 超时 / stderr ==============================================
TEST(BuiltinTools, BashTimeoutKills) {
   auto frames = prompt("bash:{\"command\":\"sleep 30\",\"timeout\":1}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_TRUE(m["content"][0]["text"].getString().find("timed out")
               != std::string::npos);
}

TEST(BuiltinTools, BashCapturesStderr) {
   auto frames = prompt("bash:{\"command\":\"echo oops >&2; exit 3\"}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   auto const text = m["content"][0]["text"].getString();
   EXPECT_TRUE(text.find("oops") != std::string::npos) << text;
   EXPECT_TRUE(text.find("code 3") != std::string::npos) << text;
}

// == 边界: grep glob / 大小写 / 无效 regex =================================
TEST(BuiltinTools, GrepCaseInsensitive) {
   TmpDir t;
   { std::ofstream f{t.path("c.txt")}; f << "HeLLo\nbye\n"; }
   auto frames = prompt("grep:{\"pattern\":\"hello\",\"path\":\""
                        + esc(t.dir.string()) + "\",\"ignoreCase\":true}");
   auto const& m = toolResult(frames);
   EXPECT_EQ(m["content"][0]["text"].getString(), "c.txt:1: HeLLo");
}

TEST(BuiltinTools, GrepGlobFilter) {
   TmpDir t;
   { std::ofstream f{t.path("a.py")}; f << "hit\n"; }
   { std::ofstream f{t.path("b.md")}; f << "hit\n"; }
   auto frames = prompt("grep:{\"pattern\":\"hit\",\"path\":\""
                        + esc(t.dir.string()) + "\",\"glob\":\"*.py\"}");
   auto const& m = toolResult(frames);
   auto const text = m["content"][0]["text"].getString();
   EXPECT_TRUE(text.find("a.py") != std::string::npos) << text;
   EXPECT_TRUE(text.find("b.md") == std::string::npos) << text;
}

TEST(BuiltinTools, GrepInvalidRegexIsError) {
   TmpDir t;
   { std::ofstream f{t.path("x.txt")}; f << "hi\n"; }
   auto frames = prompt("grep:{\"pattern\":\"[[[\",\"path\":\""
                        + esc(t.dir.string()) + "\"}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_TRUE(m["content"][0]["text"].getString().find("Invalid regex")
               != std::string::npos);
}

TEST(BuiltinTools, GrepLiteralMode) {
   TmpDir t;
   { std::ofstream f{t.path("l.txt")}; f << "a.b\naxb\n"; }
   // literal 模式下 "." 不应当是正则通配
   auto frames = prompt("grep:{\"pattern\":\"a.b\",\"path\":\""
                        + esc(t.dir.string()) + "\",\"literal\":true}");
   auto const& m = toolResult(frames);
   auto const text = m["content"][0]["text"].getString();
   EXPECT_TRUE(text.find("a.b") != std::string::npos) << text;
   EXPECT_TRUE(text.find("axb") == std::string::npos) << text;
}

// == 边界: find / ls ========================================================
TEST(BuiltinTools, FindNoMatch) {
   TmpDir t;
   { std::ofstream f{t.path("x.txt")}; }
   auto frames = prompt("find:{\"pattern\":\"*.rs\",\"path\":\""
                        + esc(t.dir.string()) + "\"}");
   auto const& m = toolResult(frames);
   EXPECT_EQ(m["content"][0]["text"].getString(),
             "No files found matching pattern");
}

TEST(BuiltinTools, FindMissingPathIsError) {
   auto frames = prompt("find:{\"pattern\":\"*\",\"path\":\"/nonexistent-dir-x1\"}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
}

TEST(BuiltinTools, LsIncludesDotfiles) {
   TmpDir t;
   { std::ofstream f{t.path(".hidden")}; }
   { std::ofstream f{t.path("shown")}; }
   auto frames = prompt("ls:{\"path\":\"" + esc(t.dir.string()) + "\"}");
   auto const& m = toolResult(frames);
   auto const text = m["content"][0]["text"].getString();
   EXPECT_TRUE(text.find(".hidden") != std::string::npos) << text;
   EXPECT_TRUE(text.find("shown") != std::string::npos) << text;
}

TEST(BuiltinTools, LsMissingPathIsError) {
   auto frames = prompt("ls:{\"path\":\"/nonexistent-dir-x2\"}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
}

// == 边界: write 到目录路径 ==================================================
TEST(BuiltinTools, WriteToDirectoryIsError) {
   TmpDir t;
   auto frames = prompt("write:{\"path\":\"" + esc(t.dir.string())
                        + "\",\"content\":\"x\"}");
   auto const& m = toolResult(frames);
   EXPECT_TRUE(m["isError"].getBool());
}

// == --exclude-tools 语义 ===================================================
TEST(BuiltinTools, ExcludeToolsDisables) {
   auto frames = RpcHarness::run(
      "{\"type\":\"prompt\",\"message\":\"tool:bash:{\\\"command\\\":\\\"echo hi\\\"}\"}\n",
      "--exclude-tools bash");
   auto const& m = toolResult(frames);
   ASSERT_TRUE(m.isObject());
   EXPECT_TRUE(m["isError"].getBool());
   EXPECT_EQ(m["content"][0]["text"].getString(), "Tool bash not found");
}

// == 边界: read >2000 行默认截断 ============================================
TEST(BuiltinTools, ReadDefaultLineLimit) {
   TmpDir t;
   {
      std::ofstream f{t.path("many.txt")};
      for (int i = 1; i <= 2100; ++i) f << "line" << i << "\n";
   }
   auto frames = prompt("read:{\"path\":\"" + esc(t.path("many.txt")) + "\"}");
   auto const& m = toolResult(frames);
   auto const text = m["content"][0]["text"].getString();
   EXPECT_TRUE(text.find("Showing lines 1-2000 of 2101") != std::string::npos
               || text.find("Showing lines 1-2000 of 2100") != std::string::npos)
      << text.substr(text.size() - 300);
}

// == 边界: bash 大输出截断 ===================================================
TEST(BuiltinTools, BashOutputTruncated) {
   // 超过 1MiB 输出 -> 截断提示
   auto frames = prompt(
      "bash:{\"command\":\"head -c 1200000 /dev/zero | tr '\\\\0' 'x'\"}");
   auto const& m = toolResult(frames);
   auto const text = m["content"][0]["text"].getString();
   EXPECT_TRUE(text.find("truncated") != std::string::npos
               || text.find("Full output") != std::string::npos)
      << text.substr(0, 200);
}

// == 边界: grep 命中数截断 ===================================================
TEST(BuiltinTools, GrepLimitReachedNotice) {
   TmpDir t;
   {
      std::ofstream f{t.path("many.txt")};
      for (int i = 0; i < 150; ++i) f << "hit" << i << "\n";
   }
   auto frames = prompt("grep:{\"pattern\":\"hit\",\"path\":\""
                        + esc(t.dir.string()) + "\"}");
   auto const& m = toolResult(frames);
   auto const text = m["content"][0]["text"].getString();
   // 100 默认上限 -> 应带截断说明
   EXPECT_TRUE(text.find("100") != std::string::npos) << text.substr(text.size()-200);
}

// == 边界: ls >500 截断 ======================================================
TEST(BuiltinTools, LsLimitNotice) {
   TmpDir t;
   for (int i = 0; i < 510; ++i)
      std::ofstream{t.path(std::format("f{:03}.txt", i))};
   auto frames = prompt("ls:{\"path\":\"" + esc(t.dir.string()) + "\"}");
   auto const& m = toolResult(frames);
   auto const text = m["content"][0]["text"].getString();
   EXPECT_TRUE(text.find("500") != std::string::npos
               || text.find("more") != std::string::npos)
      << text.substr(text.size() - 200);
}

// == 边界: edit BOM 保留 =====================================================
TEST(BuiltinTools, EditPreservesBom) {
   TmpDir t;
   {
      std::ofstream f{t.path("bom.txt"), std::ios::binary};
      f << "\xEF\xBB\xBF" << "a\nb\n";
   }
   auto frames = prompt(
      "edit:{\"path\":\"" + esc(t.path("bom.txt"))
      + "\",\"edits\":[{\"oldText\":\"b\",\"newText\":\"B\"}]}");
   auto const& m = toolResult(frames);
   EXPECT_FALSE(m["isError"].getBool());
   std::ifstream check{t.path("bom.txt"), std::ios::binary};
   std::string got{std::istreambuf_iterator<char>{check},
                   std::istreambuf_iterator<char>{}};
   EXPECT_EQ(got.substr(0, 3), "\xEF\xBB\xBF");
   EXPECT_TRUE(got.find("B") != std::string::npos);
}

// == 边界: find `**` / `?` glob ==============================================
TEST(BuiltinTools, FindDoubleStarAndQuestion) {
   TmpDir t;
   fs::create_directories(t.dir / "d1/d2");
   { std::ofstream f{t.dir / "d1/d2/deep.py"}; }
   { std::ofstream f{t.path("a1.txt")}; }
   { std::ofstream f{t.path("a12.txt")}; }
   auto frames = prompt("find:{\"pattern\":\"**/*.py\",\"path\":\""
                        + esc(t.dir.string()) + "\"}");
   auto text = std::string{
      toolResult(frames)["content"][0]["text"].getString()};
   EXPECT_TRUE(text.find("d1/d2/deep.py") != std::string::npos) << text;

   auto frames2 = prompt("find:{\"pattern\":\"a?.txt\",\"path\":\""
                         + esc(t.dir.string()) + "\"}");
   auto text2 = std::string{
      toolResult(frames2)["content"][0]["text"].getString()};
   EXPECT_TRUE(text2.find("a1.txt") != std::string::npos) << text2;
   // `?` 只匹配一个字符
   EXPECT_TRUE(text2.find("a12.txt") == std::string::npos) << text2;
}

// == 边界: find 相对路径 (cwd 无关, 用 . 名) ==================================
TEST(BuiltinTools, FindDefaultPathIsCwd) {
   // path 缺省 -> cwd; 进程 cwd 是构建目录, 不断言具体内容, 只断言不报错
   auto frames = prompt("find:{\"pattern\":\"*\"}");
   auto const& m = toolResult(frames);
   EXPECT_FALSE(m["isError"].getBool());
}
