#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <vector>
#include <esp_chip_info.h>
#include <Audio.h>

// Suggested edits applied:
// board_build.arduino.memory_type = qio_opi
// build_flags = -DBOARD_HAS_PSRAM

struct Station
{
  String name;
  String url;
  bool favorite = false;
};

static const char *AP_SSID = "ESP32-Wifi-Setup";
static const char *AP_PASS = "12345678";
static const char *DEFAULT_WIFI_SSID = "555";
static const char *DEFAULT_WIFI_PASS = "141367141367";
static const char *HOSTNAME = "Radio";

static const uint8_t PIN_I2S_BCLK = 4;
static const uint8_t PIN_I2S_LRCK = 5;
static const uint8_t PIN_I2S_DOUT = 6;

static const uint8_t PIN_I2C_SDA = 8;
static const uint8_t PIN_I2C_SCL = 9;
static const uint8_t OLED_ADDR = 0x3C;

static const char *FILE_STATIONS = "/stations.json";
static const char *FILE_WIFI = "/wifi.json";

static const uint32_t DISPLAY_REFRESH_MS = 750;
static const uint32_t STATION_OVERLAY_MS = 2000;
static const uint32_t VOLUME_OVERLAY_MS = 1500;
static const uint32_t AUDIO_RECONNECT_MS = 1000;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 5000;
static const uint32_t STATION_RECONNECT_MS = 5000;
static const uint32_t AUDIO_STALL_TIMEOUT_MS = 12000;
static const uint32_t AUDIO_BUFFER_LOW_WATERMARK = 4096;
static const uint32_t AUDIO_WATCHDOG_SAMPLE_MS = 1000;
static const uint32_t WIFI_RECONNECT_RETRY_MS = 5000;
static const uint8_t WIFI_CONNECT_RETRIES = 5;

Preferences prefs;
WebServer server(80);
DNSServer dns;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

void stopCurrentStation();
void playCurrentStation();
void stopAudio();
void audioSetVolume(int vol);
void saveRuntimePrefs();

std::vector<Station> stations;
Audio audio;
String wifiSsid;
String wifiPass;

int currentStation = 0;
int volumeValue = 50;
bool wifiConnected = false;
bool apMode = false;
bool autoStationReconnect = false;
unsigned long stationReconnectNextAt = 0;
String playStatus = "STOPPED";
String currentTrack = "";

unsigned long volumeOverlayUntil = 0;
unsigned long stationOverlayUntil = 0;
unsigned long lastDisplayRefresh = 0;
unsigned long reconnectAudioAt = 0;
static bool mdnsStarted = false;
static String connectedWifiSsid = "";

// line1 cycling and scrolling state
unsigned long line1SwitchAt = 0;
int line1Mode = 0; // 0 = station, 1 = track
int line1ScrollOffset = 0;
int line1ScrollStep = 0;
int line1ScrollMax = 0;
unsigned long line1StateStarted = 0;
const unsigned long line1Durations[2] = {2000, 3000};

enum ScreenMode
{
  SCREEN_BOOT,
  SCREEN_MAIN,
  SCREEN_VOLUME,
  SCREEN_STATION
};

ScreenMode screenMode = SCREEN_BOOT;

// Wi-Fi reconnect state must be declared before startApMode()/connectWifi().
static uint8_t wifiReconnectAttempt = 0;
static unsigned long wifiReconnectNextAt = 0;
static bool wifiReconnecting = false;

// Audio stream watchdog. Wi-Fi can remain associated while the WAN/Internet
// connection is down. In that case WiFi.status() stays WL_CONNECTED and the
// normal Wi-Fi-loss handler cannot detect the failure.
static unsigned long audioWatchdogSampleAt = 0;
static unsigned long audioLastBufferProgressAt = 0;
static uint32_t audioLastBufferFilled = 0;
static bool audioWatchdogArmed = false;

void updateDisplay();
void startApMode();
void startMdns();
void stopMdns();
void clearWifiReconnectState();
void wifiReconnectTick();
void audioReconnectTick();
void audioWatchdogTick();

void stopAudio(bool clearReconnect);
void stopAudio();
void stopAudio() { stopAudio(true); }

// =====================================================
// ESP32-audioI2S AUDIO ENGINE
// =====================================================

String audioError = "";
String audioInfo = "";
String audioBitrate = "";

void audio_info(const char *info)
{
  if (!info)
    return;

  Serial.printf("[AUDIO] %s\n", info);

  audioInfo = info;
}

void audio_showstation(const char *info)
{
  Serial.printf("[STATION] %s\n", info);

  if (info && strlen(info))
  {
    currentTrack = info;
  }
}

void audio_showstreamtitle(const char *info)
{
  Serial.printf("[TITLE] %s\n", info);

  if (info && strlen(info))
  {
    currentTrack = info;
    playStatus = "PLAYING";
  }
}

void audio_bitrate(const char *info)
{
  Serial.printf("[BITRATE] %s\n", info ? info : "");
  audioBitrate = info ? info : "";
}

void audio_commercial(const char *info)
{
  Serial.printf("[COMMERCIAL] %s\n", info);
}

void scheduleReconnect()
{
  reconnectAudioAt = millis() + AUDIO_RECONNECT_MS;
}

void audio_eof_mp3(const char *info)
{
  Serial.printf("[EOF] %s\n", info);

  playStatus = "RECONNECT";
  scheduleReconnect();
}

void audio_id3data(const char *info)
{
  Serial.printf("[ID3] %s\n", info);
}

