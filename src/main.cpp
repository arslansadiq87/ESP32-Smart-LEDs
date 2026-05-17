// ====== ESP32 FastLED + Multi-Zone Web UI (big pattern pack) ======
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <uri/UriBraces.h>
#include <FastLED.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <Update.h>


// ================== USER SETTINGS ==================
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB
#define LED_SUPPLY_V    5               // for FastLED power limiter
#define MAX_ZONES 5               // we have 4
#define MAX_PER_ZONE    600             // memory cap per zone

const char* WIFI_SSID = "YourSSID";
const char* WIFI_PASS = "YourWIFIPassword";
const char* AP_SSID   = "ESP32-LED";
const char* AP_PASS   = "12345678";     // 8+ chars if you want it secured
const char* MDNS_HOST = "rgb";
// ===================================================

// ---- ZONE DEFINITIONS (edit pins/counts/names) ----
struct ZoneHW {
  const char* name;
  uint8_t     pin;
  uint16_t    defaultCount;
};
ZoneHW ZDEF[MAX_ZONES] = {
  {"Wall",        5,  120},
  {"Monitors",   19,  90},
  {"PC Case",    18,  60},
  {"Ceiling",    23,  150},
  {"Moon Globe", 21,  30},
};

// ---- RUNTIME STATE PER ZONE ----
struct Zone {
  // Fixed
  String   name;
  uint8_t  pin;
  uint16_t maxLeds;
  CRGB*    leds;
  CLEDController* ctl;
  // Configurable (persisted)
  bool     on;
  uint16_t nLeds;
  uint8_t  brightness;          // 0-255
  uint16_t frameMs;             // 1..200
  CRGB     color;
  uint8_t  patternIndex;
  // Dynamic
  uint32_t lastFrameMs;
  uint8_t  lastNonzeroBri;
} Z[MAX_ZONES];

WebServer    server(80);
Preferences  prefs;

// ---------------- Utilities ----------------
String ipToStr(IPAddress ip){ char buf[32]; sprintf(buf,"%u.%u.%u.%u",ip[0],ip[1],ip[2],ip[3]); return String(buf); }
bool parseHexColor(String s, CRGB& out) {
  s.trim(); if (s.startsWith("#")) s.remove(0,1);
  if (s.length()!=6) return false;
  long v = strtol(s.c_str(), nullptr, 16);
  out.r = (v>>16)&0xFF; out.g = (v>>8)&0xFF; out.b = v&0xFF; return true;
}
String colorHex(CRGB c){ char col[7]; sprintf(col,"%02X%02X%02X", c.r, c.g, c.b); return String(col); }
uint16_t clamp16(int v, int lo, int hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }
uint8_t internalBrightnessToHa(uint8_t brightness) { return (uint16_t(brightness) * 255 + 47) / 95; }
uint8_t internalBrightnessToPct(uint8_t brightness) { return (uint16_t(brightness) * 100 + 47) / 95; }
uint8_t haBrightnessToInternal(int brightness) {
  brightness = constrain(brightness, 0, 255);
  return (uint16_t(brightness) * 95 + 127) / 255;
}
uint8_t pctBrightnessToInternal(int brightnessPct) {
  brightnessPct = constrain(brightnessPct, 0, 100);
  return (uint16_t(brightnessPct) * 95 + 50) / 100;
}
uint8_t internalSaturationToPct(uint8_t saturation) { return (uint16_t(saturation) * 100 + 127) / 255; }
uint8_t pctSaturationToInternal(float saturationPct) {
  if (saturationPct < 0) saturationPct = 0;
  if (saturationPct > 100) saturationPct = 100;
  return (uint16_t)(saturationPct * 255.0f / 100.0f + 0.5f);
}
uint8_t hueDegreesToInternal(float hueDegrees) {
  while (hueDegrees < 0) hueDegrees += 360.0f;
  while (hueDegrees >= 360.0f) hueDegrees -= 360.0f;
  return (uint16_t)(hueDegrees * 255.0f / 360.0f + 0.5f);
}
float internalHueToDegrees(uint8_t hue) { return hue * 360.0f / 255.0f; }

void startMdnsIfStaConnected() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!MDNS.begin(MDNS_HOST)) {
    Serial.println("mDNS start failed");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  Serial.printf("mDNS: http://%s.local/\n", MDNS_HOST);
}

// ---------------- Persistence (NVS) ----------------
void loadPrefs(){
  prefs.begin("ledctrl", true); // read
  for(uint8_t i=0;i<MAX_ZONES;i++){
    Z[i].on           = prefs.getBool(   (String("z")+i+"_on").c_str(),           true);
    Z[i].nLeds        = prefs.getUShort( (String("z")+i+"_cnt").c_str(),          ZDEF[i].defaultCount);
    Z[i].brightness = prefs.getUChar((String("z")+i+"_bri").c_str(), 128);
    if (Z[i].brightness > 95) Z[i].brightness = 95;   // hard cap now 95
    Z[i].frameMs      = prefs.getUShort( (String("z")+i+"_spd").c_str(),          20);
    Z[i].patternIndex = prefs.getUChar(  (String("z")+i+"_pat").c_str(),          0);
    uint32_t col      = prefs.getUInt(   (String("z")+i+"_col").c_str(),          0xFF0000);
    Z[i].color        = CRGB((col>>16)&0xFF, (col>>8)&0xFF, col & 0xFF);
    Z[i].lastNonzeroBri = Z[i].brightness ? Z[i].brightness : 128;
  }
  prefs.end();
}
bool savePrefs(){
  prefs.begin("ledctrl", false); // write
  for(uint8_t i=0;i<MAX_ZONES;i++){
    prefs.putBool(  (String("z")+i+"_on").c_str(),   Z[i].on);
    prefs.putUShort((String("z")+i+"_cnt").c_str(),  Z[i].nLeds);
    prefs.putUChar( (String("z")+i+"_bri").c_str(),  Z[i].brightness);
    prefs.putUShort((String("z")+i+"_spd").c_str(),  Z[i].frameMs);
    prefs.putUChar( (String("z")+i+"_pat").c_str(),  Z[i].patternIndex);
    uint32_t col = ((uint32_t)Z[i].color.r<<16) | ((uint32_t)Z[i].color.g<<8) | (uint32_t)Z[i].color.b;
    prefs.putUInt( (String("z")+i+"_col").c_str(),   col);
  }
  prefs.end();
  return true;
}


// =============== HTTP OTA update via POST /update ===============


// ---------------- Pattern glue ----------------
typedef void (*PatternFn)(uint8_t z);

