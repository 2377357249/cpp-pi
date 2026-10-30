#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <folly/json/json.h>

#include <pi/domain/tool/Tool.h>
#include <pi/infra/process/Shell.h>
#include <pi/infra/util/JsonSafe.h>
#include <pi/infra/util/RegexSafe.h>

#include "EditDiff.h"
#include "Glob.h"
#include "ImageSniff.h"
#include "OutputAccumulator.h"
#include "PathUtils.h"
#include "Truncate.h"

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// pi core/tools/* 全量移植: read/bash/edit/write/grep/find/ls (+powershell
// 名字占位; Windows 专属, POSIX 下不出现在注册表).
//
// 每个工具的 execute 语义严格按 pi 源码; 错误不抛——pi wrapToolDefinition
// 的语义 = err.message -> {content:[text:msg], isError:true}, 这里直接
// 组装成 Outcome{isError:true}.
// ---------------------------------------------------------------------------

struct ToolOutcome {
   std::string                    text;
   std::vector<domain::ImageData> images;
   folly::dynamic                 details = nullptr;
   bool                           isError = false;
};

template <class T>
struct ToolOr {
   T           value{};
   std::string error;
   bool        ok = false;
   static auto success(T v) -> ToolOr { return {std::move(v), {}, true}; }
   static auto fail(std::string m) -> ToolOr { return {{}, std::move(m), false}; }
};

// 会话环境注入 (pi resolveSpawnContext 的 PI_* 变量)
struct ToolSessionEnv {
   std::string sessionId;
   std::string sessionFile;
   std::string provider;
   std::string model;
   std::string reasoningLevel;
};

class BuiltinTools {
public:
   explicit BuiltinTools(std::string cwd) : cwd_{std::move(cwd)} {}

   void setSessionEnv(ToolSessionEnv env) { env_ = std::move(env); }
   void setAbortCheck(std::function<bool()> f) { abortCheck_ = std::move(f); }
   void setShellPath(std::string p) { shellPath_ = std::move(p); }

   // ---------- 注册表 (createAllToolDefinitions) ----------
   auto definitions() const -> std::vector<domain::Tool> {
      return {
         {"read", readDescription(), kReadSchema},
         {"bash", bashDescription("bash"), kBashSchema},
         {"powershell", bashDescription("powershell"), kBashSchema},
         {"edit", kEditDescription, kEditSchema},
         {"write", kWriteDescription, kWriteSchema},
         {"grep", grepDescription(), kGrepSchema},
         {"find", findDescription(), kFindSchema},
         {"ls", lsDescription(), kLsSchema},
      };
   }

   // ---------- 执行 ----------
   auto execute(domain::ToolCall const& call) -> ToolOutcome {
      auto args = infra::parseJsonSafe(call.input);
      folly::dynamic a = args ? *args : folly::dynamic::object();
      // 模型可能给裸串 (mock): 非对象按工具各异处理
      auto obj = a.isObject() ? a : folly::dynamic::object();

      if (call.tool == "read")  return wrap(execRead(obj, call.input));
      if (call.tool == "write") return wrap(execWrite(obj));
      if (call.tool == "edit")  return wrap(execEdit(obj));
      if (call.tool == "bash")  return wrap(execBash(obj));
      if (call.tool == "grep")  return wrap(execGrep(obj));
      if (call.tool == "find")  return wrap(execFind(obj));
      if (call.tool == "ls")    return wrap(execLs(obj));
      // echo 保留作 mock 工具
      if (call.tool == "echo")
         return {std::string{"echo: "}
                 + (obj.count("input") && obj["input"].isString()
                       ? obj["input"].getString() : call.input)};
      return {std::format("Tool {} not found", call.tool), {}, nullptr, true};
   }

private:
   static auto wrap(ToolOr<ToolOutcome> r) -> ToolOutcome {
      if (!r.ok) return {r.error, {}, nullptr, true};
      return std::move(r.value);
   }

   auto aborted() const -> bool { return abortCheck_ && abortCheck_(); }
   auto abortError() const -> ToolOr<ToolOutcome> {
      return ToolOr<ToolOutcome>::fail("Operation aborted");
   }

   // ---------- 参数便利 ----------
   static auto str(folly::dynamic const& o, std::string_view k)
      -> std::optional<std::string> {
      if (!o.isObject() || !o.count(k) || !o[k].isString()) return std::nullopt;
      return o[k].getString();
   }
   static auto num(folly::dynamic const& o, std::string_view k)
      -> std::optional<double> {
      if (!o.isObject() || !o.count(k) || !o[k].isNumber()) return std::nullopt;
      return o[k].asDouble();
   }
   static auto flag(folly::dynamic const& o, std::string_view k) -> bool {
      return o.isObject() && o.count(k) && o[k].isBool() && o[k].getBool();
   }