void audio_id3image(const uint8_t *data, uint32_t size)
{
  (void)data;
  (void)size;
}

void audioInit()
{
  audio.setPinout(
      PIN_I2S_BCLK,
      PIN_I2S_LRCK,
      PIN_I2S_DOUT);

  // buffer size: removed call to audio.setBufsize (API changed)

  // ESP32-audioI2S громкость 0-21
  int vol = map(volumeValue, 0, 100, 0, 21);

  audio.setVolume(vol);

  audio.forceMono(false);

  // Run audio decoding task on the other core to avoid starvation of WiFi/network
  // If Arduino loop runs on core 1, run audio on core 0 (recommended by library)
  audio.setAudioTaskCore(0);

  Serial.println("ESP32-audioI2S initialized");
}

void audioSetVolume(int vol)
{
  volumeValue = constrain(vol, 0, 100);

  int audioVol =
      map(volumeValue, 0, 100, 0, 21);

  audio.setVolume(audioVol);

  screenMode = SCREEN_VOLUME;
  volumeOverlayUntil =
      millis() + VOLUME_OVERLAY_MS;
}

bool audioPlayUrl(const String &url)
{
  if (url.isEmpty())
  {
    playStatus = "BAD URL";
    return false;
  }

  Serial.println();
  Serial.println("Starting stream:");
  Serial.println(url);

  currentTrack = "";
  audioBitrate = "";

  // Reset the stream watchdog for every new connection attempt.
  audioWatchdogArmed = false;
  audioWatchdogSampleAt = millis();
  audioLastBufferProgressAt = millis();
  audioLastBufferFilled = 0;

  playStatus = "CONNECTING";

  audio.stopSong();

  bool result =
      audio.connecttohost(
          url.c_str());

  if (!result)
  {
    playStatus = "CONNECT FAIL";
    Serial.println(
        "Audio connect failed");

    return false;
  }

  playStatus = "BUFFERING";

  return true;
}

void stopAudio(bool clearReconnect)
{
  audio.stopSong();

  playStatus = "STOPPED";

  currentTrack = "";
  audioBitrate = "";

  audioWatchdogArmed = false;
  audioWatchdogSampleAt = 0;
  audioLastBufferProgressAt = 0;
  audioLastBufferFilled = 0;

  if (clearReconnect)
  {
    reconnectAudioAt = 0;
  }
}

bool audioIsRunning()
{
  return audio.isRunning();
}

void audioLoop()
{
  // In AP mode there is no station playback.
  if (apMode)
    return;

  audio.loop();

  if (!audio.isRunning() && autoStationReconnect && reconnectAudioAt == 0)
  {
    // The audio library can stop the stream without sending EOF.
    // Arm the reconnect timer only once; do not restart the timer on every loop.
    if (playStatus == "PLAYING" || playStatus == "RECONNECT")
    {
      playStatus = "RECONNECT";
      scheduleReconnect();
    }
  }
}

void audioWatchdogTick()
{
  if (!autoStationReconnect || apMode || !wifiConnected || stations.empty())
    return;

  if (!audio.isRunning())
    return;

  unsigned long now = millis();

  if (audioWatchdogSampleAt != 0 &&
      (unsigned long)(now - audioWatchdogSampleAt) < AUDIO_WATCHDOG_SAMPLE_MS)
    return;

  audioWatchdogSampleAt = now;

  uint32_t filled = audio.inBufferFilled();

  if (!audioWatchdogArmed)
  {
    audioWatchdogArmed = true;
    audioLastBufferFilled = filled;
    audioLastBufferProgressAt = now;
    return;
  }

  // A growing or comfortably filled buffer means the network stream is alive.
  if (filled > audioLastBufferFilled ||
      filled >= AUDIO_BUFFER_LOW_WATERMARK)
  {
    audioLastBufferProgressAt = now;
  }

  audioLastBufferFilled = filled;

  // Important: Wi-Fi may still report WL_CONNECTED while the Internet or
  // radio server is unreachable. In that situation Audio::isRunning() can
  // remain true, so the old isRunning()-only reconnect logic never fired.
  // Force a clean reconnect after a prolonged empty/stalled input buffer.
  if (filled < AUDIO_BUFFER_LOW_WATERMARK &&
      (unsigned long)(now - audioLastBufferProgressAt) >= AUDIO_STALL_TIMEOUT_MS)
  {
    Serial.printf(
        "Audio watchdog: stream stalled (buffer=%u bytes), restarting station\n",
        (unsigned)filled);

    audioWatchdogArmed = false;
    audioLastBufferProgressAt = now;
    audioLastBufferFilled = 0;

    playStatus = "RECONNECT";
    audio.stopSong();

    reconnectAudioAt = now + 500;
    stationReconnectNextAt = now + STATION_RECONNECT_MS;
  }
}

void audioReconnectTick()
{
  if (!autoStationReconnect || apMode || !wifiConnected || stations.empty())
    return;

  if (reconnectAudioAt == 0)
    return;

  unsigned long now = millis();
  if ((long)(now - reconnectAudioAt) < 0)
    return;

  reconnectAudioAt = 0;

  if (!audio.isRunning())
  {
    Serial.println("Audio reconnect...");
    playCurrentStation();
  }
}

void saveStations()
{
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (const auto &s : stations)
  {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = s.name;
    obj["url"] = s.url;
    obj["favorite"] = s.favorite;
  }

  File f = LittleFS.open(FILE_STATIONS, "w");
  if (!f)
    return;
  serializeJsonPretty(doc, f);
  f.close();
}

