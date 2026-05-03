#pragma once

namespace esphome {

class Application {
 public:
  void feed_wdt() {}
};

extern Application App;

}  // namespace esphome