   static auto errnoText(int e, std::string_view op, std::string const& path)
      -> std::string {
      return std::format("{}: {}, {} '{}'", std::strerror(e), std::strerror(e),
                         op, path);
   }
   // Node 风格: "ENOENT: no such file or directory, access '/p'"
   static auto nodeErr(int e, std::string_view op, std::string const& path)
      -> std::string {
      auto const* code = e == ENOENT ? "ENOENT"
                         : e == EACCES ? "EACCES"
                         : e == EPERM  ? "EPERM"
                         : e == ENOTDIR ? "ENOTDIR"
                         : e == EISDIR ? "EISDIR" : "EIO";
      return std::format("{}: {}, {} '{}'", code, std::strerror(e), op, path);
   }
   static auto nodeCode(int e) -> std::string {
      return e == ENOENT ? "ENOENT" : e == EACCES ? "EACCES"
             : e == EPERM ? "EPERM" : e == ENOTDIR ? "ENOTDIR" : "EIO";
   }

   // ==================================================================
   // read — pi core/tools/read.ts
   // ==================================================================
   auto execRead(folly::dynamic const& args, std::string const& rawInput)
      -> ToolOr<ToolOutcome> {
      if (aborted()) return abortError();
      auto pathOpt = str(args, "path");
      if (!pathOpt) {
         // 裸串参数兜底 (mock): 非 JSON 输入整体当 path
         auto trimmed = rawInput;
         auto const first = trimmed.find_first_not_of(" \t\r\n");
         if (first != std::string::npos)
            trimmed = trimmed.substr(first);
         pathOpt = std::move(trimmed);
      }
      auto const& path = *pathOpt;
      auto const absolute = resolveReadPath(path, cwd_);

      // access(R_OK); 目录 -> EISDIR (Node fs.readFile 语义)
      std::error_code ec;
      if (!fs::exists(absolute, ec))
         return ToolOr<ToolOutcome>::fail(
            nodeErr(ENOENT, "access", absolute));
      if (fs::is_directory(absolute, ec))
         return ToolOr<ToolOutcome>::fail(
            nodeErr(EISDIR, "read", absolute));
      std::ifstream probe{absolute};
      if (!probe)
         return ToolOr<ToolOutcome>::fail(
            nodeErr(errno ? errno : EACCES, "access", absolute));
      probe.close();

      // 图片
      if (auto const mime = detectSupportedImageMimeTypeFromFile(absolute)) {
         std::ifstream img{absolute, std::ios::binary};
         std::string const data{std::istreambuf_iterator<char>{img},
                                std::istreambuf_iterator<char>{}};
         ToolOutcome o;
         o.text = std::format("Read image file [{}]", *mime);
         o.images.push_back({base64Encode(data), *mime});
         return ToolOr<ToolOutcome>::success(std::move(o));
      }

      // 文本: pi allLines = content.split("\n") — 含末尾空位
      std::ifstream f{absolute, std::ios::binary};
      if (!f)
         return ToolOr<ToolOutcome>::fail(
            nodeErr(errno ? errno : ENOENT, "read", absolute));
      std::string const content{std::istreambuf_iterator<char>{f},
                                std::istreambuf_iterator<char>{}};
      std::vector<std::string_view> piLines;
      {
         std::string_view sv{content};
         size_t pos = 0;
         for (;;) {
            auto const nl = sv.find('\n', pos);
            if (nl == std::string_view::npos) {
               piLines.push_back(sv.substr(pos));
               break;
            }
            piLines.push_back(sv.substr(pos, nl - pos));
            pos = nl + 1;
         }
      }
      auto const totalFileLines = static_cast<int64_t>(piLines.size());

      auto const offset = num(args, "offset");
      auto const limit  = num(args, "limit");
      size_t const startLine = offset && *offset > 0
         ? static_cast<size_t>(*offset - 1) : 0;
      auto const startDisplay = startLine + 1;

      if (startLine >= piLines.size())
         return ToolOr<ToolOutcome>::fail(std::format(
            "Offset {} is beyond end of file ({} lines total)",
            offset ? static_cast<int64_t>(*offset) : 0, piLines.size()));

      std::string selected;
      std::optional<size_t> userLimited;
      if (limit) {
         auto const endLine = std::min(startLine + static_cast<size_t>(*limit),
                                       piLines.size());
         for (size_t i = startLine; i < endLine; ++i) {
            if (i > startLine) selected += '\n';
            selected += piLines[i];
         }
         userLimited = endLine - startLine;
      } else {
         for (size_t i = startLine; i < piLines.size(); ++i) {
            if (i > startLine) selected += '\n';
            selected += piLines[i];
         }
      }

      auto const trunc = truncateHead(selected);
      std::string output;
      folly::dynamic details = nullptr;
      if (trunc.firstLineExceedsLimit) {
         auto const lineSize = formatSize(
            static_cast<int64_t>(piLines[startLine].size()));
         output = std::format(
            "[Line {} is {}, exceeds {} limit. Use bash: sed -n '{}p' {} "
            "| head -c {}]", startDisplay, lineSize,
            formatSize(kDefaultMaxBytes), startDisplay, path, kDefaultMaxBytes);
         details = folly::dynamic::object("truncation", truncationJson(trunc));
      } else if (trunc.truncated) {
         auto const endDisplay = startDisplay + trunc.outputLines - 1;
         auto const nextOffset = endDisplay + 1;
         output = trunc.content;
         if (trunc.truncatedBy == "lines")
            output += std::format(
               "\n\n[Showing lines {}-{} of {}. Use offset={} to continue.]",
               startDisplay, endDisplay, totalFileLines, nextOffset);
         else
            output += std::format(
               "\n\n[Showing lines {}-{} of {} ({} limit). Use offset={} "
               "to continue.]", startDisplay, endDisplay, totalFileLines,
               formatSize(kDefaultMaxBytes), nextOffset);
         details = folly::dynamic::object("truncation", truncationJson(trunc));
      } else if (userLimited
                 && startLine + *userLimited < piLines.size()) {
         auto const remaining = piLines.size() - (startLine + *userLimited);
         auto const nextOffset = startLine + *userLimited + 1;
         output = std::format("{}\n\n[{} more lines in file. Use offset={} "
                              "to continue.]", trunc.content, remaining,
                              nextOffset);
      } else {
         output = trunc.content;
      }
      ToolOutcome o;
      o.text = std::move(output);
      o.details = std::move(details);
      return ToolOr<ToolOutcome>::success(std::move(o));
   }

