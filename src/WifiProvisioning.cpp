#include "WifiProvisioning.h"

#include <WiFi.h>

namespace {
constexpr byte DNS_PORT = 53;
constexpr const char* AP_PASSWORD = "";  // open AP for easier onboarding

// Jak casto z AP fallbacku zkouset puvodni Wi-Fi. Restart routeru trva typicky
// 1-3 minuty - drive zarizeni v AP rezimu zustalo natrvalo az do restartu.
constexpr unsigned long STA_PROBE_INTERVAL_MS = 120000UL;

String buildApName() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String("SharpDisplay-") + suffix;
}
}  // namespace

void WifiProvisioning::begin(AppConfig* config, unsigned long connectTimeoutMs) {
  config_ = config;
  connectTimeoutMs_ = connectTimeoutMs;

  if (!config_ || !hasStoredCredentials()) {
    Serial.println("WIFI PROV: Chybi ulozene SSID, start AP captive");
    startCaptiveMode();
    return;
  }

  if (!connectStaBlocking(connectTimeoutMs_)) {
    Serial.println("WIFI PROV: STA connect timeout, fallback do captive");
    startCaptiveMode();
  }
}

void WifiProvisioning::process() {
  if (state_ == WIFI_AP_CAPTIVE && captiveRunning_) {
    dnsServer_.processNextRequest();
    processCaptiveStaRetry();
    return;
  }

  if (!config_ || !hasStoredCredentials()) return;

  if (state_ == WIFI_STA_CONNECTED && WiFi.status() != WL_CONNECTED) {
    state_ = WIFI_STA_CONNECTING;
    staConnectStartedAt_ = millis();
    lastReconnectAttemptAt_ = 0;
    Serial.println("WIFI PROV: Spojeni ztraceno, zkousim reconnect");
  }

  if (state_ != WIFI_STA_CONNECTING) return;

  if (WiFi.status() == WL_CONNECTED) {
    state_ = WIFI_STA_CONNECTED;
    Serial.printf("WIFI PROV: Znovu pripojeno, IP: %s\n", WiFi.localIP().toString().c_str());
    return;
  }

  unsigned long now = millis();
  if (lastReconnectAttemptAt_ == 0 || now - lastReconnectAttemptAt_ >= 5000UL) {
    lastReconnectAttemptAt_ = now;
    WiFi.reconnect();
    Serial.println("WIFI PROV: WiFi.reconnect()");
  }

  if (now - staConnectStartedAt_ >= connectTimeoutMs_) {
    Serial.println("WIFI PROV: Reconnect timeout, prepinam do captive");
    startCaptiveMode();
  }
}

bool WifiProvisioning::saveCredentialsAndConnect(const String& ssid, const String& password, String& statusMsg) {
  if (!config_) {
    statusMsg = "Interni chyba: config neni inicializovan";
    return false;
  }

  AppConfig updated = *config_;
  updated.wifiSsid = ssid;
  updated.wifiPassword = password;

  if (updated.wifiSsid.length() == 0) {
    statusMsg = "SSID nesmi byt prazdne";
    return false;
  }

  if (!saveConfig(updated)) {
    statusMsg = "Nepodarilo se ulozit WiFi konfiguraci";
    return false;
  }

  *config_ = updated;
  stopCaptiveMode();

  bool ok = connectStaBlocking(connectTimeoutMs_);
  if (ok) {
    statusMsg = "WiFi ulozena, pripojeno";
    return true;
  }

  startCaptiveMode();
  statusMsg = "WiFi ulozena, ale pripojeni selhalo - AP captive zustava aktivni";
  return false;
}

bool WifiProvisioning::forgetCredentials() {
  if (!config_) return false;

  AppConfig updated = *config_;
  updated.wifiSsid = "";
  updated.wifiPassword = "";
  if (!saveConfig(updated)) return false;

  *config_ = updated;
  startCaptiveMode();
  return true;
}

