## @file
## @brief ESPHome hub schema and code generation for Home IO Control.
## @ingroup hioc_codegen
##
## Defines the top-level ``home_io_control:`` schema (CONFIG_SCHEMA, FINAL_VALIDATE_SCHEMA)
## and the hub's to_code(). The parts it composes live in leaf modules that never import
## this file: hub_names.py (YAML keys, C++ handles), hub_validators.py (option tables and
## field validators), oneway_controllers.py, hub_entities.py and lr1121_update_codegen.py.
## ESPHome requires the component's schema, to_code() and metadata to live here.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import spi
from esphome.const import CONF_ID

from . import tuning as tuning_module
from .hub_names import (
    CONF_ACCEPT_FOREIGN_PAIRING,
    CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID,
    CONF_BUSY_PIN,
    CONF_DIAGNOSTIC_PROBES,
    CONF_DIO0_PIN,
    CONF_DIO1_PIN,
    CONF_DIO4_PIN,
    CONF_DISCOVER_AND_PAIR_BUTTON,
    CONF_EXPOSED_SENDERS,
    CONF_FEM,
    CONF_FEM_EN_PIN,
    CONF_FEM_PA_PIN,
    CONF_LR1121_FIRMWARE_UPDATE,
    CONF_NODE_ID,
    CONF_ONEWAY_CONTROLLERS,
    CONF_PA_PIN,
    CONF_RADIO_TYPE,
    CONF_RECOVER_ONEWAY_KEY,
    CONF_RECOVER_ONEWAY_KEY_SWITCH_ID,
    CONF_RST_PIN,
    CONF_SCAN_PAIRED_DEVICES_BUTTON,
    CONF_SYSTEM_KEY,
    CONF_TCXO_VOLTAGE,
    CONF_TX_POWER,
    CONF_VFEM_PIN,
    IOHomeAcceptForeignPairingSwitch,
    IOHomeControlComponent,
    IOHomeRecoverOneWayKeySwitch,
)
from .hub_validators import (
    FEM_PROFILES,
    PA_PIN_OPTIONS,
    RADIO_TYPE_OPTIONS,
    SPI_RADIO_TYPES,
    TCXO_VOLTAGE_OPTIONS,
    validate_fem,
    validate_radio_transport,
    validate_device_id,
    validate_node_id,
    validate_system_key,
)
from .lr1121_update_codegen import (
    LR1121_FIRMWARE_UPDATE_SCHEMA,
    create_lr1121_firmware_update,
    validate_lr1121_firmware_update,
)
from .oneway_controllers import (
    ONEWAY_CONTROLLER_SCHEMA,
    create_oneway_controller_entities,
    final_validate_oneway_controller_addresses,
    validate_oneway_controllers,
    oneway_controller_expression,
)
from .hub_entities import (
    create_discover_and_pair_button,
    create_hub_arming_switch,
    create_scan_paired_devices_button,
    inject_accept_foreign_pairing_switch_id,
    inject_discover_and_pair_button_id,
    inject_discover_and_pair_result_sensor_id,
    inject_recover_oneway_key_switch_id,
    inject_scan_paired_devices_button_id,
)

# Re-exported for the platform modules (button.py, cover.py, platform_common.py, ...), which
# import these names from the package itself. CONF_DISCOVER_AND_PAIR_BUTTON,
# IOHomeControlComponent and validate_device_id are re-exported too; this file already imports
# them above for its own use.
from .hub_names import (  # noqa: F401
    CONF_HOME_IO_CONTROL_ID,
    CONF_LOW_POWER,
    IOHomeDiscoverButton,
    IOHomePairingResultTextSensor,
    home_io_control_ns,
)
from .hub_validators import (  # noqa: F401
    device_type_expression,
    inherit_esphome_device,
    validate_device_type,
    validate_linked_remote_entry,
    validate_status_poll_interval,
)

DEPENDENCIES = ["api", "spi"]
AUTO_LOAD = ["button", "climate", "cover", "light", "lock", "number", "select", "sensor", "switch", "text_sensor"]
MULTI_CONF = False


FINAL_VALIDATE_SCHEMA = final_validate_oneway_controller_addresses


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(IOHomeControlComponent),
            cv.Required(CONF_RST_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_DIO0_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_DIO4_PIN): pins.internal_gpio_input_pin_schema,
            # The chip's IRQ line: SX1262's DIO1, or LR1121's DIO9
            cv.Optional(CONF_DIO1_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_BUSY_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_NODE_ID): validate_node_id,
            cv.Required(CONF_SYSTEM_KEY): cv.sensitive(validate_system_key),
            cv.Optional(CONF_TX_POWER, default=17): cv.int_range(min=0, max=22),
            cv.Optional(CONF_PA_PIN, default="BOOST"): cv.enum(
                PA_PIN_OPTIONS, upper=True
            ),
            cv.Required(CONF_RADIO_TYPE): cv.enum(RADIO_TYPE_OPTIONS, lower=True),
            cv.Optional(CONF_FEM_EN_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_VFEM_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_FEM_PA_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_FEM, default="none"): cv.one_of(*FEM_PROFILES, lower=True),
            cv.Optional(CONF_TCXO_VOLTAGE, default="1_8V"): cv.enum(
                TCXO_VOLTAGE_OPTIONS, upper=True
            ),
            cv.Optional(CONF_EXPOSED_SENDERS, default=[]): cv.ensure_list(
                validate_device_id
            ),
            cv.Optional(CONF_ACCEPT_FOREIGN_PAIRING, default=False): cv.boolean,
            cv.Optional(CONF_RECOVER_ONEWAY_KEY, default=False): cv.boolean,
            cv.Optional(CONF_SCAN_PAIRED_DEVICES_BUTTON, default=False): cv.boolean,
            cv.Optional(CONF_DISCOVER_AND_PAIR_BUTTON, default=False): cv.boolean,
            cv.Optional(CONF_ONEWAY_CONTROLLERS, default=[]): cv.ensure_list(
                ONEWAY_CONTROLLER_SCHEMA
            ),
            cv.Optional(CONF_DIAGNOSTIC_PROBES, default=False): cv.boolean,
            cv.Optional(CONF_LR1121_FIRMWARE_UPDATE): LR1121_FIRMWARE_UPDATE_SCHEMA,
            cv.Optional(tuning_module.CONF_TUNING): tuning_module.TUNING_CONFIG_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(spi.spi_device_schema(False, 8e6, "mode0")),
    inject_accept_foreign_pairing_switch_id,
    inject_recover_oneway_key_switch_id,
    inject_scan_paired_devices_button_id,
    inject_discover_and_pair_button_id,
    inject_discover_and_pair_result_sensor_id,
    validate_oneway_controllers,
    validate_lr1121_firmware_update,
    validate_radio_transport,
    validate_fem,
)


