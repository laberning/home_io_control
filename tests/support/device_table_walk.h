#pragma once

/// @file device_table_walk.h
/// @brief Test helper: visit every (node type, subtype) the node-type table has an exact row for.
/// @ingroup hioc_protocol

#include "proto_device_types.h"

namespace test {

/// Widest node type on the wire (10 bits).
constexpr unsigned MAX_NODE_TYPE = 0x3FF;
/// Widest subtype on the wire (6 bits).
constexpr unsigned MAX_SUBTYPE = 0x3F;

/// @brief Call `visit(const DeviceTypeInfo &)` for every exact row of the table.
/// @details The table is private to its translation unit, so the walk probes the lookup over the
///          whole wire range; a row the lookup can reach is a row the walk visits.
template<typename Visitor> void for_each_table_row(Visitor visit) {
  using namespace esphome::home_io_control;
  for (unsigned type = 0; type <= MAX_NODE_TYPE; type++) {
    for (unsigned subtype = 0; subtype <= MAX_SUBTYPE; subtype++) {
      const DeviceTypeInfo *row =
          find_exact_device_type_info(static_cast<DeviceType>(type), static_cast<uint8_t>(subtype));
      if (row != nullptr)
        visit(*row);
    }
  }
}

}  // namespace test
