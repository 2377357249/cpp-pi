#pragma once

#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <folly/json/json.h>

namespace pi::it {

// ---------------------------------------------------------------------------
// 集成测试 harness: 把 NDJSON 命令喂给真实的 `cpp-pi --mode rpc` 进程,
// 收 stdout 里每一行 JSON. 黑盒, 粒度 = 外部输入消息 -> 输出消息字段.
// ---------------------------------------------------------------------------
struct RpcHarness {
   static auto run(std::string const& ndjsonInput) -> std::vector<folly::dynamic> {
      auto base = std::string{"/tmp/cpp-pi-it-"} + std::to_string(::getpid())
                + "-" + std::to_string(counter_++);
      auto in  = base + ".in";
      auto out = base + ".out";

      { std::ofstream{in} << ndjsonInput; }

      auto cmd = std::string{CPP_PI_BIN}
               + " --mode rpc --provider mock --no-session < " + in
               + " > " + out + " 2>/dev/null";
      auto rc  = std::system(cmd.c_str());
      if (rc != 0) return {};

      std::vector<folly::dynamic> frames;
      std::ifstream f{out};
      for (std::string line; std::getline(f, line);) {
         if (!line.empty()) frames.push_back(folly::parseJson(line));
      }
      return frames;
   }

   // 过滤出某 type 的帧
   static auto ofType(std::vector<folly::dynamic> const& frames,
                      std::string_view type) -> std::vector<folly::dynamic> {
      std::vector<folly::dynamic> out;
      for (auto const& f : frames)
         if (f.count("type") && f["type"].getString() == type) out.push_back(f);
      return out;
   }

   // 过滤出 command == c 的 response 帧
   static auto response(std::vector<folly::dynamic> const& frames,
                        std::string_view command) -> folly::dynamic {
      for (auto const& f : frames)
         if (f.count("type") && f["type"].getString() == "response" &&
             f.count("command") && f["command"].getString() == command)
            return f;
      return folly::dynamic{};
   }

private:
   inline static int counter_ = 0;
};

// ---------------------------------------------------------------------------
// 交互式 harness: 逐条发命令、逐帧读响应 —— 运行期 id (entry/since) 测试需要.
// ---------------------------------------------------------------------------
class RpcProc {
public:
   static auto spawn() -> RpcProc {
      int inP[2], outP[2];
      RpcProc p;
      if (::pipe(inP) != 0 || ::pipe(outP) != 0) return p;
      p.pid_ = ::fork();
      if (p.pid_ == 0) {
         ::dup2(inP[0], 0);  ::dup2(outP[1], 1);
         ::close(inP[0]);  ::close(inP[1]);
         ::close(outP[0]); ::close(outP[1]);
         ::execl(CPP_PI_BIN, CPP_PI_BIN, "--mode", "rpc",
                 "--provider", "mock", "--no-session", nullptr);
         _exit(127);
      }
      ::close(inP[0]);
      ::close(outP[1]);
      p.inW_  = ::fdopen(inP[1], "w");
      p.outR_ = ::fdopen(outP[0], "r");
      return p;
   }

   auto send(std::string const& jsonLine) -> void {
      std::fputs(jsonLine.c_str(), inW_);
      std::fputc('\n', inW_);
      std::fflush(inW_);
   }

   auto readLine() -> std::string {
      char*  line = nullptr;
      size_t cap  = 0;
      auto   n    = ::getline(&line, &cap, outR_);
      std::string s;
      if (n > 0) s.assign(line, static_cast<size_t>(n));
      if (line) std::free(line);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
         s.pop_back();
      return s;
   }

   auto readFrame() -> folly::dynamic {
      auto line = readLine();
      return line.empty() ? folly::dynamic{} : folly::parseJson(line);
   }

   // 读到首个 type==want 的帧, 返回它; 之前的帧丢弃.
   auto readUntil(std::string_view type) -> folly::dynamic {
      for (int i = 0; i < 5000; ++i) {
         auto f = readFrame();
         if (f.count("type") && f["type"].getString() == type) return f;
      }
      return folly::dynamic{};
   }

   auto readResponse(std::string_view command) -> folly::dynamic {
      for (;;) {
         auto f = readFrame();
         if (!f.isObject()) return f;
         if (f.count("type") && f["type"].getString() == "response"
             && f.count("command") && f["command"].getString() == command)
            return f;
      }
   }

   ~RpcProc() {
      if (inW_)  std::fclose(inW_);     // 关 stdin -> 子进程退出
      if (outR_) std::fclose(outR_);
      if (pid_ > 0) { int st; ::waitpid(pid_, &st, 0); }
   }

   RpcProc(RpcProc&& o) noexcept
      : inW_(o.inW_), outR_(o.outR_), pid_(o.pid_) {
      o.inW_ = nullptr; o.outR_ = nullptr; o.pid_ = -1;
   }
   RpcProc(RpcProc const&) = delete;

private:
   RpcProc() = default;
   FILE* inW_  = nullptr;
   FILE* outR_ = nullptr;
   pid_t pid_  = -1;
};

} // namespace pi::it
