#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <random>
#include <string>

namespace pi::infra {

// ---------------------------------------------------------------------------
// 标识/时间戳工具 (pi uuidv7 / ISO8601 / 文件名时间戳).
// ---------------------------------------------------------------------------
inline auto nowMs() -> int64_t {
   return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ISO 8601 UTC, 毫秒: "2025-12-09T00:52:54.397Z" (session 条目 timestamp)
inline auto nowIso() -> std::string {
   auto const tp = std::chrono::system_clock::now();
   auto const s  = std::chrono::floor<std::chrono::seconds>(tp);
   auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp - s)
                      .count();
   return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03}Z", s, ms);
}

// 会话文件名时间戳: "2025-12-09T00-52-54-397Z"
inline auto fileStamp() -> std::string {
   auto const tp = std::chrono::system_clock::now();
   auto const s  = std::chrono::floor<std::chrono::seconds>(tp);
   auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp - s)
                      .count();
   return std::format("{:%Y-%m-%dT%H-%M-%S}-{:03}Z", s, ms);
}

// uuidv7: 48-bit unix ms + version 7 + rand_a, variant 10 + rand_b
inline auto uuidv7() -> std::string {
   constexpr uint64_t kVersion7   = 0x7000;
   constexpr uint64_t kVariant10  = 0x8000;
   constexpr uint64_t kRandA12    = 0x0fff;
   constexpr uint64_t kRandB14    = 0x3fff;
   static thread_local std::mt19937_64 rng{std::random_device{}()};
   auto const ms = static_cast<uint64_t>(nowMs());
   auto const r1 = rng(), r2 = rng();
   return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:04x}{:08x}",
                      ms >> 16, ms & 0xffff,
                      kVersion7 | (r1 & kRandA12),
                      kVariant10 | ((r1 >> 12) & kRandB14),
                      (r2 >> 32) & 0xffff, r2 & 0xffffffffULL);
}

// n 个 hex 字符的随机串 (临时文件名等)
inline auto randomHex(size_t n) -> std::string {
   static thread_local std::mt19937_64 rng{std::random_device{}()};
   std::string out;
   out.reserve(n);
   while (out.size() < n)
      out += std::format("{:016x}", rng());
   out.resize(n);
   return out;
}

} // namespace pi::infra
