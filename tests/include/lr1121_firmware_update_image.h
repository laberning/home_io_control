#pragma once

// Host-test stand-in for the header components/home_io_control/__init__.py generates at ESPHome
// build time (see _render_lr1121_firmware_header() there) from a real fetched firmware image.
// That header is never checked into the repo -- it's regenerated per-build from whatever
// lr1121_firmware_update: source: the YAML config names -- so host builds (no ESPHome codegen
// step) need a stand-in with the identical symbol shape. Picked up via -Itests/include, which
// the Makefile's INCLUDES searches after components/home_io_control/, exactly like the
// esphome/core/*.h stubs elsewhere in this directory.
//
// A small, deliberately non-trivial synthetic image (8 words, not all zero/identical) so tests
// exercising write_image()'s chunking/byte-exactness have real content to assert against.
//
// TARGET_VERSION is 0x0104 (requires bootloader 0x2101, per LR1121_KNOWN_BOOTLOADER_REQUIREMENTS)
// rather than an earlier release requiring 0x2100: this is the only target value for which the
// bootloader-rewrite post-filter's AVAILABLE path is reachable through the *real*
// cache_lr1121_flash_verdict_()/lr1121_bootloader_upgrade_path() wiring in host tests, since every
// other known target is already satisfied by bootloader 0x2100.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace home_io_control {

inline const uint32_t LR1121_FIRMWARE_UPDATE_IMAGE[] = {
    0x3C148020, 0x00000001, 0x00000002, 0x00000003, 0x00000004, 0x00000005, 0x00000006, 0x00000007,
};
inline constexpr size_t LR1121_FIRMWARE_UPDATE_IMAGE_WORDS = 8;
inline constexpr uint16_t LR1121_FIRMWARE_UPDATE_TARGET_VERSION = 0x0104;

}  // namespace home_io_control
}  // namespace esphome
