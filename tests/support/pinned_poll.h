#pragma once

/// @file pinned_poll.h
/// @brief The registers a heater kind's poll reads, by when it reads them, for the tests that pin them.

#include "proto_device_types.h"
#include "proto_heating_profiles.h"

#include "reading_store.h"
#include "test_helpers.h"

#include <vector>

namespace test {

using namespace esphome::home_io_control;

/// The registers a profile's poll reads, by when it reads them.
struct PinnedPoll {
  std::vector<DeviceRegister> every_poll;           ///< Read every poll; the first failed read ends the poll.
  std::vector<DeviceRegister> every_poll_may_miss;  ///< Read every poll after those; a miss keeps the value.
  std::vector<DeviceRegister> once_after_boot;      ///< Read until the heater has answered once.
};

/// @return The poll of a device of this kind with nothing asked for and nothing read yet, from whatever the table
///         selects it by.
inline PinnedPoll pinned_poll(DeviceType type, uint8_t subtype, DeviceModel model) {
  IoDevice dev;
  test::set_kind(dev, type, subtype, model);
  return {test::polled_registers(dev, ReadingPoll::EVERY_POLL),
          test::polled_registers(dev, ReadingPoll::EVERY_POLL_MAY_MISS),
          test::polled_registers(dev, ReadingPoll::ONCE_AFTER_BOOT)};
}

}  // namespace test
