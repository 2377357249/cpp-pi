#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pi::iface::adapter {

// ---------------------------------------------------------------------------
// pi utils/mime.ts 1:1 — 4KB 嗅探 jpeg/png/gif/webp/bmp.
// ---------------------------------------------------------------------------
inline constexpr size_t kImageSniffBytes = 4100;

namespace detail {

inline auto startsWith(std::vector<uint8_t> const& b, size_t off,
                       std::initializer_list<uint8_t> sig) -> bool {
   if (off + sig.size() > b.size()) return false;
   size_t i = 0;
   for (auto const c : sig) if (b[off + i++] != c) return false;
   return true;
}

inline auto startsWithAscii(std::vector<uint8_t> const& b, size_t off,
                            std::string_view ascii) -> bool {
   if (off + ascii.size() > b.size()) return false;
   for (size_t i = 0; i < ascii.size(); ++i)
      if (b[off + i] != static_cast<uint8_t>(ascii[i])) return false;
   return true;
}

inline auto u32be(std::vector<uint8_t> const& b, size_t off) -> uint32_t {
   return (static_cast<uint32_t>(b[off]) << 24)
        | (static_cast<uint32_t>(b[off + 1]) << 16)
        | (static_cast<uint32_t>(b[off + 2]) << 8)
        |  static_cast<uint32_t>(b[off + 3]);
}
inline auto u32le(std::vector<uint8_t> const& b, size_t off) -> uint32_t {
   return  static_cast<uint32_t>(b[off])
        | (static_cast<uint32_t>(b[off + 1]) << 8)
        | (static_cast<uint32_t>(b[off + 2]) << 16)
        | (static_cast<uint32_t>(b[off + 3]) << 24);
}
inline auto u16le(std::vector<uint8_t> const& b, size_t off) -> uint16_t {
   return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

inline auto isAnimatedPng(std::vector<uint8_t> const& b) -> bool {
   size_t off = 8;   // PNG 签名长
   while (off + 8 <= b.size()) {
      auto const len = u32be(b, off);
      if (startsWithAscii(b, off + 4, "acTL")) return true;
      if (startsWithAscii(b, off + 4, "IDAT")) return false;
      auto const next = off + 8 + len + 4;
      if (next <= off || next > b.size()) return false;
      off = next;
   }
   return false;
}

inline auto isPng(std::vector<uint8_t> const& b) -> bool {
   return b.size() >= 16 && u32be(b, 8) == 13 && startsWithAscii(b, 12, "IHDR");
}

inline auto isBmp(std::vector<uint8_t> const& b) -> bool {
   if (b.size() < 26) return false;
   auto const declaredSize = u32le(b, 2);
   auto const pixelOffset  = u32le(b, 10);
   auto const dibSize      = u32le(b, 14);
   if (declaredSize != 0 && declaredSize < 26) return false;
   if (pixelOffset < 14 + dibSize) return false;
   if (declaredSize != 0 && pixelOffset >= declaredSize) return false;

   uint32_t planes, bpp;
   if (dibSize == 12) {
      planes = u16le(b, 22); bpp = u16le(b, 24);
   } else if (dibSize >= 40 && dibSize <= 124) {
      if (b.size() < 30) return false;
      planes = u16le(b, 26); bpp = u16le(b, 28);
   } else {
      return false;
   }
   return planes == 1
          && (bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32);
}

} // namespace detail

inline auto detectSupportedImageMimeType(std::vector<uint8_t> const& b)
   -> std::optional<std::string> {
   using namespace detail;
   if (b.size() >= 4 && startsWith(b, 0, {0xff, 0xd8, 0xff}))
      return b[3] == 0xf7 ? std::nullopt
                          : std::optional<std::string>{"image/jpeg"};
   if (startsWith(b, 0, {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a}))
      return isPng(b) && !isAnimatedPng(b)
                ? std::optional<std::string>{"image/png"} : std::nullopt;
   if (startsWithAscii(b, 0, "GIF87a") || startsWithAscii(b, 0, "GIF89a"))
      return "image/gif";
   if (startsWithAscii(b, 0, "RIFF") && startsWithAscii(b, 8, "WEBP"))
      return "image/webp";
   if (startsWithAscii(b, 0, "BM") && isBmp(b)) return "image/bmp";
   return std::nullopt;
}

inline auto detectSupportedImageMimeTypeFromFile(std::string const& path)
   -> std::optional<std::string> {
   std::ifstream f{path, std::ios::binary};
   if (!f) return std::nullopt;
   std::vector<uint8_t> buf(kImageSniffBytes);
   f.read(reinterpret_cast<char*>(buf.data()), kImageSniffBytes);
   buf.resize(static_cast<size_t>(f.gcount()));
   return detectSupportedImageMimeType(buf);
}

// base64 (image 块编码)
inline auto base64Encode(std::string_view data) -> std::string {
   static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   std::string out;
   out.reserve((data.size() + 2) / 3 * 4);
   for (size_t i = 0; i < data.size(); i += 3) {
      uint32_t n = static_cast<uint8_t>(data[i]) << 16;
      if (i + 1 < data.size()) n |= static_cast<uint8_t>(data[i + 1]) << 8;
      if (i + 2 < data.size()) n |= static_cast<uint8_t>(data[i + 2]);
      out += kTable[(n >> 18) & 63];
      out += kTable[(n >> 12) & 63];
      out += i + 1 < data.size() ? kTable[(n >> 6) & 63] : '=';
      out += i + 2 < data.size() ? kTable[n & 63] : '=';
   }
   return out;
}

} // namespace pi::iface::adapter
