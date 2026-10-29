#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/LogMacros.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/RotatingFileSink.h>
#include <quill/sinks/Sink.h>

namespace pi::log {

// ---------------------------------------------------------------------------
// quill 初始化: 后台线程异步日志.
//   - console sink 走 stderr (不污染 RPC 模式的 stdout 协议通道)
//   - rotating file sink: 每分钟滚动 + 保留 30 个备份 ≈ 保留最近 30 分钟日志
// ---------------------------------------------------------------------------
inline quill::Logger* init(std::string const& logDir = "logs") {
   quill::Backend::start();

   std::filesystem::create_directories(logDir);

   quill::ConsoleSinkConfig consoleCfg;
   consoleCfg.set_stream("stderr");

   std::vector<std::shared_ptr<quill::Sink>> sinks;
   sinks.push_back(quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
      "console", consoleCfg));

   quill::RotatingFileSinkConfig cfg;
   cfg.set_open_mode('a');
   cfg.set_rotation_frequency_and_interval('M', 1);
   cfg.set_max_backup_files(30);
   sinks.push_back(quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
      logDir + "/cpp-pi.log", cfg));

   return quill::Frontend::create_or_get_logger("cpp-pi", std::move(sinks));
}

inline quill::Logger* logger() {
   return quill::Frontend::get_logger("cpp-pi");
}

} // namespace pi::log