// Forward declarations for all patterns (definitions later)
void p_solid(uint8_t);           void p_rainbow(uint8_t);
void p_rainbow_glitter(uint8_t); void p_confetti(uint8_t);
void p_sinelon(uint8_t);         void p_juggle(uint8_t);
void p_bpm(uint8_t);             void p_theater(uint8_t);
void p_twinkle(uint8_t);         void p_breathe(uint8_t);
void p_comet(uint8_t);
void p_ripple(uint8_t);          void p_meteor(uint8_t);
void p_larson(uint8_t);          void p_gradient_scroll(uint8_t);
void p_palette_cycle(uint8_t);   void p_rainbow_beat(uint8_t);
void p_police(uint8_t);
void p_fire(uint8_t);
void p_noise_colorwaves(uint8_t);void p_plasma(uint8_t);
void p_chase(uint8_t);           void p_chase_dual(uint8_t);
void p_glitter(uint8_t);         void p_candle(uint8_t);
void p_wave(uint8_t);            void p_beat_bars(uint8_t);
void p_stars(uint8_t);           void p_scanlines(uint8_t);

void p_fireflies(uint8_t z);
void p_fireworks(uint8_t z);       void p_dual_comet(uint8_t z);

void p_wave_mono(uint8_t z);       void p_wave_rainbow(uint8_t z); void p_wave_dual(uint8_t z);

// Modern/futuristic patterns
void p_neon_pulse(uint8_t z); void p_hologram(uint8_t z);

// ====== Shortcut: add/remove patterns in ONE place ======
#define PATTERN_TABLE(X) \
  X("SOLID",            p_solid) \
  X("RAINBOW",          p_rainbow) \
  X("RAINBOW_GLITTER",  p_rainbow_glitter) \
  X("CONFETTI",         p_confetti) \
  X("SINELON",          p_sinelon) \
  X("JUGGLE",           p_juggle) \
  X("BPM",              p_bpm) \
  X("THEATER",          p_theater) \
  X("TWINKLE",          p_twinkle) \
  X("BREATHE",          p_breathe) \
  X("COMET",            p_comet) \
  X("RIPPLE",           p_ripple) \
  X("METEOR",           p_meteor) \
  X("LARSON",           p_larson) \
  X("GRAD_SCROLL",      p_gradient_scroll) \
  X("PALETTE_CYCLE",    p_palette_cycle) \
  X("RAINBOW_BEAT",     p_rainbow_beat) \
  X("POLICE",           p_police) \
  X("FIRE",             p_fire) \
  X("NOISE_COLORWAVES", p_noise_colorwaves) \
  X("PLASMA",           p_plasma) \
  X("CHASE",            p_chase) \
  X("CHASE_DUAL",       p_chase_dual) \
  X("GLITTER",          p_glitter) \
  X("CANDLE",           p_candle) \
  X("WAVE",             p_wave) \
  X("BEAT_BARS",        p_beat_bars) \
  X("STARS",            p_stars) \
  X("NEON_PULSE",       p_neon_pulse) \
  X("HOLOGRAM",         p_hologram) \
  X("SCANLINES",        p_scanlines) \
  X("FIREFLIES",        p_fireflies) \
  X("FIREWORKS",        p_fireworks) \
  X("DUAL_COMET",       p_dual_comet) \
  X("WAVE_MONO",        p_wave_mono)  \
  X("WAVE_RAINBOW",     p_wave_rainbow)  \
  X("WAVE_DUAL",        p_wave_dual)

// Build pattern list automatically
struct PatternDef { const char* name; PatternFn fn; };
#define MAKE_PATTERN(name, fn) { name, fn },
PatternDef PATS[] = { PATTERN_TABLE(MAKE_PATTERN) };
const uint8_t PATTERN_COUNT = sizeof(PATS)/sizeof(PATS[0]);

// ---------------- Web UI (LittleFS) ----------------
// UI files are served from LittleFS: /index.html, /assets/app.css, /assets/app.js




// ---------------- Auth ----------------
const char* HTTP_USER = "";
const char* HTTP_PASS = "";

