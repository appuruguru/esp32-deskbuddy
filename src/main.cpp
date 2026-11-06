#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

#include "secrets.h"

// ---- Hardware (ESP32-2432S028R "CYD") --------------------------------------
// Touch controller sits on its own SPI bus, separate from the display.
static const int T_CLK = 25, T_MOSI = 32, T_MISO = 39, T_CS = 33, T_IRQ = 36;

// Raw XPT2046 range -> landscape pixels (320x240). Tweak if touch feels off.
static const int RAW_X_MIN = 200, RAW_X_MAX = 3700;
static const int RAW_Y_MIN = 240, RAW_Y_MAX = 3800;

// Portrait layout (240x320). Rotation 0 or 2 (180 degrees apart), saved in flash.
static const int W = 240, H = 320;
static const int EYE_CY = 118, EYE_LX = 66, EYE_RX = 174;
static const int MOUTH_Y = 222;

TFT_eSPI tft;
TFT_eSprite spr(&tft);
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(T_CS, T_IRQ);
Preferences prefs;
WebServer web(80);
DNSServer dns;
// True while the WiFi setup hotspot is open (see "WiFi setup hotspot" below).
static bool portalActive = false;
static void startPortal(bool manual);
static void portalTap(int x, int y);
static uint8_t rotation = 0;
static uint32_t lastInteraction = 0;

// Backlight (PWM) and ambient light sensor (photoresistor on GPIO34, input-only ADC pin).
static const int LDR_PIN = 34;
static const int BL_CHANNEL = 0;
static uint8_t backlight = 255;

static void setBacklight(uint8_t v) {
  backlight = v;
  ledcWrite(BL_CHANNEL, v);
}

// Auto-brightness levels (PWM duty 0..255): full in a bright room, very dim in a dark one, and a
// readable level for 20 s after a touch in the dark. See updateBacklight().
static const uint8_t BRIGHT_DAY = 255, BRIGHT_WAKE = 90, BRIGHT_SLEEP = 8;
static int manualBright = -1;  // >= 0 overrides auto (set via /bright)

// Averaged raw 12-bit reading (0..4095).
static int readLdr() {
  long s = 0;
  for (int i = 0; i < 16; i++) s += analogRead(LDR_PIN);
  return (int)(s / 16);
}

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

// Set when the previous run died from a crash/watchdog: network data fetching stays off until the
// next power-on, so a bad fetch can never boot-loop the device or block OTA recovery.
static bool safeMode = false;

// Consecutive failed fetches per data source, so screens can say "Retrying" instead of "Loading" forever.
static volatile uint8_t wxFails = 0, bandsFails = 0;
static volatile bool refreshNow = false;  // dev tool: re-fetch weather and bands immediately

// Developer tools: /screenshot.bmp plus remote touch/mood/time/weather controls over HTTP.
// Unauthenticated, LAN-only. Set to 0 to compile them out.
#define DEV_TOOLS 1

// A synthetic touch (tap or swipe) injected over HTTP; handleTouch() treats it like a finger.
static struct {
  bool active = false;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  uint32_t start = 0, dur = 0;
} sim;

enum Screen { SCREEN_FACE, SCREEN_CLOCK, SCREEN_BANDS, SCREEN_COUNT };
static void drawDots();  // page indicator, defined with the other screens
static Screen screen = SCREEN_FACE;
static bool screenJustChanged = false;
static bool sceneActive = false;  // clock/weather page showing the animated weather scene

static bool timeValid() { return time(nullptr) > 1700000000; }
static bool locationSet() { return strcmp(WEATHER_LAT, "0") != 0 || strcmp(WEATHER_LON, "0") != 0; }

// ---- Face -------------------------------------------------------------------
enum Mood { IDLE, ATTENTION, HAPPY, LAUGH, SURPRISED, SLEEPY };

struct Look {
  float eyeW, eyeH, smile, lid;  // smile: -1 frown .. 1 big grin; lid: 0 open .. 1 shut
  bool happyEyes, mouthO, blush;
};

static const Look LOOKS[] = {
    /* IDLE      */ {70, 100, 0.45f, 0.00f, false, false, false},
    /* ATTENTION */ {76, 112, 0.60f, 0.00f, false, false, false},
    /* HAPPY     */ {70, 100, 1.00f, 0.00f, true, false, true},
    /* LAUGH     */ {70, 100, 1.00f, 0.00f, true, false, true},
    /* SURPRISED */ {84, 132, 0.00f, 0.00f, false, true, false},
    /* SLEEPY    */ {70, 82, 0.10f, 0.55f, false, false, false},
};

static const uint16_t EYE = TFT_CYAN;
static uint16_t BLUSH;  // set in setup (needs color565)

struct FaceState {
  float eyeW = 70, eyeH = 100, smile = 0.45f, lid = 0;
  float gx = 0, gy = 0, gxT = 0, gyT = 0;
  Mood mood = IDLE;
  uint32_t moodSince = 0, moodUntil = 0;  // moodUntil == 0 -> stay until changed
  uint32_t blinkStart = 0, nextBlink = 2000, nextGaze = 1500;
} f;

static float lerpf(float a, float b, float k) { return a + (b - a) * k; }

// Elapsed ms since t, clamped at 0. Timestamps are sometimes taken a ms after `now` was sampled
// (web handlers, setMood), and a plain unsigned `now - t` would wrap to ~49 days in that case.
static uint32_t since(uint32_t now, uint32_t t) {
  int32_t d = (int32_t)(now - t);
  return d < 0 ? 0 : (uint32_t)d;
}

static void setMood(Mood m, uint32_t durationMs = 0) {
  uint32_t now = millis();
  f.mood = m;
  f.moodSince = now;
  f.moodUntil = durationMs ? now + durationMs : 0;
}

static void setScreen(Screen s) {
  if (s == screen) return;
  screen = s;
  screenJustChanged = true;
  sceneActive = false;  // leaving a page always closes its sub-view
  f.gxT = f.gyT = 0;
  if (s == SCREEN_FACE) {  // waking up to the face
    lastInteraction = millis();
    setMood(SURPRISED, 700);
  }
}

static void flipScreen() {
  rotation = rotation ? 0 : 2;
  prefs.putUChar("rot", rotation);
  tft.setRotation(rotation);
  tft.fillScreen(TFT_BLACK);
  screenJustChanged = true;
  Serial.printf("rotation -> %d\n", rotation);
}

static void drawMouth(const Look &L, int cx, int cy, int oy) {
  if (L.mouthO) {
    spr.fillSmoothCircle(cx, cy + oy, 18, EYE, TFT_BLACK);
    spr.fillSmoothCircle(cx, cy + oy, 12, TFT_BLACK, EYE);
    return;
  }
  // Stamp tightly spaced discs along the curve: smooth, with round ends. (Chained anti-aliased
  // wide lines notch each other at the joints and read as a string of beads.)
  const int steps = 56;
  const float halfW = 46, amp = 28;
  for (int i = 0; i <= steps; i++) {
    float u = -1.0f + 2.0f * i / steps;
    spr.fillCircle(cx + u * halfW, cy + oy + f.smile * amp * (1.0f - u * u), 3, EYE);
  }
}

static void drawEye(const Look &L, int cx, int cy, float blink) {
  if (L.happyEyes) {
    spr.drawSmoothArc(cx, cy + 16, 38, 26, 90, 270, EYE, TFT_BLACK, true);  // "^"
    return;
  }
  float w = f.eyeW;
  float h = max(6.0f, f.eyeH * (1.0f - 0.93f * blink));
  int x = (int)(cx - w / 2), y = (int)(cy - h / 2);
  int r = (int)max(2.0f, min(w, h) / 2 * 0.62f);
  spr.fillSmoothRoundRect(x, y, (int)w, (int)h, r, EYE, TFT_BLACK);
  if (f.lid > 0.01f) {  // sleepy lid from the top
    spr.fillRect(x - 2, y - 2, (int)w + 4, (int)(h * f.lid) + 2, TFT_BLACK);
  }
}