void loadStations()
{
  stations.clear();

  if (!LittleFS.exists(FILE_STATIONS))
  {
    stations.push_back({"Radio Record", "http://air.radiorecord.ru:8101/rr_320"});
    stations.push_back({"Relax FM", "http://ic7.101.ru:8000/v13_1"});
    saveStations();
    return;
  }

  File f = LittleFS.open(FILE_STATIONS, "r");
  if (!f)
    return;

  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok && doc.is<JsonArray>())
  {
    for (JsonObject obj : doc.as<JsonArray>())
    {
      Station s;
      s.name = obj["name"] | "Unknown";
      s.url = obj["url"] | "";
      s.favorite = obj["favorite"] | false;
      if (!s.url.isEmpty())
      {
        stations.push_back(s);
      }
    }
  }
  f.close();

  if (stations.empty())
  {
    stations.push_back({"Radio Record", "http://air.radiorecord.ru:8101/rr_320"});
    saveStations();
  }
}

void saveWifiConfig(const String &ssid, const String &pass)
{
  JsonDocument doc;
  doc["ssid"] = ssid;
  doc["pass"] = pass;

  File f = LittleFS.open(FILE_WIFI, "w");
  if (!f)
    return;
  serializeJson(doc, f);
  f.close();
}

bool loadWifiConfig()
{
  if (!LittleFS.exists(FILE_WIFI))
    return false;

  File f = LittleFS.open(FILE_WIFI, "r");
  if (!f)
    return false;

  JsonDocument doc;
  if (deserializeJson(doc, f) != DeserializationError::Ok)
  {
    f.close();
    return false;
  }

  wifiSsid = doc["ssid"] | "";
  wifiPass = doc["pass"] | "";
  f.close();

  return !wifiSsid.isEmpty();
}

void saveRuntimePrefs()
{
  if (!prefs.begin("radio", false))
  {
    Serial.println("Failed to open Preferences namespace");
    return;
  }

  prefs.putInt("volume", volumeValue);
  prefs.putInt("station", currentStation);
  prefs.end();
}

void loadRuntimePrefs()
{
  if (!prefs.begin("radio", false))
  {
    volumeValue = 50;
    currentStation = 0;
    return;
  }

  volumeValue = prefs.getInt("volume", 50);
  currentStation = prefs.getInt("station", 0);
  prefs.end();
}

void drawWrapped(const String &text, int x, int y, int width, uint8_t lineHeight)
{
  String remaining = text;
  while (!remaining.isEmpty())
  {
    int cut = remaining.length();
    while (cut > 1 && u8g2.getUTF8Width(remaining.substring(0, cut).c_str()) > width)
    {
      cut--;
    }
    if (cut < remaining.length())
    {
      int lastSpace = remaining.substring(0, cut).lastIndexOf(' ');
      if (lastSpace > 0)
        cut = lastSpace;
    }

    String line = remaining.substring(0, cut);
    line.trim();
    u8g2.drawUTF8(x, y, line.c_str());
    y += lineHeight;

    remaining = remaining.substring(cut);
    remaining.trim();
    if (y > 63)
      break;
  }
}

String getStationName()
{
  if (stations.empty())
    return "No stations";
  if (currentStation < 0 || currentStation >= (int)stations.size())
    return "No stations";
  return stations[currentStation].name;
}

void drawMainScreen()
{
  if (apMode)
  {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawUTF8(0, 10, "Setup AP");
    u8g2.drawUTF8(0, 22, AP_SSID);

    String ip = WiFi.softAPIP().toString();
    u8g2.drawUTF8(0, 34, ip.c_str());

    String vol = "Vol: " + String(volumeValue);
    u8g2.drawUTF8(0, 46, vol.c_str());

    String station = getStationName();
    drawWrapped(station, 0, 63, 128, 12);
    return;
  }

  // determine what to show on line 1 (station then track)
  unsigned long now = millis();
  if (line1StateStarted == 0)
  {
    line1StateStarted = now;
    line1Mode = 0;
    line1ScrollOffset = 0;
  }

  if (now - line1StateStarted >= line1Durations[line1Mode])
  {
    // switch mode
    line1Mode = (line1Mode + 1) % 2;
    line1StateStarted = now;
    line1ScrollOffset = 0;
  }

  String line1Text = (line1Mode == 0) ? getStationName() : (currentTrack.isEmpty() ? getStationName() : currentTrack);

  // prepare scrolling if text wider than display and in track mode (2s)
  u8g2.setFont(u8g2_font_8x13_tf);
  int textW = u8g2.getUTF8Width(line1Text.c_str());
  int availW = 128;

  if (line1Mode == 1 && textW > availW)
  {
    // simple left scroll proportional to elapsed time within the 2s window
    unsigned long elapsed = now - line1StateStarted;
    float progress = (float)elapsed / (float)line1Durations[1];
    int maxOffset = textW - availW;
    int xOffset = (int)(progress * maxOffset + 0.5);
    if (xOffset < 0)
      xOffset = 0;
    if (xOffset > maxOffset)
      xOffset = maxOffset;
    // draw shifted text with negative x to clip instead of wrapping
    u8g2.drawUTF8(-xOffset, 13, line1Text.c_str());
  }
  else
  {
    // static draw (clipped if too long)
    u8g2.drawUTF8(0, 13, line1Text.c_str());
  }

  // line 2: volume and RSSI
  u8g2.setFont(u8g2_font_6x12_tf);
  String vol = "Vol: " + String(volumeValue);
  String rssi = wifiConnected ? ("RSSI: " + String(WiFi.RSSI()) + " dBm") : "RSSI: -";
  String line2 = vol + "  " + rssi;
  u8g2.drawUTF8(0, 30, line2.c_str());

  // line3: IP and status
  String ip = wifiConnected ? WiFi.localIP().toString() : "-";
  String statusLine = "IP:" + ip + " " + playStatus;

  // If combined line is too wide, show status in shorter form
  u8g2.setFont(u8g2_font_6x12_tf);
  if (u8g2.getUTF8Width(statusLine.c_str()) > 128)
  {
    String shortStatus = playStatus;
    if (playStatus == "BUFFERING")
      shortStatus = "BUF";
    else if (playStatus == "PLAYING")
      shortStatus = "PLAY";
    else if (playStatus == "STOPPED")
      shortStatus = "STOP";
    else if (playStatus == "CONNECTING")
      shortStatus = "CONN";
    statusLine = "IP:" + ip + " " + shortStatus;
  }
  u8g2.drawUTF8(0, 44, statusLine.c_str());

  // line4: codec + bitrate
  String codecLine = audio.isRunning() ? String(audio.getCodecname()) : "";
  if (!audioBitrate.isEmpty())
    codecLine += " " + audioBitrate;
  if (codecLine.isEmpty())
    codecLine = playStatus;
  u8g2.drawUTF8(0, 58, codecLine.c_str());
}

