#pragma once

#include <unistd.h>
#include <cstdio>
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
               + " --mode rpc --provider mock < " + in + " > " + out
               + " 2>/dev/null";
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

} // namespace pi::it