static void drawFace(uint32_t now) {
  const Look &L = LOOKS[f.mood];

  float blink = 0;
  uint32_t bt = now - f.blinkStart;
  if (bt < 180) blink = bt < 80 ? bt / 80.0f : 1.0f - (bt - 80) / 100.0f;

  int ox = 0, oy = 0;
  uint32_t mt = since(now, f.moodSince);
  if (f.mood == HAPPY && mt < 500) oy = (int)(-sinf(mt / 500.0f * PI) * 10);
  if (f.mood == LAUGH) {
    ox = (int)(sinf(now / 22.0f) * 3);
    oy = (int)(sinf(now / 37.0f) * 3);
  }
  if (f.mood == SURPRISED && mt < 250) oy = (int)(-sinf(mt / 250.0f * PI) * 8);

  spr.fillSprite(TFT_BLACK);

  int ey = EYE_CY + oy + (int)f.gy;
  drawEye(L, EYE_LX + ox + (int)f.gx, ey, blink);
  drawEye(L, EYE_RX + ox + (int)f.gx, ey, blink);

  if (L.blush) {
    spr.fillEllipse(30 + ox, 172 + oy, 20, 9, BLUSH);
    spr.fillEllipse(210 + ox, 172 + oy, 20, 9, BLUSH);
  }

  drawMouth(L, W / 2 + ox + (int)(f.gx * 0.5f), MOUTH_Y, oy);

  if (f.mood == SLEEPY) {  // floating z's
    float t = (now % 3000) / 3000.0f;
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.drawString("z", 176 + (int)(t * 14), 60 - (int)(t * 26), 4);
    spr.drawString("Z", 198 + (int)(t * 10), 36 - (int)(t * 18), 4);
  }

  // WiFi indicator: red dot while offline, brief green dot after connecting.
  static uint32_t connectedAt = 0;
  if (WiFi.status() == WL_CONNECTED) {
    if (!connectedAt) connectedAt = now;
    if (now - connectedAt < 3000) spr.fillCircle(W - 8, 8, 4, TFT_GREEN);
  } else {
    connectedAt = 0;
    spr.fillCircle(W - 8, 8, 4, TFT_RED);
  }

  if (safeMode) spr.fillCircle(W - 22, 8, 4, TFT_ORANGE);  // data fetching disabled after a crash

  drawDots();
  spr.pushSprite(0, 0);
}

static void updateFace(uint32_t now) {
  const Look &L = LOOKS[f.mood];
  const float k = 0.28f;
  f.eyeW = lerpf(f.eyeW, L.eyeW, k);
  f.eyeH = lerpf(f.eyeH, L.eyeH, k);
  f.smile = lerpf(f.smile, L.smile, k);
  f.lid = lerpf(f.lid, L.lid, 0.12f);
  f.gx = lerpf(f.gx, f.gxT, 0.3f);
  f.gy = lerpf(f.gy, f.gyT, 0.3f);

  if (f.moodUntil && now > f.moodUntil) setMood(IDLE);

  if (now > f.nextBlink) {
    f.blinkStart = now;
    f.nextBlink = now + 2000 + random(0, 4000);
  }
}

// ---- Weather ----------------------------------------------------------------
struct Weather {
  bool valid = false;
  float temp = 0, feels = 0;
  int code = 0;
  bool isDay = true;
  uint16_t sunrise = 0, sunset = 0;  // today's local minutes since midnight; 0 = unknown
  float hi[4] = {}, lo[4] = {};
  int dcode[4] = {};
  uint8_t dwday[4] = {};
};

static Weather wx;
static SemaphoreHandle_t wxMu;

enum Icon { I_SUN, I_MOON, I_PARTLY, I_CLOUD, I_FOG, I_RAIN, I_SNOW, I_STORM };

static Icon iconFor(int c, bool day) {
  if (c == 0 || c == 1) return day ? I_SUN : I_MOON;
  if (c == 2) return I_PARTLY;
  if (c == 3) return I_CLOUD;
  if (c == 45 || c == 48) return I_FOG;
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return I_RAIN;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return I_SNOW;
  if (c >= 95) return I_STORM;
  return I_CLOUD;
}

static const char *wxText(int c) {
  switch (c) {
    case 0:  return "Clear";
    case 1:  return "Mostly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: case 82: return "Showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunder + hail";
    default: return "Unknown";
  }
}

// Returns true on success.
static bool fetchWeather() {
  if (ESP.getMaxAllocHeap() < 26000) {  // TLS needs several ~16 KB blocks, not one large one
    Serial.printf("weather: low heap (%u)\n", ESP.getMaxAllocHeap());
    return false;
  }
  char url[360];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
           "&current=temperature_2m,apparent_temperature,weather_code,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
           "&temperature_unit=%s&timezone=auto&forecast_days=4",
           WEATHER_LAT, WEATHER_LON, USE_FAHRENHEIT ? "fahrenheit" : "celsius");

  WiFiClientSecure client;
  client.setCACertBundle(rootca_crt_bundle_start);  // verify the server certificate
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("weather: HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("weather: JSON error %s\n", err.c_str());
    return false;
  }

  Weather w;
  w.valid = true;
  w.temp = doc["current"]["temperature_2m"] | 0.0f;
  w.feels = doc["current"]["apparent_temperature"] | 0.0f;
  w.code = doc["current"]["weather_code"] | 0;
  w.isDay = (doc["current"]["is_day"] | 1) != 0;
  // "2026-10-19T07:09" -> minutes since local midnight
  auto parseHm = [](const char *s) -> uint16_t {
    const char *t = s ? strchr(s, 'T') : nullptr;
    int h = 0, m = 0;
    if (t && sscanf(t + 1, "%d:%d", &h, &m) == 2) return h * 60 + m;
    return 0;
  };
  w.sunrise = parseHm(doc["daily"]["sunrise"][0] | "");
  w.sunset = parseHm(doc["daily"]["sunset"][0] | "");
  for (int i = 0; i < 4; i++) {
    w.hi[i] = doc["daily"]["temperature_2m_max"][i] | 0.0f;
    w.lo[i] = doc["daily"]["temperature_2m_min"][i] | 0.0f;
    w.dcode[i] = doc["daily"]["weather_code"][i] | 0;
    int y = 0, m = 0, d = 0;
    sscanf(doc["daily"]["time"][i] | "", "%d-%d-%d", &y, &m, &d);
    struct tm tmv = {};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = 12;
    mktime(&tmv);
    w.dwday[i] = tmv.tm_wday;
  }
  xSemaphoreTake(wxMu, portMAX_DELAY);
  wx = w;
  xSemaphoreGive(wxMu);
  Serial.printf("weather: %.0f, code %d, hi %.0f lo %.0f\n", w.temp, w.code, w.hi[0], w.lo[0]);
  return true;
}

// ---- HF band conditions (N0NBH solar data from hamqsl.com) ---------------------
struct Bands {
  bool valid = false;
  char sfi[8] = "", aidx[8] = "", kidx[8] = "", xray[10] = "", sunspots[8] = "", wind[10] = "", updated[40] = "";
  uint8_t cond[4][2] = {{255, 255}, {255, 255}, {255, 255}, {255, 255}};  // [group][day,night]: 0 poor 1 fair 2 good
};

static Bands bands;
static const char *BAND_GROUPS[4] = {"80m-40m", "30m-20m", "17m-15m", "12m-10m"};

static bool xmlTag(const String &x, const char *tag, char *out, size_t cap) {
  String open = String("<") + tag + ">", close = String("</") + tag + ">";
  int a = x.indexOf(open);
  if (a < 0) return false;
  a += open.length();
  int b = x.indexOf(close, a);
  if (b < 0) return false;
  String v = x.substring(a, b);
  v.trim();
  strlcpy(out, v.c_str(), cap);
  return true;
}

static bool fetchBands() {
  if (ESP.getMaxAllocHeap() < 26000) {  // TLS needs several ~16 KB blocks, not one large one
    Serial.printf("bands: low heap (%u)\n", ESP.getMaxAllocHeap());
    return false;
  }
  String body;
  {
    WiFiClientSecure client;
    client.setCACertBundle(rootca_crt_bundle_start);  // verify the server certificate
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    if (!http.begin(client, "https://www.hamqsl.com/solarxml.php")) return false;
    int code = http.GET();
    if (code != 200) {
      Serial.printf("bands: HTTP %d\n", code);
      http.end();
      return false;
    }
    body = http.getString();
    http.end();
  }

  Bands nb;
  nb.valid = xmlTag(body, "solarflux", nb.sfi, sizeof(nb.sfi));
  xmlTag(body, "aindex", nb.aidx, sizeof(nb.aidx));
  xmlTag(body, "kindex", nb.kidx, sizeof(nb.kidx));
  xmlTag(body, "xray", nb.xray, sizeof(nb.xray));
  xmlTag(body, "sunspots", nb.sunspots, sizeof(nb.sunspots));
  xmlTag(body, "solarwind", nb.wind, sizeof(nb.wind));
  xmlTag(body, "updated", nb.updated, sizeof(nb.updated));
  for (int g = 0; g < 4; g++) {
    for (int d = 0; d < 2; d++) {
      String key = String("<band name=\"") + BAND_GROUPS[g] + "\" time=\"" + (d == 0 ? "day" : "night") + "\">";
      int a = body.indexOf(key);
      if (a < 0) continue;
      a += key.length();
      String v = body.substring(a, body.indexOf('<', a));
      v.trim();
      nb.cond[g][d] = v == "Good" ? 2 : v == "Fair" ? 1 : v == "Poor" ? 0 : 255;
    }
  }
  if (!nb.valid) return false;
  xSemaphoreTake(wxMu, portMAX_DELAY);
  bands = nb;
  xSemaphoreGive(wxMu);
  Serial.printf("bands: SFI %s A %s K %s, 20m day cond %d\n", nb.sfi, nb.aidx, nb.kidx, nb.cond[1][0]);
  return true;
}

