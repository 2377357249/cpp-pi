#pragma once

#include <format>
#include <fstream>
#include <string>

#include <pi/domain/session/Session.h>
#include <pi/interface/rpc/JsonCodec.h>

namespace pi::iface {

// ---------------------------------------------------------------------------
// 会话导出 (pi export_html 命令): 简版 HTML —— 两前端共用.
// path 为空时默认 pi-session-<id>.html. 返回实际写出的路径.
// ---------------------------------------------------------------------------
inline auto exportSessionHtml(domain::Session const& session,
                              std::string            path)
   -> std::string {
   if (path.empty())
      path = std::format("pi-session-{}.html", session.id());
   std::ofstream f{path};
   f << "<!doctype html><meta charset=utf-8><title>"
     << session.id() << "</title><pre>\n";
   for (auto const& m : session.messages())
      f << "[" << rpc::roleToString(m.role) << "] " << m.text << "\n";
   return path;
}

} // namespace pi::iface