bool ensureAuth() {
  // If user/pass are empty => NO AUTH
  if (!HTTP_USER || !*HTTP_USER || !HTTP_PASS || !*HTTP_PASS) {
    return true;
  }

  if (!server.authenticate(HTTP_USER, HTTP_PASS)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// ---------------- HTTP Handlers ----------------
void sendCORS(){
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.sendHeader("Access-Control-Allow-Methods","GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers","*");
}
String contentTypeFor(const String& path){
  if(path.endsWith(".html")) return "text/html";
  if(path.endsWith(".css"))  return "text/css";
  if(path.endsWith(".js"))   return "application/javascript";
  if(path.endsWith(".svg"))  return "image/svg+xml";
  if(path.endsWith(".png"))  return "image/png";
  if(path.endsWith(".ico"))  return "image/x-icon";
  return "text/plain";
}

bool serveFile(const String& path){
  String p = path;
  if(p == "/") p = "/index.html";
  if(!LittleFS.exists(p)) return false;
  File f = LittleFS.open(p, "r");
  server.streamFile(f, contentTypeFor(p));
  f.close();
  return true;
}

void handleRoot(){
  sendCORS();
  if(!serveFile("/index.html")) server.send(404, "text/plain", "Missing /index.html in LittleFS");
}

void handleStatus() {
  if (!ensureAuth()) return;

  JsonDocument doc;

  doc["ip"] = ipToStr(WiFi.isConnected() ? WiFi.localIP() : WiFi.softAPIP());

  JsonArray patterns = doc["patterns"].to<JsonArray>();
  for (uint8_t i = 0; i < PATTERN_COUNT; i++) {
    patterns.add(PATS[i].name);
  }

  JsonObject all = doc["all"].to<JsonObject>();
  CHSV allHsv = rgb2hsv_approximate(Z[0].color);
  bool mixed = false;
  for (uint8_t i = 1; i < MAX_ZONES; i++) {
    if (Z[i].on != Z[0].on ||
        Z[i].patternIndex != Z[0].patternIndex ||
        Z[i].brightness != Z[0].brightness ||
        Z[i].color.r != Z[0].color.r ||
        Z[i].color.g != Z[0].color.g ||
        Z[i].color.b != Z[0].color.b) {
      mixed = true;
      break;
    }
  }
  all["name"] = "All Zones";
  all["on"] = Z[0].on;
  all["pattern"] = Z[0].patternIndex;
  all["pattern_name"] = PATS[Z[0].patternIndex].name;
  all["color"] = colorHex(Z[0].color);
  all["red"] = Z[0].color.r;
  all["green"] = Z[0].color.g;
  all["blue"] = Z[0].color.b;
  all["brightness"] = Z[0].brightness;
  all["brightness_pct"] = internalBrightnessToPct(Z[0].brightness);
  all["brightness_ha"] = internalBrightnessToHa(Z[0].brightness);
  all["hue"] = internalHueToDegrees(allHsv.h);
  all["saturation"] = internalSaturationToPct(allHsv.s);
  all["mixed"] = mixed;

  JsonArray zones = doc["zones"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_ZONES; i++) {
    JsonObject z = zones.add<JsonObject>();
    z["id"]         = i;
    z["name"]       = Z[i].name;
    z["on"]         = Z[i].on;
    z["pattern"]    = Z[i].patternIndex;
    z["pattern_name"] = PATS[Z[i].patternIndex].name;
    z["color"]      = colorHex(Z[i].color);
    z["brightness"] = Z[i].brightness;
    z["brightness_pct"] = internalBrightnessToPct(Z[i].brightness);
    z["brightness_ha"]  = internalBrightnessToHa(Z[i].brightness);
    z["frame_ms"]   = Z[i].frameMs;
    z["leds"]       = Z[i].nLeds;
    z["max_leds"]   = Z[i].maxLeds;
  }

  String out;
  serializeJson(doc, out);

  sendCORS();
  server.send(200, "application/json", out);
}



void setPatternByArg(uint8_t zi, String v){
  int idx=-1;
  if (v.length() && isDigit(v[0])) idx = v.toInt();
  else {
    for (uint8_t j=0;j<PATTERN_COUNT;j++)
      if (v.equalsIgnoreCase(PATS[j].name)) { idx=j; break; }
  }
  if (idx>=0 && idx<PATTERN_COUNT) Z[zi].patternIndex = idx;
}

void applyOnOffBrightness(uint8_t zi){
  if(Z[zi].on){
    if(Z[zi].brightness==0) Z[zi].brightness = Z[zi].lastNonzeroBri ? Z[zi].lastNonzeroBri : 128;
  }else{
    if(Z[zi].brightness>0) Z[zi].lastNonzeroBri = Z[zi].brightness;
  }
}

void handleCmd() {
  sendCORS();
  // if(!ensureAuth()) return;

  // Accept JSON body (POST) OR query args (GET)
  if (server.method() == HTTP_POST && server.hasArg("plain") && server.arg("plain").length()) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (!err) {
      bool all = doc["all"] | false;
      int zi   = doc["zone"] | -1;

      // Command can come in a few shapes depending on UI version.
      // Preferred: {"cmd":"ON"|"OFF"}
      // Compat (older/other UIs): {"on":true|false} or {"power":1|0} or {"state":"on"|"off"}
      const char* cmd = doc["cmd"].is<const char*>() ? doc["cmd"].as<const char*>() : nullptr;
      String cmdFromCompat;
      if (!cmd) {
        if (doc["on"].is<bool>()) {
          cmdFromCompat = doc["on"].as<bool>() ? "ON" : "OFF";
          cmd = cmdFromCompat.c_str();
        } else if (doc["power"].is<int>()) {
          cmdFromCompat = (doc["power"].as<int>() != 0) ? "ON" : "OFF";
          cmd = cmdFromCompat.c_str();
        } else if (doc["state"].is<const char*>()) {
          String s = String(doc["state"].as<const char*>());
          s.trim(); s.toUpperCase();
          if (s == "ON" || s == "OFF") cmdFromCompat = s;
          else if (s == "1" || s == "TRUE") cmdFromCompat = "ON";
          else if (s == "0" || s == "FALSE") cmdFromCompat = "OFF";
          if (cmdFromCompat.length()) cmd = cmdFromCompat.c_str();
        }
      }

      // --- debug: log incoming JSON commands ---
      {
        const char* patS = doc["pat"].is<const char*>() ? doc["pat"].as<const char*>() : nullptr;
        const char* colS = doc["col"] | nullptr;
        int briV = doc["bri"] | -1;
        int spdV = doc["spd"] | -1;
        int cntV = doc["count"] | -1;
        Serial.printf("[API] /api/cmd(JSON) all=%d zone=%d cmd=%s pat=%s col=%s bri=%d spd=%d count=%d\n",
                      all, zi, cmd ? cmd : "-", patS ? patS : "-", colS ? colS : "-", briV, spdV, cntV);
      }

      // pat can be a STRING ("RAINBOW") OR a NUMBER (3)
      int patIdx = -1;
      const char* patStr = nullptr;
      // Compat: accept "pat" or "pattern"
      JsonVariant patV = doc["pat"].isNull() ? doc["pattern"] : doc["pat"];
      if (patV.is<const char*>()) patStr = patV.as<const char*>();
      else if (patV.is<int>())    patIdx = patV.as<int>();

      // Compat: accept "col" or "color"
      const char* col = doc["col"].is<const char*>() ? doc["col"].as<const char*>() : (doc["color"] | nullptr);
      JsonVariant rgbV = doc["rgb"].isNull() ? doc["rgb_color"] : doc["rgb"];
      bool hasRgb = false;
      CRGB rgbColor = CRGB::Black;
      if (rgbV.is<JsonArray>()) {
        JsonArray rgbArr = rgbV.as<JsonArray>();
        if (rgbArr.size() == 3) {
          hasRgb = true;
          rgbColor.r = constrain(rgbArr[0].as<int>(), 0, 255);
          rgbColor.g = constrain(rgbArr[1].as<int>(), 0, 255);
          rgbColor.b = constrain(rgbArr[2].as<int>(), 0, 255);
        }
      } else if (doc["r"].is<int>() && doc["g"].is<int>() && doc["b"].is<int>()) {
        hasRgb = true;
        rgbColor.r = constrain(doc["r"].as<int>(), 0, 255);
        rgbColor.g = constrain(doc["g"].as<int>(), 0, 255);
        rgbColor.b = constrain(doc["b"].as<int>(), 0, 255);
      }

      // Compat: accept "bri" or "brightness", and "spd" or "speed"
      int bri = doc["bri"].is<int>() ? (doc["bri"] | -1) : (doc["brightness"] | -1);
      int briHa = doc["brightness_ha"].is<int>() ? (doc["brightness_ha"] | -1) : -1;
      int briPct = doc["brightness_pct"].is<int>() ? (doc["brightness_pct"] | -1) : -1;
      JsonVariant hueV = doc["hue"];
      bool hasHue = !hueV.isNull() && (hueV.is<float>() || hueV.is<int>());
      float hue = hasHue ? hueV.as<float>() : 0.0f;
      JsonVariant satV = doc["saturation"];
      bool hasSat = !satV.isNull() && (satV.is<float>() || satV.is<int>());
      float sat = hasSat ? satV.as<float>() : 0.0f;
      int spd = doc["spd"].is<int>() ? (doc["spd"] | -1) : (doc["speed"] | -1);
      int cnt = doc["count"] | -1;

      auto applyTo = [&](uint8_t z) {
        // ON/OFF
        if (cmd) {
          String v = String(cmd);
          v.trim(); v.toUpperCase();
          if (v == "ON") {
            Z[z].on = true;
          } else if (v == "OFF") {
            Z[z].on = false;
            fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black);
            if (Z[z].ctl) Z[z].ctl->showLeds(0);
          }
        }

        // Pattern (string or numeric)
        if (patStr) {
          setPatternByArg(z, String(patStr));
        } else if (patIdx >= 0) {
          Z[z].patternIndex = clamp16(patIdx, 0, (int)PATTERN_COUNT - 1);
        }

        // Color
        if (hasRgb) {
          Z[z].color = rgbColor;
        } else if (col) {
          CRGB c;
          if (parseHexColor(String(col), c)) Z[z].color = c;
        }
        if (hasHue) {
          uint8_t s = hasSat ? pctSaturationToInternal(sat) : 255;
          Z[z].color = CHSV(hueDegreesToInternal(hue), s, 255);
        }

        // Brightness
        if (briHa >= 0) {
          Z[z].brightness = haBrightnessToInternal(briHa);
          if (Z[z].brightness > 0) Z[z].lastNonzeroBri = Z[z].brightness;
        } else if (briPct >= 0) {
          Z[z].brightness = pctBrightnessToInternal(briPct);
          if (Z[z].brightness > 0) Z[z].lastNonzeroBri = Z[z].brightness;
        } else if (bri >= 0) {
          Z[z].brightness = clamp16(bri, 0, 95);
          if (Z[z].brightness > 0) Z[z].lastNonzeroBri = Z[z].brightness;
        }

        // Speed
        if (spd >= 0) {
          Z[z].frameMs = clamp16(spd, 1, 200);
        }

        // LED count
        if (cnt >= 0) {
          Z[z].nLeds = clamp16(cnt, 1, (int)Z[z].maxLeds);
        }

        applyOnOffBrightness(z);
      };

      // Apply to all zones
      if (all) {
        for (uint8_t z = 0; z < MAX_ZONES; z++) applyTo(z);
        handleStatus();
        return;
      }

      // Apply to one zone
      if (zi >= 0) {
        applyTo((uint8_t)clamp16(zi, 0, MAX_ZONES - 1));
        handleStatus();
        return;
      }

      server.send(400, "application/json", "{\"error\":\"missing_zone\"}");
      return;
    }

    server.send(400, "application/json", "{\"error\":\"bad_json\"}");
    return;
  }

  // ---------------- OLD QUERY MODE (optional legacy support) ----------------
  // Master ON/OFF
  if (server.hasArg("ALL")) {
    String v = server.arg("ALL"); v.trim(); v.toUpperCase();
    bool on = (v == "ON");
    for (uint8_t i = 0; i < MAX_ZONES; i++) {
      Z[i].on = on;
      if (!on) {
        fill_solid(Z[i].leds, Z[i].nLeds, CRGB::Black);
        if (Z[i].ctl) Z[i].ctl->showLeds(0);
      }
      applyOnOffBrightness(i);
    }
  }

  // Per-zone with Z
  if (server.hasArg("Z")) {
    uint8_t zi = clamp16(server.arg("Z").toInt(), 0, MAX_ZONES - 1);

    if (server.hasArg("CMD")) {
      String v = server.arg("CMD"); v.trim(); v.toUpperCase();
      if (v == "ON")  Z[zi].on = true;
      if (v == "OFF") {
        Z[zi].on = false;
        fill_solid(Z[zi].leds, Z[zi].nLeds, CRGB::Black);
        if (Z[zi].ctl) Z[zi].ctl->showLeds(0);
      }
    }

    for (uint8_t i = 0; i < server.args(); i++) {
      String k = server.argName(i); k.toUpperCase();
      String v = server.arg(i); v.trim();

      if (k == "PAT") setPatternByArg(zi, v);
      else if (k == "COL") {
        CRGB c; if (parseHexColor(v, c)) Z[zi].color = c;
      }
      else if (k == "HUE") {
        float hue = v.toFloat();
        CHSV hsv = rgb2hsv_approximate(Z[zi].color);
        Z[zi].color = CHSV(hueDegreesToInternal(hue), hsv.s, 255);
      }
      else if (k == "SATURATION") {
        float sat = v.toFloat();
        CHSV hsv = rgb2hsv_approximate(Z[zi].color);
        Z[zi].color = CHSV(hsv.h, pctSaturationToInternal(sat), 255);
      }
      else if (k == "BRI") {
        int b = v.toInt();
        Z[zi].brightness = clamp16(b, 0, 95);
        if (Z[zi].brightness > 0) Z[zi].lastNonzeroBri = Z[zi].brightness;
      }
      else if (k == "BRI_HA") {
        Z[zi].brightness = haBrightnessToInternal(v.toInt());
        if (Z[zi].brightness > 0) Z[zi].lastNonzeroBri = Z[zi].brightness;
      }
      else if (k == "BRI_PCT") {
        Z[zi].brightness = pctBrightnessToInternal(v.toInt());
        if (Z[zi].brightness > 0) Z[zi].lastNonzeroBri = Z[zi].brightness;
      }
      else if (k == "SPD") {
        int ms = v.toInt();
        Z[zi].frameMs = clamp16(ms, 1, 200);
      }
      else if (k == "COUNT") {
        int n = v.toInt();
        Z[zi].nLeds = clamp16(n, 1, (int)Z[zi].maxLeds);
      }
    }

    applyOnOffBrightness(zi);
  }

  handleStatus();
}