// Background fetcher: one TLS request at a time keeps heap use low.
static void dataTask(void *) {
  uint32_t nextWx = 0, nextBands = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    uint32_t now = millis();
    if (refreshNow) {
      refreshNow = false;
      nextWx = nextBands = now;
    }
    // 30 s grace after boot keeps the OTA window open even if fetching ever misbehaves.
    if (safeMode || now < 30000) continue;
    bool net = WiFi.status() == WL_CONNECTED && timeValid();
    if (net && locationSet() && (int32_t)(now - nextWx) >= 0) {
      Serial.println("weather: fetching");
      bool okw = fetchWeather();
      wxFails = okw ? 0 : (wxFails < 99 ? wxFails + 1 : 99);
      nextWx = millis() + (okw ? 15UL * 60 * 1000 : 30UL * 1000);
    }
    if (net && (int32_t)(millis() - nextBands) >= 0) {
      Serial.println("bands: fetching");
      bool okb = fetchBands();
      bandsFails = okb ? 0 : (bandsFails < 99 ? bandsFails + 1 : 99);
      nextBands = millis() + (okb ? 60UL * 60 * 1000 : 60UL * 1000);
    }
  }
}

// ---- Weather icons (drawn from shapes) ----------------------------------------
// Neon palette (RGB565): bright saturated tubes on pure black.
static const uint16_t N_CYAN = 0x07FF, N_MAGENTA = 0xF8B9, N_GREEN = 0x3FE2, N_YELLOW = 0xFF80,
                      N_ORANGE = 0xFAE0, N_BLUE = 0x047F, N_PURPLE = 0xB01F, N_WHITE = 0xE7FF;

static uint16_t dimColor(uint16_t c, int pct) {
  int r = ((c >> 11) & 31) * pct / 100, g = ((c >> 5) & 63) * pct / 100, b = (c & 31) * pct / 100;
  return (r << 11) | (g << 5) | b;
}

static void cloudShape(int cx, int cy, int s, uint16_t col) {
  spr.fillCircle(cx - s * 0.20f, cy + s * 0.04f, s * 0.17f, col);
  spr.fillCircle(cx, cy - s * 0.07f, s * 0.24f, col);
  spr.fillCircle(cx + s * 0.22f, cy + s * 0.04f, s * 0.18f, col);
  spr.fillRoundRect(cx - s * 0.36f, cy + s * 0.04f, s * 0.76f, s * 0.18f, s * 0.09f, col);
}

// A dim, slightly larger copy underneath gives the neon "glow".
static void drawCloud(int cx, int cy, int s, uint16_t col) {
  cloudShape(cx, cy, s * 1.16f, dimColor(col, 28));
  cloudShape(cx, cy, s, col);
}

static void drawSun(int cx, int cy, int s) {
  spr.fillCircle(cx, cy, s * 0.28f, dimColor(N_YELLOW, 26));
  spr.fillCircle(cx, cy, s * 0.2f, N_YELLOW);
  float wd = max(2.0f, s / 20.0f);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    spr.drawWideLine(cx + cosf(a) * s * 0.30f, cy + sinf(a) * s * 0.30f, cx + cosf(a) * s * 0.44f,
                     cy + sinf(a) * s * 0.44f, wd, N_ORANGE, TFT_BLACK);
  }
}

static void drawIcon(Icon ic, int cx, int cy, int s) {
  float wd = max(2.0f, s / 22.0f);
  switch (ic) {
    case I_SUN:
      drawSun(cx, cy, s);
      break;
    case I_MOON:
      spr.fillCircle(cx, cy, s * 0.36f, dimColor(N_WHITE, 22));
      spr.fillCircle(cx, cy, s * 0.28f, N_WHITE);
      spr.fillCircle(cx + s * 0.12f, cy - s * 0.08f, s * 0.24f, TFT_BLACK);
      break;
    case I_PARTLY:
      drawSun(cx - s * 0.14f, cy - s * 0.16f, s * 0.7f);
      drawCloud(cx + s * 0.06f, cy + s * 0.1f, s * 0.82f, N_BLUE);
      break;
    case I_CLOUD:
      drawCloud(cx, cy, s, N_BLUE);
      break;
    case I_FOG:
      for (int i = 0; i < 3; i++) {
        float y = cy - s * 0.18f + i * s * 0.18f;
        float half = s * (i == 1 ? 0.38f : 0.30f);
        spr.drawWideLine(cx - half, y, cx + half, y, wd * 3.0f, dimColor(N_CYAN, 28), TFT_BLACK);
        spr.drawWideLine(cx - half, y, cx + half, y, wd * 1.5f, N_CYAN, TFT_BLACK);
      }
      break;
    case I_RAIN:
      drawCloud(cx, cy - s * 0.14f, s * 0.9f, N_PURPLE);
      for (int i = 0; i < 3; i++) {
        float x = cx - s * 0.22f + i * s * 0.22f;
        spr.drawWideLine(x, cy + s * 0.16f, x - s * 0.06f, cy + s * 0.34f, wd, N_CYAN, TFT_BLACK);
      }
      break;
    case I_SNOW:
      drawCloud(cx, cy - s * 0.14f, s * 0.9f, N_BLUE);
      for (int i = 0; i < 3; i++) {
        float x = cx - s * 0.2f + i * s * 0.2f;
        float y = cy + s * (i == 1 ? 0.34f : 0.24f);
        spr.fillCircle(x, y, max(2.0f, s * 0.05f), N_WHITE);
      }
      break;
    case I_STORM:
      drawCloud(cx, cy - s * 0.14f, s * 0.9f, N_PURPLE);
      spr.fillTriangle(cx + s * 0.08f, cy + s * 0.08f, cx - s * 0.08f, cy + s * 0.28f, cx + s * 0.04f, cy + s * 0.28f, N_YELLOW);
      spr.fillTriangle(cx + s * 0.02f, cy + s * 0.22f, cx - s * 0.04f, cy + s * 0.42f, cx + s * 0.14f, cy + s * 0.22f, N_YELLOW);
      break;
  }
}

// ---- Clock screen -------------------------------------------------------------
static const char *DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void drawTemp(int x, int y, float t, uint16_t col) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", (int)lroundf(t));
  spr.setTextColor(col, TFT_BLACK);
  int w = spr.drawString(buf, x, y, 7);
  spr.drawCircle(x + w + 8, y + 7, 4, col);
  spr.drawString(USE_FAHRENHEIT ? "F" : "C", x + w + 16, y, 4);
}

static void drawClock(uint32_t now) {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(TL_DATUM);

  struct tm t;
  bool ok = timeValid() && getLocalTime(&t, 0);

  // Date
  char buf[40];
  if (ok) snprintf(buf, sizeof(buf), "%s, %s %d", DAYS[t.tm_wday], MONTHS[t.tm_mon], t.tm_mday);
  else    snprintf(buf, sizeof(buf), "Setting clock...");
  spr.setTextColor(N_MAGENTA, TFT_BLACK);
  spr.drawString(buf, (W - spr.textWidth(buf, 4)) / 2, 8, 4);

  // Time: draw hour, colon, minutes separately so the blinking colon never shifts the layout.
  char hh[4] = "--", mm[4] = "--";
  if (ok) {
    int h = t.tm_hour;
    if (!CLOCK_24H) h = h % 12 == 0 ? 12 : h % 12;
    snprintf(hh, sizeof(hh), CLOCK_24H ? "%02d" : "%d", h);
    snprintf(mm, sizeof(mm), "%02d", t.tm_min);
  }
  int wH = spr.textWidth(hh, 6), wC = spr.textWidth(":", 6), wM = spr.textWidth(mm, 6);
  int x0 = (W - (wH + wC + wM)) / 2;
  spr.setTextColor(N_CYAN, TFT_BLACK);
  spr.drawString(hh, x0, 36, 6);
  spr.drawString(mm, x0 + wH + wC, 36, 6);
  if (!ok || (now / 500) % 2 == 0) {
    spr.setTextColor(N_MAGENTA, TFT_BLACK);
    spr.drawString(":", x0 + wH, 36, 6);
  }
  if (ok && !CLOCK_24H) {
    const char *ap = t.tm_hour >= 12 ? "PM" : "AM";
    spr.setTextColor(N_GREEN, TFT_BLACK);
    spr.drawString(ap, (W - spr.textWidth(ap, 4)) / 2, 116, 4);
  }

  spr.drawFastHLine(8, 146, W - 16, dimColor(N_MAGENTA, 55));
  spr.drawFastHLine(8, 147, W - 16, dimColor(N_MAGENTA, 22));

  // Weather
  Weather w;
  xSemaphoreTake(wxMu, portMAX_DELAY);
  w = wx;
  xSemaphoreGive(wxMu);

  if (!w.valid) {
    const char *msg = safeMode ? "Safe mode (no data)" : !locationSet() ? "Set location"
                      : WiFi.status() != WL_CONNECTED ? "No WiFi" : wxFails ? "Retrying weather..." : "Loading weather...";
    spr.setTextColor(dimColor(N_CYAN, 60), TFT_BLACK);
    spr.drawString(msg, (W - spr.textWidth(msg, 4)) / 2, 210, 4);
  } else {
    drawIcon(iconFor(w.code, w.isDay), 48, 188, 70);
    drawTemp(100, 160, w.temp, N_YELLOW);

    const char *txt = wxText(w.code);
    spr.setTextColor(N_CYAN, TFT_BLACK);
    spr.drawString(txt, (W - spr.textWidth(txt, 4)) / 2, 222, 4);

    char hi[12], lo[12];
    snprintf(hi, sizeof(hi), "H %d", (int)lroundf(w.hi[0]));
    snprintf(lo, sizeof(lo), "L %d", (int)lroundf(w.lo[0]));
    int wh = spr.textWidth(hi, 4), wl = spr.textWidth(lo, 4);
    int xs = (W - (wh + 24 + wl)) / 2;
    spr.setTextColor(N_ORANGE, TFT_BLACK);
    spr.drawString(hi, xs, 246, 4);
    spr.setTextColor(N_BLUE, TFT_BLACK);
    spr.drawString(lo, xs + wh + 24, 246, 4);

    // Next three days
    for (int i = 1; i <= 3; i++) {
      int L = (i - 1) * 80;  // column left edge; three 80px columns
      const char *lab = DAYS[w.dwday[i] % 7];
      spr.setTextColor(N_MAGENTA, TFT_BLACK);
      spr.drawString(lab, L + 40 - spr.textWidth(lab, 2) / 2, 270, 2);
      drawIcon(iconFor(w.dcode[i], true), L + 17, 300, 30);
      char r[16];
      snprintf(r, sizeof(r), "%d/%d", (int)lroundf(w.hi[i]), (int)lroundf(w.lo[i]));
      spr.setTextColor(N_CYAN, TFT_BLACK);
      spr.drawString(r, L + 36, 292, 2);
    }
  }

  // WiFi indicator
  if (WiFi.status() != WL_CONNECTED) spr.fillCircle(W - 8, 8, 4, TFT_RED);

  drawDots();
  spr.pushSprite(0, 0);
}

