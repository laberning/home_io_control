import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_CONFIG

from . import home_io_control_ns, IOHomeControlComponent, CONF_HOME_IO_CONTROL_ID

DEPENDENCIES = ["home_io_control"]

IOHomeDiscoverButton = home_io_control_ns.class_(
    "IOHomeDiscoverButton", button.Button, cg.Component
)

CONFIG_SCHEMA = (
    button.button_schema(
        IOHomeDiscoverButton,
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    .extend(
        {
            cv.GenerateID(CONF_HOME_IO_CONTROL_ID): cv.use_id(
                IOHomeControlComponent
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await button.register_button(var, config)

    parent = await cg.get_variable(config[CONF_HOME_IO_CONTROL_ID])
    cg.add(var.set_parent(parent))
