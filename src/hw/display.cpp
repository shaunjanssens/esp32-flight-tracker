#include "hw/display.h"

#include <Arduino.h>
#include <Wire.h>

#include "app/log.h"

namespace hw {
namespace {

uint8_t g_expander_shadow = 0;

constexpr uint8_t expanderBit(uint8_t pin)
{
    return (uint8_t)(1u << (pin - 1));
}

void expanderWrite(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(kExpanderAddress);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

void expanderInit()
{
    Wire.begin((int)kI2cSda, (int)kI2cScl);
    Wire.setClock(kI2cFreqHz);

    // Everything released, then all pins set to outputs.
    g_expander_shadow = expanderBit(kExpanderLcdReset) | expanderBit(kExpanderTouchReset) |
                        expanderBit(kExpanderLcdCs) | expanderBit(kExpanderSdCs);
    expanderWrite(kExpanderOutputReg, g_expander_shadow);
    expanderWrite(kExpanderConfigReg, 0x00);
    expanderSet(kExpanderBuzzer, false);
}

void panelReset()
{
    expanderSet(kExpanderLcdReset, false);
    delay(10);
    expanderSet(kExpanderLcdReset, true);
    delay(50);
}

} // namespace

void expanderSet(uint8_t pin, bool high)
{
    if (high) {
        g_expander_shadow |= expanderBit(pin);
    } else {
        g_expander_shadow &= (uint8_t)~expanderBit(pin);
    }
    expanderWrite(kExpanderOutputReg, g_expander_shadow);
}

bool displayInit(uint8_t pclk_mhz)
{
    expanderInit();
    panelReset();

    // The bus is configured at construction; override the clock now that
    // settings have been read, before the panel is started.
    if (pclk_mhz >= 8 && pclk_mhz <= 30) {
        auto cfg = display._bus.config();
        cfg.freq_write = (uint32_t)pclk_mhz * 1000000u;
        display._bus.config(cfg);
    }

    // CS is asserted inside PanelST7701Waveshare::init().
    const bool ok = display.init();

    if (!ok) {
        app::logf("[display] panel init failed");
        return false;
    }

    display.setColorDepth(16);
    display.setRotation(0);
    display.fillScreen(TFT_BLACK);
    const uint32_t pclk = display._bus.config().freq_write;
    app::logf("[display] ST7701 up, %dx%d, pclk %u MHz (~%u Hz refresh)",
              display.width(), display.height(), (unsigned)(pclk / 1000000),
              (unsigned)(pclk / (548u * 499u)));
    return true;
}

void displayBrightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    display.setBrightness((uint8_t)(percent * 255 / 100));
}

} // namespace hw

bool PanelST7701Waveshare::init(bool use_reset)
{
    hw::expanderSet(hw::kExpanderLcdCs, false);
    const bool ok = lgfx::Panel_ST7701_Base::init(use_reset);
    hw::expanderSet(hw::kExpanderLcdCs, true);
    return ok;
}

const uint8_t *PanelST7701Waveshare::getInitCommands(uint8_t listno) const
{
    static constexpr const uint8_t list0[] = {
        0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x10,
        0xC0, 2, 0x3B, 0x00,
        0xC1, 2, 0x0B, 0x02,
        0xC2, 2, 0x07, 0x02,
        0xCC, 1, 0x10,
        0xCD, 1, 0x08,
        0xB0, 16, 0x00, 0x11, 0x16, 0x0E, 0x11, 0x06, 0x05, 0x09,
                  0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18,
        0xB1, 16, 0x00, 0x11, 0x16, 0x0E, 0x11, 0x07, 0x05, 0x09,
                  0x09, 0x21, 0x05, 0x13, 0x11, 0x2A, 0x31, 0x18,

        0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x11,
        0xB0, 1, 0x6D,
        0xB1, 1, 0x37,
        0xB2, 1, 0x81,
        0xB3, 1, 0x80,
        0xB5, 1, 0x43,
        0xB7, 1, 0x85,
        0xB8, 1, 0x20,
        0xC1, 1, 0x78,
        0xC2, 1, 0x78,
        0xD0, 1, 0x88,
        0xE0, 3, 0x00, 0x00, 0x02,
        0xE1, 11, 0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00,
                  0x00, 0x20, 0x20,
        0xE2, 13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                  0x00, 0x00, 0x00, 0x00, 0x00,
        0xE3, 4, 0x00, 0x00, 0x11, 0x00,
        0xE4, 2, 0x22, 0x00,
        0xE5, 16, 0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0,
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xE6, 4, 0x00, 0x00, 0x11, 0x00,
        0xE7, 2, 0x22, 0x00,
        0xE8, 16, 0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0,
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xEB, 7, 0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00,
        0xED, 16, 0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF,
                  0xFF, 0x54, 0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF,
        0xEF, 6, 0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F,

        0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x13,
        0xEF, 1, 0x08,

        0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x00,
        0x36, 1, 0x00,
        0x3A, 1, 0x66,
        0x11, CMD_INIT_DELAY, 255,
        0x20, CMD_INIT_DELAY, 120,
        0x29, 0,
        0xFF, 0xFF,
    };

    return (listno == 0) ? list0 : nullptr;
}