void handleSave(){
  sendCORS();
  if(!ensureAuth()) return;

  sendCORS();
  if (server.method() == HTTP_OPTIONS){ server.send(204); return; }
  savePrefs();
  server.send(200,"application/json","{\"ok\":true}");
}

// ---------------- Setup / Loop ----------------
void setupZone(uint8_t i){
  Z[i].name = ZDEF[i].name;
  Z[i].pin  = ZDEF[i].pin;

  // allocate LEDs
  Z[i].maxLeds = MAX_PER_ZONE;
  Z[i].leds = (CRGB*)malloc(sizeof(CRGB) * Z[i].maxLeds);
  if(!Z[i].leds){ Serial.printf("Zone %u malloc failed\n", i); while(true) delay(1); }

  // Attach controller directly to known pins per zone index
  // IMPORTANT: FastLED's pin is a template parameter => must be compile-time constant.
  // So we map each zone index to a fixed GPIO here.
  Z[i].ctl = nullptr;
  switch (i) {
    case 0: Z[i].ctl = &FastLED.addLeds<LED_TYPE, 5,  COLOR_ORDER>(Z[i].leds, Z[i].maxLeds); break;  // Wall  -> GPIO5
    case 1: Z[i].ctl = &FastLED.addLeds<LED_TYPE, 19, COLOR_ORDER>(Z[i].leds, Z[i].maxLeds); break;  // Monitors -> GPIO19
    case 2: Z[i].ctl = &FastLED.addLeds<LED_TYPE, 18, COLOR_ORDER>(Z[i].leds, Z[i].maxLeds); break;  // PC Case  -> GPIO18
    case 3: Z[i].ctl = &FastLED.addLeds<LED_TYPE, 23, COLOR_ORDER>(Z[i].leds, Z[i].maxLeds); break;  // Ceiling -> GPIO23
    case 4: Z[i].ctl = &FastLED.addLeds<LED_TYPE, 21, COLOR_ORDER>(Z[i].leds, Z[i].maxLeds); break;  // Moon Globe -> GPIO21
  }

  // Safety: if a zone index isn't mapped above, avoid crashing the whole firmware.
  if (!Z[i].ctl) {
    Serial.printf("Zone %u has no FastLED controller mapping (pin=%u). Fix setupZone() switch.\n", i, Z[i].pin);
    while(true) delay(1);
  }

  Z[i].ctl->setCorrection(TypicalLEDStrip);
  Z[i].lastFrameMs = 0;
}