void drawVolumeOverlay()
{
  u8g2.setFont(u8g2_font_7x14B_tf);
  u8g2.drawUTF8(34, 14, "Volume");

  u8g2.setFont(u8g2_font_logisoso24_tf);
  String s = String(volumeValue);
  int w = u8g2.getUTF8Width(s.c_str());
  u8g2.drawUTF8((128 - w) / 2, 42, s.c_str());

  int barX = 12;
  int barY = 50;
  int barW = 104;
  int fillW = map(volumeValue, 0, 100, 0, barW - 2);
  u8g2.drawFrame(barX, barY, barW, 10);
  u8g2.drawBox(barX + 1, barY + 1, fillW, 8);
}

void drawStationOverlay()
{
  // Заголовок
  u8g2.setFont(u8g2_font_7x13_tf);

  const char *title = "CONNECTING...";
  int x = (128 - u8g2.getUTF8Width(title)) / 2;
  u8g2.drawUTF8(x, 14, title);

  // Название станции
  String stationName = getStationName();

  u8g2.setFont(u8g2_font_8x13_tf);
  drawWrapped(stationName, 2, 36, 124, 13);

  // Статус
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawUTF8(2, 60, playStatus.c_str());
}

void updateDisplay()
{
  u8g2.clearBuffer();

  switch (screenMode)
  {
  case SCREEN_BOOT:
    u8g2.setFont(u8g2_font_7x14B_tf);
    u8g2.drawUTF8(18, 20, "Internet Radio");

    u8g2.setFont(u8g2_font_6x12_tf);
    if (apMode)
    {
      u8g2.drawUTF8(0, 40, "AP mode");
      u8g2.drawUTF8(0, 54, AP_SSID);
    }
    else if (wifiConnected)
    {
      u8g2.drawUTF8(0, 40, "WiFi connected");
      String ip = WiFi.localIP().toString();
      u8g2.drawUTF8(0, 54, ip.c_str());
    }
    else
    {
      u8g2.drawUTF8(0, 40, "Booting...");
    }
    break;

  case SCREEN_VOLUME:
    drawVolumeOverlay();
    break;

  case SCREEN_STATION:
    drawStationOverlay();
    break;

  case SCREEN_MAIN:
  default:
    drawMainScreen();
    break;
  }

  u8g2.sendBuffer();
}

void stopMdns()
{
  if (mdnsStarted)
  {
    MDNS.end();
    mdnsStarted = false;
  }
}

void startMdns()
{
  stopMdns();

  if (WiFi.status() == WL_CONNECTED)
  {
    if (MDNS.begin(HOSTNAME))
    {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
      Serial.printf("mDNS started: http://%s.local/\n", HOSTNAME);
    }
    else
    {
      Serial.println("mDNS start failed");
    }
  }
}

void clearWifiReconnectState()
{
  wifiReconnecting = false;
  wifiReconnectAttempt = 0;
  wifiReconnectNextAt = 0;
}