// ---- Live weather scene (tap the clock/weather page) ------------------------------
// Everything is a pure function of time (hash-seeded particles), so there is no animation state.
static uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
static float frand(uint32_t seed) { return (hash32(seed) & 0xFFFF) / 65535.0f; }

static void sceneSun(int cx, int cy, float t, float s) {
  float pulse = 1.0f + 0.06f * sinf(t * 1.6f);
  // Glows use orange/blue tints: very dark yellows and whites quantize to muddy olive in the 8-bit sprite.
  spr.fillCircle(cx, cy, 74 * s * pulse, dimColor(N_ORANGE, 22));
  spr.fillCircle(cx, cy, 58 * s * pulse, dimColor(N_ORANGE, 40));
  spr.fillCircle(cx, cy, 38 * s, N_YELLOW);
  for (int i = 0; i < 12; i++) {  // slowly rotating rays of alternating length
    float a = t * 0.35f + i * PI / 6;
    float r1 = 50 * s * pulse, r2 = (i % 2 ? 66 : 76) * s * pulse;
    spr.drawWideLine(cx + cosf(a) * r1, cy + sinf(a) * r1, cx + cosf(a) * r2, cy + sinf(a) * r2, 4, N_ORANGE, TFT_BLACK);
  }
}

static void sceneMoon(int cx, int cy, float t) {
  uint16_t inner = dimColor(N_BLUE, 40);
  spr.fillCircle(cx, cy, 62 + 2 * sinf(t), dimColor(N_BLUE, 22));
  spr.fillCircle(cx, cy, 48, inner);
  spr.fillCircle(cx, cy, 34, N_WHITE);
  spr.fillCircle(cx + 15, cy - 9, 30, inner);  // crescent bite, matching the glow behind it
}

static void sceneStars(float t, int n) {
  for (int i = 0; i < n; i++) {
    int x = frand(i * 7 + 1) * W, y = frand(i * 7 + 2) * 205;
    float b = 0.5f + 0.5f * sinf(t * (1.0f + 2.0f * frand(i * 7 + 3)) + frand(i * 7 + 4) * 6.28f);
    uint16_t col = dimColor(N_WHITE, 25 + (int)(75 * b));
    if (i % 5 == 0) {
      spr.drawFastHLine(x - 2, y, 5, col);
      spr.drawFastVLine(x, y - 2, 5, col);
    } else {
      spr.fillRect(x, y, 2, 2, col);
    }
  }
}

static void sceneClouds(float t, int count, uint16_t col, int baseY, float speed, float size, float spread = 56) {
  for (int i = 0; i < count; i++) {
    float sz = size * (0.75f + 0.5f * frand(i * 13 + 5));
    float span = W + sz * 1.4f;
    float x = fmodf(frand(i * 13 + 7) * span + (8 + 10 * frand(i * 13 + 6)) * speed * t, span) - sz * 0.7f;
    float y = baseY + (frand(i * 13 + 8) - 0.5f) * spread;
    drawCloud(x, y, sz, col);
  }
}

static void sceneRain(float t, int n, uint16_t col, float speed, int top, int ground) {
  const float span = ground - top;
  for (int i = 0; i < n; i++) {
    float x = frand(i * 5 + 1) * (W + 30);
    float len = 7 + frand(i * 5 + 2) * 8;
    float sp = speed * (0.8f + 0.5f * frand(i * 5 + 4));
    float y = top + fmodf(frand(i * 5 + 3) * span + sp * t, span);
    spr.drawLine(x, y, x - len * 0.25f, y + len, i % 3 ? dimColor(col, 70) : col);
  }
  for (int j = 0; j < 7; j++) {  // ripples where the drops land
    float ph = fmodf(t * 1.1f + frand(j * 3 + 900), 1.0f);
    spr.drawEllipse(frand(j * 3 + 901) * W, ground, 2 + ph * 14, 1 + ph * 4, dimColor(col, (int)(90 * (1 - ph))));
  }
}

static void sceneSnow(float t, int n, int top, int ground) {
  const float span = ground - top;
  for (int i = 0; i < n; i++) {
    int sz = 1 + i % 3;
    float x = frand(i * 9 + 1) * W + sinf(t * (0.6f + frand(i * 9 + 2)) + frand(i * 9 + 3) * 6.28f) * 12;
    float y = top + fmodf(frand(i * 9 + 4) * span + (14 + 10 * frand(i * 9 + 5)) * sz * 0.6f * t, span);
    spr.fillCircle(x, y, sz, sz == 1 ? dimColor(N_WHITE, 70) : N_WHITE);
  }
}

static void sceneFog(float t) {
  for (int i = 0; i < 5; i++) {
    float y = 40 + i * 34 + 6 * sinf(t * 0.5f + i);
    float cx = W / 2 + 34 * sinf(t * 0.3f + i * 1.7f), half = 72 + 24 * sinf(i * 2.1f);
    spr.drawWideLine(cx - half, y, cx + half, y, 14, dimColor(N_BLUE, 22), TFT_BLACK);
    spr.drawWideLine(cx - half * 0.7f, y, cx + half * 0.7f, y, 5, dimColor(N_CYAN, 55), TFT_BLACK);
  }
}

static bool lightningFlash(float t) {
  float ph = fmodf(t, 5.0f);  // two quick flashes near the end of every 5 s cycle
  return (ph > 4.0f && ph < 4.09f) || (ph > 4.2f && ph < 4.27f);
}

static void drawBolt(float t, int topY, int groundY) {
  uint32_t seed = (uint32_t)(t / 5.0f) * 977u + 13;  // a different bolt shape each cycle
  float x = 50 + frand(seed) * 140, y = topY;
  while (y < groundY) {
    float nx = constrain(x + (frand(seed + (uint32_t)y) - 0.5f) * 44, 20.0f, (float)W - 20);
    float ny = y + 26;
    spr.drawWideLine(x, y, nx, ny, 8, dimColor(N_YELLOW, 25), TFT_BLACK);
    spr.drawWideLine(x, y, nx, ny, 3, N_YELLOW, TFT_BLACK);
    x = nx;
    y = ny;
  }
}

static float sceneFreezeT = -1;  // dev tool: >= 0 freezes the animation clock at that time