async def to_code(config):
    # Hub-level management actions and result events are compiled behind native API
    # feature flags. Home IO Control enables the required compile-time switches here
    # so users only need a normal `api:` block in YAML.
    # ESPHome 2026.x additionally gates user-defined actions behind
    # USE_API_USER_DEFINED_ACTIONS.
    cg.add_define("USE_API_USER_DEFINED_ACTIONS")
    cg.add_define("USE_API_CUSTOM_SERVICES")
    cg.add_define("USE_API_HOMEASSISTANT_SERVICES")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if config[CONF_RADIO_TYPE] in SPI_RADIO_TYPES:
        await spi.register_spi_device(var, config)

    rst_pin = await cg.gpio_pin_expression(config[CONF_RST_PIN])
    cg.add(var.set_rst_pin(rst_pin))

    if CONF_DIO0_PIN in config:
        dio0_pin = await cg.gpio_pin_expression(config[CONF_DIO0_PIN])
        cg.add(var.set_dio0_pin(dio0_pin))

    if CONF_DIO4_PIN in config:
        dio4_pin = await cg.gpio_pin_expression(config[CONF_DIO4_PIN])
        cg.add(var.set_dio4_pin(dio4_pin))

    if CONF_DIO1_PIN in config:
        dio1_pin = await cg.gpio_pin_expression(config[CONF_DIO1_PIN])
        cg.add(var.set_dio1_pin(dio1_pin))

    if CONF_BUSY_PIN in config:
        busy_pin = await cg.gpio_pin_expression(config[CONF_BUSY_PIN])
        cg.add(var.set_busy_pin(busy_pin))

    if CONF_FEM_EN_PIN in config:
        fem_en_pin = await cg.gpio_pin_expression(config[CONF_FEM_EN_PIN])
        cg.add(var.set_fem_en_pin(fem_en_pin))

    if CONF_VFEM_PIN in config:
        vfem_pin = await cg.gpio_pin_expression(config[CONF_VFEM_PIN])
        cg.add(var.set_vfem_pin(vfem_pin))

    if CONF_FEM_PA_PIN in config:
        fem_pa_pin = await cg.gpio_pin_expression(config[CONF_FEM_PA_PIN])
        cg.add(var.set_fem_pa_pin(fem_pa_pin))

    cg.add(var.set_fem_profile(FEM_PROFILES[config[CONF_FEM]]))

    cg.add(var.set_node_id(config[CONF_NODE_ID]))
    cg.add(var.set_system_key(config[CONF_SYSTEM_KEY]))
    cg.add(var.set_tx_power(config[CONF_TX_POWER]))
    cg.add(var.set_pa_pin(config[CONF_PA_PIN]))

    cg.add(var.set_radio_type(config[CONF_RADIO_TYPE]))

    cg.add(var.set_tcxo_voltage(config[CONF_TCXO_VOLTAGE]))

    for sender_id in config[CONF_EXPOSED_SENDERS]:
        cg.add(var.add_exposed_sender(sender_id))

    for identity in config[CONF_ONEWAY_CONTROLLERS]:
        cg.add(
            var.add_oneway_controller(
                oneway_controller_expression(identity, config[CONF_NODE_ID])
            )
        )
        await create_oneway_controller_entities(identity, var)

    if config[CONF_ACCEPT_FOREIGN_PAIRING]:
        await create_hub_arming_switch(
            config,
            var,
            cls=IOHomeAcceptForeignPairingSwitch,
            id_key=CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID,
            name="Recover System Key",
        )

    if config[CONF_RECOVER_ONEWAY_KEY]:
        await create_hub_arming_switch(
            config,
            var,
            cls=IOHomeRecoverOneWayKeySwitch,
            id_key=CONF_RECOVER_ONEWAY_KEY_SWITCH_ID,
            name="Recover 1W Controller Key",
        )

    if config[CONF_SCAN_PAIRED_DEVICES_BUTTON]:
        await create_scan_paired_devices_button(config, var)

    if config[CONF_DISCOVER_AND_PAIR_BUTTON]:
        await create_discover_and_pair_button(config, var)

    cg.add(var.set_diagnostic_probes_enabled(config[CONF_DIAGNOSTIC_PROBES]))

    if CONF_LR1121_FIRMWARE_UPDATE in config:
        await create_lr1121_firmware_update(config, var)

    if tuning_module.CONF_TUNING in config:
        await tuning_module.to_code(config[tuning_module.CONF_TUNING], var)