void startApMode()
{
  clearWifiReconnectState();
  stopMdns();
  stopAudio();

  apMode = true;
  wifiConnected = false;
  connectedWifiSsid = "";
  playStatus = "AP MODE";
  screenMode = SCREEN_MAIN;

  WiFi.mode(WIFI_AP);
  WiFi.softAPdisconnect(true);
  delay(100);

  if (!WiFi.softAP(AP_SSID, AP_PASS))
  {
    Serial.println("ERROR: failed to start AP");
  }
  else
  {
    Serial.printf("AP started: %s, IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }

  dns.stop();
  dns.start(53, "*", WiFi.softAPIP());
}

bool connectWifi()
{
  clearWifiReconnectState();
  stopMdns();
  dns.stop();
  apMode = false;
  wifiConnected = false;

  String savedSsid;
  String savedPass;
  if (loadWifiConfig())
  {
    savedSsid = wifiSsid;
    savedPass = wifiPass;
  }

  auto tryConnect = [&](const String &ssid, const String &pass) -> bool
  {
    if (ssid.isEmpty())
      return false;

    for (uint8_t attempt = 1; attempt <= WIFI_CONNECT_RETRIES; ++attempt)
    {
      Serial.printf("WiFi connect attempt %u/%u to SSID: %s\n",
                    attempt, WIFI_CONNECT_RETRIES, ssid.c_str());

      WiFi.mode(WIFI_STA);
      WiFi.setHostname(HOSTNAME);
      WiFi.disconnect(false, false);
      delay(100);
      WiFi.begin(ssid.c_str(), pass.c_str());

      unsigned long connectStart = millis();
      while (WiFi.status() != WL_CONNECTED &&
             millis() - connectStart < WIFI_CONNECT_TIMEOUT_MS)
      {
        delay(50);
      }

      if (WiFi.status() == WL_CONNECTED)
      {
        wifiSsid = ssid;
        wifiPass = pass;
        connectedWifiSsid = ssid;
        wifiConnected = true;
        apMode = false;
        clearWifiReconnectState();
        dns.stop();
        startMdns();

        // Persist credentials that actually worked (including fallback).
        saveWifiConfig(ssid, pass);

        Serial.printf("WiFi connected to %s, IP: %s, RSSI: %d dBm\n",
                      ssid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
      }

      Serial.printf("WiFi connect failed for %s (status=%d)\n",
                    ssid.c_str(), (int)WiFi.status());
      WiFi.disconnect(false, false);
      delay(300);
    }

    return false;
  };

  if (tryConnect(savedSsid, savedPass))
    return true;

  if (savedSsid != DEFAULT_WIFI_SSID || savedPass != DEFAULT_WIFI_PASS)
  {
    Serial.println("Saved WiFi credentials failed, trying default fallback SSID");
    if (tryConnect(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS))
      return true;
  }

  return false;
}

void printSystemDiagnostics()
{
  esp_chip_info_t chipInfo;
  esp_chip_info(&chipInfo);

  Serial.println();
  Serial.println("=== System diagnostics ===");
  Serial.printf("Chip model: ESP32-S3\n");
  Serial.printf("Chip cores: %d\n", chipInfo.cores);
  Serial.printf("Chip revision: %d\n", chipInfo.revision);
  Serial.printf("CPU freq: %u MHz\n", getCpuFrequencyMhz());

  Serial.printf("Flash size: %u bytes\n", ESP.getFlashChipSize());
  Serial.printf("Flash speed: %u Hz\n", ESP.getFlashChipSpeed());

  Serial.printf("Heap total: %u bytes\n", ESP.getHeapSize());
  Serial.printf("Heap free: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Heap min free: %u bytes\n", ESP.getMinFreeHeap());
  Serial.printf("Largest free block: %u bytes\n", ESP.getMaxAllocHeap());

  if (psramFound())
  {
    Serial.println("PSRAM: found");
    Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());
    Serial.printf("PSRAM free: %u bytes\n", ESP.getFreePsram());
  }
  else
  {
    Serial.println("PSRAM: not found");
  }

  Serial.println("==========================");
  Serial.println();
}

void playCurrentStation()
{
  if (stations.empty())
    return;

  if (currentStation < 0)
    currentStation = 0;
  if (currentStation >= (int)stations.size())
    currentStation = 0;

  currentTrack = "";
  playStatus = "CONNECTING";
  autoStationReconnect = true;
  stationReconnectNextAt = millis() + STATION_RECONNECT_MS;
  screenMode = SCREEN_STATION;
  stationOverlayUntil = millis() + STATION_OVERLAY_MS;
  reconnectAudioAt = 0;

  if (!audioPlayUrl(stations[currentStation].url))
  {
    playStatus = "CONNECT FAIL";
    scheduleReconnect();
  }
  Serial.printf("Heap      %u\n", ESP.getFreeHeap());
  Serial.printf("Largest   %u\n", ESP.getMaxAllocHeap());
  Serial.printf("PSRAM     %u\n", ESP.getFreePsram());

  saveRuntimePrefs();
}

void stopCurrentStation()
{
  stopAudio();
  currentTrack = "";
  playStatus = "STOPPED";
  autoStationReconnect = false;
  stationReconnectNextAt = 0;
}
void stationReconnectTick()
{
  if (!autoStationReconnect || apMode || !wifiConnected || stations.empty())
    return;

  // Keep a slow safety retry for cases where the audio library does not report
  // a clean EOF/stop event. Normal reconnects are handled by audioReconnectTick().
  unsigned long now = millis();
  if ((long)(now - stationReconnectNextAt) < 0)
    return;

  stationReconnectNextAt = now + STATION_RECONNECT_MS;

  if (!audio.isRunning() &&
      (playStatus == "CONNECT FAIL" ||
       playStatus == "RECONNECT" ||
       playStatus == "BUFFERING" ||
       playStatus == "CONNECTING"))
  {
    Serial.println("Station safety reconnect");
    playCurrentStation();
  }
}

String contentType(const String &path)
{
  if (path.endsWith(".html"))
    return "text/html";
  if (path.endsWith(".css"))
    return "text/css";
  if (path.endsWith(".js"))
    return "application/javascript";
  if (path.endsWith(".json"))
    return "application/json";
  return "text/plain";
}

bool handleFileRead(String path)
{
  if (path == "/")
    path = "/index.html";
  if (!LittleFS.exists(path))
    return false;

  File f = LittleFS.open(path, "r");
  server.streamFile(f, contentType(path));
  f.close();
  return true;
}

String trimCopy(String s)
{
  s.trim();
  return s;
}

bool isHttpStreamUrl(const String &s)
{
  String v = trimCopy(s);
  v.toLowerCase();
  return v.startsWith("http://") || v.startsWith("https://");
}

bool stationUrlExists(const String &url)
{
  for (const auto &s : stations)
  {
    if (s.url == url)
    {
      return true;
    }
  }
  return false;
}

String extractNameFromExtInf(const String &line)
{
  int comma = line.indexOf(',');
  if (comma < 0)
  {
    return "";
  }

  String name = line.substring(comma + 1);
  name.trim();
  return name;
}

String fileNameFromUrl(const String &url)
{
  int slash = url.lastIndexOf('/');
  if (slash >= 0 && slash + 1 < (int)url.length())
  {
    String tail = url.substring(slash + 1);
    int q = tail.indexOf('?');
    if (q >= 0)
    {
      tail = tail.substring(0, q);
    }
    tail.trim();
    if (!tail.isEmpty())
    {
      return tail;
    }
  }
  return url;
}

int importStationsFromM3U(const String &body, int &duplicateCount, int &invalidCount)
{
  duplicateCount = 0;
  invalidCount = 0;

  String pendingName = "";
  int added = 0;
  int start = 0;

  while (start <= (int)body.length())
  {
    int end = body.indexOf('\n', start);
    String line;

    if (end < 0)
    {
      line = body.substring(start);
      start = body.length() + 1;
    }
    else
    {
      line = body.substring(start, end);
      start = end + 1;
    }

    line.replace("\r", "");
    line.trim();

    if (line.isEmpty())
    {
      continue;
    }

    if (line.startsWith("#EXTM3U"))
    {
      continue;
    }

    if (line.startsWith("#EXTINF:"))
    {
      pendingName = extractNameFromExtInf(line);
      continue;
    }

    if (line.startsWith("#"))
    {
      continue;
    }

    if (!isHttpStreamUrl(line))
    {
      invalidCount++;
      pendingName = "";
      continue;
    }

    if (stationUrlExists(line))
    {
      duplicateCount++;
      pendingName = "";
      continue;
    }

    String name = pendingName;
    if (name.isEmpty())
    {
      name = fileNameFromUrl(line);
    }

    stations.push_back({name, line});
    added++;
    pendingName = "";
  }

  if (added > 0)
  {
    saveStations();
  }

  return added;
}

void setupApi()
{
  auto sendJson = [](int code, const String &json)
  {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(code, "application/json", json);
  };

  server.on("/api/state", HTTP_OPTIONS, []()
            {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(204); });

  server.on("/api/state", HTTP_GET, [sendJson]()
            {
    JsonDocument doc;
doc["wifiConnected"] = wifiConnected;

doc["ip"] =
    apMode
    ? WiFi.softAPIP().toString()
    : WiFi.localIP().toString();


doc["rssi"] =
    wifiConnected
    ? String(WiFi.RSSI()) + " dBm"
    : "-";


if(!stations.empty() &&
   currentStation < (int)stations.size())
{
 doc["stationName"]=stations[currentStation].name;
}
else
{
 doc["stationName"]="";
}

doc["stationIndex"] = currentStation;
doc["playStatus"] = playStatus;
doc["volume"] = volumeValue;
doc["track"] = currentTrack;
doc["audioInfo"] = audioInfo;
doc["audioError"] = audioError;
doc["audioBitrate"] = audioBitrate;
// библиотека может определить кодек во время воспроизведения
doc["codec"] = audio.isRunning() ? String(audio.getCodecname()) : "AUTO";
doc["audioRunning"] =
    audio.isRunning();

doc["savedSsid"] = wifiSsid;

    String out;
    serializeJson(doc, out);
    sendJson(200, out); });

  server.on("/api/stations", HTTP_GET, [sendJson]()
            {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto &s : stations) {
      JsonObject obj = arr.add<JsonObject>();
      obj["name"] = s.name;
      obj["url"] = s.url;
      obj["favorite"] = s.favorite;
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out); });

  server.on("/api/next", HTTP_POST, [sendJson]()
            {
    if (!stations.empty()) {
      currentStation = (currentStation + 1) % stations.size();
      playCurrentStation();
    }
    sendJson(200, R"({"ok":true})"); });

  server.on("/api/prev", HTTP_POST, [sendJson]()
            {
    if (!stations.empty()) {
      currentStation = (currentStation - 1 + stations.size()) % stations.size();
      playCurrentStation();
    }
    sendJson(200, R"({"ok":true})"); });

  server.on("/api/stop", HTTP_POST, [sendJson]()
            {
    stopCurrentStation();
    sendJson(200, R"({"ok":true})"); });

  server.on("/api/play", HTTP_POST, [sendJson]()
            {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJson(400, "{\"error\":\"bad json\"}");
      return;
    }

    int idx = doc["index"] | 0;
    if (idx < 0 || idx >= (int)stations.size()) {
      sendJson(400, "{\"error\":\"bad index\"}");
      return;
    }

    currentStation = idx;
    playCurrentStation();
    sendJson(200, R"({"ok":true})"); });

  server.on("/api/volume", HTTP_POST, [sendJson]()
            {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJson(400, "{\"error\":\"bad json\"}");
      return;
    }

    int v = doc["volume"] | 50;
    audioSetVolume(v);
    saveRuntimePrefs();
    sendJson(200, R"({"ok":true})"); });

  server.on("/api/stations", HTTP_POST, [sendJson]()
            {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJson(400, "{\"error\":\"bad json\"}");
      return;
    }

    String name = doc["name"] | "";
    String url = doc["url"] | "";
    name.trim();
    url.trim();

    if (name.isEmpty() || url.isEmpty()) {
      sendJson(400, "{\"error\":\"name/url required\"}");
      return;
    }

    if (!isHttpStreamUrl(url)) {
      sendJson(400, "{\"error\":\"URL must start with http:// or https://\"}");
      return;
    }

    if (stationUrlExists(url)) {
      sendJson(409, "{\"error\":\"station already exists\"}");
      return;
    }

    stations.push_back({name, url, false});
    saveStations();
    sendJson(200, R"({"ok":true})"); });

  server.on("/api/stations/favorite", HTTP_POST, [sendJson]()
            {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJson(400, "{\"error\":\"bad json\"}");
      return;
    }

    int idx = doc["index"] | -1;
    bool favorite = doc["favorite"] | false;

    if (idx < 0 || idx >= (int)stations.size()) {
      sendJson(400, "{\"error\":\"bad index\"}");
      return;
    }

    stations[idx].favorite = favorite;
    saveStations();
    sendJson(200, R"({"ok":true})"); });

  server.on("/api/stations/move", HTTP_POST, [sendJson]()
            {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJson(400, "{\"error\":\"bad json\"}");
      return;
    }

    int idx = doc["index"] | -1;
    String dir = doc["direction"] | "";

    if (idx < 0 || idx >= (int)stations.size()) {
      sendJson(400, "{\"error\":\"bad index\"}");
      return;
    }

    int targetIdx = idx;
    if (dir == "up") {
      targetIdx = idx - 1;
    } else if (dir == "down") {
      targetIdx = idx + 1;
    } else {
      sendJson(400, "{\"error\":\"bad direction\"}");
      return;
    }

    if (targetIdx >= 0 && targetIdx < (int)stations.size()) {
      bool currentWasSelected = (currentStation == idx);
      bool targetWasSelected = (currentStation == targetIdx);

      std::swap(stations[idx], stations[targetIdx]);

      if (currentWasSelected) {
        currentStation = targetIdx;
      } else if (targetWasSelected) {
        currentStation = idx;
      }

      saveStations();
      saveRuntimePrefs();
    }
    sendJson(200, R"({"ok":true})"); });

  server.on("/api/stations", HTTP_DELETE, [sendJson]()
            {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJson(400, "{\"error\":\"bad json\"}");
      return;
    }

    int idx = doc["index"] | -1;
    if (idx < 0 || idx >= (int)stations.size()) {
      sendJson(400, "{\"error\":\"bad index\"}");
      return;
    }

    bool deletedCurrent = (idx == currentStation);

    stations.erase(stations.begin() + idx);

    if (stations.empty()) {
      currentStation = 0;
      stopCurrentStation();
    } else {
      if (currentStation >= (int)stations.size()) {
        currentStation = stations.size() - 1;
      }
      if (deletedCurrent) {
        playCurrentStation();
      }
    }

    saveStations();
    saveRuntimePrefs();
    sendJson(200, "{\"ok\":true}"); });

  server.on("/api/import-m3u", HTTP_POST, [sendJson]()
            {
    String body = server.arg("plain");
    body.trim();

    if (body.isEmpty()) {
      sendJson(400, "{\"error\":\"empty body\"}");
      return;
    }

    int duplicates = 0;
    int invalid = 0;
    int added = importStationsFromM3U(body, duplicates, invalid);

    JsonDocument doc;
    doc["ok"] = true;
    doc["added"] = added;
    doc["duplicates"] = duplicates;
    doc["invalid"] = invalid;
    doc["total"] = stations.size();

    String out;
    serializeJson(doc, out);
    sendJson(200, out); });

  server.on("/api/wifi/scan", HTTP_GET, [sendJson]()
            {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    int8_t n = WiFi.scanNetworks();
    if (n == WIFI_SCAN_RUNNING) {
      // scan started in background
      sendJson(202, "{\"ok\":false,\"error\":\"scan running\"}");
      return;
    }

    if (n <= 0) {
      // no networks found
      String out;
      serializeJson(doc, out);
      sendJson(200, out);
      return;
    }

    for (int i = 0; i < n; ++i) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = WiFi.SSID(i);
      o["rssi"] = WiFi.RSSI(i);
      o["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    }

    String out;
    serializeJson(doc, out);
    WiFi.scanDelete();
    sendJson(200, out); });

  server.on("/wifi", HTTP_POST, []()
            {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    String ssid;
    String pass;

    // Try parse JSON body first (client posts JSON)
    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
      JsonDocument doc;
      if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok) {
        ssid = String((const char*) (doc["ssid"] | ""));
        pass = String((const char*) (doc["pass"] | ""));
      }
    }

    // Fallback to form-encoded args
    if (ssid.isEmpty()) {
      ssid = server.arg("ssid");
    }
    if (pass.isEmpty()) {
      pass = server.arg("pass");
    }

    if (ssid.isEmpty()) {
      server.send(400, "text/plain", "SSID required");
      return;
    }

    saveWifiConfig(ssid, pass);
    server.send(200, "text/plain", "Saved. Reboot device."); });

  // API-compatible JSON endpoint: /api/wifi
  server.on("/api/wifi", HTTP_POST, [sendJson]()
            {
    JsonDocument resp;

    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
      JsonDocument doc;
      if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
        sendJson(400, "{\"error\":\"bad json\"}");
        return;
      }

      String ssid = String((const char*)(doc["ssid"] | ""));
      String pass = String((const char*)(doc["pass"] | ""));

      if (ssid.isEmpty()) {
        sendJson(400, "{\"error\":\"ssid required\"}");
        return;
      }

      saveWifiConfig(ssid, pass);

      resp["ok"] = true;
      resp["reboot"] = true;
      String out;
      serializeJson(resp, out);
      sendJson(200, out);

      delay(500);
      ESP.restart();
      return;
    }

    sendJson(400, "{\"error\":\"empty body\"}"); });

  server.on("/update", HTTP_POST, []()
            {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", Update.hasError() ? "OTA FAIL" : "OTA OK, rebooting");
      delay(500);
      ESP.restart(); }, []()
            {
      HTTPUpload &upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Update.begin(UPDATE_SIZE_UNKNOWN);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        Update.write(upload.buf, upload.currentSize);
      } else if (upload.status == UPLOAD_FILE_END) {
        Update.end(true);
      } });

  server.onNotFound([]()
                    {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (!handleFileRead(server.uri())) {
      server.send(404, "text/plain", "Not found");
    } });
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  printSystemDiagnostics();

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  u8g2.setI2CAddress(OLED_ADDR << 1);
  u8g2.begin();

  screenMode = SCREEN_BOOT;
  updateDisplay();

  if (!LittleFS.begin(true))
  {
    Serial.println("ERROR: LittleFS mount failed, attempting format...");
    LittleFS.format();
    if (!LittleFS.begin())
    {
      Serial.println("FATAL: LittleFS unrecoverable, entering AP-only mode");
      // Continue anyway — at least AP mode will work
    }
    else
    {
      Serial.println("LittleFS formatted and mounted successfully");
    }
  }

  loadStations();
  loadRuntimePrefs();

  if (currentStation < 0)
    currentStation = 0;
  if (currentStation >= (int)stations.size() && !stations.empty())
    currentStation = 0;

  audioInit();

  Audio::audio_info_callback = [](Audio::msg_t m)
  {
    switch (m.e)
    {
    case Audio::evt_info:
      audio_info(m.msg);
      break;
    case Audio::evt_name:
      audio_showstation(m.msg);
      break;
    case Audio::evt_streamtitle:
      audio_showstreamtitle(m.msg);
      break;
    case Audio::evt_bitrate:
      audio_bitrate(m.msg);
      break;
    case Audio::evt_eof:
      audio_eof_mp3(m.msg);
      break;
    case Audio::evt_id3data:
      audio_id3data(m.msg);
      break;
    case Audio::evt_log:
      if (m.msg)
        Serial.printf("[AUDIO LOG] %s\n", m.msg);
      break;
    default:
      audio_info(m.msg);
      break;
    }
  };

  audioSetVolume(volumeValue);
  screenMode = SCREEN_BOOT;

  if (!connectWifi())
  {
    startApMode();
  }

  setupApi();
  server.begin();

  screenMode = SCREEN_MAIN;
  updateDisplay();

  if (wifiConnected && !stations.empty())
  {
    playCurrentStation();
  }
}