   // ==================================================================
   // write — pi core/tools/write.ts
   // ==================================================================
   auto execWrite(folly::dynamic const& args) -> ToolOr<ToolOutcome> {
      if (aborted()) return abortError();
      auto const pathOpt = str(args, "path");
      auto const content = str(args, "content").value_or(
         args.count("content") && args["content"].isString()
            ? args["content"].getString() : "");
      if (!pathOpt)
         return ToolOr<ToolOutcome>::fail("path is required");
      auto const absolute = resolveToCwd(*pathOpt, cwd_);
      auto const dir = fs::path{absolute}.parent_path();
      std::error_code ec;
      if (!dir.empty()) fs::create_directories(dir, ec);
      if (ec)
         return ToolOr<ToolOutcome>::fail(
            nodeErr(ec.value(), "mkdir", dir.string()));
      std::ofstream f{absolute, std::ios::binary | std::ios::trunc};
      if (!f)
         return ToolOr<ToolOutcome>::fail(
            nodeErr(errno ? errno : EIO, "write", absolute));
      f << content;
      if (!f)
         return ToolOr<ToolOutcome>::fail(
            nodeErr(errno ? errno : EIO, "write", absolute));
      return ToolOr<ToolOutcome>::success(
         {std::format("Successfully wrote to {}", *pathOpt)});
   }

   // ==================================================================
   // edit — pi core/tools/edit.ts (+ prepareEditArguments)
   // ==================================================================
   auto execEdit(folly::dynamic args) -> ToolOr<ToolOutcome> {
      if (aborted()) return abortError();
      // prepareEditArguments: edits 可能是 JSON 串 / 单对象 / legacy 双字段
      if (!args.isObject())
         return ToolOr<ToolOutcome>::fail(
            "Edit tool input is invalid. edits must contain at least one "
            "replacement.");
      if (args.count("edits") && args["edits"].isString()) {
         if (auto p = infra::parseJsonSafe(args["edits"].getString());
             p && p->isArray())
            args["edits"] = *p;
         else if (p && p->isObject()
                  && p->count("oldText") && p->count("newText"))
            args["edits"] = folly::dynamic::array(*p);
      } else if (args.count("edits") && args["edits"].isObject()
                 && args["edits"].count("oldText")
                 && args["edits"].count("newText")) {
         args["edits"] = folly::dynamic::array(args["edits"]);
      }
      std::vector<Edit> edits;
      if (args.count("edits") && args["edits"].isArray()) {
         for (auto const& e : args["edits"]) {
            if (!e.isObject() || !e.count("oldText") || !e["oldText"].isString()
                || !e.count("newText") || !e["newText"].isString())
               continue;
            edits.push_back({e["oldText"].getString(), e["newText"].getString()});
         }
      }
      if (args.count("oldText") && args["oldText"].isString()
          && args.count("newText") && args["newText"].isString())
         edits.push_back({args["oldText"].getString(),
                          args["newText"].getString()});
      if (edits.empty())
         return ToolOr<ToolOutcome>::fail(
            "Edit tool input is invalid. edits must contain at least one "
            "replacement.");

      auto const path = str(args, "path").value_or("");
      auto const absolute = resolveToCwd(path, cwd_);

      // access(R_OK|W_OK)
      std::error_code ec;
      auto const st = fs::status(absolute, ec);
      if (ec || !fs::exists(st))
         return ToolOr<ToolOutcome>::fail(std::format(
            "Could not edit file: {}. Error code: {}.",
            path, nodeCode(ec ? ec.value() : ENOENT)));

      std::ifstream in{absolute, std::ios::binary};
      if (!in)
         return ToolOr<ToolOutcome>::fail(std::format(
            "Could not edit file: {}. Error code: EACCES.", path));
      std::string const raw{std::istreambuf_iterator<char>{in},
                            std::istreambuf_iterator<char>{}};

      auto const [bom, content] = splitBom(raw);
      auto const ending         = detectLineEnding(content);
      auto const normalized     = normalizeToLF(content);
      auto const applied = applyEditsToNormalizedContent(normalized, edits, path);
      if (!applied.ok)
         return ToolOr<ToolOutcome>::fail(applied.error.message);

      auto const finalContent = bom
         + restoreLineEndings(applied.value.newContent, ending);
      std::ofstream out{absolute, std::ios::binary | std::ios::trunc};
      if (!out)
         return ToolOr<ToolOutcome>::fail(std::format(
            "Could not edit file: {}. Error code: EACCES.", path));
      out << finalContent;
      out.close();

      auto const diffR = generateDiffString(applied.value.baseContent,
                                            applied.value.newContent);
      auto const patch = generateUnifiedPatch(path, applied.value.baseContent,
                                              applied.value.newContent);
      folly::dynamic details = folly::dynamic::object
         ("diff", diffR.diff)("patch", patch);
      if (diffR.firstChangedLine)
         details["firstChangedLine"] = static_cast<int64_t>(*diffR.firstChangedLine);

      ToolOutcome o;
      o.text = std::format("Successfully replaced {} block(s) in {}.",
                           edits.size(), path);
      o.details = std::move(details);
      return ToolOr<ToolOutcome>::success(std::move(o));
   }

