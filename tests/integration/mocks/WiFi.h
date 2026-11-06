#pragma once
#include <Arduino.h>
#include "esp_wifi.h"
constexpr int WL_IDLE_STATUS = 0;
constexpr int WL_CONNECTED = 3;
constexpr int WL_CONNECT_FAILED = 4;
constexpr int WL_DISCONNECTED = 6;
constexpr int WIFI_AP_STA = 3;
constexpr int WIFI_SCAN_RUNNING = -1;
struct MockWifi {
  struct Network { String ssid; int rssi; };
  int connection = WL_DISCONNECTED, modeValue = WIFI_AP_STA;
  int beginCalls = 0, disconnectCalls = 0, scanCalls = 0, deleteCalls = 0;
  int scanResult = WIFI_SCAN_RUNNING, scanStart = WIFI_SCAN_RUNNING;
  bool lastScanAsync = false, lastScanHidden = false, lastScanPassive = false;
  uint32_t lastScanMaxMsPerChan = 0;
  bool disconnectOk = true, beginOk = true, disconnectRequested = false;
  bool lastDisconnectWifiOff = false, lastDisconnectErase = false;
  bool persistentEnabled = true, autoReconnectEnabled = true;
  bool beganWithOpenPassword = false;
  int statusRebases = 0;
  String begunSsid, begunPassword;
  IPAddress address{192, 168, 1, 42};
  std::vector<Network> networks;
  int status() const { return connection; }
  void _setStatus(int value) { ++statusRebases; connection = value; }
  int getMode() const { return modeValue; }
  void mode(int value) { modeValue = value; }
  bool disconnect(bool wifiOff, bool erase) {
    ++disconnectCalls;
    lastDisconnectWifiOff = wifiOff;
    lastDisconnectErase = erase;
    if (disconnectOk) disconnectRequested = true;
    // The pinned call does not deliver DISCONNECTED or clear the old association inline.
    return disconnectOk;
  }
  int begin(const char* ssid, const char* password, int, const void*, bool) {
    ++beginCalls; begunSsid = ssid; begunPassword = password;
    beganWithOpenPassword = !password || !*password;
    if (beginOk) address = IPAddress(0, 0, 0, 0);  // Pinned DHCP setup clears the old IP.
    // A successful SDK launch returns the cached Arduino status, not new association proof.
    return beginOk ? connection : WL_CONNECT_FAILED;
  }
  void dropAssociation() { MockSta::associated = false; }
  void deliverDisconnected() {
    connection = WL_DISCONNECTED;
    address = IPAddress(0, 0, 0, 0);
  }
  void radioAssociate(const String& ssid) {
    MockSta::association = {};
    std::memcpy(MockSta::association.ssid, ssid.c_str(), std::min<size_t>(ssid.length(), 32));
    MockSta::associated = true;
  }
  void deliverGotIp(IPAddress ip = IPAddress(192, 168, 1, 42)) {
    connection = WL_CONNECTED;
    address = ip;
  }
  void associate(const String& ssid) {
    radioAssociate(ssid);
    deliverGotIp();
  }
  IPAddress localIP() const { return address; }
  int RSSI() const { return -42; }
  int RSSI(int index) const { return networks.at(index).rssi; }
  String SSID(int index) const { return networks.at(index).ssid; }
  int scanComplete() const { return scanResult; }
  void scanDelete() { ++deleteCalls; }
  int scanNetworks(bool async, bool hidden, bool passive, uint32_t maxMsPerChan) {
    ++scanCalls;
    lastScanAsync = async;
    lastScanHidden = hidden;
    lastScanPassive = passive;
    lastScanMaxMsPerChan = maxMsPerChan;
    return scanStart;
  }
  void persistent(bool value) { persistentEnabled = value; }
  void setAutoReconnect(bool value) { autoReconnectEnabled = value; }
  void softAPConfig(IPAddress, IPAddress, IPAddress) {}
  bool softAP(const char*, const char*, int, bool, int) { return true; }
};
inline MockWifi WiFi;