void wifiReconnectTick()
{
  if (!wifiReconnecting)
    return;

  unsigned long now = millis();
  if ((long)(now - wifiReconnectNextAt) < 0)
    return;

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    apMode = false;
    connectedWifiSsid = wifiSsid;
    clearWifiReconnectState();
    dns.stop();
    startMdns();

    playStatus = "WIFI OK";
    screenMode = SCREEN_MAIN;
    Serial.printf("WiFi reconnected, IP: %s, RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());

    if (!stations.empty())
      playCurrentStation();
    return;
  }

  // A temporary Internet outage must not force the radio into setup/AP mode.
  // Keep retrying the saved Wi-Fi credentials indefinitely. The initial boot
  // connection still falls back to AP mode in setup().
  if (wifiReconnectAttempt >= WIFI_CONNECT_RETRIES)
    wifiReconnectAttempt = 0;

  ++wifiReconnectAttempt;
  Serial.printf("WiFi reconnect %u/%u\n",
                wifiReconnectAttempt, WIFI_CONNECT_RETRIES);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.disconnect(false, false);
  delay(50);
  if (wifiSsid.isEmpty())
  {
    wifiSsid = DEFAULT_WIFI_SSID;
    wifiPass = DEFAULT_WIFI_PASS;
  }

  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  // Do not hammer the access point while it is unavailable.
  wifiReconnectNextAt = now + WIFI_RECONNECT_RETRY_MS;
}

void loop()
{
  server.handleClient();

  if (apMode)
    dns.processNextRequest();

  audioLoop();
  audioWatchdogTick();
  audioReconnectTick();

  if (wifiReconnecting)
    wifiReconnectTick();

  stationReconnectTick();

  unsigned long now = millis();

  if (screenMode == SCREEN_VOLUME && (long)(now - volumeOverlayUntil) >= 0)
    screenMode = SCREEN_MAIN;

  if (screenMode == SCREEN_STATION && (long)(now - stationOverlayUntil) >= 0)
    screenMode = SCREEN_MAIN;

  if (now - lastDisplayRefresh >= DISPLAY_REFRESH_MS)
  {
    lastDisplayRefresh = now;
    updateDisplay();
  }

  // Do not interfere with AP mode or an already running reconnect attempt.
  if (!apMode && !wifiReconnecting && wifiConnected && WiFi.status() != WL_CONNECTED)
  {
    wifiConnected = false;
    connectedWifiSsid = "";
    playStatus = "WIFI LOST";
    currentTrack = "";
    screenMode = SCREEN_MAIN;
    stopAudio();

    wifiReconnecting = true;
    wifiReconnectAttempt = 0;
    wifiReconnectNextAt = millis() + 300;
    stopMdns();
  }
}