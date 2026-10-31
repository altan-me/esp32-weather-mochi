/*
 *  MOCHI SKY - an 8-bit weather display for ESP32 + ST7789 240x240
 *
 *  Wiring (same as clawd-mochi)
 *    SDA / MOSI -> GPIO 10
 *    SCL / SCK  -> GPIO 8
 *    RES        -> GPIO 2
 *    DC         -> GPIO 1
 *    CS         -> GPIO 4
 *    BL         -> GPIO 3
 *    VCC        -> 3.3V only
 *    GND        -> GND
 *
 *  Libraries:
 *    Adafruit GFX Library
 *    Adafruit ST7735 and ST7789 Library
 *    ArduinoJson 7.x
 *
 *  Weather: Open-Meteo (no account, no API key).
 *  Edit the config below, upload, done.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <math.h>

#if ARDUINOJSON_VERSION_MAJOR < 7
#error "This sketch needs ArduinoJson 7.x (Library Manager -> ArduinoJson)"
#endif

// ─────────────────────────────────────────────────────────────
//  CONFIG - edit these
// ─────────────────────────────────────────────────────────────
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Right-click your spot in Google Maps, or use latlong.net
#define LATITUDE      51.5074
#define LONGITUDE     -0.1278
#define FAHRENHEIT 0        // 1 = °F, 0 = °C

#define REFRESH_MINUTES 15
#define PAGE_SECONDS 10     // how long each of the two big pages stays up

// ─────────────────────────────────────────────────────────────
//  PINS
// ─────────────────────────────────────────────────────────────
#define TFT_SCK 8
#define TFT_MOSI 10
#define TFT_CS 4
#define TFT_DC 1
#define TFT_RST 2
#define TFT_BL 3

#define SCR_W 240
#define SCR_H 240

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// Declared above the first function so the Arduino auto-prototypes can see them.
enum Icon : uint8_t
{
  I_SUN,
  I_PARTLY,
  I_CLOUD,
  I_FOG,
  I_DRIZZLE,
  I_RAIN,
  I_SNOW,
  I_STORM
};

// ─────────────────────────────────────────────────────────────
//  COLORS - light pink theme
// ─────────────────────────────────────────────────────────────
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t C_BG = rgb565(26, 28, 44);          // dark navy
constexpr uint16_t C_PANEL = rgb565(35, 37, 58);
constexpr uint16_t C_PANEL_HI = rgb565(58, 62, 88);
constexpr uint16_t C_INK = rgb565(10, 10, 20);         // outlines / shadows
constexpr uint16_t C_CREAM = rgb565(255, 255, 255);    // clouds
constexpr uint16_t C_SHADE = rgb565(216, 167, 192);    // muted pink text
constexpr uint16_t C_CLOUD_SHADE = rgb565(201, 143, 166); // cloud base / fog
constexpr uint16_t C_SUN = rgb565(255, 205, 117);
constexpr uint16_t C_SUNDK = rgb565(239, 125, 87);
constexpr uint16_t C_MOON = rgb565(244, 232, 193);
constexpr uint16_t C_RAIN = rgb565(65, 166, 246);
constexpr uint16_t C_ICE = rgb565(115, 239, 247);
constexpr uint16_t C_PINK = rgb565(255, 143, 163);     // blush / divider
constexpr uint16_t C_GREEN = rgb565(124, 227, 139);
constexpr uint16_t C_BERRY = rgb565(255, 158, 199);    // primary text
constexpr uint16_t C_FROST = rgb565(255, 209, 227);    // condition text
constexpr uint16_t C_TEAL = rgb565(155, 212, 245);     // low temp text
constexpr uint16_t C_RED = rgb565(255, 138, 138);      // offline / error
constexpr uint16_t C_ORANGE = rgb565(255, 180, 92);    // updating / stale

// ─────────────────────────────────────────────────────────────
//  WEATHER STATE
// ─────────────────────────────────────────────────────────────
struct Weather
{
  int temp = 0;
  int feels = 0;
  int hi = 0;
  int lo = 0;
  int rain = -1;
  int code = 0;
  int tomorrowCode = 0;
  uint8_t icon = I_CLOUD;
  uint8_t tomorrowIcon = I_CLOUD;
  bool day = true;
  bool valid = false;
} wx;

enum Screen : uint8_t
{
  SCREEN_BOOT,
  SCREEN_WEATHER,
  SCREEN_ERROR
};
Screen screen = SCREEN_BOOT;

bool refreshing = false;
uint32_t lastOk = 0;
uint32_t lastFail = 0;
uint32_t lastTry = 0;
uint32_t lastStatusMs = 0;
int lastWifiStatus = WL_IDLE_STATUS;
uint8_t lastWifiReason = 0;
bool pageTomorrow = false;
uint32_t pageStart = 0;

// ─────────────────────────────────────────────────────────────
//  SPRITES - tiny hand-drawn pixel art
// ─────────────────────────────────────────────────────────────
static const char *const SUN_BIG[] = {
    "......OOOO......",
    "....OOYYYYOO....",
    "...OYYYYYYYYO...",
    "..OYYYYYYYYYYO..",
    ".OYYYYYYYYYYYYO.",
    ".OYYYYYYYYYYYYO.",
    "OYYYYYYYYYYYYYYO",
    "OYYYYYYYYYYYYYYO",
    "OYYYYYYYYYYYYYYO",
    "OYYYYYYYYYYYYYYO",
    ".OYYYYYYYYYYYYO.",
    ".OYYYYYYYYYYYYO.",
    "..OYYYYYYYYYYO..",
    "...OYYYYYYYYO...",
    "....OOYYYYOO....",
    "......OOOO......",
};

static const char *const MOON_BIG[] = {
    "......OOOO......",
    "....OOMMMMOO....",
    "...OMMMMMMMMO...",
    "..OMOOMMMMMMMO..",
    ".OMMMMMMMMMMMMO.",
    ".OMMMMMMMMMOMMO.",
    "OMMMMMMMMMMMMMMO",
    "OMMMMMMMMMMMMMMO",
    "OMMMMMMMMMOMMMMO",
    "OMMMMMMMMMMMMMMO",
    ".OMMMMMMMMMMMMO.",
    ".OMMMMMMMMMMMMO.",
    "..OMMMMMMMMMMO..",
    "...OMMMMMOMMMO..",
    "....OOMMMMOO....",
    "......OOOO......",
};

static const char *const SUN_SMALL[] = {
    "....OYYO....",
    "Y...OYYO...Y",
    ".Y..OYYO..Y.",
    "..OYYYYYYO..",
    "..OYYYYYYO..",
    "OYYYYYYYYYYO",
    "OYYYYYYYYYYO",
    "..OYYYYYYO..",
    "..OYYYYYYO..",
    ".Y..OYYO..Y.",
    "Y...OYYO...Y",
    "....OYYO....",
};

static const char *const CLOUD[] = {
    "......................",
    ".......KKKKKKKK.......",
    ".....KKWWWWWWWWKK.....",
    "....KWWWWWWWWWWWWK....",
    "...KWWWWWWWWWWWWWWK...",
    "..KWWWWWWWWWWWWWWWWK..",
    ".KWWWWWWWWWWWWWWWWWWK.",
    "KWWWWWWWWWWWWWWWWWWWWK",
    "KWWWWWWWWWWWWWWWWWWWWK",
    "KWWWWWWWWWWWWWWWWWWWWK",
    "KWWWWWWWWWWWWWWWWWWWWK",
    ".KBBBBBBBBBBBBBBBBBBK.",
    "..KKKKKKKKKKKKKKKKKK..",
    "......................",
};

static const char *const BOLT[] = {
    "..KK.",
    ".KYK.",
    ".KYK.",
    "KYYK.",
    "KYYYK",
    "..KYK",
    "..KYK",
    ".KYK.",
    ".KK..",
};

uint16_t spriteColor(char c)
{
  switch (c)
  {
  case 'K':
    return C_INK;
  case 'W':
    return C_CREAM;
  case 'B':
    return C_CLOUD_SHADE;
  case 'Y':
    return C_SUN;
  case 'O':
    return C_SUNDK;
  case 'M':
    return C_MOON;
  case 'R':
    return C_RAIN;
  case 'L':
    return C_ICE;
  case 'P':
    return C_PINK;
  default:
    return 0;
  }
}

void drawSprite(Adafruit_GFX &g, const char *const *art, uint8_t w, uint8_t h,
                int16_t x, int16_t y, uint8_t s)
{
  for (uint8_t r = 0; r < h; r++)
  {
    const char *row = art[r];
    for (uint8_t c = 0; c < w; c++)
    {
      char ch = row[c];
      if (ch == 0 || ch == '.')
        continue;
      uint16_t col = spriteColor(ch);
      if (col)
        g.fillRect(x + c * s, y + r * s, s, s, col);
    }
  }
}

// Eyes, blush and a little smile. s = pixel size, eyeY = top of the 2x2 eye.
void drawFace(Adafruit_GFX &g, int16_t x, int16_t y, uint8_t s,
              uint8_t e1x, uint8_t e2x, uint8_t eyeY, bool blink, uint16_t hi)
{
  if (blink)
  {
    g.fillRect(x + e1x * s, y + (eyeY + 1) * s, 2 * s, s, C_INK);
    g.fillRect(x + e2x * s, y + (eyeY + 1) * s, 2 * s, s, C_INK);
  }
  else
  {
    g.fillRect(x + e1x * s, y + eyeY * s, 2 * s, 2 * s, C_INK);
    g.fillRect(x + e2x * s, y + eyeY * s, 2 * s, 2 * s, C_INK);
    g.fillRect(x + e1x * s, y + eyeY * s, s, s, hi);
    g.fillRect(x + e2x * s, y + eyeY * s, s, s, hi);
  }

  g.fillRect(x + (e1x - 2) * s, y + (eyeY + 4) * s, s, s, C_PINK);
  g.fillRect(x + (e2x + 2) * s, y + (eyeY + 4) * s, s, s, C_PINK);

  uint8_t mx = (e1x + e2x + 1) / 2;
  uint8_t my = eyeY + 4;
  g.fillRect(x + (mx - 1) * s, y + my * s, 2 * s, s, C_INK);
  g.fillRect(x + (mx - 2) * s, y + (my - 1) * s, s, s, C_INK);
  g.fillRect(x + (mx + 1) * s, y + (my - 1) * s, s, s, C_INK);
}

// ─────────────────────────────────────────────────────────────
//  TEXT / UI HELPERS
// ─────────────────────────────────────────────────────────────
void drawText(const char *s, int16_t x, int16_t y, uint8_t size,
              uint16_t col, uint16_t shadow)
{
  tft.setTextSize(size);
  tft.setTextColor(shadow);
  tft.setCursor(x + 2, y + 2);
  tft.print(s);
  tft.setTextColor(col);
  tft.setCursor(x, y);
  tft.print(s);
}

void drawCentered(const char *s, int16_t y, uint8_t size,
                  uint16_t col, uint16_t shadow)
{
  int16_t x = (SCR_W - (int16_t)strlen(s) * 6 * size) / 2;
  drawText(s, x, y, size, col, shadow);
}

const char *wifiStatusText(int status)
{
  switch (status)
  {
  case WL_NO_SSID_AVAIL:
    return "SSID NOT FOUND (2.4G?)";
  case WL_CONNECT_FAILED:
    return "WRONG PASSWORD?";
  case WL_CONNECTION_LOST:
    return "CONNECTION LOST";
  case WL_CONNECTED:
    return "CONNECTED";
  default:
    return "CONNECTING...";
  }
}

// ESP-IDF disconnect reason -> short on-screen hint (full code goes to serial)
const char *wifiReasonText(uint8_t reason)
{
  switch (reason)
  {
  case 2:
    return "AUTH EXPIRED (PASSWORD?)";
  case 15:
  case 204:
    return "WRONG PASSWORD?";
  case 23:
    return "NEEDS ENTERPRISE LOGIN";
  case 200:
    return "BEACON TIMEOUT";
  case 201:
    return "AP NOT FOUND";
  case 202:
    return "AUTH REJECTED (MAC FILTER?)";
  case 203:
  case 205:
    return "AP REFUSED CONNECTION";
  default:
    return "CONNECT FAILED";
  }
}

// two lines under the boot / error text, redrawn while WiFi connects
void drawWifiInfo(const char *line1, const char *line2)
{
  tft.fillRect(0, 194, SCR_W, 28, C_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_SHADE);
  tft.setCursor(14, 198);
  tft.print(line1);
  tft.setCursor(14, 212);
  tft.print(line2);
}

void drawDegree(int16_t x, int16_t y, int16_t d, uint16_t col)
{
  tft.fillRect(x, y, d, 2, col);
  tft.fillRect(x, y + d - 2, d, 2, col);
  tft.fillRect(x, y, 2, d, col);
  tft.fillRect(x + d - 2, y, 2, d, col);
}

int16_t drawTempNumber(int16_t x, int16_t y, int value, uint8_t size, uint16_t col)
{
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);

  tft.setTextSize(size);
  tft.setTextColor(C_INK);
  tft.setCursor(x + 2, y + 2);
  tft.print(buf);
  tft.setTextColor(col);
  tft.setCursor(x, y);
  tft.print(buf);

  int16_t ex = tft.getCursorX() + 3;
  int16_t d = (size >= 5) ? 10 : 8;
  int16_t ey = y + (size >= 4 ? 4 : 2);
  drawDegree(ex, ey, d, col);
  return ex + d + 4;
}

void drawSignal()
{
  bool online = (WiFi.status() == WL_CONNECTED);
  int bars = 0;
  if (online)
  {
    int rssi = WiFi.RSSI();
    bars = (rssi > -60) ? 3 : (rssi > -75) ? 2
                                           : 1;
  }

  tft.fillRect(204, 2, 34, 20, C_PANEL);
  for (int i = 0; i < 3; i++)
  {
    int h = 5 + i * 5;
    tft.fillRect(212 + i * 7, 19 - h, 4, h,
                 (i < bars) ? C_GREEN : C_PANEL_HI);
  }
}

uint16_t statusColor()
{
  if (WiFi.status() != WL_CONNECTED)
    return C_RED;
  if (refreshing || !wx.valid || (int32_t)(lastFail - lastOk) > 0)
    return C_ORANGE;
  return C_GREEN;
}

// header dot: red = offline, orange = updating/stale, green = fresh
void drawStatusDot(bool force)
{
  uint32_t now = millis();
  if (!force && (uint32_t)(now - lastStatusMs) < 1000)
    return;
  lastStatusMs = now;
  tft.fillRect(189, 7, 12, 12, C_PANEL);
  tft.fillRect(191, 9, 8, 8, statusColor());
}

// ─────────────────────────────────────────────────────────────
//  WEATHER ICON SCENES
// ─────────────────────────────────────────────────────────────
void drawStars(Adafruit_GFX &g, uint32_t t)
{
  static const int16_t stars[][2] = {
      {6, 8}, {102, 10}, {8, 104}, {100, 100}, {58, 2}};
  for (uint8_t i = 0; i < 5; i++)
  {
    bool big = ((t / 250 + i * 37) % 100) < 50;
    int16_t x = stars[i][0], y = stars[i][1];
    g.fillRect(x, y, 5, 5, C_BERRY);
    if (big)
    {
      g.fillRect(x + 5, y + 5, 5, 5, C_BERRY);
      g.fillRect(x - 5, y + 5, 5, 5, C_BERRY);
      g.fillRect(x + 5, y - 5, 5, 5, C_BERRY);
      g.fillRect(x - 5, y - 5, 5, 5, C_BERRY);
    }
  }
}

void drawSunRays(Adafruit_GFX &g, uint32_t t)
{
  for (uint8_t i = 0; i < 8; i++)
  {
    float a = t * 0.00035f + i * (PI / 4.0f);
    float rr = 52.0f + 3.0f * sinf(t * 0.004f + i * 1.3f);
    int16_t x = (int16_t)lroundf((60 + cosf(a) * rr) / 5.0f) * 5 - 2;
    int16_t y = (int16_t)lroundf((60 + sinf(a) * rr) / 5.0f) * 5 - 2;
    g.fillRect(x, y, 5, 5, C_SUN);
  }
}

GFXcanvas16 iconCanvas(120, 120);

void renderScene(uint8_t icon, bool day, uint32_t t, int16_t ox, int16_t oy)
{
  if (!iconCanvas.getBuffer())
    return;

  GFXcanvas16 &g = iconCanvas;
  g.fillScreen(C_BG);

  bool blink = (t % 4300) < 130;
  int16_t bob = (int16_t)lroundf(sinf(t / 640.0f) * 3);
  int16_t bob2 = (int16_t)lroundf(sinf(t / 700.0f + 1.7f) * 3);

  switch (icon)
  {
  case I_SUN:
    if (day)
    {
      drawSunRays(g, t);
      drawSprite(g, SUN_BIG, 16, 16, 12, 12 + bob, 6);
      drawFace(g, 12, 12 + bob, 6, 5, 10, 6, blink, C_SUN);
    }
    else
    {
      drawStars(g, t);
      drawSprite(g, MOON_BIG, 16, 16, 12, 12 + bob, 6);
      drawFace(g, 12, 12 + bob, 6, 5, 10, 6, blink, C_MOON);
    }
    break;

  case I_PARTLY:
    if (day)
    {
      drawSprite(g, SUN_SMALL, 12, 12, 2, 2 + bob2, 5);
      drawFace(g, 2, 2 + bob2, 5, 3, 7, 4, blink, C_SUN);
    }
    else
    {
      drawStars(g, t);
      drawSprite(g, MOON_BIG, 16, 16, 4, 2 + bob2, 4);
      drawFace(g, 4, 2 + bob2, 4, 5, 10, 6, blink, C_MOON);
    }
    drawSprite(g, CLOUD, 22, 14, 8, 32 + bob, 5);
    drawFace(g, 8, 32 + bob, 5, 6, 13, 5, blink, C_CREAM);
    break;

  case I_CLOUD:
    drawSprite(g, CLOUD, 22, 14, 5, 28 + bob, 5);
    drawFace(g, 5, 28 + bob, 5, 6, 13, 5, blink, C_CREAM);
    break;

  case I_FOG:
    drawSprite(g, CLOUD, 22, 14, 16, 2 + bob / 2, 4);
    drawFace(g, 16, 2 + bob / 2, 4, 6, 13, 5, blink, C_CREAM);
    for (int i = 0; i < 3; i++)
    {
      int16_t dx = (int16_t)lroundf(sinf(t / 900.0f + i * 1.6f) * 7);
      int16_t y = 70 + i * 16;
      int16_t w = 76 - i * 8;
      int16_t x = 22 + i * 6 + dx;
      g.fillRect(x, y, w, 10, C_CLOUD_SHADE);
      g.fillRect(x - 5, y + 5, 5, 5, C_CLOUD_SHADE);
      g.fillRect(x + w, y + 5, 5, 5, C_CLOUD_SHADE);
    }
    break;

  case I_DRIZZLE:
  case I_RAIN:
  {
    drawSprite(g, CLOUD, 22, 14, 5, 0 + bob / 2, 5);
    drawFace(g, 5, 0 + bob / 2, 5, 6, 13, 5, blink, C_CREAM);
    int n = (icon == I_DRIZZLE) ? 3 : 4;
    for (int i = 0; i < n; i++)
    {
      int16_t x = 8 + i * (100 / n) + (i & 1) * 4;
      int16_t y = 76 + ((t / (icon == I_DRIZZLE ? 11 : 7) + i * 29) % 48);
      y = (y / 5) * 5;
      g.fillRect(x + 5, y, 5, 5, C_RAIN);
      g.fillRect(x, y + 5, 10, (icon == I_DRIZZLE) ? 5 : 10, C_RAIN);
    }
    break;
  }

  case I_SNOW:
    drawSprite(g, CLOUD, 22, 14, 5, 0 + bob / 2, 5);
    drawFace(g, 5, 0 + bob / 2, 5, 6, 13, 5, blink, C_CREAM);
    for (int i = 0; i < 4; i++)
    {
      int16_t x = 14 + i * 24 + (int16_t)lroundf(sinf(t / 520.0f + i * 1.7f) * 6);
      int16_t y = 76 + ((t / 11 + i * 23) % 46);
      x = (x / 5) * 5;
      y = (y / 5) * 5;
      g.fillRect(x + 5, y, 5, 20, C_ICE);
      g.fillRect(x, y + 5, 20, 5, C_ICE);
      g.fillRect(x + 5, y + 5, 5, 5, C_CREAM);
    }
    break;

  case I_STORM:
  {
    bool flash = (t % 2700) < 130;
    if (flash)
      g.fillScreen(C_PANEL);
    drawSprite(g, CLOUD, 22, 14, 5, 0 + bob / 2, 5);
    drawFace(g, 5, 0 + bob / 2, 5, 6, 13, 5, blink, C_CREAM);
    drawSprite(g, BOLT, 5, 9, 48, 72, 5);
    break;
  }

  default:
    drawSprite(g, CLOUD, 22, 14, 5, 28 + bob, 5);
    drawFace(g, 5, 28 + bob, 5, 6, 13, 5, blink, C_CREAM);
    break;
  }

  tft.drawRGBBitmap(ox, oy, iconCanvas.getBuffer(), 120, 120);
}

// ─────────────────────────────────────────────────────────────
//  TEXT MAPPING
// ─────────────────────────────────────────────────────────────
// short single word so it can be drawn huge; the icon carries the detail
const char *condWord(int code)
{
  if (code <= 1)
    return "CLEAR";
  if (code == 2)
    return "PARTLY";
  if (code == 3)
    return "OVERCAST";
  if (code == 45 || code == 48)
    return "FOG";
  if (code >= 51 && code <= 57)
    return "DRIZZLE";
  if (code >= 61 && code <= 67)
    return "RAIN";
  if (code >= 71 && code <= 77)
    return "SNOW";
  if (code >= 80 && code <= 82)
    return "SHOWERS";
  if (code == 85 || code == 86)
    return "SNOW";
  if (code >= 95)
    return "STORM";
  return "WEATHER";
}

// largest font up to maxSize that still fits the 240 px width
uint8_t condSize(const char *s, uint8_t maxSize)
{
  size_t n = strlen(s);
  while (maxSize > 3 && n * 6 * maxSize > SCR_W - 16)
    maxSize--;
  return maxSize;
}

Icon iconForCode(int code)
{
  if (code <= 1)
    return I_SUN;
  if (code == 2)
    return I_PARTLY;
  if (code == 3)
    return I_CLOUD;
  if (code == 45 || code == 48)
    return I_FOG;
  if (code >= 51 && code <= 57)
    return I_DRIZZLE;
  if (code >= 61 && code <= 67)
    return I_RAIN;
  if (code >= 71 && code <= 77)
    return I_SNOW;
  if (code >= 80 && code <= 82)
    return I_RAIN;
  if (code == 85 || code == 86)
    return I_SNOW;
  if (code >= 95)
    return I_STORM;
  return I_CLOUD;
}

// ─────────────────────────────────────────────────────────────
//  SCREEN VIEWS
// ─────────────────────────────────────────────────────────────
void drawHeader(const char *label)
{
  tft.fillRect(0, 0, SCR_W, 24, C_PANEL);
  tft.fillRect(0, 24, SCR_W, 2, C_PINK);
  tft.setTextSize(2);
  tft.setTextColor(C_BERRY);
  tft.setCursor(8, 4);
  tft.print(label);
  drawSignal();
  drawStatusDot(true);
}

// NOW page: 120 px scene, ~49 px temperature, 42 px condition word
void drawNowPage()
{
  tft.fillScreen(C_BG);
  drawHeader("NOW");
  renderScene(wx.icon, wx.day, millis(), 60, 26);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d", wx.temp);
  uint8_t n = strlen(buf);
  uint8_t ts = (n <= 2) ? 7 : (n == 3 ? 6 : 5);
  int16_t tw = n * 6 * ts + 17;
  int16_t end = drawTempNumber((SCR_W - tw) / 2, 196 - 7 * ts, wx.temp, ts, C_BERRY);

  tft.setTextSize(2);
  tft.setTextColor(C_SHADE);
  tft.setCursor(end, 180);
  tft.print(FAHRENHEIT ? "F" : "C");

  const char *word = condWord(wx.code);
  drawCentered(word, 198, condSize(word, 6), C_FROST, C_INK);
}

void drawRow(int16_t y, const char *label, int value, uint16_t col)
{
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);
  uint8_t n = strlen(buf);
  uint8_t s = (n <= 2) ? 10 : (n == 3 ? 8 : 6);
  drawText(label, 20, y + (7 * s - 42) / 2, 6, col, C_INK);
  drawTempNumber(68, y, value, s, col);
}

// TOMORROW page: 70 px high/low, 42 px H/L, 35 px condition, rain at the bottom
void drawTomorrowPage()
{
  tft.fillScreen(C_BG);
  drawHeader("TOMORROW");

  drawRow(28, "H", wx.hi, C_BERRY);
  drawRow(102, "L", wx.lo, C_TEAL);

  const char *word = condWord(wx.tomorrowCode);
  drawCentered(word, 176, condSize(word, 5), C_FROST, C_INK);

  char rain[16];
  if (wx.rain >= 0)
    snprintf(rain, sizeof(rain), "%d%% RAIN", wx.rain);
  else
    strcpy(rain, "RAIN --");
  drawCentered(rain, 212, 4, C_RAIN, C_INK);
}

void drawWeatherUI()
{
  pageStart = millis();
  if (pageTomorrow)
    drawTomorrowPage();
  else
    drawNowPage();
}

void drawBootScreen()
{
  drawText("MOCHI SKY", 12, 160, 4, C_BERRY, C_INK);
  drawWifiInfo("WIFI: CONNECTING...", "");
}

void drawErrorText()
{
  bool online = (WiFi.status() == WL_CONNECTED);

  drawText(online ? "NO DATA" : "NO WIFI",
           14, 156, 5, C_RED, C_INK);

  char l1[48];
  snprintf(l1, sizeof(l1), "WIFI: %.30s", WIFI_SSID);

  const char *why = "AP OK, API FAILED";
  if (!online)
    why = lastWifiReason ? wifiReasonText(lastWifiReason) : wifiStatusText(lastWifiStatus);

  drawWifiInfo(l1, why);

  tft.setTextColor(C_SHADE);
  tft.setCursor(14, 226);
  tft.print("RETRYING...");
}

void showErrorScreen()
{
  tft.fillScreen(C_BG);
  drawErrorText();
  renderScene(I_DRIZZLE, true, millis(), 4, 28);
}

// ─────────────────────────────────────────────────────────────
//  NETWORK
// ─────────────────────────────────────────────────────────────
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info)
{
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
  {
    lastWifiReason = info.wifi_sta_disconnected.reason;
    Serial.printf("wifi: disconnect reason %u\n", (unsigned)lastWifiReason);
  }
}

bool fetchWeather()
{
  if (WiFi.status() != WL_CONNECTED)
    return false;

  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(LATITUDE, 4);
  url += "&longitude=";
  url += String(LONGITUDE, 4);
  url += "&current=temperature_2m,apparent_temperature,weather_code,is_day";
  url += "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max";
  url += "&forecast_days=2&timezone=auto";
#if FAHRENHEIT
  url += "&temperature_unit=fahrenheit";
#endif

  WiFiClientSecure client;
  client.setInsecure(); // shortcut: no RTC yet, so no cert check (weather data only)

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  if (!http.begin(client, url.c_str()))
    return false;

  int status = http.GET();
  if (status != HTTP_CODE_OK)
  {
    Serial.printf("HTTP %d\n", status);
    http.end();
    client.stop();
    return false;
  }

  JsonDocument filter;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["apparent_temperature"] = true;
  filter["current"]["weather_code"] = true;
  filter["current"]["is_day"] = true;
  filter["daily"]["weather_code"] = true;
  filter["daily"]["temperature_2m_max"] = true;
  filter["daily"]["temperature_2m_min"] = true;
  filter["daily"]["precipitation_probability_max"] = true;

  String payload = http.getString();
  http.end();
  client.stop();

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err)
  {
    Serial.printf("JSON: %s\n", err.c_str());
    return false;
  }

  float temp = doc["current"]["temperature_2m"] | NAN;
  float feels = doc["current"]["apparent_temperature"] | NAN;
  int code = doc["current"]["weather_code"] | 0;
  int isDay = doc["current"]["is_day"] | 1;

  JsonArray dcodes = doc["daily"]["weather_code"];
  if (dcodes.size() < 2)
    return false;

  float hi = doc["daily"]["temperature_2m_max"][1] | NAN;
  float lo = doc["daily"]["temperature_2m_min"][1] | NAN;
  int rain = doc["daily"]["precipitation_probability_max"][1] | -1;
  int tcode = dcodes[1] | 0;

  if (!isfinite(temp) || !isfinite(feels) || !isfinite(hi) || !isfinite(lo))
  {
    return false;
  }

  wx.temp = (int)lroundf(temp);
  wx.feels = (int)lroundf(feels);
  wx.hi = (int)lroundf(hi);
  wx.lo = (int)lroundf(lo);
  wx.rain = rain;
  wx.code = code;
  wx.tomorrowCode = tcode;
  wx.icon = iconForCode(code);
  wx.tomorrowIcon = iconForCode(tcode);
  wx.day = (isDay != 0);
  wx.valid = true;
  lastOk = millis();

  Serial.printf("weather: %d code %d, tomorrow %d/%d code %d, rain %d%%\n",
                wx.temp, wx.code, wx.lo, wx.hi, wx.tomorrowCode, wx.rain);
  return true;
}

bool connectWiFi(uint32_t timeoutMs, bool animate)
{
  if (WiFi.status() == WL_CONNECTED)
    return true;

  Serial.printf("wifi: connecting to \"%s\"\n", WIFI_SSID);
  lastWifiReason = 0;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  uint32_t lastInfo = start - 1000;
  while (WiFi.status() != WL_CONNECTED &&
         (uint32_t)(millis() - start) < timeoutMs)
  {
    if (animate)
      renderScene(I_PARTLY, true, millis(), 4, 28);
    if (screen != SCREEN_WEATHER &&
        (uint32_t)(millis() - lastInfo) > 500)
    {
      lastInfo = millis();
      lastWifiStatus = WiFi.status();
      char l1[48], l2[48];
      snprintf(l1, sizeof(l1), "WIFI: %.30s", WIFI_SSID);
      snprintf(l2, sizeof(l2), "%lus  %s",
               (unsigned long)((millis() - start) / 1000),
               lastWifiReason ? wifiReasonText(lastWifiReason)
                              : wifiStatusText(lastWifiStatus));
      drawWifiInfo(l1, l2);
    }
    delay(40);
  }
  lastWifiStatus = WiFi.status();
  if (WiFi.status() != WL_CONNECTED)
  {
    // one scan per boot so the log shows whether the SSID is even visible
    Serial.printf("wifi: failed, status %d\n", (int)WiFi.status());
    static bool scanned = false;
    if (!scanned)
    {
      scanned = true;
      int n = WiFi.scanNetworks();
      Serial.printf("wifi: %d networks visible (ESP32 sees 2.4 GHz only)\n", n);
      for (int i = 0; i < n; i++)
        Serial.printf("wifi:   \"%s\" rssi %d ch %d enc %d\n",
                      WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i),
                      (int)WiFi.channel(i), (int)WiFi.encryptionType(i));
      WiFi.scanDelete();
    }
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("wifi: connected, ip %s\n", WiFi.localIP().toString().c_str());
  return WiFi.status() == WL_CONNECTED;
}

void refreshWeather()
{
  if (refreshing)
    return;
  refreshing = true;
  lastTry = millis();
  if (screen == SCREEN_WEATHER)
    drawStatusDot(true);

  if (WiFi.status() != WL_CONNECTED)
    connectWiFi(8000, false);
  bool ok = fetchWeather();
  refreshing = false;

  if (ok)
  {
    screen = SCREEN_WEATHER;
    drawWeatherUI();
  }
  else
  {
    lastFail = millis();
    if (!wx.valid)
    {
      screen = SCREEN_ERROR;
      showErrorScreen();
    }
    else
    {
      drawStatusDot(true);
    }
  }
}

void retryError()
{
  if (refreshing)
    return;
  refreshing = true;
  lastTry = millis();

  if (WiFi.status() != WL_CONNECTED)
    connectWiFi(8000, false);
  bool ok = fetchWeather();
  refreshing = false;

  if (ok)
  {
    screen = SCREEN_WEATHER;
    drawWeatherUI();
  }
  else
  {
    lastFail = millis();
    drawErrorText();
  }
}

// ─────────────────────────────────────────────────────────────
//  SETUP / LOOP
// ─────────────────────────────────────────────────────────────
void setup()
{
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 240);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);

  tft.fillScreen(C_BG);
  drawBootScreen();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  if (connectWiFi(20000, true) && fetchWeather())
  {
    screen = SCREEN_WEATHER;
    drawWeatherUI();
  }
  else
  {
    screen = SCREEN_ERROR;
    showErrorScreen();
  }
  lastTry = millis();
}

void loop()
{
  uint32_t now = millis();

  if (screen == SCREEN_WEATHER)
  {
    if ((uint32_t)(now - pageStart) >= (uint32_t)PAGE_SECONDS * 1000UL)
    {
      pageTomorrow = !pageTomorrow;
      drawWeatherUI();
    }
    if (!pageTomorrow)
      renderScene(wx.icon, wx.day, now, 60, 26);
    drawStatusDot(false);

    uint32_t interval = (uint32_t)REFRESH_MINUTES * 60000UL;
    if (wx.valid && (int32_t)(lastFail - lastOk) > 0)
    {
      interval = 120000UL; // stale: try again sooner
    }
    if ((uint32_t)(now - lastTry) > interval && !refreshing)
    {
      refreshWeather();
    }
  }
  else if (screen == SCREEN_ERROR)
  {
    renderScene(I_DRIZZLE, true, now, 4, 28);
    if ((uint32_t)(now - lastTry) > 20000 && !refreshing)
    {
      retryError();
    }
  }

  delay(40);
}