   // ==================================================================
   // bash — pi core/tools/bash.ts (+createLocalShellOperations)
   // ==================================================================
   auto execBash(folly::dynamic const& args) -> ToolOr<ToolOutcome> {
      if (aborted()) return abortError();
      auto const command = str(args, "command").value_or("");
      auto const timeoutS = num(args, "timeout");

      // resolveTimeoutMs 校验
      int64_t timeoutMs = -1;
      if (timeoutS) {
         if (!std::isfinite(*timeoutS) || *timeoutS <= 0)
            return ToolOr<ToolOutcome>::fail(
               "Invalid timeout: must be a finite number of seconds");
         double const ms = *timeoutS * 1000;
         if (ms > kBashMaxTimeoutMs)
            return ToolOr<ToolOutcome>::fail(std::format(
               "Invalid timeout: maximum is {} seconds",
               kBashMaxTimeoutMs / 1000.0));
         timeoutMs = static_cast<int64_t>(ms);
      }

      // env: PI_* 注入
      std::vector<std::pair<std::string, std::string>> env;
      if (!env_.sessionId.empty())       env.emplace_back("PI_SESSION_ID", env_.sessionId);
      if (!env_.sessionFile.empty())     env.emplace_back("PI_SESSION_FILE", env_.sessionFile);
      if (!env_.provider.empty())        env.emplace_back("PI_PROVIDER", env_.provider);
      if (!env_.model.empty())           env.emplace_back("PI_MODEL", env_.model);
      if (!env_.reasoningLevel.empty())  env.emplace_back("PI_REASONING_LEVEL", env_.reasoningLevel);

      // cwd 不存在
      std::error_code ec;
      if (!fs::exists(cwd_, ec))
         return ToolOr<ToolOutcome>::fail(std::format(
            "Working directory does not exist: {}\nCannot execute bash commands.",
            cwd_));

      OutputAccumulator acc{"pi-bash"};
      auto const started = std::chrono::steady_clock::now();
      auto r = infra::spawnShell(command,
                                 shellPath_.empty() ? "/bin/bash" : shellPath_,
                                 {.cwd = cwd_, .env = env,
                                  .timeoutMs = timeoutMs,
                                  .onData = [&](std::string_view d) {
                                     acc.append(d);
                                  },
                                  .aborted = [this] { return aborted(); }});
      acc.finish();

      auto snapshot = acc.snapshot(true);
      auto fmtOut = [&](std::string_view emptyText = "(no output)")
         -> std::pair<std::string, folly::dynamic> {
         auto const& trunc = snapshot.truncation;
         std::string text = snapshot.content.empty()
                            ? std::string{emptyText} : snapshot.content;
         folly::dynamic details = nullptr;
         if (trunc.truncated) {
            details = folly::dynamic::object(
               "truncation", truncationJson(trunc));
            if (snapshot.fullOutputPath)
               details["fullOutputPath"] = *snapshot.fullOutputPath;
            auto const startLine = trunc.totalLines - trunc.outputLines + 1;
            auto const endLine = trunc.totalLines;
            if (trunc.lastLinePartial) {
               auto const lastSize = formatSize(acc.lastLineBytes());
               text += std::format(
                  "\n\n[Showing last {} of line {} (line is {}). "
                  "Full output: {}]", formatSize(trunc.outputBytes),
                  endLine, lastSize, *snapshot.fullOutputPath);
            } else if (trunc.truncatedBy == "lines") {
               text += std::format(
                  "\n\n[Showing lines {}-{} of {}. Full output: {}]",
                  startLine, endLine, trunc.totalLines,
                  *snapshot.fullOutputPath);
            } else {
               text += std::format(
                  "\n\n[Showing lines {}-{} of {} ({} limit). "
                  "Full output: {}]", startLine, endLine, trunc.totalLines,
                  formatSize(kDefaultMaxBytes), *snapshot.fullOutputPath);
            }
         }
         return {std::move(text), std::move(details)};
      };
      auto appendStatus = [](std::string const& text, std::string const& status) {
         return text.empty() ? status : text + "\n\n" + status;
      };

      if (r.aborted) {
         auto [text, det] = fmtOut("");
         return ToolOr<ToolOutcome>::fail(
            appendStatus(text, "Command aborted"));
      }
      if (r.timedOut) {
         auto [text, det] = fmtOut("");
         return ToolOr<ToolOutcome>::fail(appendStatus(text, std::format(
            "Command timed out after {} seconds",
            timeoutS ? trimNum(*timeoutS) : "0")));
      }
      if (!r.spawnError.empty())
         return ToolOr<ToolOutcome>::fail(r.spawnError);
      if (r.exitCode < 0) {
         auto [text, det] = fmtOut();
         return ToolOr<ToolOutcome>::fail(
            appendStatus(text, "Command terminated without an exit code"));
      }

      acc.closeTempFile();
      auto [text, details] = fmtOut();
      auto const wallSec = std::chrono::duration<double>(
         std::chrono::steady_clock::now() - started).count();
      if (r.exitCode != 0)
         return ToolOr<ToolOutcome>::success(
            {appendStatus(text, std::format("Command exited with code {}",
                                            r.exitCode)),
             {}, std::move(details), true});
      return ToolOr<ToolOutcome>::success({std::move(text), {},
                                           std::move(details), false});
   }

