#pragma once

#include <Arduino.h>

namespace net {

enum class WifiState : uint8_t {
    Idle,
    Connecting,
    Connected,
    PortalActive,     // no credentials, or connecting failed: own AP is up
};

/**
 * Connect with the stored credentials, or raise the setup AP if there are none.
 * Starts a task on core 0 that owns the web server, mDNS and OTA.
 */
bool wifiBegin();

WifiState wifiState();

/** SSID we are connected to, or the AP name while the portal is up. */
const char *wifiNetwork();

/** Local address as text ("" when not connected). */
const char *wifiAddress();

int wifiRssi();

/** Forget the stored credentials and reboot into the setup portal. */
void wifiForget();

} // namespace net
