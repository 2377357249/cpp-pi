#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#   include <crt_externs.h>
#endif

namespace pi::infra {

namespace detail {
inline auto environPtr() -> char** {
#if defined(__APPLE__)
   return *_NSGetEnviron();
#else
   return ::environ;
#endif
}
} // namespace detail

// ---------------------------------------------------------------------------
// 技术设施: spawn shell/程序并捕获合并输出 (stdout+stderr), 支持超时与
// 进程组终止. 对齐 pi createLocalShellOperations 的语义:
//   - detached 子进程独占进程组, kill = 杀整棵树
//   - 退出码: 无 exit code 时按信号约定 128+signo, 再兜底 1
// ---------------------------------------------------------------------------
struct ShellResult {
   std::string output;
   int         exitCode;
   bool        truncated;
};

struct SpawnOpts {
   std::string cwd;                                  // 空 = 继承
   std::vector<std::pair<std::string, std::string>> env; // 叠加到 environ
   int64_t     timeoutMs = -1;                        // <0 = 无超时
   std::function<void(std::string_view)> onData;      // 输出到达回调(合并流)
   std::function<bool()> aborted;                     // 外部中止钩子
};

struct SpawnResult {
   int     exitCode = -1;    // -1 = 无退出码
   bool    timedOut = false;
   bool    aborted  = false;
   std::string spawnError;   // 非空 = spawn 本身失败
};

// envp 由 environ + opts.env 合成
inline auto buildEnvp(std::vector<std::pair<std::string, std::string>> const& env)
   -> std::vector<std::string> {
   std::vector<std::string> merged;
   for (char** e = detail::environPtr(); e && *e; ++e) merged.emplace_back(*e);
   for (auto const& [k, v] : env) merged.emplace_back(k + "=" + v);
   return merged;
}

// 低层 spawn: argv 形式, 合并 stdout+stderr 到一条管道按到达序回调.
// 同步; onData 在同线程被调用.
inline auto spawnMerged(std::vector<std::string> const& argv, SpawnOpts const& opts)
   -> SpawnResult {
   SpawnResult res;

   int outP[2];
   if (::pipe(outP) != 0) {
      res.spawnError = std::strerror(errno);
      return res;
   }

   auto const timeout = std::chrono::milliseconds{opts.timeoutMs};
   auto const start   = std::chrono::steady_clock::now();

   pid_t const pid = ::fork();
   if (pid == 0) {
      // 子: 独占进程组 (等价 pi detached:true), 双端合并到 outP[1]
      ::setsid();
      ::dup2(outP[1], STDOUT_FILENO);
      ::dup2(outP[1], STDERR_FILENO);
      ::close(outP[0]);
      ::close(outP[1]);
      if (!opts.cwd.empty()) ::chdir(opts.cwd.c_str());

      auto envs   = buildEnvp(opts.env);
      auto cargv  = std::vector<char*>{};
      auto cenv   = std::vector<char*>{};
      cargv.reserve(argv.size() + 1);
      for (auto const& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
      cargv.push_back(nullptr);
      cenv.reserve(envs.size() + 1);
      for (auto const& e : envs) cenv.push_back(const_cast<char*>(e.c_str()));
      cenv.push_back(nullptr);
      ::execve(argv[0].c_str(), cargv.data(), cenv.data());
      _exit(127);
   }
   ::close(outP[1]);

   if (pid < 0) {
      ::close(outP[0]);
      res.spawnError = std::strerror(errno);
      return res;
   }

   auto killTree = [&] {
      ::kill(-pid, SIGTERM);          // 进程组整树
      // 给 200ms 优雅期再 SIGKILL
      auto const grace = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds{200};
      while (std::chrono::steady_clock::now() < grace) {
         if (::waitpid(pid, nullptr, WNOHANG) == pid) return;
         ::usleep(5000);
      }
      ::kill(-pid, SIGKILL);
   };

   pollfd pfd{outP[0], POLLIN, 0};
   std::array<char, 8192> buf{};
   bool done = false;

   while (!done) {
      if (opts.aborted && opts.aborted()) { res.aborted = true; killTree(); }
      if (timeout.count() > 0
          && std::chrono::steady_clock::now() - start > timeout) {
         res.timedOut = true;
         killTree();
      }
      int const rc = ::poll(&pfd, 1, 50);
      if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
         auto const n = ::read(outP[0], buf.data(), buf.size());
         if (n <= 0) { done = true; }
         else if (opts.onData) opts.onData({buf.data(), static_cast<size_t>(n)});
      } else if (rc < 0 && errno != EINTR) {
         done = true;
      } else if (rc == 0) {
         // 无数据: 看子进程还在不在 (防子进程已退出但管道有残留句柄)
         if (::waitpid(pid, nullptr, WNOHANG) == pid) {
            // 再 drain 一轮
            for (;;) {
               auto const n = ::read(outP[0], buf.data(), buf.size());
               if (n <= 0) break;
               if (opts.onData) opts.onData({buf.data(), static_cast<size_t>(n)});
            }
            done = true;
         }
      }
   }
   ::close(outP[0]);

   int status = 0;
   ::waitpid(pid, &status, 0);
   if (WIFEXITED(status))        res.exitCode = WEXITSTATUS(status);
   else if (WIFSIGNALED(status)) res.exitCode = 128 + WTERMSIG(status);
   return res;
}

// shell -c 形式便捷入口
inline auto spawnShell(std::string const& command,
                       std::string const& shellPath, SpawnOpts const& opts)
   -> SpawnResult {
   return spawnMerged({shellPath, "-c", command}, opts);
}

// 兼容旧接口: 同步执行并截断到 maxBytes.
inline auto runShell(std::string const& cmd, size_t maxBytes = 8192)
   -> ShellResult {
   std::string out;
   bool        truncated = false;
   auto        r = spawnShell(cmd, "/bin/bash", {
      .onData = [&](std::string_view chunk) {
         out += chunk;
         if (out.size() > maxBytes) { out.resize(maxBytes); truncated = true; }
      },
   });
   if (r.exitCode < 0) return {"spawn failed: " + r.spawnError, 127, false};
   return {std::move(out), r.exitCode, truncated};
}

} // namespace pi::infra
