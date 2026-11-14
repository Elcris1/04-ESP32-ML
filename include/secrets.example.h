#pragma once

// Copy this file to secrets.h and replace the placeholders with your local
// credentials. Keep secrets.h out of version control.
constexpr const char *WIFI_SSID = "YOUR_WIFI_SSID";
constexpr const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Optional MQTT credentials. Leave empty if the broker requires no login.
constexpr const char *MQTT_USER = "";
constexpr const char *MQTT_PASSWORD = "";
constexpr const char *MQTT_CLIENT_ID = "ESP32";
