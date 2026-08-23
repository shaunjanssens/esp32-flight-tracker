#pragma once

#include <driver/gpio.h>
#include <stdint.h>

/**
 * Waveshare ESP32-S3-Touch-LCD-2.1 hardware map.
 *
 * The panel is an ST7701 driven over the 16-bit RGB interface, with its
 * command channel on a 3-wire SPI and its reset and chip-select behind a
 * TCA9554 I2C expander. Touch (CST820), the RTC and the IMU share I2C0 with
 * that expander.
 */
namespace hw {

constexpr int kScreenWidth  = 480;
constexpr int kScreenHeight = 480;

// ST7701 command channel (3-wire SPI)
constexpr gpio_num_t kLcdSclk = GPIO_NUM_2;
constexpr gpio_num_t kLcdMosi = GPIO_NUM_1;

// RGB timing pins
constexpr gpio_num_t kLcdHsync = GPIO_NUM_38;
constexpr gpio_num_t kLcdVsync = GPIO_NUM_39;
constexpr gpio_num_t kLcdDe    = GPIO_NUM_40;
constexpr gpio_num_t kLcdPclk  = GPIO_NUM_41;

// RGB565 data lines, blue 0-4, green 0-5, red 0-4
constexpr gpio_num_t kLcdData[16] = {
    GPIO_NUM_5,  GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_47,
    GPIO_NUM_21, GPIO_NUM_14, GPIO_NUM_13, GPIO_NUM_12,
    GPIO_NUM_11, GPIO_NUM_10, GPIO_NUM_9,  GPIO_NUM_46,
    GPIO_NUM_3,  GPIO_NUM_8,  GPIO_NUM_18, GPIO_NUM_17,
};

// Panel timing. 16 MHz over 548 x 499 total pixels is about 58 Hz.
constexpr uint32_t kPclkHz = 16000000;
constexpr int kHsyncPulseWidth = 8;
constexpr int kHsyncBackPorch  = 10;
constexpr int kHsyncFrontPorch = 50;
constexpr int kVsyncPulseWidth = 3;
constexpr int kVsyncBackPorch  = 8;
constexpr int kVsyncFrontPorch = 8;

// Shared I2C bus
constexpr gpio_num_t kI2cScl = GPIO_NUM_7;
constexpr gpio_num_t kI2cSda = GPIO_NUM_15;
constexpr uint32_t   kI2cFreqHz = 400000;

// CST820 capacitive touch (single touch only - no pinch gestures possible)
constexpr gpio_num_t kTouchInt = GPIO_NUM_16;
constexpr uint8_t kTouchAddress = 0x15;
constexpr uint8_t kTouchReadReg = 0x01;
constexpr uint8_t kTouchSleepReg = 0xFE;

// TCA9554 expander, pins numbered EXIO1..EXIO8
constexpr uint8_t kExpanderAddress = 0x20;
constexpr uint8_t kExpanderOutputReg = 0x01;
constexpr uint8_t kExpanderConfigReg = 0x03;
constexpr uint8_t kExpanderLcdReset = 1;
constexpr uint8_t kExpanderTouchReset = 2;
constexpr uint8_t kExpanderLcdCs = 3;
constexpr uint8_t kExpanderSdCs = 4;
constexpr uint8_t kExpanderBuzzer = 8;

constexpr gpio_num_t kBacklight = GPIO_NUM_6;
constexpr uint32_t kBacklightPwmHz = 20000;

} // namespace hw
