#pragma once

#include <cstdint>
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

/// Host stand-in for ESPHome's random_uint32(): returns test_random::set()'s value (0 by default),
/// so schedules that draw jitter are deterministic in tests.
uint32_t random_uint32();

namespace test_random {
/// Make every following random_uint32() call return @p value.
void set(uint32_t value);
}  // namespace test_random

}  // namespace esphome