static void drawScene(uint32_t now) {
  const float t = sceneFreezeT >= 0 ? sceneFreezeT : now / 1000.0f;
  const int GROUND = 214;
  Weather w;
  xSemaphoreTake(wxMu, portMAX_DELAY);
  w = wx;
  xSemaphoreGive(wxMu);

  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(TL_DATUM);
  const bool night = !w.isDay;
  const int c = w.code;

  switch (iconFor(c, w.isDay)) {
    case I_SUN:
      sceneSun(W / 2, 96, t, 1.0f);
      break;
    case I_MOON:
      sceneStars(t, 50);
      sceneMoon(W / 2, 96, t);
      break;
    case I_PARTLY:
      if (night) {
        sceneStars(t, 30);
        sceneMoon(88, 80, t);
      } else {
        sceneSun(96, 92, t, 0.8f);
      }
      sceneClouds(t, 3, N_BLUE, 140, 1.0f, 90, 44);
      break;
    case I_CLOUD:
      sceneClouds(t, 7, N_BLUE, 108, 0.8f, 100, 120);
      break;
    case I_FOG:
      sceneFog(t);
      sceneClouds(t, 2, dimColor(N_BLUE, 55), 60, 0.5f, 90);
      break;
    case I_RAIN: {
      int n = (c >= 51 && c <= 57) ? 40 : (c == 61 || c == 80) ? 70 : (c == 65 || c == 82) ? 140 : 100;
      sceneClouds(t, 4, N_PURPLE, 58, 0.7f, 100);
      sceneRain(t, n, N_CYAN, 150, 80, GROUND);
      break;
    }
    case I_SNOW:
      sceneClouds(t, 4, N_BLUE, 58, 0.5f, 100);
      sceneSnow(t, 70, 80, GROUND);
      break;
    case I_STORM: {
      bool flash = lightningFlash(t);
      if (flash) spr.fillRect(0, 0, W, GROUND, dimColor(N_PURPLE, 18));
      sceneClouds(t, 4, flash ? N_WHITE : N_PURPLE, 58, 0.9f, 105);
      sceneRain(t, 120, N_CYAN, 190, 80, GROUND);
      if (flash) drawBolt(t, 80, GROUND);
      break;
    }
  }

  // Info bar: temperature, high/low and the condition.
  spr.drawFastHLine(8, 224, W - 16, dimColor(N_MAGENTA, 55));
  drawTemp(14, 232, w.temp, N_YELLOW);
  char hi[12], lo[12];
  snprintf(hi, sizeof(hi), "H %d", (int)lroundf(w.hi[0]));
  snprintf(lo, sizeof(lo), "L %d", (int)lroundf(w.lo[0]));
  spr.setTextColor(N_ORANGE, TFT_BLACK);
  spr.drawString(hi, 160, 234, 4);
  spr.setTextColor(N_BLUE, TFT_BLACK);
  spr.drawString(lo, 160, 260, 4);
  const char *txt = wxText(c);
  spr.setTextColor(N_CYAN, TFT_BLACK);
  spr.drawString(txt, (W - spr.textWidth(txt, 4)) / 2, 288, 4);

  drawDots();
  spr.pushSprite(0, 0);
}

// ---- HF bands screen ------------------------------------------------------------
static const uint16_t N_RED = 0xF9E7;

static uint16_t condColor(uint8_t c) { return c == 2 ? N_GREEN : c == 1 ? N_YELLOW : c == 0 ? N_RED : dimColor(N_CYAN, 35); }
static const char *condText(uint8_t c) { return c == 2 ? "GOOD" : c == 1 ? "FAIR" : c == 0 ? "POOR" : "--"; }

static void centerText(const char *s, int cx, int y, int font) {
  spr.drawString(s, cx - spr.textWidth(s, font) / 2, y, font);
}

static void drawBands(uint32_t now) {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(TL_DATUM);

  Bands b;
  bool day = true;
  xSemaphoreTake(wxMu, portMAX_DELAY);
  b = bands;
  if (wx.valid) day = wx.isDay;
  xSemaphoreGive(wxMu);

  spr.setTextColor(N_MAGENTA, TFT_BLACK);
  centerText("HF BANDS", W / 2, 8, 4);

  if (!b.valid) {
    spr.setTextColor(dimColor(N_CYAN, 60), TFT_BLACK);
    centerText(safeMode ? "Safe mode (no data)" : WiFi.status() != WL_CONNECTED ? "No WiFi"
               : bandsFails ? "Retrying band data..." : "Loading band data...", W / 2, 150, 4);
    drawDots();
    spr.pushSprite(0, 0);
    return;
  }

  // Solar indices, colored by how good they are for HF.
  int sfi = atoi(b.sfi), a = atoi(b.aidx), k = atoi(b.kidx);
  uint16_t cS = sfi >= 100 ? N_GREEN : sfi >= 70 ? N_YELLOW : N_RED;
  uint16_t cA = a < 10 ? N_GREEN : a < 30 ? N_YELLOW : N_RED;
  uint16_t cK = k <= 2 ? N_GREEN : k <= 4 ? N_YELLOW : N_RED;
  const char *lab[3] = {"SFI", "A-INDEX", "K-INDEX"};
  const char *val[3] = {b.sfi, b.aidx, b.kidx};
  const uint16_t col[3] = {cS, cA, cK};
  for (int i = 0; i < 3; i++) {
    int cx = 40 + i * 80;
    spr.setTextColor(N_CYAN, TFT_BLACK);
    centerText(lab[i], cx, 40, 2);
    spr.setTextColor(col[i], TFT_BLACK);
    centerText(val[i], cx, 58, 4);
  }
  spr.drawFastHLine(8, 90, W - 16, dimColor(N_MAGENTA, 55));

  // Day / night columns; the one that matches the sun right now is brighter.
  spr.setTextColor(day ? N_CYAN : dimColor(N_CYAN, 40), TFT_BLACK);
  centerText("DAY", 121, 96, 2);
  spr.setTextColor(day ? dimColor(N_CYAN, 40) : N_CYAN, TFT_BLACK);
  centerText("NIGHT", 199, 96, 2);

  for (int g = 0; g < 4; g++) {
    int top = 114 + g * 42;
    char name[12];
    strlcpy(name, BAND_GROUPS[g], sizeof(name));
    for (char *p = name; *p; p++) if (*p == '-') *p = '/';
    spr.setTextColor(N_CYAN, TFT_BLACK);
    spr.drawString(name, 4, top + 10, 2);
    for (int d = 0; d < 2; d++) {
      int x = 84 + d * 78;
      uint8_t c = b.cond[g][d];
      uint16_t cc = condColor(c);
      spr.fillSmoothRoundRect(x, top, 74, 36, 8, dimColor(cc, 16), TFT_BLACK);
      spr.drawSmoothRoundRect(x, top, 8, 6, 74, 36, cc, TFT_BLACK);
      spr.setTextColor(cc, dimColor(cc, 16));
      centerText(condText(c), x + 37, top + 10, 2);
    }
  }

  // Extra readings and when the data was last updated (UTC).
  char hm[8] = "";
  const char *gmt = strstr(b.updated, "GMT");
  if (gmt && gmt - b.updated >= 5) snprintf(hm, sizeof(hm), "%.4sZ", gmt - 5);
  char l1[40], l2[40];
  snprintf(l1, sizeof(l1), "X-ray %s   Spots %s", b.xray, b.sunspots);
  snprintf(l2, sizeof(l2), "Wind %s   upd %s", b.wind, hm);
  spr.setTextColor(dimColor(N_CYAN, 70), TFT_BLACK);
  centerText(l1, W / 2, 284, 2);
  centerText(l2, W / 2, 299, 2);

  if (WiFi.status() != WL_CONNECTED) spr.fillCircle(W - 8, 8, 4, TFT_RED);
  drawDots();
  spr.pushSprite(0, 0);
}

// Page indicator: one dot per screen, current one bright.
static void drawDots() {
  int x0 = W / 2 - (SCREEN_COUNT - 1) * 7;
  for (int i = 0; i < SCREEN_COUNT; i++) {
    bool cur = i == screen;
    spr.fillCircle(x0 + i * 14, 316, cur ? 3 : 2, cur ? N_CYAN : dimColor(N_CYAN, 30));
  }
}

// ---- Auto brightness ----------------------------------------------------------
// The photoresistor reads LOWER when it is brighter, and it also sees the screen's own backlight.
// ldrRatio() is (reading at this backlight duty) / (reading with the backlight off), measured on
// this board in a lit, dim and dark room; dividing by it recovers the room's own light level.
static float ldrRatio(uint8_t bl) {
  static const uint8_t X[] = {0, 8, 40, 128, 255};
  static const float Y[] = {1.00f, 0.90f, 0.67f, 0.41f, 0.31f};
  for (int i = 1; i < 5; i++) {
    if (bl <= X[i]) return Y[i - 1] + (Y[i] - Y[i - 1]) * (float)(bl - X[i - 1]) / (X[i] - X[i - 1]);
  }
  return Y[4];
}

static float ambLog = 6.9f;  // ln(estimated room reading with backlight off); ~284 lit room .. ~1760 dark

static void updateBacklight(uint32_t now) {
  static uint32_t lastSample = 0;
  static float cur = 255;
  if (since(now, lastSample) >= 250) {  // sample the room a few times a second, averaged in the log domain
    lastSample = now;
    float r0 = max(1.0f, readLdr() / ldrRatio(backlight));
    ambLog = lerpf(ambLog, logf(r0), 0.2f);
  }
  float target;
  if (manualBright >= 0) {
    target = manualBright;
  } else {
    // 0 = bright room (lights on / daylight) .. 1 = dark room
    float t = constrain((ambLog - logf(500.0f)) / (logf(1500.0f) - logf(500.0f)), 0.0f, 1.0f);
    target = BRIGHT_DAY + (BRIGHT_SLEEP - BRIGHT_DAY) * t;
    if (since(now, lastInteraction) < 20000) target = max(target, BRIGHT_WAKE * min(1.0f, t * 1.25f));  // touched in the dark
  }
  cur = lerpf(cur, target, 0.06f);  // ease so changes aren't jarring
  uint8_t duty = (uint8_t)(cur + 0.5f);
  if (duty != backlight) setBacklight(duty);
}

