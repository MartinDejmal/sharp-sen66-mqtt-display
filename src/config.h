#pragma once

#include <Arduino.h>

struct AppConfig {
  // --- Wi-Fi ---
  String wifiSsid = "";
  String wifiPassword = "";

  // --- MQTT ---
  String mqttServer = "";
  int mqttPort = 1883;
  String mqttUser = "";
  String mqttPassword = "";
  String mqttClientId = "sharp-sen66";
  String mqttBaseTopic = "sharp";       // prefix vsech topicu zarizeni
  String haPrefix = "homeassistant";    // discovery prefix HA (viz MQTT integrace)

  // --- Web UI ---
  String webUser = "admin";
  String webPassword = "";              // prazdne = bez autentizace

  // --- TMEP.cz ---
  String tmepDomain = "";
  String tmepParams = "tempV=*TEMP*&humV=*HUM*&pm1=*PM1*&pm2=*PM2*&pm4=*PM4*&pm10=*PM10*&voc=*VOC*&nox=*NOX*&co2=*CO2*";

  // --- Intervaly (ms) ---
  unsigned long mqttPublishInterval = 10000;
  unsigned long tmepRequestInterval = 60000;
  unsigned long displayRefreshInterval = 2000;
  unsigned long mqttWarmupDelay = 60000;

  // --- Senzor / displej ---
  float temperatureOffset = -2.0f;
  uint8_t displayRotation = 2;
  bool displayInvertRequested = false;
};

bool loadConfig(AppConfig& config);
bool saveConfig(AppConfig& config);  // sanitizuje predanou strukturu na miste
bool validateConfig(const AppConfig& config);