LGFX display;

LGFX::LGFX()
{
    {
        auto cfg = _panel.config();
        cfg.pin_rst = -1;                 // reset is on the expander
        cfg.memory_width  = hw::kScreenWidth;
        cfg.memory_height = hw::kScreenHeight;
        cfg.panel_width   = hw::kScreenWidth;
        cfg.panel_height  = hw::kScreenHeight;
        cfg.offset_x = 0;
        cfg.offset_y = 0;
        _panel.config(cfg);
    }
    {
        auto cfg = _panel.config_detail();
        cfg.pin_cs = -1;                  // driven through the expander
        cfg.pin_sclk = (int)hw::kLcdSclk;
        cfg.pin_mosi = (int)hw::kLcdMosi;
        cfg.use_psram = 2;                // two framebuffers in PSRAM
        _panel.config_detail(cfg);
    }
    {
        auto cfg = _bus.config();
        cfg.panel = &_panel;
        cfg.pin_d0  = (int)hw::kLcdData[0];
        cfg.pin_d1  = (int)hw::kLcdData[1];
        cfg.pin_d2  = (int)hw::kLcdData[2];
        cfg.pin_d3  = (int)hw::kLcdData[3];
        cfg.pin_d4  = (int)hw::kLcdData[4];
        cfg.pin_d5  = (int)hw::kLcdData[5];
        cfg.pin_d6  = (int)hw::kLcdData[6];
        cfg.pin_d7  = (int)hw::kLcdData[7];
        cfg.pin_d8  = (int)hw::kLcdData[8];
        cfg.pin_d9  = (int)hw::kLcdData[9];
        cfg.pin_d10 = (int)hw::kLcdData[10];
        cfg.pin_d11 = (int)hw::kLcdData[11];
        cfg.pin_d12 = (int)hw::kLcdData[12];
        cfg.pin_d13 = (int)hw::kLcdData[13];
        cfg.pin_d14 = (int)hw::kLcdData[14];
        cfg.pin_d15 = (int)hw::kLcdData[15];
        cfg.pin_henable = (int)hw::kLcdDe;
        cfg.pin_vsync   = (int)hw::kLcdVsync;
        cfg.pin_hsync   = (int)hw::kLcdHsync;
        cfg.pin_pclk    = (int)hw::kLcdPclk;
        cfg.freq_write  = hw::kPclkHz;
        cfg.hsync_pulse_width = hw::kHsyncPulseWidth;
        cfg.hsync_back_porch  = hw::kHsyncBackPorch;
        cfg.hsync_front_porch = hw::kHsyncFrontPorch;
        cfg.vsync_pulse_width = hw::kVsyncPulseWidth;
        cfg.vsync_back_porch  = hw::kVsyncBackPorch;
        cfg.vsync_front_porch = hw::kVsyncFrontPorch;
        cfg.hsync_polarity = 0;
        cfg.vsync_polarity = 0;
        cfg.pclk_active_neg = 0;
        cfg.pclk_idle_high = 0;
        cfg.de_idle_high = 0;
        _bus.config(cfg);
        _panel.setBus(&_bus);
    }
    {
        auto cfg = _light.config();
        cfg.pin_bl = (int)hw::kBacklight;
        cfg.freq = hw::kBacklightPwmHz;
        cfg.pwm_channel = 1;
        cfg.invert = false;
        _light.config(cfg);
        _panel.setLight(&_light);
    }
    setPanel(&_panel);
}