   // ==================================================================
   // grep — pi core/tools/grep.ts (原生实现, 语义同 rg --json 路径)
   // ==================================================================
   auto execGrep(folly::dynamic const& args) -> ToolOr<ToolOutcome> {
      if (aborted()) return abortError();
      auto const pattern = str(args, "pattern").value_or("");
      auto const searchDir = str(args, "path").value_or(".");
      auto const searchPath = resolveToCwd(searchDir, cwd_);

      std::error_code ec;
      auto const st = fs::status(searchPath, ec);
      if (ec || !fs::exists(st))
         return ToolOr<ToolOutcome>::fail(
            std::format("Path not found: {}", searchPath));
      bool const isDir = fs::is_directory(st);

      auto const contextV = [&] {
         auto c = num(args, "context").value_or(0);
         return c > 0 ? static_cast<size_t>(c) : 0;
      }();
      auto const limit = [&] {
         auto l = num(args, "limit").value_or(kGrepDefaultLimit);
         return std::max(1.0, l);
      }();
      auto const ignoreCase = flag(args, "ignoreCase");
      auto const literal    = flag(args, "literal");
      auto const globFilter = str(args, "glob");

      // 构建匹配器 (regex 编译经 infra 非抛异常适配)
      std::string pat = pattern;
      if (literal) {
         std::string esc;
         for (char c : pat) {
            if (std::string_view{R"(\.^$|()[]{}*+?)"}.find(c) != std::string_view::npos)
               esc += '\\';
            esc += c;
         }
         pat = std::move(esc);
      }
      auto re = infra::makeRegex(pat, ignoreCase);
      if (!re)
         return ToolOr<ToolOutcome>::fail(
            std::format("Invalid regex: {}", re.error()));

      // 遍历收集匹配
      struct Match { std::string file; int64_t line; std::string text; };
      std::vector<Match> matches;
      bool limitReached = false, linesTruncated = false;

      std::vector<std::string> files;
      if (isDir) walkFiles(searchPath, globFilter, files);
      else files.push_back(searchPath);

      for (auto const& file : files) {
         if (matches.size() >= static_cast<size_t>(limit)) { limitReached = true; break; }
         std::ifstream f{file};
         if (!f) continue;
         std::string line;
         int64_t lineNo = 0;
         while (std::getline(f, line)) {
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!std::regex_search(line, *re)) continue;
            matches.push_back({file, lineNo, line});
            if (matches.size() >= static_cast<size_t>(limit)) {
               limitReached = true;
               break;
            }
         }
      }

      if (matches.empty())
         return ToolOr<ToolOutcome>::success({"No matches found"});

      auto formatPath = [&](std::string const& file) -> std::string {
         if (isDir) {
            auto rel = fs::relative(file, searchPath, ec);
            if (!ec && !rel.empty() && *rel.begin() != "..")
               return rel.generic_string();
         }
         return fs::path{file}.filename().string();
      };

      // context 需要原文件行缓存
      std::unordered_map<std::string, std::vector<std::string>> cache;
      auto fileLines = [&](std::string const& file) -> std::vector<std::string>& {
         auto& v = cache[file];
         if (v.empty()) {
            std::ifstream f{file};
            std::string l;
            while (std::getline(f, l)) {
               if (!l.empty() && l.back() == '\r') l.pop_back();
               v.push_back(l);
            }
         }
         return v;
      };

      std::vector<std::string> outLines;
      for (auto const& m : matches) {
         auto const rel = formatPath(m.file);
         if (contextV == 0) {
            auto [t, wasTrunc] = truncateLine(m.text);
            if (wasTrunc) linesTruncated = true;
            outLines.push_back(std::format("{}:{}: {}", rel, m.line, t));
            continue;
         }
         auto const& lines = fileLines(m.file);
         if (lines.empty()) {
            outLines.push_back(std::format("{}:{}: (unable to read file)",
                                           rel, m.line));
            continue;
         }
         auto const start = std::max<int64_t>(1, m.line - static_cast<int64_t>(contextV));
         auto const end = std::min<int64_t>(lines.size(),
                                            m.line + static_cast<int64_t>(contextV));
         for (auto c = start; c <= end; ++c) {
            auto [t, wasTrunc] = truncateLine(lines[c - 1]);
            if (wasTrunc) linesTruncated = true;
            outLines.push_back(c == m.line
               ? std::format("{}:{}: {}", rel, c, t)
               : std::format("{}-{}- {}", rel, c, t));
         }
      }

