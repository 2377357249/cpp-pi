#pragma once

#include <string>

namespace pi::domain {

// 值对象: 一个具体模型(某 provider 下的某个 id)
struct Model {
   std::string provider;
   std::string id;
};

} // namespace pi::domain