// ================ HomeKit/Apple Home Support via REST API ================
// These endpoints allow Apple Home Shortcuts to control your lights
// Since HomeSpan requires newer Arduino ESP32, we provide REST API for Home Shortcuts integration

struct LedZoneLight {
  uint8_t zone_idx;
  
  LedZoneLight(uint8_t z) : zone_idx(z) {}
  
  void updateFromHomeKit(JsonObject cmd) {
    if (cmd["on"].is<bool>()) {
      Z[zone_idx].on = cmd["on"];
    }
    if (cmd["brightness"].is<int>()) {
      Z[zone_idx].brightness = haBrightnessToInternal(cmd["brightness"]);
    }
    if (cmd["brightness_pct"].is<int>()) {
      Z[zone_idx].brightness = pctBrightnessToInternal(cmd["brightness_pct"]);
    }
    if (cmd["hue"].is<float>()) {
      // Convert 0-360 to 0-255
      uint8_t h = (uint16_t)(cmd["hue"].as<float>() / 360.0 * 255);
      uint8_t s = cmd["saturation"].is<int>() ? cmd["saturation"] : 255;
      Z[zone_idx].color = CHSV(h, s, 255);
    }
    if (cmd["pattern"].is<int>()) {
      uint8_t pattern = cmd["pattern"];
      if (pattern < PATTERN_COUNT) {
        Z[zone_idx].patternIndex = pattern;
      }
    }
  }
  
  JsonObject toJson(JsonObject obj) {
    CHSV hsv = rgb2hsv_approximate(Z[zone_idx].color);
    obj["zone"] = zone_idx;
    obj["name"] = Z[zone_idx].name;
    obj["on"] = Z[zone_idx].on;
    obj["brightness"] = internalBrightnessToHa(Z[zone_idx].brightness);
    obj["brightness_pct"] = internalBrightnessToPct(Z[zone_idx].brightness);
    obj["hue"] = (hsv.h / 255.0 * 360.0);
    obj["saturation"] = hsv.s;
    obj["pattern"] = Z[zone_idx].patternIndex;
    obj["pattern_name"] = PATS[Z[zone_idx].patternIndex].name;
    return obj;
  }
};

void handleFsList() {
  if (!ensureAuth()) return;
  sendCORS();
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) { server.send(500, "text/plain", "LittleFS root not dir"); return; }
  String out;
  File f = root.openNextFile();
  while (f) {
    out += String(f.isDirectory() ? "DIR  " : "FILE ") + f.name() + "  " + String((unsigned)f.size()) + "\n";
    f = root.openNextFile();
  }
  server.send(200, "text/plain", out);
}


void setup() {
  Serial.begin(115200);
  delay(200);

  // Mount LittleFS (UI assets)
  if(!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  // Init zones defaults
  for(uint8_t i=0;i<MAX_ZONES;i++){
    setupZone(i);
    Z[i].on = true;
    Z[i].nLeds = ZDEF[i].defaultCount;
    Z[i].brightness = 128;
    Z[i].frameMs = 20;
    Z[i].color = CRGB::Red;
    Z[i].patternIndex = 0;
    Z[i].lastNonzeroBri = 128;
    fill_solid(Z[i].leds, Z[i].nLeds, CRGB::Black);
    Z[i].ctl->showLeds(0);
  }

  // Load persisted config
  loadPrefs();

  FastLED.setMaxPowerInVoltsAndMilliamps(LED_SUPPLY_V, 3000);

  // Wi-Fi: try STA, else AP
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status()!=WL_CONNECTED && (millis()-t0)<8000) delay(200);
  if (WiFi.status()==WL_CONNECTED) Serial.printf("WiFi: %s\n", ipToStr(WiFi.localIP()).c_str());
  else { WiFi.softAP(AP_SSID, AP_PASS); Serial.printf("AP: %s\n", ipToStr(WiFi.softAPIP()).c_str()); }
  startMdnsIfStaConnected();


  // OTA firmware update endpoint
  server.on("/update", HTTP_POST, []() {
    sendCORS();
    if (!ensureAuth()) return;
    if (Update.hasError()) {
      server.send(500, "text/plain", "Update failed");
    } else {
      server.send(200, "text/plain", "Update successful. Rebooting...");
      delay(1000);
      ESP.restart();
    }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.setDebugOutput(true);
      Serial.printf("Update: %s\n", upload.filename.c_str());
      // Multipart uploads do not provide a reliable firmware size at start.
      // Let Update select the OTA partition and stream until the upload ends.
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %u bytes\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
      Serial.setDebugOutput(false);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.end();
      Serial.println("Update was aborted");
    }
  });

  // UI
  server.on("/", HTTP_GET, handleRoot);

  // Static assets + SPA fallback
  // Silence common browser auto-requests
  server.on("/favicon.ico", HTTP_GET, [](){ server.send(204); });
  server.on("/apple-touch-icon.png", HTTP_GET, [](){ server.send(204); });
  server.on("/manifest.json", HTTP_GET, [](){ server.send(204); });
  server.on("/health", HTTP_GET, [](){ sendCORS(); server.send(200, "application/json", "{\"ok\":true}"); });

  server.onNotFound([](){
    Serial.printf("[404] %s %s\n",
      (server.method()==HTTP_GET?"GET":server.method()==HTTP_POST?"POST":server.method()==HTTP_OPTIONS?"OPTIONS":"OTHER"),
      server.uri().c_str());
    sendCORS();
    if(serveFile(server.uri())) return;
    server.send(404, "text/plain", "Not found");
  });

  // HTTP OTA update is handled by the /update endpoint above.

