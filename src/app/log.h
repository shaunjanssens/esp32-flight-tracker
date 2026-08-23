#pragma once

#include <Arduino.h>

namespace app {

/**
 * Print to the serial console and keep the last few kB in RAM, so a device
 * with no cable attached can still be asked what went wrong (GET /api/log).
 */
void logf(const char *format, ...);

/** The retained log, oldest line first. */
String logDump();

} // namespace app
