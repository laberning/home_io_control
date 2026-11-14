#pragma once

/// @file heating_test_data.h
/// @brief Heater register replies shared by the heater tests.

#include <cstdint>
#include <vector>

namespace heating_test_data {

/// The model name an Atlantic Adelis reports in register 0x0119 (corpus atlantic_adelis_probe_register_01_sweep).
inline const std::vector<uint8_t> ADELIS_NAME_REPLY = {0x0C, 0x60, 0x01, 0x19, 'A', 'D', 'E', 'L', 'I',
                                                       'S',  ' ',  '2',  ' ',  'D', 'I', 'G', 'I', 'T'};

}  // namespace heating_test_data