// API (also keep old endpoints for compatibility)
  server.on("/api/fs", HTTP_GET, handleFsList);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/status",     HTTP_GET, handleStatus);

  server.on("/api/cmd",    HTTP_OPTIONS, [](){ sendCORS(); server.send(204); });
  server.on("/api/cmd",    HTTP_POST,    handleCmd);
  server.on("/api/cmd",    HTTP_GET,     handleCmd); // accept GET too (legacy/cached UI)
  server.on("/cmd",        HTTP_OPTIONS, [](){ sendCORS(); server.send(204); });
  server.on("/cmd",        HTTP_GET,     handleCmd);

  server.on("/api/save",   HTTP_OPTIONS, [](){ sendCORS(); server.send(204); });
  server.on("/api/save",   HTTP_POST,    handleSave);
  server.on("/save",       HTTP_OPTIONS, [](){ sendCORS(); server.send(204); });
  server.on("/save",       HTTP_POST,    handleSave);

  // ====== HomeKit / Apple Home Shortcuts API ======
  // These endpoints allow Apple Home Shortcuts to control lights without HomeKit hub
  server.on("/homekit/zones", HTTP_GET, [](){
    sendCORS();
    JsonDocument doc;
    JsonArray zones = doc["zones"].to<JsonArray>();
    for(uint8_t i = 0; i < MAX_ZONES; i++) {
      LedZoneLight zone(i);
      JsonObject zoneObj = zones.add<JsonObject>();
      zone.toJson(zoneObj);
    }
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  server.on(UriBraces("/homekit/zone/{}"), HTTP_OPTIONS, [](){ sendCORS(); server.send(204); });
  server.on(UriBraces("/homekit/zone/{}"), HTTP_GET, [](){
    sendCORS();
    int zoneId = server.pathArg(0).toInt();
    if(zoneId < 0 || zoneId >= MAX_ZONES) { server.send(404, "text/plain", "Zone not found"); return; }
    
    JsonDocument doc;
    LedZoneLight zone(zoneId);
    JsonObject zoneObj = doc.to<JsonObject>();
    zone.toJson(zoneObj);
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  server.on(UriBraces("/homekit/zone/{}"), HTTP_POST, [](){
    sendCORS();
    int zoneId = server.pathArg(0).toInt();
    if(zoneId < 0 || zoneId >= MAX_ZONES) { server.send(404, "text/plain", "Zone not found"); return; }
    
    if(!server.hasArg("plain")) { server.send(400, "text/plain", "No JSON body"); return; }
    String body = server.arg("plain");
    JsonDocument doc;
    if(deserializeJson(doc, body)) { server.send(400, "text/plain", "Invalid JSON"); return; }
    
    LedZoneLight zone(zoneId);
    zone.updateFromHomeKit(doc.as<JsonObject>());
    savePrefs();
    
    JsonDocument response;
    JsonObject responseObj = response.to<JsonObject>();
    zone.toJson(responseObj);
    String json;
    serializeJson(response, json);
    server.send(200, "application/json", json);
  });

  server.begin();
  Serial.println("\nHomeKit/Apple Home Shortcuts API available at:");
  Serial.printf("  Dashboard: http://%s/\n", ipToStr(WiFi.isConnected() ? WiFi.localIP() : WiFi.softAPIP()).c_str());
  Serial.println("  Health:    /health");
  Serial.println("  GET  /homekit/zones         - List all zones");
  Serial.println("  GET  /homekit/zone/0        - Get zone 0 status");
  Serial.println("  POST /homekit/zone/0        - Update zone 0 (JSON body)");
}

void loop() {
  server.handleClient();

  uint32_t now = millis();
  for(uint8_t i=0;i<MAX_ZONES;i++){
    if(now - Z[i].lastFrameMs >= Z[i].frameMs){
      Z[i].lastFrameMs = now;
      PATS[Z[i].patternIndex].fn(i);
      uint8_t scale = Z[i].on ? min<uint8_t>(Z[i].brightness, 95) : 0;
      Z[i].ctl->showLeds(scale);
    }
  }
}

// ================== PATTERN IMPLEMENTATIONS ==================
// Helper safe setters
inline void safeSet(uint8_t z, int idx, CRGB c){
  if(idx>=0 && idx<(int)Z[z].nLeds) Z[z].leds[idx] = c;
}
inline void safeAdd(uint8_t z, int idx, CRGB c){
  if(idx>=0 && idx<(int)Z[z].nLeds) Z[z].leds[idx] += c;
}

void p_solid(uint8_t z){
  fill_solid(Z[z].leds, Z[z].nLeds, Z[z].on ? Z[z].color : CRGB::Black);
}
void p_rainbow(uint8_t z){
  static uint8_t hue[MAX_ZONES]={0};
  if(!Z[z].on){ fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black); return; }
  fill_rainbow(Z[z].leds, Z[z].nLeds, hue[z]++);
}
void p_rainbow_glitter(uint8_t z){
  p_rainbow(z);
  if(!Z[z].on) return;
  if(random8()<30){ int p = random16(Z[z].nLeds); safeAdd(z,p,CRGB::White); }
}
void p_confetti(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds, Z[z].nLeds, 255); return; }
  fadeToBlackBy(Z[z].leds, Z[z].nLeds, 10);
  Z[z].leds[random16(Z[z].nLeds)] += CHSV(random8(), 200, 255);
}
void p_sinelon(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds, Z[z].nLeds, 255); return; }
  fadeToBlackBy(Z[z].leds, Z[z].nLeds, 20);
  int pos = beatsin16(13, 0, Z[z].nLeds-1);
  Z[z].leds[pos] += CHSV(beat8(10), 255, 255);
}
void p_juggle(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds, Z[z].nLeds, 255); return; }
  fadeToBlackBy(Z[z].leds, Z[z].nLeds, 20);
  byte d=0; for(int i=0;i<8;i++){ safeAdd(z, beatsin16(i+7,0,Z[z].nLeds-1), CHSV(d,200,255)); d+=32; }
}
void p_bpm(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black); return; }
  uint8_t bpm=62; CRGBPalette16 pal=PartyColors_p; uint8_t beat=beatsin8(bpm,64,255);
  for(int i=0;i<Z[z].nLeds;i++) Z[z].leds[i]=ColorFromPalette(pal,(i*2)+beat);
}
void p_theater(uint8_t z){
  static uint8_t idx[MAX_ZONES]={0};
  if(!Z[z].on){ fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black); return; }
  fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black);
  for(int i=idx[z]; i<(int)Z[z].nLeds; i+=3) safeSet(z,i,Z[z].color);
  idx[z]=(idx[z]+1)%3;
}
void p_twinkle(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds, Z[z].nLeds, 255); return; }
  for(int i=0;i<Z[z].nLeds;i++){ Z[z].leds[i].fadeToBlackBy(16); if(random8()<10) Z[z].leds[i]+=CHSV(random8(),200,255); }
}
void p_breathe(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black); return; }
  uint8_t s = beatsin8(8, 10, 255); CRGB c = Z[z].color; c.nscale8_video(s);
  fill_solid(Z[z].leds, Z[z].nLeds, c);
}
void p_comet(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds, Z[z].nLeds, 255); return; }
  static int pos[MAX_ZONES]={0}; static int dir[MAX_ZONES]={1,1,1,1};
  fadeToBlackBy(Z[z].leds, Z[z].nLeds, 40);
  safeSet(z,pos[z],Z[z].color);
  pos[z]+=dir[z]; if(pos[z]<=0||pos[z]>=Z[z].nLeds-1) dir[z]*=-1;
}
void p_ripple(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds, Z[z].nLeds, 255); return; }
  static uint16_t center[MAX_ZONES]; static uint8_t step[MAX_ZONES]; static uint8_t hue[MAX_ZONES];
  if(step[z]==0){ center[z]=random16(Z[z].nLeds); hue[z]=random8(); }
  fadeToBlackBy(Z[z].leds, Z[z].nLeds, 20);
  int left = center[z] - step[z]; int right = center[z] + step[z];
  if(step[z] < 64){
    if(left>=0)  safeAdd(z,left,  CHSV(hue[z],200, 255 - step[z]*4));
    if(right<(int)Z[z].nLeds) safeAdd(z,right, CHSV(hue[z],200, 255 - step[z]*4));
    step[z]++;
  } else step[z]=0;
}
void p_meteor(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds, Z[z].nLeds, 255); return; }
  static int pos[MAX_ZONES]; if(pos[z]==0) pos[z] = Z[z].nLeds-1;
  fadeToBlackBy(Z[z].leds, Z[z].nLeds, 30);
  for(int i=0;i<10;i++){ int p=pos[z]-i; if(p>=0){ CRGB c=Z[z].color; c.fadeToBlackBy(i*22); safeAdd(z,p,c);} }
  pos[z]--; if(pos[z]<0) pos[z]=Z[z].nLeds-1;
}
void p_larson(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds, Z[z].nLeds, 255); return; }
  static int pos[MAX_ZONES]={0}; static int dir[MAX_ZONES]={1,1,1,1};
  fadeToBlackBy(Z[z].leds, Z[z].nLeds, 25);
  safeSet(z,pos[z],Z[z].color);
  pos[z]+=dir[z]; if(pos[z]<=0||pos[z]>=Z[z].nLeds-1) dir[z]*=-1;
}
void p_gradient_scroll(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black); return; }
  static uint8_t off[MAX_ZONES]={0};
  for(int i=0;i<Z[z].nLeds;i++) Z[z].leds[i]=CHSV(off[z]+i*3,200,255);
  off[z]+=2;
}
void p_palette_cycle(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black); return; }
  static uint8_t idx[MAX_ZONES]={0}; static uint16_t t[MAX_ZONES]={0};
  CRGBPalette16 palettes[] = { RainbowColors_p, PartyColors_p, HeatColors_p, CloudColors_p, LavaColors_p, OceanColors_p, ForestColors_p };
  const uint8_t NUMP = sizeof(palettes)/sizeof(palettes[0]);
  if(++t[z] % 600 == 0) idx[z] = (idx[z]+1)%NUMP;
  for(int i=0;i<Z[z].nLeds;i++) Z[z].leds[i] = ColorFromPalette(palettes[idx[z]], i*4 + beat8(7));
}
void p_rainbow_beat(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black); return; }
  uint8_t a=beatsin8(17,0,255), b=beatsin8(13,0,255);
  for(int i=0;i<Z[z].nLeds;i++) Z[z].leds[i]=CHSV(i+a+b,255,255);
}
void p_police(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black); return; }
  static uint8_t ph[MAX_ZONES]={0}; fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black);
  uint8_t block = max<uint8_t>(1, Z[z].nLeds/6); uint8_t p = (ph[z]/8)%6;
  for(int i=0;i<block;i++){ int idx=p*block+i; if(idx<(int)Z[z].nLeds) Z[z].leds[idx]=(p%2==0)?CRGB::Red:CRGB::Blue; }
  ph[z]++;
}
void p_fire(uint8_t z){
  // Simple 1D fire
  static byte heat[MAX_ZONES][MAX_PER_ZONE];
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds,Z[z].nLeds,255); return; }
  for(int i=0;i<Z[z].nLeds;i++) heat[z][i] = qsub8(heat[z][i], random8(0, ((55*Z[z].frameMs)/20)+2));
  for(int k=Z[z].nLeds-1;k>=2;k--) heat[z][k]=(heat[z][k-1]+heat[z][k-2]+heat[z][k-2])/3;
  if(random8()< (120*Z[z].frameMs)/20 ){ int y = random16(0, min<int>(7,Z[z].nLeds)); heat[z][y] = qadd8(heat[z][y], random8(160,255)); }
  for(int j=0;j<Z[z].nLeds;j++){ CRGB c= HeatColor(heat[z][j]); Z[z].leds[j]=c; }
}
void p_noise_colorwaves(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds,Z[z].nLeds,255); return; }
  static uint16_t t[MAX_ZONES]={0}; t[z]+=2;
  for(int i=0;i<Z[z].nLeds;i++){ uint8_t n=inoise8(i*10,t[z]); Z[z].leds[i]=ColorFromPalette(RainbowColors_p, n); }
}
void p_plasma(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black); return; }
  static uint16_t t[MAX_ZONES]={0}; t[z]+=1;
  for(int i=0;i<Z[z].nLeds;i++){ uint8_t v = (sin8(i*4 + t[z]) + sin8(i*5 + t[z]/2))>>1; Z[z].leds[i]=CHSV(v,200,255); }
}
void p_chase(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black); return; }
  static uint16_t k[MAX_ZONES]={0}; fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black);
  int p = k[z] % max<int>(1,Z[z].nLeds); safeSet(z,p,Z[z].color); k[z]++;
}
void p_chase_dual(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black); return; }
  static uint16_t k[MAX_ZONES]={0}; fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black);
  int p1 = k[z] % max<int>(1,Z[z].nLeds);
  int p2 = (Z[z].nLeds-1) - p1;
  safeSet(z,p1,Z[z].color); safeSet(z,p2, Z[z].color);
  k[z]++;
}
void p_glitter(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds,Z[z].nLeds,255); return; }
  fadeToBlackBy(Z[z].leds,Z[z].nLeds,20);
  if(random8()<40){ int p=random16(Z[z].nLeds); safeAdd(z,p,CRGB::White); }
}
void p_candle(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black); return; }
  static uint8_t jitter[MAX_ZONES]={0}; jitter[z]=random8(80,255);
  CRGB base = CRGB(255,147,41); base.fadeToBlackBy(random8(40));
  for(int i=0;i<Z[z].nLeds;i++){ CRGB c = base; c.fadeLightBy(random8(80)); Z[z].leds[i]=c; }
}
void p_wave(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black); return; }
  static uint8_t h[MAX_ZONES]={0}; h[z]+=1;
  for(int i=0;i<Z[z].nLeds;i++){ uint8_t v = sin8(i*6 + h[z]); Z[z].leds[i]=CHSV(v, 200, 255); }
}
void p_beat_bars(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black); return; }
  uint8_t b = beatsin8(8, 0, Z[z].nLeds-1);
  fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black);
  for(int i=0;i<=b;i++) Z[z].leds[i] = Z[z].color;
}
void p_stars(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds,Z[z].nLeds,255); return; }
  fadeToBlackBy(Z[z].leds,Z[z].nLeds,10);
  if(random8()<30){ int p=random16(Z[z].nLeds); safeAdd(z,p,CRGB::White); }
}
void p_scanlines(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds,Z[z].nLeds,CRGB::Black); return; }
  static uint8_t k[MAX_ZONES]={0}; k[z]++;
  for(int i=0;i<Z[z].nLeds;i++) Z[z].leds[i] = ( ((i+k[z])%2)==0 ? Z[z].color : CRGB::Black );
}