      std::string raw;
      for (auto const& l : outLines) { raw += l; raw += '\n'; }
      if (!raw.empty()) raw.pop_back();
      auto const trunc = truncateHead(raw, {INT64_MAX, kDefaultMaxBytes});
      std::string output = trunc.content;

      folly::dynamic details = nullptr;
      std::vector<std::string> notices;
      if (limitReached) {
         notices.push_back(std::format(
            "{} matches limit reached. Use limit={} for more, or refine "
            "pattern", static_cast<int64_t>(limit),
            static_cast<int64_t>(limit) * 2));
         if (!details.isObject()) details = folly::dynamic::object;
         details["matchLimitReached"] = static_cast<int64_t>(limit);
      }
      if (trunc.truncated) {
         notices.push_back(std::format("{} limit reached",
                                       formatSize(kDefaultMaxBytes)));
         if (!details.isObject()) details = folly::dynamic::object;
         details["truncation"] = truncationJson(trunc);
      }
      if (linesTruncated) {
         notices.push_back(std::format(
            "Some lines truncated to {} chars. Use read tool to see full "
            "lines", kGrepMaxLineLength));
         if (!details.isObject()) details = folly::dynamic::object;
         details["linesTruncated"] = true;
      }
      if (!notices.empty()) {
         output += "\n\n[";
         for (size_t i = 0; i < notices.size(); ++i) {
            if (i) output += ". ";
            output += notices[i];
         }
         output += "]";
      }
      return ToolOr<ToolOutcome>::success(
         {std::move(output), {}, std::move(details), false});
   }

   // ==================================================================
   // find — pi core/tools/find.ts (原生实现, 语义同 fd 路径)
   // ==================================================================
   auto execFind(folly::dynamic const& args) -> ToolOr<ToolOutcome> {
      if (aborted()) return abortError();
      auto const pattern = str(args, "pattern").value_or("");
      auto const searchDir = str(args, "path").value_or(".");
      auto const searchPath = resolveToCwd(searchDir, cwd_);
      auto const limit = static_cast<size_t>(
         num(args, "limit").value_or(kFindDefaultLimit));

      std::error_code ec;
      if (!fs::exists(searchPath, ec))
         return ToolOr<ToolOutcome>::fail(
            std::format("Path not found: {}", searchPath));

      // fd --full-path 规则: 模式含 '/' 匹配全路径 (自动补 **/ 前缀)
      auto const fullPath = pattern.find('/') != std::string::npos;
      auto effPattern = pattern;
      if (fullPath && !pattern.starts_with('/')
          && !pattern.starts_with("**/") && pattern != "**")
         effPattern = "**/" + pattern;

      std::vector<std::string> results;
      GitIgnoreStack ignore;
      collectMatches(searchPath, "", effPattern, fullPath, ignore, results,
                     limit);
      if (results.empty())
         return ToolOr<ToolOutcome>::success({"No files found matching pattern"});

      auto const limitReached = results.size() >= limit;
      std::string raw;
      for (auto const& r : results) { raw += r; raw += '\n'; }
      raw.pop_back();
      auto const trunc = truncateHead(raw, {INT64_MAX, kDefaultMaxBytes});
      std::string output = trunc.content;

      folly::dynamic details = nullptr;
      std::vector<std::string> notices;
      if (limitReached) {
         notices.push_back(std::format(
            "{} results limit reached. Use limit={} for more, or refine "
            "pattern", limit, limit * 2));
         details = folly::dynamic::object;
         details["resultLimitReached"] = static_cast<int64_t>(limit);
      }
      if (trunc.truncated) {
         notices.push_back(std::format("{} limit reached",
                                       formatSize(kDefaultMaxBytes)));
         if (!details.isObject()) details = folly::dynamic::object;
         details["truncation"] = truncationJson(trunc);
      }
      if (!notices.empty()) {
         output += "\n\n[";
         for (size_t i = 0; i < notices.size(); ++i) {
            if (i) output += ". ";
            output += notices[i];
         }
         output += "]";
      }
      return ToolOr<ToolOutcome>::success(
         {std::move(output), {}, std::move(details), false});
   }

   // ==================================================================
   // ls — pi core/tools/ls.ts
   // ==================================================================
   auto execLs(folly::dynamic const& args) -> ToolOr<ToolOutcome> {
      if (aborted()) return abortError();
      auto const dirPath = resolveToCwd(str(args, "path").value_or("."), cwd_);
      auto const limit = static_cast<size_t>(
         num(args, "limit").value_or(kLsDefaultLimit));

      std::error_code ec;
      if (!fs::exists(dirPath, ec))
         return ToolOr<ToolOutcome>::fail(
            std::format("Path not found: {}", dirPath));
      if (!fs::is_directory(dirPath, ec))
         return ToolOr<ToolOutcome>::fail(
            std::format("Not a directory: {}", dirPath));

      std::vector<std::string> entries;
      for (auto const& de : fs::directory_iterator(dirPath, ec))
         entries.push_back(de.path().filename().string());
      if (ec)
         return ToolOr<ToolOutcome>::fail(
            std::format("Cannot read directory: {}", ec.message()));
      std::ranges::sort(entries, {}, [](std::string const& s) {
         std::string l = s;
         for (auto& c : l) c = static_cast<char>(std::tolower(c));
         return l;
      });

      std::vector<std::string> results;
      bool limitReached = false;
      for (auto const& e : entries) {
         if (results.size() >= limit) { limitReached = true; break; }
         std::error_code sec;
         if (fs::is_directory(dirPath + "/" + e, sec) && !sec)
            results.push_back(e + "/");
         else if (sec) continue;         // stat 不到的条目跳过
         else results.push_back(e);
      }
      if (results.empty())
         return ToolOr<ToolOutcome>::success({"(empty directory)"});

      std::string raw;
      for (auto const& r : results) { raw += r; raw += '\n'; }
      raw.pop_back();
      auto const trunc = truncateHead(raw, {INT64_MAX, kDefaultMaxBytes});
      std::string output = trunc.content;

      folly::dynamic details = nullptr;
      std::vector<std::string> notices;
      if (limitReached) {
         notices.push_back(std::format(
            "{} entries limit reached. Use limit={} for more",
            limit, limit * 2));
         details = folly::dynamic::object;
         details["entryLimitReached"] = static_cast<int64_t>(limit);
      }
      if (trunc.truncated) {
         notices.push_back(std::format("{} limit reached",
                                       formatSize(kDefaultMaxBytes)));
         if (!details.isObject()) details = folly::dynamic::object;
         details["truncation"] = truncationJson(trunc);
      }
      if (!notices.empty()) {
         output += "\n\n[";
         for (size_t i = 0; i < notices.size(); ++i) {
            if (i) output += ". ";
            output += notices[i];
         }
         output += "]";
      }
      return ToolOr<ToolOutcome>::success(
         {std::move(output), {}, std::move(details), false});
   }

   // ==================================================================
   // 遍历 (grep/find 共用): gitignore + .git 跳过 + glob 过滤
   // ==================================================================
   void walkFiles(std::string const& root,
                  std::optional<std::string> const& globFilter,
                  std::vector<std::string>& out) {
      GitIgnoreStack ignore;
      walkDir(root, "", ignore, [&](fs::path const& p, std::string_view rel) {
         if (globFilter) {
            auto const relPosix = std::string{rel};
            auto const base = fs::path{relPosix}.filename().string();
            if (!globMatch(*globFilter, relPosix)
                && !globMatch(*globFilter, base)
                && !globMatch("**/" + *globFilter, relPosix))
               return;
         }
         out.push_back(p.string());
      });
   }

   void collectMatches(std::string const& root, std::string_view rel,
                       std::string const& pattern, bool fullPath,
                       GitIgnoreStack& ignore, std::vector<std::string>& out,
                       size_t limit) {
      walkDir(root, "", ignore, [&](fs::path const& p, std::string_view r) {
         if (out.size() >= limit) return;
         auto const relPosix = std::string{r};
         bool hit = fullPath
            ? globMatch(pattern, relPosix)
            : globMatch(pattern, p.filename().string());
         if (hit) out.push_back(relPosix);
      });
   }

   template <class F>
   void walkDir(std::string const& root, std::string const& rel,
                GitIgnoreStack& ignore, F&& emit) {
      std::error_code ec;
      auto const dir = rel.empty() ? root : root + "/" + rel;
      // 本层 .gitignore 入栈 (baseDir 前缀作用域天然限到子树)
      auto const gi = dir + "/.gitignore";
      if (fs::exists(gi, ec)) ignore.load(gi, rel);
      std::vector<fs::path> ents;
      for (auto const& de : fs::directory_iterator(dir, ec))
         ents.push_back(de.path());
      std::ranges::sort(ents);
      for (auto const& p : ents) {
         auto const name = p.filename().string();
         auto const childRel = rel.empty() ? name : rel + "/" + name;
         bool const isDir = fs::is_directory(p, ec);
         if (isDir && name == ".git") continue;         // rg/fd 恒跳过 .git
         if (ignore.isIgnored(childRel, isDir)) continue;
         if (isDir) walkDir(root, childRel, ignore, emit);
         else emit(p, childRel);
      }
   }

   // ==================================================================
   // schema / description (pi typebox 输出的等价 JSON)
   // ==================================================================
   static auto truncationJson(TruncationResult const& t) -> folly::dynamic {
      return folly::dynamic::object
         ("truncated", t.truncated)
         ("truncatedBy", t.truncatedBy.empty() ? folly::dynamic(nullptr)
                                               : folly::dynamic{t.truncatedBy})
         ("totalLines", t.totalLines)("totalBytes", t.totalBytes)
         ("outputLines", t.outputLines)("outputBytes", t.outputBytes)
         ("lastLinePartial", t.lastLinePartial)
         ("firstLineExceedsLimit", t.firstLineExceedsLimit)
         ("maxLines", t.maxLines)("maxBytes", t.maxBytes);
   }

   static auto readDescription() -> std::string {
      return std::format(
         "Read the contents of a file. Supports text files and images (jpg, "
         "png, gif, webp, bmp). Images are sent as attachments. For text "
         "files, output is truncated to {} lines or {}KB (whichever is hit "
         "first). Use offset/limit for large files. When you need the full "
         "file, continue with offset until complete.",
         kDefaultMaxLines, kDefaultMaxBytes / 1024);
   }
   static auto bashDescription(std::string_view shellName) -> std::string {
      return std::format(
         "Execute a {} command in the current working directory. Returns "
         "stdout and stderr. Output is truncated to last {} lines or {}KB "
         "(whichever is hit first). If truncated, full output is saved to a "
         "temp file. Optionally provide a timeout in seconds.",
         shellName, kDefaultMaxLines, kDefaultMaxBytes / 1024);
   }
   static auto grepDescription() -> std::string {
      return std::format(
         "Search file contents for a pattern. Returns matching lines with "
         "file paths and line numbers. Respects .gitignore. Output is "
         "truncated to {} matches or {}KB (whichever is hit first). Long "
         "lines are truncated to {} chars.",
         kGrepDefaultLimit, kDefaultMaxBytes / 1024, kGrepMaxLineLength);
   }
   static auto findDescription() -> std::string {
      return std::format(
         "Search for files by glob pattern. Returns matching file paths "
         "relative to the search directory. Respects .gitignore. Output is "
         "truncated to {} results or {}KB (whichever is hit first).",
         kFindDefaultLimit, kDefaultMaxBytes / 1024);
   }
   static auto lsDescription() -> std::string {
      return std::format(
         "List directory contents. Returns entries sorted alphabetically, "
         "with '/' suffix for directories. Includes dotfiles. Output is "
         "truncated to {} entries or {}KB (whichever is hit first).",
         kLsDefaultLimit, kDefaultMaxBytes / 1024);
   }

   static constexpr double kBashMaxTimeoutMs = 2147483647.0;
   static constexpr int64_t kGrepDefaultLimit = 100;
   static constexpr int64_t kFindDefaultLimit = 1000;
   static constexpr int64_t kLsDefaultLimit   = 500;

   inline static const std::string kReadSchema = R"schema({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to read (relative or absolute)"},"offset":{"type":"number","description":"Line number to start reading from (1-indexed)"},"limit":{"type":"number","description":"Maximum number of lines to read"}},"required":["path"]})schema";
   inline static const std::string kBashSchema = R"schema({"type":"object","properties":{"command":{"type":"string","description":"Shell command to execute"},"timeout":{"type":"number","description":"Timeout in seconds (optional, no default timeout)"}},"required":["command"]})schema";
   inline static const std::string kWriteSchema = R"schema({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to write (relative or absolute)"},"content":{"type":"string","description":"Content to write to the file"}},"required":["path","content"]})schema";
   inline static const std::string kEditSchema = R"schema({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to edit (relative or absolute)"},"edits":{"type":"array","items":{"type":"object","properties":{"oldText":{"type":"string","description":"Exact text for one targeted replacement. It must be unique in the original file and must not overlap with any other edits[].oldText in the same call."},"newText":{"type":"string","description":"Replacement text for this targeted edit."}},"required":["oldText","newText"]},"description":"One or more targeted replacements. Each edit is matched against the original file, not incrementally. Do not include overlapping or nested edits. If two changes touch the same block or nearby lines, merge them into one edit instead."}},"required":["path","edits"]})schema";
   inline static const std::string kGrepSchema = R"schema({"type":"object","properties":{"pattern":{"type":"string","description":"Search pattern (regex or literal string)"},"path":{"type":"string","description":"Directory or file to search (default: current directory)"},"glob":{"type":"string","description":"Filter files by glob pattern, e.g. '*.ts' or '**/*.spec.ts'"},"ignoreCase":{"type":"boolean","description":"Case-insensitive search (default: false)"},"literal":{"type":"boolean","description":"Treat pattern as literal string instead of regex (default: false)"},"context":{"type":"number","description":"Number of lines to show before and after each match (default: 0)"},"limit":{"type":"number","description":"Maximum number of matches to return (default: 100)"}},"required":["pattern"]})schema";
   inline static const std::string kFindSchema = R"schema({"type":"object","properties":{"pattern":{"type":"string","description":"Glob pattern to match files, e.g. '*.ts', '**/*.json', or 'src/**/*.spec.ts'"},"path":{"type":"string","description":"Directory to search in (default: current directory)"},"limit":{"type":"number","description":"Maximum number of results (default: 1000)"}},"required":["pattern"]})schema";
   inline static const std::string kLsSchema = R"schema({"type":"object","properties":{"path":{"type":"string","description":"Directory to list (default: current directory)"},"limit":{"type":"number","description":"Maximum number of entries to return (default: 500)"}},"required":[]})schema";

   static constexpr const char* kEditDescription =
      "Edit a single file using exact text replacement. Every edits[].oldText "
      "must match a unique, non-overlapping region of the original file. If "
      "two changes affect the same block or nearby lines, merge them into "
      "one edit instead of emitting overlapping edits. Do not include large "
      "unchanged regions just to connect distant changes.";
   static constexpr const char* kWriteDescription =
      "Write content to a file. Creates the file if it doesn't exist, "
      "overwrites if it does. Automatically creates parent directories.";

   static auto trimNum(double v) -> std::string {
      auto const i = static_cast<int64_t>(v);
      return v == i ? std::to_string(i) : std::format("{}", v);
   }

   std::string     cwd_;
   ToolSessionEnv  env_;
   std::string     shellPath_;
   std::function<bool()> abortCheck_;
};

} // namespace pi::iface::adapter
