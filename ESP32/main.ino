/*
 * ============================================================
 *  VerifAI SmartSwitch — ESP32 Firmware  v45  PRODUCTION
 * ============================================================
 *  CHANGES FROM v44:
 *    1. ethIp global added — captures ETH IP at connect time.
 *       Exposed in /status JSON as "ethIp". Mirrors wifiStaIp.
 *    2. Browser reconnection poller now tries ETH IP and WiFi IP
 *       in parallel for instant UI recovery after network switch.
 *    3. CABLE_PULL_CONFIRM_MS = 6000UL (safe: 3s above relay).
 *    4. T_ETH_HOTPLUG_CHECK = 2000UL, T_HTTP_POLL = 3000UL.
 *    5. tickETHHotplug now waits for DHCP before retrying.
 *
 *  !! EDIT THESE TO MATCH YOUR SETUP !!
 *  ─────────────────────────────────────
 *  SERVER_IP       192.168.1.114  your PC running the Python server
 *  SERVER_PORT     8080
 *  mDNS hostname   smartswitch     → http://smartswitch.local
 *  AP fallback     192.168.4.1     → always reachable
 *  ETH/WiFi IP     assigned by router DHCP (check Serial Monitor)
 *  ─────────────────────────────────────
 *
 *  Hardware:
 *    W5500  SCK->18  MISO->19  MOSI->23  CS->5   RST->27
 *    Relay  -> GPIO25 (HIGH=ON)
 *    Button -> GPIO4  (INPUT_PULLUP, hold 3s = config mode)
 *    LED    -> GPIO2  (blinks 1Hz alive, blinks 3x on btn hold)
 *
 *  LittleFS files:  /index.html  /style.css  /app.js
 *                   /config.html  /success.html
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ETH.h>
#include <SPI.h>
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "esp_mac.h"

// ================================================================
//  PINS
// ================================================================
#define PIN_RELAY 25
#define PIN_LED 2
#define PIN_BTN 4
#define ETH_SCLK 18
#define ETH_MISO 19
#define ETH_MOSI 23
#define ETH_CS 5
#define ETH_RST 27

// ================================================================
//  NETWORK CONFIG  ← EDIT THESE
// ================================================================
#define SERVER_IP "192.168.1.117"
#define SERVER_PORT 8090
#define MDNS_NAME "smartswitch"

// ================================================================
//  DEVICE DEFAULTS
// ================================================================
#define DEF_DEVNAME "SmartSwitch"
#define DEF_USER "admin"
#define DEF_PASS "admin"
#define AP_SSID "SmartSwitch-Setup"
#define AP_PASS "12345678"

// ================================================================
//  TIMING
// ================================================================
#define T_AUTO_INTERVAL_DEFAULT 120000UL
#define T_AUTO_INTERVAL_MIN 10000UL
#define T_AUTO_INTERVAL_MAX 3600000UL
#define T_AUTO_PAUSE 10000UL
#define T_RELAY_RESTART 3000UL
#define T_ETH_TIMEOUT 8000UL
#define T_WIFI_TIMEOUT 12000UL
#define T_WIFI_RETRY 3000UL
#define T_BTN_HOLD 3000UL
#define T_RECV_LINE 800UL
#define T_RECV_BODY 3000UL
#define SOCK_RECV_TIMEOUT_S 2
#define SOCK_SEND_TIMEOUT_S 2
#define T_ETH_RECOV_RETRY 12000UL
#define T_ETH_CABLE_RETRY 6000UL
#define T_ETH_BROWNOUT_WIN 1000UL
#define T_ETH_HOTPLUG_CHECK 2000UL
#define T_HTTP_POLL 3000UL
#define T_MDNS_REANNOUNCE 60000UL
#define WDT_TIMEOUT_SEC 30
#define HEAP_MIN_BYTES 10000u
#define HEAP_LOG_INTERVAL 60000UL
#define CABLE_PULL_CONFIRM_MS 10000UL
#define T_ETH_HOTPLUG_ATTEMPT 12000UL
#define T_MDNS_WATCHDOG 300000UL
#define AP_CONFIG_TIMEOUT_MS 300000UL  

// ================================================================
//  GLOBALS
// ================================================================
Preferences prefs;
String devName, wSSID, wPass, loginUser, loginPass;
String targetIP = "";
String myIP = "";
String wifiStaIp = "";  // WiFi STA IP captured at connect time
String ethIp = "";      // ETH IP captured at connect time (v45)

unsigned long autoIntervalMs = T_AUTO_INTERVAL_DEFAULT;
bool autoEnabled = true;

bool targetOnline = false;
int pingFailCount = 0;
unsigned long lastPingAt = 0;
unsigned long targetLastSeen = 0;

volatile bool ethUp = false;
volatile bool wifiUp = false;
bool netReady = false;
bool ethRecovPend = false;
bool ethInitDone = false;
bool wifiFallbackActive = false;
bool ethHotplugAttempting = false;
unsigned long ethDroppedAt = 0;
unsigned long ethLastRecovAt = 0;
unsigned long ethCableCheckAt = 0;
unsigned long ethHotplugAttemptAt = 0;
unsigned long lastWifiRetry = 0;
uint8_t wifiFailReason = 0;
bool wifiTestOk = false;

volatile bool relayState = false;
volatile bool relayRestart = false;
volatile bool relayLocked = false;
unsigned long relayRestartAt = 0;
int pendRelayAction = -1;

bool autoActive = false;
unsigned long autoNextFire = 0;
unsigned long lastBtnMs = 0;
bool autoRestarting = false;
unsigned long autoRestartAt = 0;

bool manOn = false;
int manH = 0, manM = 0, manS = 0, manAct = 2;
unsigned long manInterval = 0;
unsigned long manNextFire = 0;
unsigned long manFired = 0;

bool apOn = true;
unsigned long apStartMs = 0;

unsigned long btnStartMs = 0;
bool btnFired = false;

bool pendStop = false;
unsigned long pendStopAt = 0;
bool pendWifi = false;
bool pendReboot = false;

bool serverConnected = false;
bool firstBootSent = false;
unsigned long lastPollAt = 0;
unsigned long lastReportedRelayState = 255;
String lastReportedNetType = "";
unsigned long lastHeapLogAt = 0;
unsigned long lastMDNSAt = 0;
unsigned long ethWifiDropAt = 0;
bool ethWifiDropPend = false;
unsigned long lastMDNSCheckAt = 0;
unsigned long lastHTTPClientAt = 0;  // tracks when we last served a browser client
AsyncWebServer asyncServer(8080);
static int srvFd = -1;

// ================================================================
//  RELAY TIMER
// ================================================================
static esp_timer_handle_t relayTimer = nullptr;
volatile bool relayTimerFired = false;

static void IRAM_ATTR relayTimerCb(void*) {
  if (!relayLocked) relayTimerFired = true;
}
void destroyRelayTimer() {
  if (!relayTimer) return;
  esp_timer_stop(relayTimer);
  esp_timer_delete(relayTimer);
  relayTimer = nullptr;
}
void createRelayTimer() {
  destroyRelayTimer();
  esp_timer_create_args_t a;
  memset(&a, 0, sizeof(a));
  a.callback = relayTimerCb;
  a.name = "relay";
  a.dispatch_method = ESP_TIMER_TASK;
  esp_timer_create(&a, &relayTimer);
}
void armRelayTimer(uint32_t ms) {
  if (!relayTimer) createRelayTimer();
  esp_timer_stop(relayTimer);
  esp_timer_start_once(relayTimer, (uint64_t)ms * 1000ULL);
}

// ================================================================
//  NVS CONFIG
// ================================================================
void loadConfig() {
  prefs.begin("sw", true);
  devName = prefs.getString("dev", DEF_DEVNAME);
  wSSID = prefs.getString("ssid", "");
  wPass = prefs.getString("pass", "");
  loginUser = prefs.getString("user", DEF_USER);
  loginPass = prefs.getString("pw", DEF_PASS);
  autoIntervalMs = prefs.getULong("autoMs", T_AUTO_INTERVAL_DEFAULT);
  autoEnabled = prefs.getBool("autoEn", true);
  if (autoIntervalMs < T_AUTO_INTERVAL_MIN) autoIntervalMs = T_AUTO_INTERVAL_MIN;
  if (autoIntervalMs > T_AUTO_INTERVAL_MAX) autoIntervalMs = T_AUTO_INTERVAL_MAX;
  prefs.end();
  if (loginUser.length() == 0) loginUser = DEF_USER;
  if (loginPass.length() == 0) loginPass = DEF_PASS;
  Serial.println("[CFG] User: " + loginUser + " autoIntervalMs: " + String(autoIntervalMs) + " autoEnabled: " + String(autoEnabled));
}

void saveWifiConfig(const String& dev, const String& ssid, const String& pass) {
  prefs.begin("sw", false);
  prefs.putString("dev", dev);
  prefs.putString("ssid", ssid);
  if (pass.length()) prefs.putString("pass", pass);
  prefs.end();
  devName = dev;
  wSSID = ssid;
  if (pass.length()) wPass = pass;
  wifiTestOk = false;
  wifiFailReason = 0;
}

void setAutoInterval(unsigned long ms) {
  if (ms < T_AUTO_INTERVAL_MIN) ms = T_AUTO_INTERVAL_MIN;
  if (ms > T_AUTO_INTERVAL_MAX) ms = T_AUTO_INTERVAL_MAX;
  autoIntervalMs = ms;
  prefs.begin("sw", false);
  prefs.putULong("autoMs", ms);
  prefs.end();
  if (autoActive) {
    autoNextFire = millis() + autoIntervalMs;
  }
  Serial.println("[CFG] Auto-restart interval set to " + String(ms) + " ms");
}

void setAutoEnabled(bool en) {
  autoEnabled = en;
  prefs.begin("sw", false);
  prefs.putBool("autoEn", en);
  prefs.end();
  if (en) {
    autoActive = true;
    autoRestarting = false;
    autoNextFire = millis() + autoIntervalMs;
    Serial.println("[CFG] Auto-restart ENABLED — next fire in " + String(autoIntervalMs) + " ms");
  } else {
    autoActive = false;
    autoRestarting = false;
    Serial.println("[CFG] Auto-restart DISABLED");
  }
}

// ================================================================
//  RELAY
// ================================================================
void ledBlink(unsigned long durationMs);
void setRelay(bool on) {
  relayState = on;
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
  Serial.println(on ? "[Relay] ON" : "[Relay] OFF");
  ledBlink(1000);
  if (on) delay(200);
}
void cancelRelay() {
  if (relayTimer) esp_timer_stop(relayTimer);
  relayTimerFired = false;
  relayRestart = false;
  relayLocked = false;
}
void doRestart() {
  cancelRelay();
  setRelay(false);
  relayRestart = true;
  relayRestartAt = millis();
  createRelayTimer();
  armRelayTimer(T_RELAY_RESTART);
  Serial.println("[Relay] RESTART — back ON in 3s");
}
void webRelayAction(int action) {
  lastBtnMs = millis();
  cancelRelay();
  if (action == 0) setRelay(false);
  else if (action == 1) setRelay(true);
  else if (action == 2) doRestart();
}
void schedRelayAction(int action) {
  cancelRelay();
  if (action == 0) setRelay(false);
  else if (action == 1) setRelay(true);
  else if (action == 2) doRestart();
}
void tickRelayRestart() {
  if (relayTimerFired) {
    relayTimerFired = false;
    relayRestart = false;
    setRelay(true);
    return;
  }
  if (relayRestart && (millis() - relayRestartAt) >= T_RELAY_RESTART + 1000UL) {
    relayTimerFired = false;
    relayRestart = false;
    setRelay(true);
  }
}

// ================================================================
//  SCHEDULERS
// ================================================================
void startAutoScheduler() {
  if (!autoEnabled) {
    Serial.println("[AUTO] Watchdog disabled — not starting");
    return;
  }
  autoActive = true;
  autoNextFire = millis() + autoIntervalMs;
  Serial.printf("[AUTO] Scheduler started — first restart in %lu ms\n", autoIntervalMs);
}

void tickAutoScheduler() {
  if (!autoActive) return;
  unsigned long now = millis();
  bool paused = (lastBtnMs > 0) && ((now - lastBtnMs) < T_AUTO_PAUSE);
  if (paused) {
    autoNextFire = now + autoIntervalMs;
    autoRestarting = false;
    return;
  }
  if (autoRestarting) {
    if ((now - autoRestartAt) >= T_RELAY_RESTART) {
      autoRestarting = false;
      setRelay(true);
      autoNextFire = millis() + autoIntervalMs;
      Serial.printf("[AUTO] Device ON — next restart in %lu ms\n", autoIntervalMs);
    }
    return;
  }
  if ((now - autoNextFire) < 0x80000000UL && now >= autoNextFire) {
    Serial.println("[AUTO] Triggering restart");
    setRelay(false);
    autoRestarting = true;
    autoRestartAt = now;
  }
}

void applyManual(int h, int m, int s, int act, bool on) {
  manH = h;
  manM = m;
  manS = s;
  manAct = act;
  manOn = on;
  manInterval = ((unsigned long)h * 3600UL + (unsigned long)m * 60UL + s) * 1000UL;
  manNextFire = millis() + manInterval;
  if (on) manFired = 0;
}
void tickManualScheduler() {
  if (!manOn || manInterval == 0) return;
  unsigned long now = millis();
  if ((now - manNextFire) < 0x80000000UL && now >= manNextFire) {
    schedRelayAction(manAct);
    manNextFire = now + manInterval;
    manFired++;
  }
}

// ================================================================
//  HTTP POLLING
// ================================================================
String currentNetType() {
  if (ethUp) return "Ethernet";
  if (wifiUp) return "WiFi";
  return "";
}

#define HPOST_CONN_MS 800UL
#define HPOST_SEND_MS 300UL
#define HPOST_RECV_MS 800UL

static bool _fdReady(int fd, bool forWrite, unsigned long timeout_ms) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  int r = select(fd + 1,
                 forWrite ? NULL : &fds,
                 forWrite ? &fds : NULL,
                 NULL, &tv);
  return (r > 0);
}

String httpPost(const String& path, const String& body) {
  if (!ethUp && !wifiUp) return "";

  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return "";
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  int nd = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));

  struct sockaddr_in saddr;
  memset(&saddr, 0, sizeof(saddr));
  saddr.sin_family = AF_INET;
  saddr.sin_port = htons(SERVER_PORT);
  saddr.sin_addr.s_addr = inet_addr(SERVER_IP);
  int cr = connect(fd, (struct sockaddr*)&saddr, sizeof(saddr));
  if (cr < 0 && errno != EINPROGRESS) {
    close(fd);
    return "";
  }
  if (cr != 0) {
    if (!_fdReady(fd, true, HPOST_CONN_MS)) {
      close(fd);
      return "";
    }
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
      close(fd);
      return "";
    }
  }

  String req = "POST " + path + " HTTP/1.1\r\n"
                                "Host: " SERVER_IP "\r\n"
                                "Content-Type: application/json\r\n"
                                "Content-Length: "
               + String(body.length()) + "\r\n"
                                         "Connection: close\r\n\r\n"
               + body;
  if (!_fdReady(fd, true, HPOST_SEND_MS)) {
    close(fd);
    return "";
  }
  if (::send(fd, req.c_str(), req.length(), MSG_NOSIGNAL) <= 0) {
    close(fd);
    return "";
  }

  String resp = "";
  bool inBody = false;
  String line = "";
  unsigned long t0 = millis();
  while ((millis() - t0) < HPOST_RECV_MS) {
    if (!_fdReady(fd, false, 200)) {
      if (inBody && resp.length() > 0) break;
      esp_task_wdt_reset();
      continue;
    }
    char buf[64];
    int n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (!inBody) {
        if (c == '\n') {
          if (line.length() == 0 || line == "\r") {
            inBody = true;
            line = "";
          } else line = "";
        } else if (c != '\r') line += c;
      } else {
        resp += c;
        if (resp.length() > 512) goto done;
      }
    }
    esp_task_wdt_reset();
  }
done:
  close(fd);
  return resp;
}

void handleServerResponse(const String& resp) {
  if (resp.length() == 0) return;
  int ci = resp.indexOf("\"cmd\"");
  if (ci < 0) return;
  int vs = resp.indexOf('"', ci + 5) + 1;
  int ve = resp.indexOf('"', vs);
  if (vs <= 0 || ve <= vs) return;
  String cmd = resp.substring(vs, ve);
  if (cmd == "ON") webRelayAction(1);
  else if (cmd == "OFF") webRelayAction(0);
  else if (cmd == "RESTART") webRelayAction(2);
}

void tickServerClient() {
  if (!ethUp && !wifiUp) {
    serverConnected = false;
    return;
  }
  unsigned long now = millis();
  if ((now - lastPollAt) < T_HTTP_POLL) return;
  // If a browser client was served very recently, defer heartbeat
  // so tickHTTP() gets priority on next loop iterations
  if ((now - lastHTTPClientAt) < 2000UL) {
    lastPollAt = now - T_HTTP_POLL + 1000UL;  // retry in 1s
    return;
  }
  lastPollAt = now;
  String nt = currentNetType();
  int rssi = (wifiUp && !ethUp) ? (int)WiFi.RSSI() : 0;
  uint8_t baseMac[6];
  esp_read_mac(baseMac, ESP_MAC_WIFI_STA);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           baseMac[0], baseMac[1], baseMac[2],
           baseMac[3], baseMac[4], baseMac[5]);
  String mac = String(macStr);                                      
  String firstBootFlag = firstBootSent ? "false" : "true";
  if (!firstBootSent) firstBootSent = true;
String body = "{\"name\":\"" + devName + "\",\"mac\":\"" + mac + "\",\"firstBoot\":" + firstBootFlag + ",\"ip\":\"" + myIP + "\",\"net\":\"" + nt + "\",\"relay\":" + String(relayState ? 1 : 0) + ",\"rssi\":" + String(rssi) + ",\"targetIP\":\"" + targetIP + "\",\"targetOnline\":" + String(targetOnline ? 1 : 0) + ",\"pingFails\":" + String(pingFailCount) + ",\"autoIntervalMs\":" + String(autoIntervalMs) + ",\"autoEnabled\":" + String(autoEnabled ? "true" : "false") + "}";
  esp_task_wdt_reset();
  String resp = httpPost("/device/heartbeat", body);
  esp_task_wdt_reset();
  bool wasConnected = serverConnected;
  serverConnected = (resp.length() > 0);
  if (!wasConnected && serverConnected) Serial.println("[Server] Connected  IP=" SERVER_IP);
  if (wasConnected && !serverConnected) Serial.println("[Server] Disconnected — retrying");
  if (serverConnected) handleServerResponse(resp);
}

void serverConnect() {
  lastPollAt = 0;
}

// ================================================================
//  mDNS RESTART
// ================================================================
static bool _mdnsRestarting = false;
void restartMDNS() {
  if (_mdnsRestarting) return;  // prevent concurrent calls
  _mdnsRestarting = true;
  MDNS.end();
  delay(200);  // increased from 100ms to 200ms for stability
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("http", "tcp", 8080);
    Serial.println("[mDNS] OK → http://" MDNS_NAME ".local");
  } else {
    Serial.println("[mDNS] FAILED — will retry in 60s");
  }
  _mdnsRestarting = false;
}

// ================================================================
//  HEAP WATCHDOG
// ================================================================
void tickHeapWatchdog() {
  unsigned long now = millis();
  if ((now - lastHeapLogAt) < HEAP_LOG_INTERVAL) return;
  lastHeapLogAt = now;
  uint32_t fh = ESP.getFreeHeap();
  if (fh < HEAP_MIN_BYTES) {
    Serial.println("[WARN] Heap critical — rebooting");
    delay(500);
    ESP.restart();
  }
}

// ================================================================
//  NETWORK EVENTS
// ================================================================
void onNetEvent(WiFiEvent_t ev) {
  switch (ev) {
    case ARDUINO_EVENT_ETH_START:
      ETH.setHostname(devName.c_str());
      break;

    case ARDUINO_EVENT_ETH_GOT_IP:
      ethUp = true;
      ethInitDone = true;
      ethRecovPend = false;
      myIP = ETH.localIP().toString();
      ethIp = ETH.localIP().toString();
      wifiFallbackActive = false;
      lastWifiRetry = millis();
      ethWifiDropAt = millis() + 4000UL;  // ← ADD: 4s grace period
      ethWifiDropPend = true;             // ← ADD
      Serial.println("[ETH] Connected  IP=" + myIP + " — WiFi drops in 4s");
      restartMDNS();
      if (!netReady) {
        netReady = true;
        startAutoScheduler();
      }
      lastPollAt = 0;
      lastMDNSAt = 0;
      break;

    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
      if (ethUp) {
        ethDroppedAt = millis();
        ethRecovPend = true;
        ethLastRecovAt = 0;
      }
      ethUp = false;
      if (wifiUp) myIP = WiFi.localIP().toString();
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiTestOk = true;
      wifiStaIp = WiFi.localIP().toString();
      Serial.println("[WiFi] STA got IP=" + wifiStaIp + " wifiTestOk=true");
      if (ethUp) {
        WiFi.disconnect(false);
        wifiUp = false;
        wifiFallbackActive = false;
        Serial.println("[WiFi] Dropped — ETH has priority (wifiStaIp saved)");
      } else {
        wifiUp = true;
        myIP = wifiStaIp;
        Serial.println("[WiFi] Connected  IP=" + myIP);
        restartMDNS();
        if (!netReady) {
          netReady = true;
          startAutoScheduler();
        }
        lastPollAt = 0;
      }
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiUp = false;
      if (!ethUp) myIP = "";
      break;

    default: break;
  }
}

// ================================================================
//  ETH INIT
// ================================================================
bool initETH() {
  Serial.println("[ETH] Initialising W5500...");
  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, LOW);
  delay(200);
  digitalWrite(ETH_RST, HIGH);
  delay(200);
  SPI.begin(ETH_SCLK, ETH_MISO, ETH_MOSI, ETH_CS);
  if (!ETH.begin(ETH_PHY_W5500, 0, ETH_CS, -1, ETH_RST, SPI)) {
    Serial.println("[ETH] begin() failed");
    return false;
  }
  unsigned long t0 = millis();
  while (!ethUp && (millis() - t0) < T_ETH_TIMEOUT) {
    esp_task_wdt_reset();
    yield();
    delay(5);
  }
  if (ethUp) ethInitDone = true;
  return ethUp;
}

// ================================================================
//  WIFI INIT
// ================================================================
bool initWiFi() {
  if (!wSSID.length()) return false;
  Serial.println("[WiFi] Connecting to " + wSSID + "...");
  wifiFailReason = 0;
  WiFi.begin(wSSID.c_str(), wPass.c_str());
  unsigned long t0 = millis();
  while (!wifiUp && (millis() - t0) < T_WIFI_TIMEOUT) {
    esp_task_wdt_reset();
    yield();
    delay(10);
  }
  return wifiUp;
}

// ================================================================
//  AP
// ================================================================
void startAP() {
  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),
    IPAddress(192, 168, 4, 1),
    IPAddress(255, 255, 255, 0));
  if (WiFi.softAP(AP_SSID, AP_PASS)) {
    apOn = true;
    apStartMs = millis();
    Serial.println("[AP] SSID:" AP_SSID "  Pass:" AP_PASS "  IP:192.168.4.1");
  }
}
void stopAP() {
    if (!apOn) return;
    WiFi.softAPdisconnect(true);
    apOn = false;
    Serial.println("[AP] Turned OFF — network configured");
}
void tickAPManager() {
    if (!apOn) return;

    // If network is connected and AP has been on long enough → turn off
    if ((ethUp || wifiUp) && wSSID.length()) {
        // Give 30s grace after network connects so customer
        // can still reach 192.168.4.1 if they were on it
        if ((millis() - apStartMs) > 30000UL) {
            stopAP();
            return;
        }
    }

    // Auto-off after 5 min regardless — prevents AP staying on forever
    // if customer accidentally held button or never finished setup
    if ((millis() - apStartMs) > AP_CONFIG_TIMEOUT_MS) {
        // Only auto-off if network is available
        // If no network at all, keep AP on so customer can configure
        if (ethUp || wifiUp) {
            stopAP();
        }
    }
}

// ================================================================
//  ETH RECOVERY
// ================================================================
void doETHReinit() {
  ETH.end();
  SPI.end();
  delay(50);
  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, LOW);
  delay(150);
  digitalWrite(ETH_RST, HIGH);
  delay(150);
  SPI.begin(ETH_SCLK, ETH_MISO, ETH_MOSI, ETH_CS);
  delay(20);
  if (!ETH.begin(ETH_PHY_W5500, 0, ETH_CS, -1, ETH_RST, SPI)) return;
  ethLastRecovAt = millis();
}
bool wasBrownout() {
  if (relayRestartAt == 0 || ethDroppedAt < relayRestartAt) return false;
  return (ethDroppedAt - relayRestartAt) < T_ETH_BROWNOUT_WIN;
}
void tickETHRecovery() {
  if (!ethRecovPend || ethUp) {
    if (ethUp) ethRecovPend = false;
    return;
  }

  unsigned long now = millis();
  unsigned long elapsed = now - ethDroppedAt;

  // PHASE 1: Confirmation window — relay transient guard
  if (elapsed < CABLE_PULL_CONFIRM_MS) {
    if (ethLastRecovAt == 0 || (now - ethLastRecovAt) >= 500UL) {
      doETHReinit();
      ethLastRecovAt = now;
    }
    return;
  }

  // PHASE 2: Cable physically absent — start WiFi fallback
  if (wSSID.length() && !wifiUp && !wifiFallbackActive) {
    wifiFallbackActive = true;
    Serial.println("[ETH] Cable absent — starting WiFi fallback");
    WiFi.begin(wSSID.c_str(), wPass.c_str());
  }

  if ((now - ethLastRecovAt) >= T_ETH_CABLE_RETRY) {
    doETHReinit();
    ethLastRecovAt = now;
  }
}

void tickETHHotplug() {
  if (ethUp || ethRecovPend || !ethInitDone) return;
  if (!wifiUp && !apOn) return;
 if (wifiFallbackActive && wifiUp) return;

  unsigned long now = millis();

  if (ethHotplugAttempting) {
    if (ethUp) {
      ethHotplugAttempting = false;
      return;
    }
    if ((now - ethHotplugAttemptAt) < T_ETH_HOTPLUG_ATTEMPT) return;
    ethHotplugAttempting = false;
  }

  if ((now - ethCableCheckAt) < T_ETH_HOTPLUG_CHECK) return;
  ethCableCheckAt = now;

  ETH.end();
  SPI.end();
  delay(50);
  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, LOW);
  delay(100);
  digitalWrite(ETH_RST, HIGH);
  delay(150);
  SPI.begin(ETH_SCLK, ETH_MISO, ETH_MOSI, ETH_CS);
  delay(20);

  if (ETH.begin(ETH_PHY_W5500, 0, ETH_CS, -1, ETH_RST, SPI)) {
    ethHotplugAttempting = true;
    ethHotplugAttemptAt = now;
  }
}

// ================================================================
//  BUTTON
// ================================================================
void tickBtnInline() {
  esp_task_wdt_reset();
  bool p = (digitalRead(PIN_BTN) == LOW);
  if (p) {
    if (!btnStartMs) {
      btnStartMs = millis();
      btnFired = false;
    }
    if (!btnFired && (millis() - btnStartMs) >= T_BTN_HOLD) {
      btnFired = true;
      ledBlink(2000);
      if (!apOn) {
        startAP();
        Serial.println("[BTN] AP restarted for reconfiguration");
      }
      Serial.println("[BTN] Connect to WiFi: " AP_SSID "  Pass: " AP_PASS);
      Serial.println("[BTN] Then open: http://192.168.4.1");
    }
  } else {
    btnStartMs = 0;
    btnFired = false;
  }
}
void tickBtn() {
  bool p = (digitalRead(PIN_BTN) == LOW);
  if (p) {
    if (!btnStartMs) {
      btnStartMs = millis();
      btnFired = false;
    }
    if (!btnFired && (millis() - btnStartMs) >= T_BTN_HOLD) {
      btnFired = true;
      ledBlink(2000);
      if (!apOn) {
        startAP();
        Serial.println("[BTN] AP restarted for reconfiguration");
      }
      Serial.println("[BTN] Connect to WiFi: " AP_SSID "  Pass: " AP_PASS);
      Serial.println("[BTN] Then open: http://192.168.4.1");
    }
  } else {
    btnStartMs = 0;
    btnFired = false;
  }
}
// ================================================================
//  LED
// ================================================================
unsigned long ledOffAt = 0;
void ledBlink(unsigned long durationMs) {
  digitalWrite(PIN_LED, HIGH);
  ledOffAt = millis() + durationMs;
}
void tickLED() {
  if (ledOffAt > 0 && millis() >= ledOffAt) {
    digitalWrite(PIN_LED, LOW);
    ledOffAt = 0;
  }
}

// ================================================================
//  WIFI RETRY
// ================================================================
void tickWiFiRetry() {
  if (ethUp || ethRecovPend || wifiUp || !wSSID.length()) return;
  if ((millis() - lastWifiRetry) > T_WIFI_RETRY) {
    lastWifiRetry = millis();
    WiFi.disconnect(false);
    delay(100);
    WiFi.begin(wSSID.c_str(), wPass.c_str());
  }
}

// ================================================================
//  DEFERRED ACTIONS
// ================================================================
void tickDeferred() {
  unsigned long now = millis();
  if (pendStop && (now - pendStopAt) < 0x80000000UL && now >= pendStopAt) {
    pendStop = false;
    if (pendWifi && wSSID.length()) {
      pendWifi = false;
      WiFi.disconnect(false);
      delay(100);
      WiFi.begin(wSSID.c_str(), wPass.c_str());
    }
  }
}

// ================================================================
//  LittleFS HELPERS
// ================================================================
String readLittleFSFile(const String& path) {
  if (!LittleFS.exists(path)) {
    Serial.println("[FS] Missing: " + path);
    return "";
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.println("[FS] Open failed: " + path);
    return "";
  }
  String c = f.readString();
  f.close();
  return c;
}

// ================================================================
//  RAW HTTP HELPERS
// ================================================================
void sendAll(int fd, const char* data, size_t len) {
  size_t sent = 0;
  unsigned long t0 = millis();
  while (sent < len && (millis() - t0) < 5000UL) {
    int n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n <= 0) break;
    sent += n;
  }
}
void sendResp(int fd, int code, const char* ct, const String& body, bool cache = false) {
  const char* st;
  if (code == 200) st = "200 OK";
  else if (code == 302) st = "302 Found";
  else if (code == 400) st = "400 Bad Request";
  else if (code == 401) st = "401 Unauthorized";
  else if (code == 503) st = "503 Service Unavailable";
  else st = "404 Not Found";
  const char* cc = cache ? "public, max-age=86400" : "no-store";
  char hdr[300];
  snprintf(hdr, sizeof(hdr),
           "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
           "Connection: close\r\nCache-Control: %s\r\n"
           "Access-Control-Allow-Origin: *\r\n\r\n",
           st, ct, (unsigned)body.length(), cc);
  sendAll(fd, hdr, strlen(hdr));
  if (body.length()) sendAll(fd, body.c_str(), body.length());
}
void serveFile(int fd, const String& fsPath, bool cache = false, const char* ct = "text/html; charset=utf-8") {
  String content = readLittleFSFile(fsPath);
  if (content.length() == 0) {
    String err = "503 File missing: " + fsPath + "\nUpload LittleFS data folder first.";
    sendResp(fd, 503, "text/plain", err);
    return;
  }
  sendResp(fd, 200, ct, content, cache);
}
void send401(int fd) {
  String h = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"" + devName + "\"\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
  sendAll(fd, h.c_str(), h.length());
}
String urlDecode(const String& s) {
  String r = "";
  r.reserve(s.length());
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (c == '+') r += ' ';
    else if (c == '%' && i + 2 < (int)s.length()) {
      auto hv = [](char x) -> int {
        return x >= '0' && x <= '9' ? x - '0' : x >= 'a' && x <= 'f' ? x - 'a' + 10
                                              : x >= 'A' && x <= 'F' ? x - 'A' + 10
                                                                     : 0;
      };
      r += (char)((hv(s[i + 1]) << 4) | hv(s[i + 2]));
      i += 2;
    } else r += c;
  }
  return r;
}
String getParam(const String& body, const String& key) {
  String search = key + "=";
  int idx = -1;
  if (body.startsWith(search)) idx = 0;
  else {
    int f = body.indexOf("&" + search);
    if (f >= 0) idx = f + 1;
  }
  if (idx < 0) return "";
  int start = idx + search.length(), end = body.indexOf('&', start);
  if (end < 0) end = body.length();
  return urlDecode(body.substring(start, end));
}
bool checkBasicAuth(const String& authHdr) {
  if (!authHdr.startsWith("Basic ")) return false;
  String enc = authHdr.substring(6);
  enc.trim();
  unsigned char dec[128];
  size_t olen = 0;
  if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &olen,
                            (const unsigned char*)enc.c_str(), enc.length())
      != 0) return false;
  dec[olen] = 0;
  return String((char*)dec) == loginUser + ":" + loginPass;
}

// ================================================================
//  RECV HELPERS
// ================================================================
bool recvLine(int fd, String& line, uint32_t ms = T_RECV_LINE) {
  line = "";
  line.reserve(128);
  unsigned long t0 = millis();
  while ((millis() - t0) < ms) {
    char c;
    int n = recv(fd, &c, 1, MSG_DONTWAIT);
    if (n == 1) {
      if (c == '\n') return true;
      if (c != '\r') line += c;
      t0 = millis();
    } else {
      esp_task_wdt_reset();
      tickBtnInline();
      yield();
      delay(2);
    }
  }
  return line.length() > 0;
}
void recvBody(int fd, String& body, int len, uint32_t ms = T_RECV_BODY) {
  body = "";
  if (len <= 0) return;
  body.reserve(min(len, 512));
  unsigned long t0 = millis();
  while ((int)body.length() < len && (millis() - t0) < ms) {
    char buf[64];
    int want = min(len - (int)body.length(), 64);
    int n = recv(fd, buf, want, MSG_DONTWAIT);
    if (n > 0) {
      for (int i = 0; i < n; i++) body += (char)buf[i];
      t0 = millis();
    } else {
      esp_task_wdt_reset();
      tickBtnInline();
      yield();
      delay(2);
    }
  }
}

// ================================================================
//  STATUS JSON
// ================================================================
String statusJSON() {
  unsigned long now = millis();
  bool autoPaused = (lastBtnMs > 0) && ((now - lastBtnMs) < T_AUTO_PAUSE);
  unsigned long autoRem = 0;
  if (autoActive && !autoPaused && (autoNextFire - now) < 0x80000000UL && autoNextFire > now)
    autoRem = autoNextFire - now;
  unsigned long manRem = 0;
  if (manOn && manInterval > 0 && (manNextFire - now) < 0x80000000UL && manNextFire > now)
    manRem = manNextFire - now;
  static char jbuf[1150];  // enlarged to fit ethIp field (v45)
  snprintf(jbuf, sizeof(jbuf),
           "{\"relay\":%s,\"restart\":%s,\"locked\":%s,"
          "\"eth\":%s,\"wifi\":%s,"
           "\"ip\":\"%s\",\"srvConn\":%s,"
           "\"auto\":%s,\"autoPaused\":%s,\"autoRem\":%lu,"
           "\"autoIntervalMs\":%lu,\"autoEnabled\":%s,"
           "\"manOn\":%s,\"manH\":%d,\"manM\":%d,\"manS\":%d,\"manAct\":%d,\"manRem\":%lu,"
           "\"targetIP\":\"%s\",\"targetOnline\":%s,\"pingFails\":%d,"
           "\"rssi\":%d,\"devName\":\"%s\",\"manFired\":%lu,"
           "\"wifiFailReason\":%d,\"wifiTestOk\":%s,\"wifiStaIp\":\"%s\",\"ethIp\":\"%s\",\"ssid\":\"%s\"}",
           relayState ? "true" : "false", relayRestart ? "true" : "false", relayLocked ? "true" : "false",
           ethUp ? "true" : "false", wifiUp ? "true" : "false",
           myIP.c_str(), serverConnected ? "true" : "false",
           autoActive ? "true" : "false", autoPaused ? "true" : "false", autoRem,
           autoIntervalMs, autoEnabled ? "true" : "false",
           manOn ? "true" : "false", manH, manM, manS, manAct, manRem,
           targetIP.c_str(), targetOnline ? "true" : "false", pingFailCount,
           (int)WiFi.RSSI(), devName.c_str(), manFired,
           (int)wifiFailReason, wifiTestOk ? "true" : "false", wifiStaIp.c_str(), ethIp.c_str(), (wifiUp ? wSSID.c_str() : ""));
  return String(jbuf);
}

// ================================================================
//  HTTP REQUEST HANDLER — PORT 80
// ================================================================
void handleHTTPClient(int fd, uint32_t remoteIP_nbo) {
  struct timeval tv;
  tv.tv_sec = SOCK_RECV_TIMEOUT_S;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  tv.tv_sec = SOCK_SEND_TIMEOUT_S;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  String reqLine;
  if (!recvLine(fd, reqLine, T_RECV_LINE)) return;
  int s1 = reqLine.indexOf(' '), s2 = reqLine.lastIndexOf(' ');
  if (s1 < 0 || s2 <= s1) return;
  String method = reqLine.substring(0, s1);
  String fullPath = reqLine.substring(s1 + 1, s2);
  String path = fullPath, queryStr = "";
  int qi = fullPath.indexOf('?');
  if (qi >= 0) {
    path = fullPath.substring(0, qi);
    queryStr = fullPath.substring(qi + 1);
  }

  String authHdr = "";
  int cLen = 0;
  while (true) {
    String line;
    bool got = recvLine(fd, line, T_RECV_LINE);
    if (!got || line.length() == 0) break;
    String ll = line;
    ll.toLowerCase();
    if (ll.startsWith("authorization:")) {
      authHdr = line.substring(line.indexOf(':') + 1);
      authHdr.trim();
    }
    if (ll.startsWith("content-length:")) {
      String cl = line.substring(line.indexOf(':') + 1);
      cl.trim();
      cLen = cl.toInt();
    }
  }
  String body;
  if (cLen > 0) recvBody(fd, body, cLen, T_RECV_BODY);
  bool authed = checkBasicAuth(authHdr);

  auto qParam = [&](const String& key) -> String {
    String v = getParam(queryStr, key);
    if (v.length()) return v;
    return getParam(body, key);
  };

  if (path == "/" && method == "GET") {
    serveFile(fd, "/index.html");
  } else if (path == "/config" && method == "GET") {
    serveFile(fd, "/config.html");
  } else if (path == "/config" && method == "POST") {
    String dn = getParam(body, "devname"), ssid = getParam(body, "ssid"), pass = getParam(body, "pass");
    if (!dn.length()) {
      String pg = readLittleFSFile("/config.html");
      if (pg.length() == 0) {
        sendResp(fd, 503, "text/plain", "config.html missing");
        return;
      }
      pg.replace("<!--ERROR-->", "<div class=\"err\">Device name cannot be empty.</div>");
      pg.replace("__DN__", devName);
      pg.replace("__SSID__", wSSID);
      sendResp(fd, 200, "text/html; charset=utf-8", pg);
    } else {
      saveWifiConfig(dn, ssid, pass);
      String pg = readLittleFSFile("/success.html");
      if (pg.length() == 0) {
        sendResp(fd, 503, "text/plain", "success.html missing");
      } else {
        pg.replace("__SSID__", ssid.length()
                                 ? "WiFi saved. Connecting to <b>" + ssid + "</b>.<br>Open: <b>http://" MDNS_NAME ".local</b>"
                                 : "No SSID — device stays on 192.168.4.1");
        sendResp(fd, 200, "text/html; charset=utf-8", pg);
      }
      pendStop = true;
      pendStopAt = millis() + 3000UL;
      pendWifi = ssid.length() > 0;
    }
  } else if (path == "/login" && method == "POST") {
    String user = getParam(body, "username");
    String pass = getParam(body, "password");
    if (user.length() == 0 || pass.length() == 0) {
      int ui = body.indexOf("username");
      int pi = body.indexOf("password");
      if (ui >= 0) {
        int vs = body.indexOf(':', ui) + 1;
        while (vs < (int)body.length() && (body[vs] == ' ' || body[vs] == '"')) vs++;
        int ve = vs;
        while (ve < (int)body.length() && body[ve] != '"' && body[ve] != ',' && body[ve] != '}') ve++;
        user = body.substring(vs, ve);
        user.trim();
      }
      if (pi >= 0) {
        int vs = body.indexOf(':', pi) + 1;
        while (vs < (int)body.length() && (body[vs] == ' ' || body[vs] == '"')) vs++;
        int ve = vs;
        while (ve < (int)body.length() && body[ve] != '"' && body[ve] != ',' && body[ve] != '}') ve++;
        pass = body.substring(vs, ve);
        pass.trim();
      }
    }
    if (user == loginUser && pass == loginPass)
      sendResp(fd, 200, "application/json", "{\"ok\":true,\"name\":\"" + devName + "\"}");
    else
      sendResp(fd, 401, "application/json", "{\"ok\":false,\"msg\":\"Invalid credentials\"}");
  } else if (path == "/status" && method == "GET") {
    sendResp(fd, 200, "application/json", statusJSON());
  } else if (path == "/network/ip" && method == "GET") {
    char netbuf[128];
    snprintf(netbuf, sizeof(netbuf), "{\"ip\":\"%s\",\"eth\":%s,\"wifi\":%s}",
             myIP.c_str(), ethUp ? "true" : "false", wifiUp ? "true" : "false");
    sendResp(fd, 200, "application/json", String(netbuf));
  } else if (path == "/device/info" && method == "GET") {
    String j = "{\"relay\":\"" + String(relayState ? "ON" : "OFF") + "\",\"ip\":\"" + myIP + "\",\"rssi\":" + String((int)WiFi.RSSI()) + ",\"eth\":" + String(ethUp ? "true" : "false") + ",\"wifi\":" + String(wifiUp ? "true" : "false") + ",\"targetIP\":\"" + targetIP + "\",\"devName\":\"" + devName + "\",\"autoIntervalMs\":" + String(autoIntervalMs) + ",\"autoEnabled\":" + String(autoEnabled ? "true" : "false") + "}";
    sendResp(fd, 200, "application/json", j);
  } else if (path == "/wifi/scan" && method == "GET") {
    WiFi.scanNetworks(true, true);
    unsigned long t0 = millis();
    while (WiFi.scanComplete() < 0 && (millis() - t0) < 8000UL) {
      esp_task_wdt_reset();
      yield();
      delay(100);
    }
    int n = WiFi.scanComplete();
    if (n < 0) n = 0;
    String json = "[";
    for (int i = 0; i < n; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"enc\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? 1 : 0) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    sendResp(fd, 200, "application/json", json);
  } else if (path == "/wifi/set" && method == "POST") {
    String ssid = "", pass = "";
    int si = body.indexOf("\"ssid\""), pi = body.indexOf("\"password\"");
    if (si >= 0) {
      int vs = body.indexOf('"', si + 6) + 1, ve = body.indexOf('"', vs);
      if (vs > 0 && ve > vs) ssid = body.substring(vs, ve);
    }
    if (pi >= 0) {
      int vs = body.indexOf('"', pi + 10) + 1, ve = body.indexOf('"', vs);
      if (vs > 0 && ve > vs) pass = body.substring(vs, ve);
    }
    if (ssid.isEmpty()) {
      sendResp(fd, 400, "application/json", "{\"status\":\"error\",\"msg\":\"SSID required\"}");
    } else {
      saveWifiConfig(devName, ssid, pass);
      pendStop = true;
      pendStopAt = millis() + 500UL;
      pendWifi = true;
      sendResp(fd, 200, "application/json",
               "{\"status\":\"ok\",\"msg\":\"Saved. Open: http://" MDNS_NAME ".local\"}");
    }
 } else if (path == "/relay/on" && method == "POST") {
    setRelay(true);
    sendResp(fd, 200, "application/json", "{\"status\":\"ok\",\"relay\":\"ON\",\"state\":true}");
} else if (path == "/relay/off" && method == "POST") {
    setRelay(false);
    sendResp(fd, 200, "application/json", "{\"status\":\"ok\",\"relay\":\"OFF\",\"state\":false}");
} else if (path == "/relay/restart" && method == "POST") {
    pendRelayAction = 2;   // keep deferred — doRestart() is safe to defer
    sendResp(fd, 200, "application/json", "{\"status\":\"ok\",\"relay\":\"RESTARTED\"}");
} else if (path == "/relay/status" && method == "GET") {
    sendResp(fd, 200, "application/json", "{\"relay\":\"" + String(relayState ? "ON" : "OFF") + "\"}");
  } else if (path == "/scheduler" && method == "POST") {
    int h = constrain(getParam(body, "h").toInt(), 0, 99);
    int m = constrain(getParam(body, "m").toInt(), 0, 59);
    int s = constrain(getParam(body, "s").toInt(), 0, 59);
    int act = constrain(getParam(body, "action").toInt(), 0, 2);
    bool en = getParam(body, "enabled") == "1";
    if (en && h == 0 && m == 0 && s == 0)
      sendResp(fd, 400, "application/json", "{\"ok\":false,\"msg\":\"Non-zero interval required\"}");
    else {
      applyManual(h, m, s, act, en);
      sendResp(fd, 200, "application/json", "{\"ok\":true}");
    }
  } else if (path == "/set-restart-interval" && (method == "POST" || method == "GET")) {
    String valStr = qParam("value");
    if (valStr.length() == 0) {
      sendResp(fd, 400, "application/json", "{\"status\":\"error\",\"msg\":\"value param required\"}");
    } else {
      unsigned long ms = (unsigned long)valStr.toDouble();
      setAutoInterval(ms);
      char rbuf[128];
      snprintf(rbuf, sizeof(rbuf),
               "{\"status\":\"ok\",\"autoIntervalMs\":%lu,\"autoIntervalSec\":%lu}",
               autoIntervalMs, autoIntervalMs / 1000UL);
      sendResp(fd, 200, "application/json", String(rbuf));
    }
  } else if (path == "/set-restart-enabled" && (method == "POST" || method == "GET")) {
    String valStr = qParam("value");
    if (valStr.length() == 0) {
      sendResp(fd, 400, "application/json", "{\"status\":\"error\",\"msg\":\"value param required (0 or 1)\"}");
    } else {
      bool en = (valStr == "1" || valStr == "true");
      setAutoEnabled(en);
      char rbuf[128];
      snprintf(rbuf, sizeof(rbuf),
               "{\"status\":\"ok\",\"autoEnabled\":%s,\"autoActive\":%s}",
               autoEnabled ? "true" : "false", autoActive ? "true" : "false");
      sendResp(fd, 200, "application/json", String(rbuf));
    }
  } else if (path == "/target-ip/set" && method == "POST") {
    String ip = "";
    int ii = body.indexOf("\"ip\"");
    if (ii >= 0) {
      int vs = body.indexOf('"', ii + 4) + 1, ve = body.indexOf('"', vs);
      if (vs > 0 && ve > vs) ip = body.substring(vs, ve);
    }
    if (ip.isEmpty()) sendResp(fd, 400, "application/json", "{\"status\":\"error\",\"msg\":\"IP required\"}");
    else {
      targetIP = ip;
      sendResp(fd, 200, "application/json", "{\"status\":\"ok\",\"ip\":\"" + ip + "\"}");
    }
  } else if (path == "/target-ip/status" && method == "GET") {
    sendResp(fd, 200, "application/json", "{\"ip\":\"" + targetIP + "\"}");
  } else if (path == "/api/reboot" && method == "POST") {
    if (!authed) {
      send401(fd);
      return;
    }
    sendResp(fd, 200, "application/json", "{\"ok\":true}");
    shutdown(fd, SHUT_WR);
    delay(100);
    pendReboot = true;
  } else if (path == "/api/change-password" && method == "POST") {
    String newPass = "";
    int pi = body.indexOf("\"password\"");
    if (pi >= 0) {
      int vs = body.indexOf('"', pi + 10) + 1;
      int ve = body.indexOf('"', vs);
      if (vs > 0 && ve > vs) newPass = body.substring(vs, ve);
    }
    if (newPass.length() < 4) {
      sendResp(fd, 400, "application/json", "{\"ok\":false,\"msg\":\"Password too short (min 4 chars)\"}");
    } else {
      loginPass = newPass;
      prefs.begin("sw", false);
      prefs.putString("pw", newPass);
      prefs.end();
      Serial.println("[CFG] Password changed");
      sendResp(fd, 200, "application/json", "{\"ok\":true}");
    }
  } else if (method == "GET" && LittleFS.exists(path)) {
    String ct = "text/plain";
    if (path.endsWith(".html")) ct = "text/html; charset=utf-8";
    else if (path.endsWith(".jpg") || path.endsWith(".jpeg")) ct = "image/jpeg";
    else if (path.endsWith(".png")) ct = "image/png";
    else if (path.endsWith(".css")) ct = "text/css";
    else if (path.endsWith(".js")) ct = "application/javascript";
    serveFile(fd, path, true, ct.c_str());
  } else {
    sendResp(fd, 404, "text/plain", "404 Not found: " + path);
  }
}

// ================================================================
//  RAW HTTP SERVER — INIT + TICK
// ================================================================
void initServer() {
  srvFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (srvFd < 0) {
    Serial.println("[WEB] socket() failed");
    return;
  }
  int opt = 1;
  setsockopt(srvFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  int fl = fcntl(srvFd, F_GETFL, 0);
  fcntl(srvFd, F_SETFL, fl | O_NONBLOCK);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(80);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(srvFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    Serial.println("[WEB] bind() failed");
    close(srvFd);
    srvFd = -1;
    return;
  }
  if (listen(srvFd, 5) < 0) {
    Serial.println("[WEB] listen() failed");
    close(srvFd);
    srvFd = -1;
    return;
  }
  Serial.println("[WEB] HTTP server ready on port 80");
}
void tickHTTP() {
  if (srvFd < 0) return;
  while (true) {
    struct sockaddr_in cAddr;
    socklen_t cLen = sizeof(cAddr);
    int cFd = accept(srvFd, (struct sockaddr*)&cAddr, &cLen);
    if (cFd < 0) break;
    lastHTTPClientAt = millis();  // ← ADD THIS LINE
    int flag = 1;
    setsockopt(cFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    pendRelayAction = -1;
    esp_task_wdt_reset();
    handleHTTPClient(cFd, cAddr.sin_addr.s_addr);
    esp_task_wdt_reset();
    shutdown(cFd, SHUT_WR);
    delay(30);
    close(cFd);
    if (pendRelayAction >= 0) {
      int act = pendRelayAction;
      pendRelayAction = -1;
      delay(10);
      webRelayAction(act);
    }
    esp_task_wdt_reset();
    yield();
    if (pendReboot) {
      delay(300);
      ESP.restart();
    }
  }
}

// ================================================================
//  ASYNC REST SERVER — PORT 8080
// ================================================================
void addCORS(AsyncWebServerResponse* resp) {
  resp->addHeader("Access-Control-Allow-Origin", "*");
  resp->addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
}
void sendAsyncJSON(AsyncWebServerRequest* req, int code, const String& payload) {
  AsyncWebServerResponse* resp = req->beginResponse(code, "application/json", payload);
  addCORS(resp);
  req->send(resp);
}
void setupAsyncServer() {
  asyncServer.on("/relay/on", HTTP_POST, [](AsyncWebServerRequest* req) {
    webRelayAction(1);
    sendAsyncJSON(req, 200, "{\"status\":\"ok\",\"relay\":\"ON\"}");
  });
  asyncServer.on("/relay/off", HTTP_POST, [](AsyncWebServerRequest* req) {
    webRelayAction(0);
    sendAsyncJSON(req, 200, "{\"status\":\"ok\",\"relay\":\"OFF\"}");
  });
  asyncServer.on("/relay/restart", HTTP_POST, [](AsyncWebServerRequest* req) {
    webRelayAction(2);
    sendAsyncJSON(req, 200, "{\"status\":\"ok\",\"relay\":\"RESTARTED\"}");
  });
  asyncServer.on("/relay/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    sendAsyncJSON(req, 200, "{\"relay\":\"" + String(relayState ? "ON" : "OFF") + "\"}");
  });
  asyncServer.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    sendAsyncJSON(req, 200, statusJSON());
  });

  asyncServer.on("/set-restart-interval", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("value", true) && !req->hasParam("value")) {
      sendAsyncJSON(req, 400, "{\"status\":\"error\",\"msg\":\"value param required\"}");
      return;
    }
    String valStr = req->hasParam("value", true)
                      ? req->getParam("value", true)->value()
                      : req->getParam("value")->value();
    unsigned long ms = (unsigned long)valStr.toDouble();
    setAutoInterval(ms);
    char rbuf[128];
    snprintf(rbuf, sizeof(rbuf),
             "{\"status\":\"ok\",\"autoIntervalMs\":%lu,\"autoIntervalSec\":%lu}",
             autoIntervalMs, autoIntervalMs / 1000UL);
    sendAsyncJSON(req, 200, String(rbuf));
  });

  asyncServer.on("/set-restart-interval", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("value")) {
      sendAsyncJSON(req, 400, "{\"status\":\"error\",\"msg\":\"value param required\"}");
      return;
    }
    unsigned long ms = (unsigned long)req->getParam("value")->value().toDouble();
    setAutoInterval(ms);
    char rbuf[128];
    snprintf(rbuf, sizeof(rbuf),
             "{\"status\":\"ok\",\"autoIntervalMs\":%lu,\"autoIntervalSec\":%lu}",
             autoIntervalMs, autoIntervalMs / 1000UL);
    sendAsyncJSON(req, 200, String(rbuf));
  });

  asyncServer.on("/set-restart-enabled", HTTP_POST, [](AsyncWebServerRequest* req) {
    String valStr = "";
    if (req->hasParam("value", true)) valStr = req->getParam("value", true)->value();
    else if (req->hasParam("value")) valStr = req->getParam("value")->value();
    if (valStr.length() == 0) {
      sendAsyncJSON(req, 400, "{\"status\":\"error\",\"msg\":\"value param required (0 or 1)\"}");
      return;
    }
    bool en = (valStr == "1" || valStr == "true");
    setAutoEnabled(en);
    char rbuf[128];
    snprintf(rbuf, sizeof(rbuf),
             "{\"status\":\"ok\",\"autoEnabled\":%s,\"autoActive\":%s}",
             autoEnabled ? "true" : "false", autoActive ? "true" : "false");
    sendAsyncJSON(req, 200, String(rbuf));
  });

  asyncServer.on("/set-restart-enabled", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("value")) {
      sendAsyncJSON(req, 400, "{\"status\":\"error\",\"msg\":\"value param required (0 or 1)\"}");
      return;
    }
    bool en = (req->getParam("value")->value() == "1" || req->getParam("value")->value() == "true");
    setAutoEnabled(en);
    char rbuf[128];
    snprintf(rbuf, sizeof(rbuf),
             "{\"status\":\"ok\",\"autoEnabled\":%s,\"autoActive\":%s}",
             autoEnabled ? "true" : "false", autoActive ? "true" : "false");
    sendAsyncJSON(req, 200, String(rbuf));
  });

  asyncServer.on(
    "/login", HTTP_POST, [](AsyncWebServerRequest* req) {}, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, data, len)) {
        sendAsyncJSON(req, 400, "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
        return;
      }
      String u = doc["username"] | "", p = doc["password"] | "";
      if (u == loginUser && p == loginPass)
        sendAsyncJSON(req, 200, "{\"ok\":true,\"name\":\"" + devName + "\"}");
      else
        sendAsyncJSON(req, 401, "{\"ok\":false,\"msg\":\"Invalid credentials\"}");
    });

  asyncServer.on("/device/info", HTTP_GET, [](AsyncWebServerRequest* req) {
    String j = "{\"relay\":\"" + String(relayState ? "ON" : "OFF") + "\",\"ip\":\"" + myIP + "\",\"rssi\":" + String((int)WiFi.RSSI()) + ",\"eth\":" + String(ethUp ? "true" : "false") + ",\"wifi\":" + String(wifiUp ? "true" : "false") + ",\"targetIP\":\"" + targetIP + "\",\"devName\":\"" + devName + "\",\"autoIntervalMs\":" + String(autoIntervalMs) + ",\"autoEnabled\":" + String(autoEnabled ? "true" : "false") + "}";
    sendAsyncJSON(req, 200, j);
  });

  asyncServer.on(
    "/api/change-password", HTTP_POST, [](AsyncWebServerRequest* req) {}, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, data, len)) {
        sendAsyncJSON(req, 400, "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
        return;
      }
      String newPass = doc["password"] | "";
      if (newPass.length() < 4) {
        sendAsyncJSON(req, 400, "{\"ok\":false,\"msg\":\"Password too short (min 4 chars)\"}");
        return;
      }
      loginPass = newPass;
      prefs.begin("sw", false);
      prefs.putString("pw", newPass);
      prefs.end();
      Serial.println("[CFG] Password changed");
      sendAsyncJSON(req, 200, "{\"ok\":true}");
    });

  asyncServer.on(
    "/wifi/set", HTTP_POST, [](AsyncWebServerRequest* req) {}, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, data, len)) {
        sendAsyncJSON(req, 400, "{\"status\":\"error\",\"msg\":\"Invalid JSON\"}");
        return;
      }
      String ssid = doc["ssid"] | "", pass = doc["password"] | "";
      if (ssid.isEmpty()) {
        sendAsyncJSON(req, 400, "{\"status\":\"error\",\"msg\":\"SSID required\"}");
        return;
      }
      saveWifiConfig(devName, ssid, pass);
      sendAsyncJSON(req, 200, "{\"status\":\"ok\",\"msg\":\"Saved. Open: http://" MDNS_NAME ".local\"}");
    });

  asyncServer.on(
    "/scheduler/set", HTTP_POST, [](AsyncWebServerRequest* req) {}, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, data, len)) {
        sendAsyncJSON(req, 400, "{\"status\":\"error\",\"msg\":\"Invalid JSON\"}");
        return;
      }
      int h = doc["hours"] | 0, m = doc["minutes"] | 0, s = doc["seconds"] | 0, act = doc["action"] | 2;
      bool en = doc["enabled"] | true;
      applyManual(h, m, s, act, en);
      sendAsyncJSON(req, 200, "{\"status\":\"ok\",\"totalSeconds\":" + String((uint32_t)h * 3600 + m * 60 + s) + "}");
    });

  asyncServer.on(
    "/target-ip/set", HTTP_POST, [](AsyncWebServerRequest* req) {}, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, data, len)) {
        sendAsyncJSON(req, 400, "{\"status\":\"error\",\"msg\":\"Invalid JSON\"}");
        return;
      }
      String ip = doc["ip"] | "";
      if (ip.isEmpty()) {
        sendAsyncJSON(req, 400, "{\"status\":\"error\",\"msg\":\"IP required\"}");
        return;
      }
      targetIP = ip;
      sendAsyncJSON(req, 200, "{\"status\":\"ok\",\"ip\":\"" + ip + "\"}");
    });

  asyncServer.on("/target-ip/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    sendAsyncJSON(req, 200, "{\"ip\":\"" + targetIP + "\"}");
  });

  asyncServer.serveStatic("/", LittleFS, "/").setCacheControl("public, max-age=86400");
  asyncServer.onNotFound([](AsyncWebServerRequest* req) {
    if (req->method() == HTTP_OPTIONS) {
      AsyncWebServerResponse* r = req->beginResponse(204);
      addCORS(r);
      req->send(r);
    } else sendAsyncJSON(req, 404, "{\"status\":\"error\",\"msg\":\"Not found\"}");
  });
  asyncServer.begin();
  Serial.println("[REST] API server ready on port 8080");
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  esp_log_level_set("*", ESP_LOG_NONE);

  Serial.println("\n================================");
  Serial.println("  VerifAI SmartSwitch  v45");
  Serial.println("  PRODUCTION BUILD");
  Serial.println("  ethIp + parallel reconnect");
  Serial.println("================================");

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  digitalWrite(PIN_RELAY, LOW);
  digitalWrite(PIN_LED, LOW);

  esp_task_wdt_config_t wdt_cfg = { .timeout_ms = WDT_TIMEOUT_SEC * 1000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&wdt_cfg);
  esp_task_wdt_add(NULL);
  Serial.printf("[WDT] Armed >%ds\n", WDT_TIMEOUT_SEC);

  loadConfig();
  createRelayTimer();

  WiFi.onEvent(onNetEvent);
  WiFi.mode(WIFI_AP_STA);
  startAP();
  restartMDNS();
  initServer();

  if (!LittleFS.begin(true)) Serial.println("[FS] Mount FAILED — re-upload data folder");
  else Serial.println("[FS] LittleFS OK");

  setupAsyncServer();
  setRelay(true);
  manOn = false;
  manInterval = 0;
  lastHeapLogAt = millis();

  // ── Start WiFi in background BEFORE ETH init ──────────────
  if (wSSID.length()) {
    Serial.println("[NET] Pre-starting WiFi in background...");
    WiFi.begin(wSSID.c_str(), wPass.c_str());
  }

  bool netStarted = false;
  if (initETH()) {
    netStarted = true;
    Serial.println("--------------------------------");
    Serial.println("[NET] Ethernet ready");
    Serial.println("[UI]  http://" + myIP + "  or  http://" MDNS_NAME ".local");
    Serial.println("--------------------------------");
  }
  ethInitDone = true;

  if (!netStarted && wSSID.length()) {
    Serial.println("[NET] Waiting for WiFi (already connecting)...");
    unsigned long t0 = millis();
    unsigned long wifiWait = wifiUp ? 0 : T_WIFI_TIMEOUT;  // skip wait if already connected
    while (!wifiUp && (millis() - t0) < wifiWait) {
      esp_task_wdt_reset();
      yield();
      delay(10);
    }
    if (wifiUp) {
      netStarted = true;
      Serial.println("--------------------------------");
      Serial.println("[NET] WiFi ready (no ETH)");
      Serial.println("[UI]  http://" + myIP + "  or  http://" MDNS_NAME ".local");
      Serial.println("--------------------------------");
    } else {
      // Not connected yet — don't block, let loop() + tickWiFiRetry handle it
      Serial.println("[NET] WiFi connecting in background — entering loop");
    }
  }

 // AFTER (fixed)
if (!netStarted && !wSSID.length() && !ethUp) {
    Serial.println("--------------------------------");
    Serial.println("[NET] No network — hold button 3s to configure");
    Serial.println("--------------------------------");
}

  Serial.printf("[AUTO] Restart interval: %lu ms (%lu min %lu s) enabled: %s\n",
                autoIntervalMs, autoIntervalMs / 60000, (autoIntervalMs % 60000) / 1000,
                autoEnabled ? "YES" : "NO");
  Serial.printf("[Heap] Free: %u bytes\n", ESP.getFreeHeap());
  Serial.println("[SYS] Ready — ETH priority | ethIp+wifiStaIp tracking ON");
  Serial.println("================================\n");
}

// ================================================================
//  TARGET IP PING WATCHDOG
// ================================================================
#define T_PING_INTERVAL 30000UL
#define PING_FAIL_MAX 3
#define PING_PORT 80
#define PING_TIMEOUT_MS 2000

bool doPing(const String& ip) {
  if (!ip.length()) return false;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PING_PORT);
  addr.sin_addr.s_addr = inet_addr(ip.c_str());
  if (addr.sin_addr.s_addr == INADDR_NONE) return false;
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return false;
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  connect(fd, (struct sockaddr*)&addr, sizeof(addr));
  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(fd, &wfds);
  struct timeval tv;
  tv.tv_sec = PING_TIMEOUT_MS / 1000;
  tv.tv_usec = (PING_TIMEOUT_MS % 1000) * 1000;
  int result = select(fd + 1, NULL, &wfds, NULL, &tv);
  close(fd);
  return (result > 0);
}

void tickPingTarget() {
  if (targetIP.length() == 0) return;
  if (!ethUp && !wifiUp) return;
  unsigned long now = millis();
  if ((now - lastPingAt) < T_PING_INTERVAL) return;
  lastPingAt = now;
  esp_task_wdt_reset();
  bool alive = doPing(targetIP);
  esp_task_wdt_reset();
  if (alive) {
    if (!targetOnline) Serial.println("[Ping] Target " + targetIP + " is ONLINE");
    targetOnline = true;
    pingFailCount = 0;
    targetLastSeen = now;
  } else {
    pingFailCount++;
    targetOnline = false;
    Serial.printf("[Ping] Target %s OFFLINE — fail %d/%d\n",
                  targetIP.c_str(), pingFailCount, PING_FAIL_MAX);
    if (pingFailCount >= PING_FAIL_MAX) {
      Serial.println("[Ping] 3 consecutive fails — restarting relay");
      pingFailCount = 0;
      webRelayAction(2);
    }
  }
}
// ================================================================
//  ETH WIFI HANDOFF
// ================================================================
void tickETHWifiHandoff() {
  if (!ethWifiDropPend || !ethUp) {
    ethWifiDropPend = false;
    return;
  }
  if (millis() < ethWifiDropAt) return;
  ethWifiDropPend = false;
  wifiUp = false;
  WiFi.disconnect(true);
  Serial.println("[ETH] Deferred WiFi disconnect complete");
}
void tickMDNSWatchdog() {
  if (!ethUp && !wifiUp) return;
  unsigned long now = millis();
  if ((now - lastMDNSCheckAt) < T_MDNS_WATCHDOG) return;
  lastMDNSCheckAt = now;
  IPAddress resolved = MDNS.queryHost(MDNS_NAME, 1000);
  if (resolved == IPAddress(0, 0, 0, 0)) {
    Serial.println("[mDNS] Watchdog: not responding — restarting mDNS");
    restartMDNS();
    lastMDNSAt = now;
  } else {
    Serial.println("[mDNS] Watchdog: OK → " + resolved.toString());
  }
}
// ================================================================
//  LOOP
// ================================================================
void loop() {
  esp_task_wdt_reset();
  tickHTTP();
  esp_task_wdt_reset();
  yield();
  tickServerClient();
  esp_task_wdt_reset();
  yield();
  tickETHRecovery();
  tickETHHotplug();
  tickETHWifiHandoff();
  tickBtn();
  tickLED();
  tickRelayRestart();
  tickAutoScheduler();
  tickManualScheduler();
  tickWiFiRetry();
  tickDeferred();
  tickHeapWatchdog();
  tickPingTarget();
  tickAPManager();
  if ((ethUp || wifiUp) && (millis() - lastMDNSAt) > T_MDNS_REANNOUNCE) {
    lastMDNSAt = millis();
    restartMDNS();
  }
  yield();
}