void p_fireflies(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds, Z[z].nLeds, 255); return; }
  fadeToBlackBy(Z[z].leds, Z[z].nLeds, 10);
  // a few wandering lights
  static int pos[ MAX_ZONES ][5];         // 5 fireflies
  static int vel[ MAX_ZONES ][5];
  for(int k=0;k<5;k++){
    if(vel[z][k]==0) vel[z][k] = (random8(2)?1:-1);
    pos[z][k] += vel[z][k];
    if(pos[z][k] < 0 || pos[z][k] >= (int)Z[z].nLeds){ vel[z][k]*=-1; pos[z][k]+=vel[z][k]; }
    Z[z].leds[pos[z][k]] += CHSV(random8(40,90), 200, random8(180,255));
  }
}

void p_fireworks(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds,Z[z].nLeds,255); return; }
  static int shellPos[MAX_ZONES]={-1}; static int shellVel[MAX_ZONES]={0};
  static bool burst[MAX_ZONES]={false}; static uint8_t hue[MAX_ZONES]={0};

  fadeToBlackBy(Z[z].leds, Z[z].nLeds, 30);

  if(shellPos[z]<0){ // launch new shell
    shellPos[z]=0; shellVel[z]=random8(3,6); burst[z]=false; hue[z]=random8();
  }

  if(!burst[z]){ // rising
    shellPos[z]+=shellVel[z];
    if(shellPos[z] >= (int)Z[z].nLeds*3/4){ burst[z]=true; }
    if(shellPos[z]<(int)Z[z].nLeds) Z[z].leds[shellPos[z]] += CHSV(hue[z],200,255);
  } else { // explode
    for(int i=0;i<12;i++){
      int p = shellPos[z] + (int)(sin16((i*5461)) *  (int) (12) >> 15); // spokes
      if(p>=0 && p<(int)Z[z].nLeds) Z[z].leds[p] += CHSV(hue[z]+i*8,200,255);
    }
    // reset after bloom fades
    if(random8()<10){ shellPos[z]=-1; }
  }
}

