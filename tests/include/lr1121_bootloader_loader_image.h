#pragma once

// Host-test stand-in for the header components/home_io_control/__init__.py generates at ESPHome
// build time (see _render_lr1121_bootloader_loader_header() there) from a real fetched
// `lr1121_loader_2100.bin` image. Mirrors tests/include/lr1121_firmware_update_image.h's role for
// the transceiver image -- see that file's comment for why a stand-in is needed at all.
//
// LR1121_BOOTLOADER_LOADER_FW is 0x2100 (LR1121_LOADER_2100), matching the loader image Semtech
// actually publishes: Semtech's equality rule requires this to equal the currently-running
// bootloader version before the loader may be used at all, so a chip
// at bootloader 0x2100 is the only case in which the loader image is ever applicable.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace home_io_control {

inline const uint32_t LR1121_BOOTLOADER_LOADER_IMAGE[] = {
    0x3C148020,
    0x10000001,
    0x10000002,
    0x10000003,
};
inline constexpr size_t LR1121_BOOTLOADER_LOADER_IMAGE_WORDS = 4;
inline constexpr uint16_t LR1121_BOOTLOADER_LOADER_FW = 0x2100;

}  // namespace home_io_control
}  // namespace esphome
