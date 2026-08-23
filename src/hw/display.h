#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32/Light_PWM.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

#include "hw/board_pins.h"

namespace hw {

/** Set an expander output pin (EXIO1..EXIO8) high or low. */
void expanderSet(uint8_t pin, bool high);

/**
 * Bring up I2C, the expander and the panel at `pclk_mhz`. Returns false if the
 * panel fails.
 */
bool displayInit(uint8_t pclk_mhz);

/** Backlight, 0-100. */
void displayBrightness(uint8_t percent);

} // namespace hw

/**
 * ST7701 with this panel's own initialisation sequence.
 *
 * LovyanGFX's stock Panel_ST7701 carries a generic register list; this display
 * needs the Waveshare values (power, VCOM and gamma), without which the panel
 * comes up blank white. The bytes below are the panel vendor's - the same
 * sequence Espressif ships in its board support for this board.
 *
 * CS lives on the I2C expander rather than a GPIO, so it is asserted around
 * initialisation by hand.
 */
class PanelST7701Waveshare : public lgfx::Panel_ST7701_Base {
public:
    bool init(bool use_reset) override;

protected:
    const uint8_t *getInitCommands(uint8_t listno) const override;
};

/**
 * The panel behind an ST7701 with its CS on the expander.
 *
 * LovyanGFX drives this board's RGB bus with a double framebuffer in PSRAM
 * (use_psram = 2) and hands us sprites to compose into, which is the
 * arrangement this panel is stable with.
 */
class LGFX : public lgfx::LGFX_Device {
public:
    LGFX();
    lgfx::Bus_RGB _bus;
    PanelST7701Waveshare _panel;
    lgfx::Light_PWM _light;
};

extern LGFX display;