// A tap on a non-face page. Swipes handle navigation; each page decides what a tap does.
static void pageTap() {
  if (screen == SCREEN_CLOCK) {  // clock/weather: toggle the live weather scene
    xSemaphoreTake(wxMu, portMAX_DELAY);
    bool have = wx.valid;
    xSemaphoreGive(wxMu);
    if (have) {
      sceneActive = !sceneActive;
      screenJustChanged = true;
    }
  }
  // SCREEN_BANDS: nothing yet
}

// ---- Touch ------------------------------------------------------------------
static void handleTouch(uint32_t now) {
  static uint32_t lastSeen = 0, downAt = 0, lastLog = 0;
  static bool down = false, flipDone = false, wakeOnly = false, setupDone = false;
  static int downX = 0, downY = 0, lastX = 0, lastY = 0, maxMove = 0;

  int x = lastX, y = lastY;  // keep the last position through the debounce window, not (0,0)
  bool simActive = false;
  if (sim.active) {
    uint32_t el = since(now, sim.start);
    if (el > sim.dur) {
      sim.active = false;
    } else {
      float t = sim.dur > 40 ? min(1.0f, (float)el / (sim.dur - 40)) : 1.0f;  // reach the end point before release
      x = sim.x0 + (int)((sim.x1 - sim.x0) * t);
      y = sim.y0 + (int)((sim.y1 - sim.y0) * t);
      lastSeen = now;
      lastX = x;
      lastY = y;
      simActive = true;
    }
  }
  if (!simActive && ts.touched()) {
    TS_Point p = ts.getPoint();
    int xl = constrain(map(p.x, RAW_X_MIN, RAW_X_MAX, 0, 320), 0, 319);  // landscape frame
    int yl = constrain(map(p.y, RAW_Y_MIN, RAW_Y_MAX, 0, 240), 0, 239);
    if (rotation == 0) { x = 239 - yl; y = xl; }
    else               { x = yl;       y = 319 - xl; }
    lastSeen = now;
    lastX = x;
    lastY = y;
    if (now - lastLog > 250) {
      lastLog = now;
      Serial.printf("touch raw=(%d,%d) screen=(%d,%d)\n", p.x, p.y, x, y);
    }
  }
  bool touching = (now - lastSeen) < 90 && lastSeen != 0;  // debounce flicker
  bool onFace = screen == SCREEN_FACE && !portalActive;

  if (touching && !down) {  // press
    down = true;
    flipDone = false;
    setupDone = false;
    wakeOnly = backlight < 30;  // a touch on a dimmed screen only wakes it; it must not also change page
    downAt = now;
    downX = x;
    downY = y;
    maxMove = 0;
    lastInteraction = now;
    if (onFace) setMood(f.mood == SLEEPY ? SURPRISED : ATTENTION, f.mood == SLEEPY ? 1100 : 0);
  } else if (touching && down) {  // held / dragging
    lastInteraction = now;
    maxMove = max(maxMove, abs(x - downX) + abs(y - downY));
    if (onFace) {
      f.gxT = (x - W / 2) / (float)(W / 2) * 14;
      f.gyT = (y - H / 2) / (float)(H / 2) * 12;
      if (f.mood == ATTENTION && now - downAt > 1200) setMood(LAUGH);
    }
    // Hold ANY corner for 2 s to flip the screen 180 degrees.
    bool corner = (downX < 56 || downX > W - 56) && (downY < 44 || downY > H - 44);
    if (!flipDone && corner && maxMove < 30 && now - downAt > 2000) {
      flipDone = true;
      flipScreen();
      if (onFace) setMood(SURPRISED, 800);
    }
    // Hold the middle of the HF bands page for 4 s to open the WiFi setup hotspot.
    if (!setupDone && !portalActive && screen == SCREEN_BANDS && !corner && maxMove < 30 && now - downAt > 4000) {
      setupDone = true;
      startPortal(true);
    }
  } else if (!touching && down) {  // release
    down = false;
    f.gxT = f.gyT = 0;
    int dx = lastX - downX, dy = lastY - downY;
    bool swipe = !flipDone && now - downAt < 900 && abs(dx) > 70 && abs(dy) < 80;
    bool tap = !flipDone && now - downAt < 600 && maxMove < 40;
    if (portalActive) {
      if (tap) portalTap(lastX, lastY);
    } else if (flipDone) {
      if (onFace) setMood(IDLE);
    } else if (wakeOnly) {
      if (onFace && f.mood == ATTENTION) setMood(IDLE);
    } else if (swipe) {  // swipe left = next page, swipe right = previous (wraps around)
      setScreen((Screen)(((int)screen + (dx < 0 ? 1 : SCREEN_COUNT - 1)) % SCREEN_COUNT));
    } else if (!onFace && tap) {
      pageTap();
    } else if (onFace) {
      if (f.mood == ATTENTION && tap) setMood(HAPPY, 1800);
      else if (f.mood == LAUGH) setMood(HAPPY, 800);
      else if (f.mood == ATTENTION) setMood(IDLE);
    }
  }

  if (!down && onFace) {
    if (f.mood == IDLE && now > f.nextGaze) {
      f.gxT = random(-14, 15);
      f.gyT = random(-10, 11);
      f.nextGaze = now + 1200 + random(0, 3000);
    }
    if (f.mood != SLEEPY && since(now, lastInteraction) > 45000) {
      f.gxT = f.gyT = 0;
      setMood(SLEEPY);
    }
  }
}

// ---- WiFi setup hotspot ------------------------------------------------------
// If the saved network can't be joined within 40 s of boot (moved house, new router, wrong password), or on a
// 4 s hold on the HF bands page, the board opens its own WPA2 hotspot "DeskBuddy-XXXX" (password shown on
// screen) with a captive setup page. Credentials are saved in flash (prefs) and override the ones compiled in
// from secrets.h. Leaving the setup (saving, or tapping the on-screen button) restarts the board.
#ifndef PORTAL_TEST_LAN
#define PORTAL_TEST_LAN 0  // test builds only: serve the setup page to LAN clients as well
#endif
static String curSsid, curPass;  // credentials in use: saved in flash, else the ones from secrets.h
static String apSsid, apPass, netOptions;
static bool portalManual = false, portalSaved = false;
static uint32_t portalStartedAt = 0, portalRestartAt = 0, portalTryAt = 0, portalTryUntil = 0;

static bool hasCreds() { return curSsid.length() > 0 && curSsid != "YOUR_WIFI_NAME"; }

static void loadCreds() {
  curSsid = prefs.getString("ssid", WIFI_SSID);
  curPass = prefs.getString("pass", WIFI_PASS);
}

// Only clients that joined the hotspot may use the setup page (not other devices on the home network).
static bool fromAp() {
#if PORTAL_TEST_LAN
  return portalActive;
#else
  return portalActive && web.client().localIP() == WiFi.softAPIP();
#endif
}

static String htmlEsc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  o += "&amp;"; break;
      case '<':  o += "&lt;"; break;
      case '>':  o += "&gt;"; break;
      case '"':  o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default:   o += c;
    }
  }
  return o;
}

static void scanNetworks() {
  int n = WiFi.scanNetworks();
  netOptions = "";
  String seen[20];
  int cnt = 0;
  for (int i = 0; i < n && cnt < 20; i++) {
    String s = WiFi.SSID(i);
    bool dup = s.length() == 0;
    for (int j = 0; j < cnt && !dup; j++) dup = seen[j] == s;
    if (dup) continue;
    seen[cnt++] = s;
    netOptions += "<option value=\"" + htmlEsc(s) + "\">" + htmlEsc(s) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
}

static String portalPage(const char *msg = "") {
  String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>DeskBuddy WiFi</title>"
               "<style>body{font-family:sans-serif;margin:0;padding:20px;background:#111;color:#eee}"
               "h2{color:#0ff}select,input{width:100%;box-sizing:border-box;padding:12px;margin:6px 0 14px;"
               "font-size:16px;border-radius:8px;border:0}button{width:100%;padding:14px;font-size:17px;border:0;"
               "border-radius:8px;background:#0ff;color:#000;font-weight:bold}a{color:#0ff}small{color:#999}"
               "</style><h2>DeskBuddy WiFi setup</h2>");
  if (*msg) {
    h += "<p>";
    h += msg;
    h += "</p>";
  }
  h += F("<form method=post action=/save><label>Network</label><select name=sel>");
  h += netOptions;
  h += F("<option value=''>Other (type below)</option></select>"
         "<label>Or type the network name</label><input name=ssid maxlength=32 autocapitalize=off autocorrect=off>"
         "<label>Password</label><input name=pass type=password maxlength=63>"
         "<button>Save and connect</button></form>"
         "<p><a href=/rescan>Rescan networks</a></p>"
         "<small>2.4 GHz networks only. Leave the password empty for an open network.</small>");
  return h;
}

