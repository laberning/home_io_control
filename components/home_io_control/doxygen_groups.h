#pragma once

/// @file doxygen_groups.h
/// @brief Doxygen group definitions for the Home IO Control component.
/// @ingroup hioc_arch

/// @defgroup hioc_arch Architecture Layers
/// @brief High-level documentation groups for the Home IO Control component.
///
/// These groups organize the generated documentation around the main layers of the
/// project instead of only by file names. This makes it easier to move between the
/// protocol core, radio backends, and ESPHome integration points.

/// @defgroup hioc_protocol Protocol Layer
/// @ingroup hioc_arch
/// @brief Frame model, crypto helpers, command builders, the node-type and capability table, and the heater and hot
/// water codecs.

/// @defgroup hioc_radio Radio Driver Layer
/// @ingroup hioc_arch
/// @brief The radio driver interface, the SX1276/SX1262/LR1121 drivers, the shared software PHY and the idle preamble
/// hold.

/// @defgroup hioc_hub Controller Layer
/// @ingroup hioc_arch
/// @brief Hub orchestration and its collaborators: the exchange and pairing engines, queued operations and polls,
/// heating, one-way handling, management actions, the entity ports, and the pure decision helpers.

/// @defgroup hioc_entities ESPHome Integration Layer
/// @ingroup hioc_arch
/// @brief ESPHome-facing entity backends and YAML/code-generation modules.

/// @defgroup hioc_tuning Pairing/Radio Diagnostics Tuning
/// @ingroup hioc_hub
/// @brief Runtime-tunable pairing and radio diagnostics layer and its Home Assistant entities.

/// @defgroup hioc_platforms Runtime Entities
/// @ingroup hioc_entities
/// @brief C++ entity implementations exposed to Home Assistant through ESPHome.

/// @defgroup hioc_codegen Python Code Generation
/// @ingroup hioc_entities
/// @brief ESPHome Python schema validation and code-generation modules.