String WifiProvisioning::getStateText() const {
  switch (state_) {
    case WIFI_STA_CONNECTED:
      return "WIFI_STA_CONNECTED";
    case WIFI_AP_CAPTIVE:
      return "WIFI_AP_CAPTIVE";
    case WIFI_STA_CONNECTING:
    default:
      return "WIFI_STA_CONNECTING";
  }
}

bool WifiProvisioning::connectStaBlocking(unsigned long timeoutMs) {
  if (!config_ || !hasStoredCredentials()) return false;

  stopCaptiveMode();

  state_ = WIFI_STA_CONNECTING;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(config_->wifiSsid.c_str(), config_->wifiPassword.c_str());
  Serial.printf("WIFI PROV: Pripojuji k SSID '%s'\n", config_->wifiSsid.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    state_ = WIFI_STA_CONNECTED;
    staConnectStartedAt_ = 0;
    lastReconnectAttemptAt_ = 0;
    Serial.printf("WIFI PROV: STA pripojeno, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  return false;
}

void WifiProvisioning::startCaptiveMode() {
  stopCaptiveMode();

  state_ = WIFI_AP_CAPTIVE;
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_AP);

  apSsid_ = buildApName();
  bool apOk = WiFi.softAP(apSsid_.c_str(), AP_PASSWORD);
  delay(50);
  apIp_ = WiFi.softAPIP();

  dnsServer_.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer_.start(DNS_PORT, "*", apIp_);
  captiveRunning_ = true;

  captiveStartedAt_ = millis();
  lastStaProbeAt_ = captiveStartedAt_;
  staProbeActive_ = false;

  Serial.printf("WIFI PROV: AP %s (%s), open=%s, dns=%s\n",
                apSsid_.c_str(), apIp_.toString().c_str(),
                AP_PASSWORD[0] == '\0' ? "ano" : "ne",
                apOk ? "ok" : "fail");
}

void WifiProvisioning::stopCaptiveMode() {
  if (captiveRunning_) {
    dnsServer_.stop();
    captiveRunning_ = false;
  }

  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    WiFi.softAPdisconnect(true);
  }
}

// V AP fallbacku (SSID je ulozene) se pravidelne zkousi puvodni sit. AP pri tom
// bezi dal (rezim AP+STA), takze portal zustava dostupny. Pokud je k AP nekdo
// pripojeny, pokus se odklada, aby se mu nerozbilo prave probihajici nastaveni.
void WifiProvisioning::processCaptiveStaRetry() {
  if (!hasStoredCredentials()) return;

  const unsigned long now = millis();

  if (staProbeActive_) {
    if (WiFi.status() == WL_CONNECTED) {
      staProbeActive_ = false;
      stopCaptiveMode();
      WiFi.mode(WIFI_STA);
      state_ = WIFI_STA_CONNECTED;
      staConnectStartedAt_ = 0;
      lastReconnectAttemptAt_ = 0;
      Serial.printf("WIFI PROV: Navrat z AP fallbacku po %lu s, IP: %s\n",
                    (now - captiveStartedAt_) / 1000UL,
                    WiFi.localIP().toString().c_str());
      return;
    }
    if (now - staProbeStartedAt_ >= connectTimeoutMs_) {
      staProbeActive_ = false;
      lastStaProbeAt_ = now;
      WiFi.disconnect(false, false);  // zastavit STA, AP nechat bezet
      Serial.println("WIFI PROV: Puvodni Wi-Fi stale nedostupna, zustavam v AP");
    }
    return;
  }

  if (now - lastStaProbeAt_ < STA_PROBE_INTERVAL_MS) return;

  if (WiFi.softAPgetStationNum() > 0) {
    lastStaProbeAt_ = now;  // nekdo je v portalu - zkusime pozdeji
    return;
  }

  staProbeActive_ = true;
  staProbeStartedAt_ = now;
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(config_->wifiSsid.c_str(), config_->wifiPassword.c_str());
  Serial.printf("WIFI PROV: Zkousim puvodni SSID '%s' (AP bezi dal)\n",
                config_->wifiSsid.c_str());
}

bool WifiProvisioning::hasStoredCredentials() const {
  return config_ && config_->wifiSsid.length() > 0;
}
