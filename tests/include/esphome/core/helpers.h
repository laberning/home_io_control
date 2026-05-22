#pragma once

#include <string>

namespace esphome {

class StringRef {
 public:
  StringRef() = default;
  StringRef(const char *value) : value_(value != nullptr ? value : "") {}
  StringRef(const std::string &value) : value_(value) {}

  const std::string &str() const { return this->value_; }
  operator const std::string &() const { return this->value_; }

 private:
  std::string value_;
};

uint32_t fnv1_hash(const char *str);

}  // namespace esphome