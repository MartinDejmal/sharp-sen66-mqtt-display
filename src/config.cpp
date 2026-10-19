#include "config.h"

#include <Preferences.h>
#include <cmath>

namespace {
constexpr const char* NS = "appcfg";

String sanitizeTopic(String topic, const char* fallback) {
  topic.trim();
  while (topic.endsWith("/")) topic.remove(topic.length() - 1);
  while (topic.startsWith("/")) topic.remove(0, 1);
  if (topic.length() == 0) topic = fallback;
  return topic;
}

void sanitize(AppConfig& cfg) {
  if (cfg.mqttPort < 1 || cfg.mqttPort > 65535) cfg.mqttPort = 1883;
  if (cfg.displayRotation > 3) cfg.displayRotation = 2;
  if (cfg.displayRefreshInterval < 500) cfg.displayRefreshInterval = 2000;
  if (cfg.mqttPublishInterval < 1000) cfg.mqttPublishInterval = 10000;
  if (cfg.tmepRequestInterval < 1000) cfg.tmepRequestInterval = 60000;
  if (cfg.mqttWarmupDelay < 1000) cfg.mqttWarmupDelay = 60000;
  if (!isfinite(cfg.temperatureOffset)) cfg.temperatureOffset = -2.0f;
  if (cfg.temperatureOffset < -20.0f) cfg.temperatureOffset = -20.0f;
  if (cfg.temperatureOffset > 20.0f) cfg.temperatureOffset = 20.0f;
  if (cfg.mqttClientId.length() == 0) cfg.mqttClientId = "sharp-sen66";
  if (cfg.webUser.length() == 0) cfg.webUser = "admin";
  cfg.mqttBaseTopic = sanitizeTopic(cfg.mqttBaseTopic, "sharp");
  cfg.haPrefix = sanitizeTopic(cfg.haPrefix, "homeassistant");
}
}  // namespace

bool validateConfig(const AppConfig& cfg) {
  if (cfg.mqttPort < 1 || cfg.mqttPort > 65535) return false;
  if (cfg.displayRotation > 3) return false;
  if (cfg.displayRefreshInterval < 500) return false;
  if (cfg.mqttPublishInterval < 1000) return false;
  if (cfg.tmepRequestInterval < 1000) return false;
  if (cfg.mqttWarmupDelay < 1000) return false;
  if (!isfinite(cfg.temperatureOffset)) return false;
  if (cfg.temperatureOffset < -20.0f || cfg.temperatureOffset > 20.0f) return false;
  // mqttServer smi byt prazdny: zarizeni pak jede jen s displejem + TMEP
  return true;
}

bool loadConfig(AppConfig& config) {
  Preferences pref;
  if (!pref.begin(NS, true)) {
    sanitize(config);
    return false;
  }

  config.wifiSsid = pref.getString("wifi_ssid", config.wifiSsid);
  config.wifiPassword = pref.getString("wifi_pass", config.wifiPassword);

  config.mqttServer = pref.getString("mqtt_server", config.mqttServer);
  config.mqttPort = pref.getInt("mqtt_port", config.mqttPort);
  config.mqttUser = pref.getString("mqtt_user", config.mqttUser);
  config.mqttPassword = pref.getString("mqtt_pass", config.mqttPassword);
  config.mqttClientId = pref.getString("mqtt_client", config.mqttClientId);
  config.mqttBaseTopic = pref.getString("mqtt_base", config.mqttBaseTopic);
  config.haPrefix = pref.getString("ha_prefix", config.haPrefix);

  config.webUser = pref.getString("web_user", config.webUser);
  config.webPassword = pref.getString("web_pass", config.webPassword);

  config.tmepDomain = pref.getString("tmep_domain", config.tmepDomain);
  config.tmepParams = pref.getString("tmep_params", config.tmepParams);

  config.mqttPublishInterval = pref.getULong("mqtt_pub_ms", config.mqttPublishInterval);
  config.tmepRequestInterval = pref.getULong("tmep_req_ms", config.tmepRequestInterval);
  config.displayRefreshInterval = pref.getULong("disp_ref_ms", config.displayRefreshInterval);
  config.mqttWarmupDelay = pref.getULong("mqtt_warmup", config.mqttWarmupDelay);

  config.temperatureOffset = pref.getFloat("temp_offset", config.temperatureOffset);
  config.displayRotation = pref.getUChar("disp_rot", config.displayRotation);
  config.displayInvertRequested = pref.getBool("disp_inv", config.displayInvertRequested);

  pref.end();
  sanitize(config);
  return true;
}

bool saveConfig(AppConfig& config) {
  sanitize(config);
  if (!validateConfig(config)) return false;
  const AppConfig& cfg = config;

  Preferences pref;
  if (!pref.begin(NS, false)) return false;

  pref.putString("wifi_ssid", cfg.wifiSsid);
  pref.putString("wifi_pass", cfg.wifiPassword);

  pref.putString("mqtt_server", cfg.mqttServer);
  pref.putInt("mqtt_port", cfg.mqttPort);
  pref.putString("mqtt_user", cfg.mqttUser);
  pref.putString("mqtt_pass", cfg.mqttPassword);
  pref.putString("mqtt_client", cfg.mqttClientId);
  pref.putString("mqtt_base", cfg.mqttBaseTopic);
  pref.putString("ha_prefix", cfg.haPrefix);

  pref.putString("web_user", cfg.webUser);
  pref.putString("web_pass", cfg.webPassword);

  pref.putString("tmep_domain", cfg.tmepDomain);
  pref.putString("tmep_params", cfg.tmepParams);

  pref.putULong("mqtt_pub_ms", cfg.mqttPublishInterval);
  pref.putULong("tmep_req_ms", cfg.tmepRequestInterval);
  pref.putULong("disp_ref_ms", cfg.displayRefreshInterval);
  pref.putULong("mqtt_warmup", cfg.mqttWarmupDelay);

  pref.putFloat("temp_offset", cfg.temperatureOffset);
  pref.putUChar("disp_rot", cfg.displayRotation);
  pref.putBool("disp_inv", cfg.displayInvertRequested);

  pref.end();
  return true;
}
