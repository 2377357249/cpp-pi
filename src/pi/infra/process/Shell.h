#pragma once

#include <array>
#include <cstdio>
#include <string>
#include <sys/wait.h>

namespace pi::infra {

// ---------------------------------------------------------------------------
// 技术设施: 同步执行 shell 命令, stderr 并入 stdout, 输出截断到 maxBytes.
// ---------------------------------------------------------------------------
struct ShellResult {
   std::string output;
   int         exitCode;
   bool        truncated;
};

inline auto runShell(std::string const& cmd, size_t maxBytes = 8192)
   -> ShellResult {
   std::string           out;
   std::array<char, 4096> buf{};
   bool                  truncated = false;

   FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
   if (!pipe) return {"popen failed", 127, false};

   while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
      out += buf.data();
      if (out.size() > maxBytes) {
         out.resize(maxBytes);
         truncated = true;
         break;
      }
   }
   auto rc = ::pclose(pipe);
   return {std::move(out), WEXITSTATUS(rc), truncated};
}

} // namespace pi::infra
