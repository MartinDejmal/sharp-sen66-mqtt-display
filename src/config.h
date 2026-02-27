#pragma once

#include <Arduino.h>

struct AppConfig {
  String wifiSsid = "";
  String wifiPassword = "";

  String mqttServer = "192.168.0.1";
  int mqttPort = 1883;
  String mqttUser = "";
  String mqttPassword = "";
  String mqttClientId = "sharp";

  unsigned long mqttPublishInterval = 10000;
  unsigned long displayRefreshInterval = 2000;
  unsigned long mqttWarmupDelay = 60000;

  uint8_t displayRotation = 2;
  bool displayInvertRequested = false;
};

bool loadConfig(AppConfig& config);
bool saveConfig(const AppConfig& config);
bool validateConfig(const AppConfig& config);