static void portalRoutes() {
  web.on("/rescan", []() {
    if (!fromAp()) { web.send(404, "text/plain", "not found\n"); return; }
    scanNetworks();
    web.sendHeader("Location", "/");
    web.send(303);
  });
  web.on("/save", HTTP_POST, []() {
    if (!fromAp()) { web.send(404, "text/plain", "not found\n"); return; }
    String ssid = web.arg("ssid");
    ssid.trim();
    if (ssid.length() == 0) ssid = web.arg("sel");
    String pass = web.arg("pass");
    if (ssid.length() == 0 || ssid.length() > 32 || (pass.length() > 0 && (pass.length() < 8 || pass.length() > 63))) {
      web.send(400, "text/html", portalPage("Pick a network, and use a password of 8 to 63 characters."));
      return;
    }
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    web.send(200, "text/html",
             "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
             "<body style='font-family:sans-serif;padding:20px;background:#111;color:#eee'><h2 style='color:#0ff'>Saved</h2>"
             "<p>DeskBuddy is restarting and will join <b>" + htmlEsc(ssid) + "</b>. You can close this page and "
             "reconnect your phone to your normal WiFi.</p><p>If it can't connect, this setup hotspot comes back.</p>");
    Serial.println("WiFi setup: saved new network, restarting");
    portalSaved = true;  // restart once the response has gone out
    portalRestartAt = millis() + 1500;
  });
  // Phones probe well-known URLs to detect a captive portal; redirecting everything makes them open the page.
  web.onNotFound([]() {
    if (fromAp()) {
      web.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
      web.send(302);
    } else {
      web.send(404, "text/plain", "not found\n");
    }
  });
}

static void startWeb();

static void startPortal(bool manual) {
  if (portalActive) return;
  portalManual = manual;
  portalSaved = false;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char b[24];
  snprintf(b, sizeof(b), "DeskBuddy-%02X%02X", mac[4], mac[5]);
  apSsid = b;
  snprintf(b, sizeof(b), "%08lu", (unsigned long)(esp_random() % 100000000UL));
  apPass = b;
  Serial.printf("WiFi setup: opening hotspot '%s'%s\n", apSsid.c_str(), manual ? " (manual)" : "");
  if (!manual) {  // nothing to keep connected: drop the failing attempt before scanning
    WiFi.disconnect(true);
    delay(100);
  }
  WiFi.mode(WIFI_STA);
  scanNetworks();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str(), apPass.c_str(), 1, 0, 2);
  delay(100);
  dns.start(53, "*", WiFi.softAPIP());
  startWeb();
  portalStartedAt = portalTryAt = millis();
  portalTryUntil = 0;
  portalActive = true;
  setBacklight(255);
}

static void servicePortal(uint32_t now) {
  dns.processNextRequest();
  if (portalSaved) {
    if ((int32_t)(now - portalRestartAt) >= 0) ESP.restart();
    return;
  }
  bool inUse = WiFi.softAPgetStationNum() > 0;
  if (portalManual) {  // opened by hand: don't leave the hotspot up forever
    if (!inUse && since(now, portalStartedAt) > 300000) ESP.restart();
    return;
  }
  if (!hasCreds()) return;
  // Opened because the saved network failed: it may just be down (power cut, router still booting), so keep
  // trying it quietly, but never while someone is using the hotspot (a connect attempt hops channels).
  bool trying = portalTryUntil != 0;
  if (inUse) {
    if (trying) { WiFi.disconnect(false); portalTryUntil = 0; }
    portalTryAt = now;
  } else if (trying) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi setup: saved network is back, restarting");
      ESP.restart();
    } else if ((int32_t)(now - portalTryUntil) >= 0) {
      WiFi.disconnect(false);
      portalTryUntil = 0;
      portalTryAt = now;
    }
  } else if (since(now, portalTryAt) > 60000) {
    WiFi.begin(curSsid.c_str(), curPass.c_str());
    portalTryUntil = now + 15000;
  }
}

static bool portalHasButton() { return portalManual || hasCreds(); }

static void portalTap(int x, int y) {
  if (portalHasButton() && !portalSaved && x > 20 && x < 220 && y > 262 && y < 308) ESP.restart();
}

static void drawPortal() {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(N_MAGENTA, TFT_BLACK);
  centerText("WIFI SETUP", W / 2, 8, 4);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  centerText("1. Join this WiFi on your phone:", W / 2, 50, 2);
  spr.setTextColor(N_CYAN, TFT_BLACK);
  centerText(apSsid.c_str(), W / 2, 70, 4);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  centerText("Password:", W / 2, 106, 2);
  spr.setTextColor(N_YELLOW, TFT_BLACK);
  centerText(apPass.c_str(), W / 2, 124, 4);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  centerText("2. Choose your home network", W / 2, 164, 2);
  centerText("on the page that opens.", W / 2, 182, 2);
  centerText("No page? Open this address:", W / 2, 206, 2);
  spr.setTextColor(N_CYAN, TFT_BLACK);
  centerText("192.168.4.1", W / 2, 224, 4);
  if (portalSaved) {
    spr.setTextColor(N_GREEN, TFT_BLACK);
    centerText("Saved! Restarting...", W / 2, 276, 4);
  } else if (portalHasButton()) {
    spr.drawRoundRect(20, 262, 200, 46, 10, N_MAGENTA);
    spr.setTextColor(N_MAGENTA, TFT_BLACK);
    centerText(portalManual ? "Cancel" : "Retry saved WiFi", W / 2, 275, 2);
    if (!portalManual) {
      spr.setTextColor(TFT_RED, TFT_BLACK);
      centerText("Can't reach saved WiFi", W / 2, 246, 2);
    }
  }
  spr.pushSprite(0, 0);
}