void p_dual_comet(uint8_t z){
  if(!Z[z].on){ fadeToBlackBy(Z[z].leds, Z[z].nLeds, 255); return; }

  static int p[MAX_ZONES]={0};
  static int d[MAX_ZONES]={1,1,1,1};

  fadeToBlackBy(Z[z].leds, Z[z].nLeds, 40);

  // Head is the selected RGB color
  CRGB head = Z[z].color;

  // Tail: compute opposite hue from head using HSV
  CHSV hsvHead = rgb2hsv_approximate(head);  // <-- returns CHSV
  uint8_t tailHue = hsvHead.h + 128;         // opposite hue
  CHSV hsvTail(tailHue, hsvHead.s, min<uint8_t>(200, hsvHead.v)); // a bit dimmer
  CRGB tail = hsvTail;                        // convert to RGB

  safeSet(z, p[z], head);
  if(p[z]-1 >= 0) safeSet(z, p[z]-1, tail);

  p[z] += d[z];
  if(p[z] <= 0 || p[z] >= (int)Z[z].nLeds-1) d[z] *= -1;
}

void p_wave_mono(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black); return; }

  // speed scales with frameMs (lower ms = faster)
  static uint16_t ph[MAX_ZONES]={0};
  uint8_t speed = constrain((uint8_t)(12 + (200 - Z[z].frameMs)/6), 8, 40);
  ph[z] += speed;

  // spatial frequency: adjust 2..10 for tighter/looser waves
  const uint8_t kSpatial = 5;

  for(int i=0;i<Z[z].nLeds;i++){
    uint8_t w = sin8(i * kSpatial + (ph[z] >> 1)); // 0..255
    // scale your selected color by the wave
    CRGB c = Z[z].color;
    c.nscale8_video(w);
    Z[z].leds[i] = c;
  }
}

void p_wave_rainbow(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black); return; }

  static uint16_t ph[MAX_ZONES]={0};
  uint8_t speed = constrain((uint8_t)(10 + (200 - Z[z].frameMs)/7), 8, 36);
  ph[z] += speed;

  const uint8_t kSpatial = 4;          // hue step across strip
  uint8_t breathe = beatsin8(7, 160, 255); // slow global brightness swell

  for(int i=0;i<Z[z].nLeds;i++){
    uint8_t hue = i * kSpatial + (ph[z] >> 2);
    uint8_t val = qmul8(breathe, sin8(i * (kSpatial+1) + (ph[z] >> 1))); // blend two motions
    Z[z].leds[i] = CHSV(hue, 255, val);
  }
}

void p_wave_dual(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black); return; }

  static uint16_t phA[MAX_ZONES]={0};
  static uint16_t phB[MAX_ZONES]={0};

  uint8_t spA = constrain((uint8_t)(8 + (200 - Z[z].frameMs)/8), 6, 28);
  uint8_t spB = constrain((uint8_t)(6 + (200 - Z[z].frameMs)/10), 5, 24);
  phA[z] += spA;          // wave A moves right
  phB[z] -= spB;          // wave B moves left

  const uint8_t kSpatialA = 5;
  const uint8_t kSpatialB = 7;

  for(int i=0;i<Z[z].nLeds;i++){
    // two waves in brightness
    uint8_t v1 = sin8(i * kSpatialA + (phA[z] >> 1));
    uint8_t v2 = sin8(i * kSpatialB + (phB[z] >> 1));
    uint8_t v  = qadd8(v1>>1, v2>>1); // interference (0..255)

    // color: base on your selected color but let hue drift a bit
    CHSV base = rgb2hsv_approximate(Z[z].color);
    base.h += (phA[z] >> 4);          // slow hue drift
    base.v  = v;                      // brightness from waves

    Z[z].leds[i] = base;              // HSV → RGB
  }
}


// ================== Modern / Futuristic patterns (custom) ==================

void p_neon_pulse(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black); return; }

  // Neon cyan <-> magenta with a breathing pulse + tiny glitter
  uint8_t pulse = beatsin8(18, 40, 255);           // 40..255
  uint8_t mix   = beatsin8(7,  0, 255);            // blend factor
  CRGB a = CHSV(160, 255, pulse);                  // cyan
  CRGB b = CHSV(224, 255, pulse);                  // magenta
  CRGB c = blend(a, b, mix);

  fill_solid(Z[z].leds, Z[z].nLeds, c);

  // subtle sparkle
  if (random8() < 18) {
    uint16_t i = random16(Z[z].nLeds);
    Z[z].leds[i] += CRGB(80,80,80);
  }
}

void p_hologram(uint8_t z){
  if(!Z[z].on){ fill_solid(Z[z].leds, Z[z].nLeds, CRGB::Black); return; }

  static uint16_t t[MAX_ZONES]={0}; t[z]+=2;

  // Noisy cyan/magenta shimmer + scanline flicker
  for (uint16_t i=0;i<Z[z].nLeds;i++){
    uint8_t n  = inoise8(i*18, t[z]);
    uint8_t hh = 160 + (n >> 2);                 // around cyan
    CRGB col   = CHSV(hh, 220, 200);

    // scanlines: every 6th pixel is brighter, with slight time wobble
    if (((i + (t[z]>>2)) % 6) == 0) col.nscale8_video(255);
    else                           col.nscale8_video(150);

    // occasional glitch pixels
    if (random8() < 4) col += CHSV(224, 255, 120); // magenta pop

    Z[z].leds[i] = col;
  }

  // soften
  blur1d(Z[z].leds, Z[z].nLeds, 40);
}