// ---- Web page ---------------------------------------------------------------
static void startWeb() {
  static bool running = false;
  if (running) return;  // called both when WiFi comes up and when the setup hotspot opens
  running = true;
  portalRoutes();
  web.on("/", []() {
    if (fromAp()) { web.send(200, "text/html", portalPage()); return; }
    web.send(200, "text/html",
             "<meta name=viewport content='width=device-width,initial-scale=1'>"
             "<body style='font-family:sans-serif;padding:24px;line-height:2'><h2>DeskBuddy</h2>"
             "<a href='/screen?show=face'>Show face</a><br>"
             "<a href='/screen?show=clock'>Show clock &amp; weather</a><br>"
             "<a href='/screen?show=bands'>Show HF band conditions</a><br>"
             "<a href='/flip'>Flip screen 180&deg;</a></body>");
  });
  web.on("/flip", []() {
    flipScreen();
    web.sendHeader("Location", "/");
    web.send(303);
  });
  web.on("/screen", []() {
    String s = web.arg("show");
    setScreen(s == "clock" ? SCREEN_CLOCK : s == "bands" ? SCREEN_BANDS : SCREEN_FACE);
    web.sendHeader("Location", "/");
    web.send(303);
  });
#if DEV_TOOLS
  auto ok = [](const String &msg = "ok") { web.send(200, "text/plain", msg + "\n"); };

  // Current screen contents as a 24-bit BMP (the sprite is what gets pushed to the panel).
  web.on("/screenshot.bmp", [=]() {
    const int rowBytes = W * 3;
    const uint32_t imgSize = rowBytes * H, fileSize = 54 + imgSize, off = 54, dib = 40;
    const int32_t bw = W, bh = H;
    const uint16_t planes = 1, bpp = 24;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    memcpy(hdr + 2, &fileSize, 4);
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &bw, 4);
    memcpy(hdr + 22, &bh, 4);
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &imgSize, 4);
    uint8_t *buf = (uint8_t *)malloc(rowBytes * 4);
    if (!buf) { web.send(500, "text/plain", "no memory\n"); return; }
    web.setContentLength(fileSize);
    web.send(200, "image/bmp", "");
    web.sendContent((const char *)hdr, 54);
    for (int y = H - 1; y >= 0; y -= 4) {  // BMP rows run bottom-up; 4 rows per write
      int n = 0;
      for (int k = 0; k < 4 && y - k >= 0; k++) {
        for (int x = 0; x < W; x++) {
          uint16_t c = spr.readPixel(x, y - k);
          buf[n++] = (c & 31) * 255 / 31;
          buf[n++] = ((c >> 5) & 63) * 255 / 63;
          buf[n++] = ((c >> 11) & 31) * 255 / 31;
        }
      }
      web.sendContent((const char *)buf, n);
    }
    free(buf);
  });

  // /mood?m=idle|attention|happy|laugh|surprised|sleepy  (held for a minute so it can be photographed)
  web.on("/mood", [=]() {
    String m = web.arg("m");
    Mood mm = m == "attention" ? ATTENTION : m == "happy" ? HAPPY : m == "laugh" ? LAUGH
            : m == "surprised" ? SURPRISED : m == "sleepy" ? SLEEPY : IDLE;
    setScreen(SCREEN_FACE);
    setMood(mm, mm == IDLE || mm == SLEEPY ? 0 : 60000);
    lastInteraction = millis();
    ok(m);
  });

  // /touch?x=&y=[&x2=&y2=][&ms=]  tap (default) or drag/swipe in screen coordinates (240x320)
  web.on("/touch", [=]() {
    sim.x0 = web.arg("x").toInt();
    sim.y0 = web.arg("y").toInt();
    sim.x1 = web.hasArg("x2") ? web.arg("x2").toInt() : sim.x0;
    sim.y1 = web.hasArg("y2") ? web.arg("y2").toInt() : sim.y0;
    sim.dur = web.hasArg("ms") ? web.arg("ms").toInt() : 120;
    sim.start = millis();
    sim.active = true;
    ok();
  });

  // /settime?h=13&m=45  sets today's local time (NTP re-syncs within an hour)
  web.on("/settime", [=]() {
    time_t n = time(nullptr);
    struct tm t;
    localtime_r(&n, &t);
    if (web.hasArg("h")) t.tm_hour = web.arg("h").toInt();
    if (web.hasArg("m")) t.tm_min = web.arg("m").toInt();
    t.tm_sec = 0;
    struct timeval tv = {mktime(&t), 0};
    settimeofday(&tv, nullptr);
    ok();
  });

  // /weather?code=63&temp=48&day=1&d1=0&d2=3&d3=95  injects fake weather (refresh replaces it)
  web.on("/weather", [=]() {
    Weather w;
    xSemaphoreTake(wxMu, portMAX_DELAY);
    w = wx;
    xSemaphoreGive(wxMu);
    time_t n = time(nullptr);
    struct tm t;
    localtime_r(&n, &t);
    w.valid = true;
    w.temp = web.hasArg("temp") ? web.arg("temp").toFloat() : 48;
    w.code = web.arg("code").toInt();
    w.isDay = web.arg("day") != "0";
    for (int i = 0; i < 4; i++) {
      w.hi[i] = w.temp + 8 - i;
      w.lo[i] = w.temp - 8 + i;
      w.dwday[i] = (t.tm_wday + i) % 7;
      if (i) w.dcode[i] = web.arg(String("d") + i).toInt();
    }
    w.dcode[0] = w.code;
    xSemaphoreTake(wxMu, portMAX_DELAY);
    wx = w;
    xSemaphoreGive(wxMu);
    ok();
  });

  web.on("/refresh", [=]() {
    refreshNow = true;
    ok("refreshing");
  });

  // /scene?on=1|0  opens/closes the live weather scene on the clock page
  web.on("/scene", [=]() {
    setScreen(SCREEN_CLOCK);
    sceneActive = web.arg("on") != "0";
    sceneFreezeT = web.hasArg("t") ? web.arg("t").toFloat() : -1;  // ?t=4.05 pins the scene to that moment
    screenJustChanged = true;
    ok(sceneActive ? "scene" : "clock");
  });

  web.on("/status", [=]() {
    char b[220];
    snprintf(b, sizeof(b), "screen=%d mood=%d rotation=%d heap=%u maxblock=%u rssi=%d uptime=%lus safe=%d timeValid=%d ldr=%d bl=%d amb=%d manual=%d scene=%d",
             (int)screen, (int)f.mood, (int)rotation, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.RSSI(),
             millis() / 1000, (int)safeMode, (int)timeValid(), readLdr(), (int)backlight, (int)expf(ambLog),
             manualBright, (int)sceneActive);
    ok(b);
  });

  // /bright?v=0..255 sets the backlight; /ldr returns 8 averaged light readings over ~0.4 s
  // /bright?v=0..255 holds a manual brightness; /bright?auto=1 returns to sun-based auto
  web.on("/bright", [=]() {
    if (web.hasArg("auto")) manualBright = -1;
    else manualBright = constrain(web.arg("v").toInt(), 0, 255);
    ok(manualBright < 0 ? String("auto") : String(manualBright));
  });
  web.on("/ldr", [=]() {
    String s;
    for (int i = 0; i < 8; i++) {
      s += String(readLdr()) + (i < 7 ? "," : "");
      delay(50);
    }
    ok(s);
  });
#endif
  web.begin();
}

// ---- WiFi + OTA -------------------------------------------------------------
static const char *wifiReason(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:   return "SSID not found";
    case WL_CONNECT_FAILED:  return "connect failed";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED:    return "disconnected / auth failed";
    case WL_IDLE_STATUS:     return "idle";
    default:                 return "unknown";
  }
}

static void startOta() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Updating...", W / 2, 120, 4);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    tft.fillRect(20, 160, 200 * done / total, 16, TFT_GREEN);
    tft.drawRect(20, 160, 200, 16, TFT_WHITE);
  });
  ArduinoOTA.onError([](ota_error_t) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("Update failed", W / 2, 120, 4);
  });
  ArduinoOTA.begin();
}

static void beginWifi() {
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(curSsid.c_str(), curPass.c_str());
}

static void serviceWifi(uint32_t now) {
  static bool wasConnected = false, started = false;
  static uint32_t lastTry = 0;
  wl_status_t st = WiFi.status();
  bool connected = st == WL_CONNECTED;

  if (connected && !wasConnected) {
    if (!started) {  // start OTA (and its mDNS name) only once the network is up
      startOta();
      startWeb();
      started = true;
    }
    configTzTime(TZ_POSIX, "pool.ntp.org", "time.nist.gov");
    Serial.printf("WiFi up: %s.local  %s\n", OTA_HOSTNAME, WiFi.localIP().toString().c_str());
  }
  wasConnected = connected;

  if (portalActive) return;  // the setup hotspot manages WiFi itself
  if (!connected && now - lastTry > 15000) {
    lastTry = now;
    Serial.printf("WiFi: %s (%d), retrying\n", wifiReason(st), (int)st);
    beginWifi();
  }
  // Never got online since power-up: open the setup hotspot so the network can be changed from a phone.
  if (!started && !connected && now > 40000) startPortal(false);
}

// ---- Arduino entry points ----------------------------------------------------
void setup() {
  Serial.begin(115200);
  esp_reset_reason_t rr = esp_reset_reason();
  safeMode = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT;
  Serial.printf("reset reason %d%s\n", (int)rr, safeMode ? " -> SAFE MODE (data fetch disabled)" : "");
  disableCore0WDT();  // a slow TLS handshake on core 0 must not trip the idle-task watchdog
  wxMu = xSemaphoreCreateMutex();
  prefs.begin("deskbuddy", false);
  rotation = prefs.getUChar("rot", 0) == 2 ? 2 : 0;

  tft.init();
  ledcSetup(BL_CHANNEL, 5000, 8);  // 8-bit PWM backlight
  ledcAttachPin(TFT_BL, BL_CHANNEL);
  setBacklight(255);
  analogReadResolution(12);
  analogSetPinAttenuation(LDR_PIN, ADC_11db);
  tft.invertDisplay(true);    // this CYD panel shows inverted colors without it
  tft.setRotation(rotation);  // portrait, 240x320
  tft.fillScreen(TFT_BLACK);
  BLUSH = tft.color565(255, 105, 140);

  spr.setColorDepth(8);  // 77 KB full-screen buffer
  if (!spr.createSprite(W, H)) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("Out of memory (sprite)", W / 2, 150, 4);
    while (true) delay(1000);
  }

  touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  loadCreds();
  if (hasCreds()) beginWifi();
  else startPortal(false);  // nothing saved yet: go straight to the setup hotspot
  setMood(IDLE);
  lastInteraction = millis();
  // Priority 0 (same as idle) so the fetcher time-slices with the idle task instead of starving it.
  xTaskCreatePinnedToCore(dataTask, "data", 12288, nullptr, 0, nullptr, 0);
}

void loop() {
  uint32_t now = millis();
  serviceWifi(now);
  ArduinoOTA.handle();
  web.handleClient();
  now = millis();  // handlers may have taken a while; keep `now` current for the checks below
  if (portalActive) servicePortal(now);

  static uint32_t lastTick = 0, lastClock = 0;
  if (now - lastTick < 33) return;  // ~30 fps tick
  lastTick = now;

  handleTouch(now);
  updateFace(now);
  if (portalActive) {
    if (backlight != 255) setBacklight(255);  // the hotspot password must stay readable in a dark room
    if (screenJustChanged || now - lastClock > 200) {
      lastClock = now;
      drawPortal();
    }
    screenJustChanged = false;
    return;
  }
  updateBacklight(now);

  // After a minute asleep, show the clock as an ambient display.
  if (screen == SCREEN_FACE && f.mood == SLEEPY && since(now, f.moodSince) > 60000 && timeValid()) {
    setScreen(SCREEN_CLOCK);
  }

  if (screen == SCREEN_FACE) {
    drawFace(now);
  } else if (screen == SCREEN_CLOCK && sceneActive) {
    drawScene(now);  // animated: redraw every tick
  } else if (screenJustChanged || now - lastClock > 200) {
    lastClock = now;
    if (screen == SCREEN_CLOCK) drawClock(now);
    else drawBands(now);
  }
  screenJustChanged = false;
}
