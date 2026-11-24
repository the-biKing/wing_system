#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_VL53L0X.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>

#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <driver/rtc_io.h>

#include "config.h"
#include "custom_font.h"
#include "hud_icons.h"
#include "sfx.h"
#include "web_hud.h"

// WS2812 幻彩 RGB 燈條實例 (GPIO 16)
Adafruit_NeoPixel strip(CFG_WS2812_NUM_LEDS, PIN_WS2812, NEO_GRB + NEO_KHZ800);

// 2S 電池電量監測變數 (GPIO 36)
float sysBatteryVolt = 7.4f;
int sysBatteryPct = 85;
unsigned long lastBatSampleTime = 0;

// 全域 NVS 與系統設定
Preferences preferences;
bool isMuted = false;

// --- 戰術 HUD 16-bit RGB565 色盤 ---
#define COLOR_BG 0x0000        // 深空黑底
#define COLOR_CYAN 0x07FF      // 霓虹青 (主瞄準線)
#define COLOR_AMBER 0xFD20     // 戰術琥珀橘 (警示/切角高光)
#define COLOR_GREEN 0x07E0     // 矩陣綠 (在線/正常)
#define COLOR_DARKGREY 0x18E3  // 底層未激活刻度暗灰
#define COLOR_MIDGREY 0x3A4E   // 輔助結構線框灰
#define COLOR_LIGHTGREY 0xAD55 // 標籤字體白灰
#define COLOR_WHITE 0xFFFF     // 強光核心點

Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_DC, TFT_RST);

// --- 點陣狀態圖標 ---
const uint8_t PROGMEM icon_wifi_8x8[] = {0b00111100, 0b01000010, 0b10011001,
                                         0b00100100, 0b01000010, 0b00011000,
                                         0b00000000, 0b00011000};

const uint8_t PROGMEM icon_bt_8x9[] = {0b00010000, 0b00011000, 0b01010100,
                                       0b00111000, 0b00010000, 0b00111000,
                                       0b01010100, 0b00011000, 0b00010000};

// ================= 自訂戰術點陣字型渲染核心 =================

const uint8_t *getCustomGlyph(char c) {
  if (c >= 'a' && c <= 'z')
    c -= 32;

  switch (c) {
  case '0':
    return font_0;
  case '1':
    return font_1;
  case '2':
    return font_2;
  case '3':
    return font_3;
  case '4':
    return font_4;
  case '5':
    return font_5;
  case '6':
    return font_6;
  case '7':
    return font_7;
  case '8':
    return font_8;
  case '9':
    return font_9;
  case 'A':
    return font_A;
  case 'B':
    return font_B;
  case 'C':
    return font_C;
  case 'D':
    return font_D;
  case 'E':
    return font_E;
  case 'F':
    return font_F;
  case 'G':
    return font_G;
  case 'H':
    return font_H;
  case 'I':
    return font_I;
  case 'J':
    return font_J;
  case 'K':
    return font_K;
  case 'L':
    return font_L;
  case 'M':
    return font_M;
  case 'N':
    return font_N;
  case 'O':
    return font_O;
  case 'P':
    return font_P;
  case 'Q':
    return font_Q;
  case 'R':
    return font_R;
  case 'S':
    return font_S;
  case 'T':
    return font_T;
  case 'U':
    return font_U;
  case 'V':
    return font_V;
  case 'W':
    return font_W;
  case 'X':
    return font_X;
  case 'Y':
    return font_Y;
  case 'Z':
    return font_Z;
  default:
    return nullptr;
  }
}

void drawTacticalChar(int x, int y, char c, uint16_t color, uint16_t bg,
                      uint8_t size = 1) {
  if (c == ' ') {
    if (bg != color)
      tft.fillRect(x, y, FONT_CHAR_W * size, FONT_CHAR_H * size, bg);
    return;
  }

  const uint8_t *glyph = getCustomGlyph(c);
  if (!glyph) {
    tft.drawChar(x, y, c, color, bg, size);
    return;
  }

  for (int r = 0; r < FONT_CHAR_H; r++) {
    uint8_t line = pgm_read_byte(&glyph[r]);
    for (int col = 0; col < FONT_CHAR_W; col++) {
      if (line & (0x80 >> col)) {
        if (size == 1)
          tft.drawPixel(x + col, y + r, color);
        else
          tft.fillRect(x + col * size, y + r * size, size, size, color);
      } else if (bg != color) {
        if (size == 1)
          tft.drawPixel(x + col, y + r, bg);
        else
          tft.fillRect(x + col * size, y + r * size, size, size, bg);
      }
    }
  }
}

int getTacticalStringWidth(const char *str, uint8_t size = 1) {
  int len = strlen(str);
  if (len == 0)
    return 0;
  int spacing = (size == 1) ? 1 : 2;
  return (len * FONT_CHAR_W * size) + ((len - 1) * spacing);
}

int drawTacticalString(int x, int y, const char *str, uint16_t color,
                       uint16_t bg, uint8_t size = 1) {
  int curX = x;
  int spacing = (size == 1) ? 1 : 2;
  while (*str) {
    drawTacticalChar(curX, y, *str, color, bg, size);
    curX += (FONT_CHAR_W * size) + spacing;
    str++;
  }
  return curX;
}

void drawTacticalPrintf(int x, int y, uint16_t color, uint16_t bg, uint8_t size,
                        const char *format, ...) {
  char buf[36];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  drawTacticalString(x, y, buf, color, bg, size);
}

// ================= 狀態機與參數結構 =================

SysState currentState = STATE_HOME;
int carouselIndex = PAGE_LIGHT;
const int TOTAL_CAROUSEL = 3;

const char *const LIGHT_MODE_NAMES[TOTAL_LIGHT_MODES] = {
    "STATIC", "RAINBOW", "BREATHE", "THUNDER", "RAINING", "PARTY"};

LightConfig lightCfg, lightBackup;

void hsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g,
              uint8_t &b) {
  if (s == 0) {
    uint8_t val255 = (uint8_t)((v * 255) / 100);
    r = g = b = val255;
    return;
  }
  h = h % 360;
  float s_f = s / 100.0f;
  float v_f = v / 100.0f;
  float c = v_f * s_f;
  float x = c * (1.0f - fabs(fmod(h / 60.0f, 2.0f) - 1.0f));
  float m = v_f - c;
  float rf = 0, gf = 0, bf = 0;
  if (h < 60) {
    rf = c;
    gf = x;
    bf = 0;
  } else if (h < 120) {
    rf = x;
    gf = c;
    bf = 0;
  } else if (h < 180) {
    rf = 0;
    gf = c;
    bf = x;
  } else if (h < 240) {
    rf = 0;
    gf = x;
    bf = c;
  } else if (h < 300) {
    rf = x;
    gf = 0;
    bf = c;
  } else {
    rf = c;
    gf = 0;
    bf = x;
  }

  r = (uint8_t)((rf + m) * 255.0f + 0.5f);
  g = (uint8_t)((gf + m) * 255.0f + 0.5f);
  b = (uint8_t)((bf + m) * 255.0f + 0.5f);
}

uint16_t hsvToRgb565(uint16_t h, uint8_t s, uint8_t v) {
  uint8_t r, g, b;
  hsvToRgb(h, s, v, r, g, b);
  return tft.color565(r, g, b);
}

// ================= 動態燈效動畫引擎 (Dynamic Light Mode Engine)
// =================
unsigned long lastLightAnimTick = 0;
float animRainbowPhase = 0.0f;
float animBreathePhase = 0.0f;

// 雷電 (THUNDER) 隨機頻閃狀態機
enum ThunderState { THUNDER_IDLE, THUNDER_FLASHING };
ThunderState thunderState = THUNDER_IDLE;
unsigned long thunderNextEvent = 0;
int thunderFlashCount = 0;
uint8_t thunderBrightness = 0;

// 雨滴 (RAINING) 旋轉滑行角度 (0.0 ~ 360.0 度)
float rainAngle = 0.0f;

// 派對 (PARTY) 隨機頻率與色盤
unsigned long partyNextSwitch = 0;
uint16_t partyCurHue = 0;
uint8_t partyCurSat = 100;
uint8_t partyCurBri = 80;

void updateThunderAnimation(unsigned long now) {
  if (thunderState == THUNDER_IDLE) {
    thunderBrightness = 0;
    if (now >= thunderNextEvent) {
      thunderState = THUNDER_FLASHING;
      thunderFlashCount = random(4, 9); // 4 到 8 次快速閃爍微脈衝
      thunderNextEvent = now;
    }
  } else if (thunderState == THUNDER_FLASHING) {
    if (now >= thunderNextEvent) {
      if (thunderFlashCount > 0) {
        thunderFlashCount--;
        if (thunderFlashCount % 2 == 0) {
          // 強光閃爍：80% ~ 100% 之 lightCfg.val
          thunderBrightness = (uint8_t)(lightCfg.val * random(80, 101) / 100);
          thunderNextEvent = now + random(25, 60);
        } else {
          // 閃爍間隔驟暗：0% ~ 20% 之 lightCfg.val
          thunderBrightness = (uint8_t)(lightCfg.val * random(0, 21) / 100);
          thunderNextEvent = now + random(20, 50);
        }
      } else {
        // 本次雷擊結束，返回等待
        thunderState = THUNDER_IDLE;
        thunderBrightness = 0;
        // 依據 lightCfg.speed (1 ~ 100) 決定下一次雷擊等待時間 (100最快、1最慢)
        unsigned long baseWait =
            map(constrain((int)lightCfg.speed, 1, 100), 1, 100, 3200, 250);
        thunderNextEvent = now + random(baseWait / 2, baseWait * 3 / 2);
      }
    }
  }
}

void updatePartyAnimation(unsigned long now) {
  if (now >= partyNextSwitch) {
    partyCurHue = random(0, 360);
    partyCurSat = random(70, 101);
    // 亮度隨機，但嚴格由 lightCfg.val 作為上限守護 (guarded by brightness)
    partyCurBri = (uint8_t)(random(40, 101) * lightCfg.val / 100);
    // 切換間隔由 speed 控制 (100最快 ~60ms, 1最慢 ~700ms)
    unsigned long interval =
        map(constrain((int)lightCfg.speed, 1, 100), 1, 100, 700, 60);
    partyNextSwitch = now + interval;
  }
}

void drawLightPreviewCircle(int rx, int ry, int r) {
  if (lightCfg.style == MODE_STATIC) {
    uint16_t curCol = hsvToRgb565(lightCfg.hue, lightCfg.sat, lightCfg.val);
    tft.fillCircle(rx, ry, r - 1, curCol);
    tft.drawCircle(rx, ry, r, COLOR_WHITE);
  } else if (lightCfg.style == MODE_RAINBOW) {
    // 彩虹由中心向外流動：hue 變為流動速度，sat 與 val 維持
    uint16_t cCol =
        hsvToRgb565((int)animRainbowPhase % 360, lightCfg.sat, lightCfg.val);
    tft.drawPixel(rx, ry, cCol);
    for (int cr = 1; cr <= r - 1; cr++) {
      int ringHue = ((int)(animRainbowPhase - cr * 25.0f) % 360 + 360) % 360;
      uint16_t ringCol = hsvToRgb565(ringHue, lightCfg.sat, lightCfg.val);
      tft.drawCircle(rx, ry, cr, ringCol);
    }
    tft.drawCircle(rx, ry, r, COLOR_WHITE);
  } else if (lightCfg.style == MODE_BREATHING) {
    // 呼吸模式：平滑亮度脈動，頻率由 speed 控制
    float breath = 0.5f + 0.5f * sin(animBreathePhase);
    uint8_t curBri = (uint8_t)(lightCfg.val * (0.05f + 0.95f * breath));
    uint16_t bCol = hsvToRgb565(lightCfg.hue, lightCfg.sat, curBri);
    tft.fillCircle(rx, ry, r - 1, bCol);
    tft.drawCircle(rx, ry, r, COLOR_WHITE);
  } else if (lightCfg.style == MODE_THUNDER) {
    // 雷電模式：隨機頻閃，最高亮度受 val 控制
    uint16_t tCol =
        hsvToRgb565(lightCfg.hue, (lightCfg.sat * 2) / 3, thunderBrightness);
    tft.fillCircle(rx, ry, r - 1, tCol);
    tft.drawCircle(rx, ry, r, COLOR_WHITE);
  } else if (lightCfg.style == MODE_RAINING) {
    // 雨滴模式：3~6 顆像素群沿圓周滑行，支援負值速度逆向滑行
    tft.fillCircle(rx, ry, r - 4, 0x0821); // 暗色戰術內核
    for (int i = 0; i < 32; i++) {
      float deg = i * (360.0f / 32.0f);
      float rad = deg * 0.0174532925f;
      int px = rx + (int)((r - 1.5f) * cos(rad) + 0.5f);
      int py = ry + (int)((r - 1.5f) * sin(rad) + 0.5f);

      // 兩組對稱雨滴群 (間隔 180 度)
      float diff1, diff2;
      if (lightCfg.speed >= 0) {
        // 正向順時針：彗尾在角度落後方向
        diff1 = fmod(rainAngle - deg + 360.0f, 360.0f);
        diff2 = fmod((rainAngle + 180.0f) - deg + 360.0f, 360.0f);
      } else {
        // 負向逆時針：彗尾在角度超前方向
        diff1 = fmod(deg - rainAngle + 360.0f, 360.0f);
        diff2 = fmod(deg - (rainAngle + 180.0f) + 360.0f, 360.0f);
      }
      float minDiff = (diff1 < diff2) ? diff1 : diff2;
      float briFactor = 0.04f;
      if (minDiff < 75.0f) {
        briFactor = 1.0f - (minDiff / 75.0f) * 0.85f;
      }
      uint8_t pBri = (uint8_t)(lightCfg.val * briFactor);
      tft.drawPixel(px, py, hsvToRgb565(lightCfg.hue, lightCfg.sat, pBri));
    }
    tft.drawCircle(rx, ry, r, COLOR_WHITE);
  } else if (lightCfg.style == MODE_PARTY) {
    // 派對模式：忽略 hue/sat，隨機跳色，最高亮度受 val 保護
    uint16_t pCol = hsvToRgb565(partyCurHue, partyCurSat, partyCurBri);
    tft.fillCircle(rx, ry, r - 1, pCol);
    tft.drawCircle(rx, ry, r, COLOR_WHITE);
  }
}

void updatePhysicalWs2812() {
  uint16_t numPix = strip.numPixels();
  if (numPix == 0)
    return;

  // 若燈條亮度為 0 或關閉，關閉 NMOS 供電以徹底阻斷靜態漏電 (GPIO 19 = LOW)
  if (lightCfg.val == 0) {
    strip.clear();
    strip.show();
    digitalWrite(PIN_WS2812_EN, LOW);
    return;
  }

  // 燈條使用中，確保 NMOS 導通供電 (GPIO 19 = HIGH)
  if (digitalRead(PIN_WS2812_EN) == LOW) {
    digitalWrite(PIN_WS2812_EN, HIGH);
    delay(20); // 剛開電給予足夠時間讓燈條電路與濾波電容充電穩定
  }

  // 18A 電流安全防護：動態閉迴路亮度限幅
  static uint8_t maxSafeVal = 100;
  static unsigned long lastOverCurrentAlarm = 0;
  bool isOverCurrent = (sysCurr1 >= CFG_OVERCURRENT_LIMIT_A);

  if (isOverCurrent) {
    // 電流達到或超過 18A：主動向下壓制最大容許亮度，阻止發出更高亮度
    if (maxSafeVal > (uint8_t)(lightCfg.val * 0.85f)) {
      maxSafeVal = (uint8_t)(lightCfg.val * 0.85f);
    } else if (maxSafeVal > 2) {
      maxSafeVal--;
    }
    // 警報音效 (每 1.5 秒警告一次)
    if (millis() - lastOverCurrentAlarm >= 1500) {
      lastOverCurrentAlarm = millis();
      sfxWarning();
      Serial.printf("[OVERCURRENT] sysCurr1: %.2fA >= %.1fA! Brightness clamped to %d%%\n",
                    sysCurr1, CFG_OVERCURRENT_LIMIT_A, maxSafeVal);
    }
  } else if (sysCurr1 < (CFG_OVERCURRENT_LIMIT_A - CFG_OVERCURRENT_HYST_A)) {
    // 電流回落至 16.5A 以下：平滑放寬上限
    static unsigned long lastRecoveryTick = 0;
    if (millis() - lastRecoveryTick >= 250) {
      lastRecoveryTick = millis();
      if (maxSafeVal < 100) maxSafeVal++;
    }
  }

  uint8_t effectiveVal = min((uint8_t)lightCfg.val, maxSafeVal);

  if (lightCfg.style == MODE_STATIC) {
    uint8_t r, g, b;
    hsvToRgb(lightCfg.hue, lightCfg.sat, effectiveVal, r, g, b);
    uint32_t col = strip.Color(r, g, b);
    for (uint16_t i = 0; i < numPix; i++)
      strip.setPixelColor(i, col);
  } else if (lightCfg.style == MODE_RAINBOW) {
    for (uint16_t i = 0; i < numPix; i++) {
      int ringHue =
          ((int)(animRainbowPhase + (i * 360 / numPix)) % 360 + 360) % 360;
      uint8_t r, g, b;
      hsvToRgb(ringHue, lightCfg.sat, effectiveVal, r, g, b);
      strip.setPixelColor(i, strip.Color(r, g, b));
    }
  } else if (lightCfg.style == MODE_BREATHING) {
    float breath = 0.5f + 0.5f * sin(animBreathePhase);
    uint8_t curBri = (uint8_t)(effectiveVal * (0.05f + 0.95f * breath));
    uint8_t r, g, b;
    hsvToRgb(lightCfg.hue, lightCfg.sat, curBri, r, g, b);
    uint32_t col = strip.Color(r, g, b);
    for (uint16_t i = 0; i < numPix; i++)
      strip.setPixelColor(i, col);
  } else if (lightCfg.style == MODE_THUNDER) {
    float scale = (lightCfg.val > 0) ? ((float)effectiveVal / (float)lightCfg.val) : 1.0f;
    uint8_t clampedThunder = (uint8_t)(thunderBrightness * scale);
    uint8_t r, g, b;
    hsvToRgb(lightCfg.hue, (lightCfg.sat * 2) / 3, clampedThunder, r, g, b);
    uint32_t col = strip.Color(r, g, b);
    for (uint16_t i = 0; i < numPix; i++)
      strip.setPixelColor(i, col);
  } else if (lightCfg.style == MODE_RAINING) {
    float degStep = 360.0f / (float)numPix;
    for (uint16_t i = 0; i < numPix; i++) {
      float deg = i * degStep;
      float diff1, diff2;
      if (lightCfg.speed >= 0) {
        diff1 = fmod(rainAngle - deg + 360.0f, 360.0f);
        diff2 = fmod((rainAngle + 180.0f) - deg + 360.0f, 360.0f);
      } else {
        diff1 = fmod(deg - rainAngle + 360.0f, 360.0f);
        diff2 = fmod(deg - (rainAngle + 180.0f) + 360.0f, 360.0f);
      }
      float minDiff = (diff1 < diff2) ? diff1 : diff2;
      float briFactor = 0.04f;
      if (minDiff < 90.0f) {
        briFactor = 1.0f - (minDiff / 90.0f) * 0.85f;
      }
      uint8_t pBri = (uint8_t)(effectiveVal * briFactor);
      uint8_t r, g, b;
      hsvToRgb(lightCfg.hue, lightCfg.sat, pBri, r, g, b);
      strip.setPixelColor(i, strip.Color(r, g, b));
    }
  } else if (lightCfg.style == MODE_PARTY) {
    float scale = (lightCfg.val > 0) ? ((float)effectiveVal / (float)lightCfg.val) : 1.0f;
    uint8_t clampedParty = (uint8_t)(partyCurBri * scale);
    uint8_t r, g, b;
    hsvToRgb(partyCurHue, partyCurSat, clampedParty, r, g, b);
    uint32_t col = strip.Color(r, g, b);
    for (uint16_t i = 0; i < numPix; i++)
      strip.setPixelColor(i, col);
  }

  strip.show();

  // 序列埠狀態監視 (數值變動或每 3 秒輸出一次)
  static unsigned long lastWs2812Log = 0;
  static uint8_t lastLoggedStyle = 255;
  static uint16_t lastLoggedHue = 999;
  static uint8_t lastLoggedVal = 255;
  bool stateChanged = (lightCfg.style != lastLoggedStyle || lightCfg.hue != lastLoggedHue || effectiveVal != lastLoggedVal);
  if (stateChanged || (millis() - lastWs2812Log >= 3000)) {
    lastWs2812Log = millis();
    lastLoggedStyle = lightCfg.style;
    lastLoggedHue = lightCfg.hue;
    lastLoggedVal = effectiveVal;
    Serial.printf("[WS2812] Style:%s | Hue:%d | Sat:%d | Val:%d(Eff:%d) | EN(GPIO19):%d\n",
                  LIGHT_MODE_NAMES[lightCfg.style], lightCfg.hue, lightCfg.sat, lightCfg.val, effectiveVal,
                  digitalRead(PIN_WS2812_EN));
  }
}

void handleLightAnimation() {
  unsigned long now = millis();
  if (now - lastLightAnimTick < 33)
    return;
  float dt = (now - lastLightAnimTick) / 1000.0f;
  lastLightAnimTick = now;

  float rSpeed = (float)lightCfg.speed;
  animRainbowPhase += rSpeed * 1.8f * dt * 60.0f;
  if (animRainbowPhase >= 360.0f)
    animRainbowPhase -= 360.0f;

  float bSpeed = (float)lightCfg.speed;
  animBreathePhase += bSpeed * 0.05f * dt * 60.0f;
  if (animBreathePhase >= 6.283185f)
    animBreathePhase -= 6.283185f;

  updateThunderAnimation(now);

  float rainSpeed = (float)lightCfg.speed;
  rainAngle += rainSpeed * 2.5f * dt * 60.0f;
  if (rainAngle >= 360.0f)
    rainAngle -= 360.0f;
  if (rainAngle < 0.0f)
    rainAngle += 360.0f;

  updatePartyAnimation(now);

  // 同步更新實體 WS2812 幻彩燈條
  updatePhysicalWs2812();

  if (currentState == STATE_HOME && carouselIndex == PAGE_LIGHT) {
    drawLightPreviewCircle(63, 62, 8);
  }
}

MotorConfig motorCfg, motorBackup;

GuardrailState motorGuardStatus = GUARD_OK;

// 伺服控制器運作狀態
bool motorServoActive = false;         // 正在執行閉迴路伺服定點位移
bool motorServoArrived = false;        // 已抵達定點狀態 (顯示 ARRIVED! PWM:0)
float motorTargetPct = 50.0f;          // 目標百分比 [0.0 ~ 100.0%]
float motorTargetMm = 77.4f;           // 最終目標物理位置 [mm]
float motorActiveTargetMm = 77.4f;     // 動態追蹤軌跡設定點 [mm]
float maxProfileVelMmS = 30.0f;        // 軌跡最大位移速度 (30 mm/s)
const float MAX_PWM_SLEW_UP = 15.0f;   // 每個控制週期 (30ms) 最大升速 PWM 步階
const float MAX_PWM_SLEW_DOWN = 35.0f; // 每個控制週期最大減速 PWM 步階

// PID 參數
float motorKp = 4.0f;
float motorKi = 0.8f;
float motorKd = 1.2f;
float motorIntegralErr = 0.0f;
float motorCurrentOutPwm = 0.0f;
float lastMotorMeasuredDistMm = 77.4f;
float motorFilteredVelocityMmS = 0.0f;
unsigned long motorHighEffortStartTime = 0;
float motorStallCheckStartDist = 0.0f;
unsigned long lastMotorCtrlTimeMs = 0;
const unsigned long MOTOR_CTRL_PERIOD_MS = 30;
static bool lastMotorForwardState = true;

// 蘇格蘭軛機構目標毫米換算 (含軟體安全邊界 clamp)
float calcTargetMm(float pct) {
  float safe_min = CFG_DEFAULT_Y_MIN_MM + SOFT_LIMIT_LOW_OFFSET_MM;
  float safe_max = CFG_DEFAULT_Y_MAX_MM - SOFT_LIMIT_HIGH_OFFSET_MM;
  pct = constrain(pct, 0.0f, 100.0f);
  return safe_min + (pct / 100.0f) * (safe_max - safe_min);
}

// Scotch Yoke 位置相依摩擦補償 (U形力矩曲線: 端點~102, 中段~30)
float getScotchYokeFeedforward(float posMm) {
  float norm_pos = (posMm - CFG_DEFAULT_Y_MIN_MM) / CFG_DEFAULT_STROKE_SPAN_MM;
  norm_pos = constrain(norm_pos, 0.0f, 1.0f);
  float eta = fabs(norm_pos - 0.5f) * 2.0f;
  float eta3 = eta * eta * eta;
  float u_ff =
      CFG_DEFAULT_U_MID_BREAKAWAY +
      (CFG_DEFAULT_U_DEAD_BREAKAWAY - CFG_DEFAULT_U_MID_BREAKAWAY) * eta3;
  return u_ff * 255.0f;
}

// DS18B20 1-Wire 實例
OneWire oneWire(PIN_TEMP_DS18B20);
DallasTemperature tempSensor(&oneWire);

// VL53L0X ToF 飛行時間測距感測器實例 (I2C: SDA=GPIO21, SCL=GPIO15)
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
bool vl53l0xReady = false;
int16_t tofDistanceMm = -1; // -1 表示感測器未就緒或超出測量範圍
float currentTofDistMm = 0.0f;
unsigned long lastTofSampleTime = 0;
unsigned long lastTofValidReadTime = 0;

// ToF 雙級濾波器 (5-Tap 中值濾波 + 一階 IIR 低通)
const int TOF_MEDIAN_WINDOW = 5;
float tofMedianBuf[TOF_MEDIAN_WINDOW] = {0};
uint8_t tofMedianIdx = 0;
bool tofMedianFilled = false;
float iirTofDistanceMm = 0.0f;
bool iirTofInitialized = false;
const float TOF_IIR_ALPHA = 0.70f;

float getTofMedian(float *arr, int n) {
  float temp[TOF_MEDIAN_WINDOW];
  for (int i = 0; i < n; i++)
    temp[i] = arr[i];
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (temp[i] > temp[j]) {
        float t = temp[i];
        temp[i] = temp[j];
        temp[j] = t;
      }
    }
  }
  return temp[n / 2];
}

// 馬達底層硬體輸出函式
void setMotorRaw(bool forward, uint8_t duty) {
  if (duty == 0 || motorGuardStatus != GUARD_OK) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_MOTOR_PWM, 0);
#else
    ledcWrite(MOTOR_PWM_CH, 0);
#endif
    digitalWrite(PIN_MOTOR_DIR, LOW);
    return;
  }

  if (forward != lastMotorForwardState) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_MOTOR_PWM, 0);
#else
    ledcWrite(MOTOR_PWM_CH, 0);
#endif
    delay(10);
    digitalWrite(PIN_MOTOR_DIR, forward ? HIGH : LOW);
    delayMicroseconds(50);
    lastMotorForwardState = forward;
  } else {
    digitalWrite(PIN_MOTOR_DIR, forward ? HIGH : LOW);
  }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, duty);
#else
  ledcWrite(MOTOR_PWM_CH, duty);
#endif
}

void emergencyStopMotor(GuardrailState reason) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, 0);
#else
  ledcWrite(MOTOR_PWM_CH, 0);
#endif
  digitalWrite(PIN_MOTOR_DIR, LOW);
  motorServoActive = false;
  motorServoArrived = false;
  motorCfg.speed = 0;
  motorGuardStatus = reason;
  motorCurrentOutPwm = 0.0f;
  sfxWarning();
  Serial.printf("[MOTOR ALARM] Guardrail Tripped: %d\n", reason);
}

void stopMotorAll() {
  setMotorRaw(true, 0);
  motorServoActive = false;
  motorServoArrived = false;
  motorCfg.speed = 0;
  motorCurrentOutPwm = 0.0f;
  motorIntegralErr = 0.0f;
}

void startMotorServoToPct(float pct) {
  if (motorGuardStatus != GUARD_OK || !vl53l0xReady ||
      currentTofDistMm <= 0.0f) {
    return;
  }
  pct = constrain(pct, 0.0f, 100.0f);
  motorTargetPct = pct;
  motorTargetMm = calcTargetMm(pct);
  motorActiveTargetMm = currentTofDistMm;
  motorIntegralErr = 0.0f;
  motorCurrentOutPwm = 0.0f;
  motorHighEffortStartTime = 0;
  motorServoArrived = false;
  motorServoActive = true;
  motorCfg.speed = 0;
  Serial.printf("[SERVO START] Moving to %0.1f%% (%0.2f mm)\n", pct,
                motorTargetMm);
}

float sysTemp = 25.0;  // 預設室溫初值，背景自動更新
float sysCurr1 = 0.0f; // 電流感測器 1 讀數 (ACS712 20A, GPIO 35)
float sysCurr2 = 0.0f; // 電流感測器 2 讀數 (ACS712T-05B, GPIO 34)
float zeroCurrentMv1 = CFG_ACS712_1_ZERO_MV; // 0A 基準電壓 1
float zeroCurrentMv2 = CFG_ACS712_2_ZERO_MV; // 0A 基準電壓 2
unsigned long lastCurrentSampleTime = 0;     // 電流採樣計時器

// 非阻塞感測器取樣計時器
unsigned long lastTempRequestTime = 0;
bool tempConversionPending = false;
unsigned long lastThermalAlarmTime = 0; // 超溫警報重複計時器
unsigned long resetPressStartTime = 0;  // 四鍵長按計時器
unsigned long sleepPressStartTime = 0;  // 首頁長按 BACK 3 秒睡眠計時器
unsigned long mutePressStartTime = 0;   // 長按左右雙鍵 1.5 秒靜音計時器
bool muteToggleTriggered = false;       // 靜音切換觸發防連擊旗標

int cursorRow = 0;
bool isEditing = false;

unsigned long lastBlinkTime = 0;
bool blinkState = true;
unsigned long stateSwitchLockout = 0;

struct TacticalButton {
  uint8_t pin;
  bool lastReading;
  bool stableState;
  unsigned long lastDebounceTime;
  bool justPressed;
  unsigned long pressStartTime;
  unsigned long lastRepeatTime;
  bool repeatTriggered;
} btns[4] = {{PIN_KEY_A, HIGH, HIGH, 0, false, 0, 0, false},
             {PIN_KEY_B, HIGH, HIGH, 0, false, 0, 0, false},
             {PIN_KEY_C, HIGH, HIGH, 0, false, 0, 0, false},
             {PIN_KEY_D, HIGH, HIGH, 0, false, 0, 0, false}};

const unsigned long DEBOUNCE_DELAY = 45;

// ================= 序列埠按鍵模擬 (Serial Monitor QWER) =================
void handleSerialInput() {
  while (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() == 0)
      continue;

    if (input.length() == 1) {
      char raw = input.charAt(0);
      char ch = toupper(raw);
      int btnIdx = -1;
      const char *label = "";

      // QWER 戰術佈局 (使用者指定):
      // Q: [< / UP / -]   (Left / Button C)
      // W: [> / DOWN / +] (Right / Button B)
      // E: [ENTER]        (Confirm / Button A)
      // R: [BACK]         (Return / Button D)
      if (ch == 'Q' || ch == 'C') {
        btnIdx = 2; // Button C (Left / Up / -)
        label = "BTN_C [Left/Up/-]";
      } else if (ch == 'W' || ch == 'B') {
        btnIdx = 1; // Button B (Right / Down / +)
        label = "BTN_B [Right/Down/+]";
      } else if (ch == 'E' || ch == 'A') {
        btnIdx = 0; // Button A (ENTER)
        label = "BTN_A [ENTER]";
      } else if (ch == 'R' || ch == 'D') {
        btnIdx = 3; // Button D (BACK)
        label = "BTN_D [BACK]";
      } else if (ch == 'X') {
        // 模擬四鍵同時按下觸發 System Reset
        triggerSystemReset();
        return;
      }

      if (btnIdx != -1) {
        btns[btnIdx].justPressed = true;
        Serial.printf("[SIM INPUT] '%c' -> Triggered %s\n", raw, label);
      } else {
        Serial.printf("[SIM INPUT] '%c' ignored.\n", raw);
      }
    } else {
      // 處理多字元指令 (Serial Commands)
      if (input.startsWith("pos ") || input.startsWith("POS ")) {
        float p = input.substring(4).toFloat();
        p = constrain(p, 0.0f, 100.0f);
        motorCfg.targetPos = (uint8_t)p;
        startMotorServoToPct(p);
        if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR)
          updateMotorSubpage();
      } else if (input.equalsIgnoreCase("stop") ||
                 input.equalsIgnoreCase("disarm")) {
        stopMotorAll();
        if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR)
          updateMotorSubpage();
        Serial.println(F("[MOTOR] Stopped."));
      } else if (input.equalsIgnoreCase("reset")) {
        motorGuardStatus = GUARD_OK;
        motorCurrentOutPwm = 0.0f;
        motorIntegralErr = 0.0f;
        if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR)
          updateMotorSubpage();
        Serial.println(F("[MOTOR] Guardrail reset."));
      }
    }
  }
}

void scanButtons() {
  unsigned long now = millis();
  for (int i = 0; i < 4; i++) {
    btns[i].justPressed = false;
    btns[i].repeatTriggered = false;
    bool reading = digitalRead(btns[i].pin);
    if (reading != btns[i].lastReading)
      btns[i].lastDebounceTime = now;
    if ((now - btns[i].lastDebounceTime) > CFG_BUTTON_DEBOUNCE_MS) {
      if (reading != btns[i].stableState) {
        btns[i].stableState = reading;
        if (btns[i].stableState == LOW && now > stateSwitchLockout) {
          btns[i].justPressed = true;
          btns[i].pressStartTime = now;
          btns[i].lastRepeatTime = now;
        }
      }
    }
    btns[i].lastReading = reading;

    // 長按連發檢測：按住超過 CFG_BUTTON_REPEAT_START_MS 後，每隔
    // CFG_BUTTON_REPEAT_RATE_MS 觸發一次 repeatTriggered
    if (btns[i].stableState == LOW && !btns[i].justPressed &&
        now > stateSwitchLockout) {
      if (now - btns[i].pressStartTime >= CFG_BUTTON_REPEAT_START_MS) {
        if (now - btns[i].lastRepeatTime >= CFG_BUTTON_REPEAT_RATE_MS) {
          btns[i].repeatTriggered = true;
          btns[i].lastRepeatTime = now;
        }
      }
    }
  }

  // 接收 Serial Monitor 模擬輸入
  handleSerialInput();
}

void drawTechBar(int x, int y, int w, int h, int val, int maxVal,
                 uint16_t activeColor) {
  tft.drawRect(x, y, w, h, COLOR_MIDGREY);
  int fillW = map(constrain(val, 0, maxVal), 0, maxVal, 0, w - 2);
  if (fillW > 0)
    tft.fillRect(x + 1, y + 1, fillW, h - 2, activeColor);
  if (w - 2 - fillW > 0)
    tft.fillRect(x + 1 + fillW, y + 1, w - 2 - fillW, h - 2, COLOR_BG);
}

void drawHsvTrueColorBar(int x, int y, int w, int h, int mode, int currentVal,
                         int maxVal, bool isSelected) {
  tft.drawRect(x, y, w, h,
               isSelected ? (isEditing ? COLOR_AMBER : COLOR_CYAN)
                          : COLOR_MIDGREY);
  int innerW = w - 2;
  int innerH = h - 2;
  int fillW = map(constrain(currentVal, 0, maxVal), 0, maxVal, 0, innerW);

  for (int i = 0; i < innerW; i++) {
    uint16_t barCol;
    if (mode == 0) {
      uint16_t sampleH = (uint16_t)((i * 359) / (innerW - 1));
      barCol = hsvToRgb565(sampleH, 100, 100);
    } else if (mode == 1) {
      uint8_t sampleS = (uint8_t)((i * 100) / (innerW - 1));
      barCol = hsvToRgb565(lightCfg.hue, sampleS,
                           lightCfg.val > 20 ? lightCfg.val : 80);
    } else {
      uint8_t sampleV = (uint8_t)((i * 100) / (innerW - 1));
      barCol = hsvToRgb565(lightCfg.hue, lightCfg.sat, sampleV);
    }

    if (i < fillW)
      tft.drawFastVLine(x + 1 + i, y + 1, innerH, barCol);
    else
      tft.drawFastVLine(x + 1 + i, y + 1, innerH, COLOR_BG);
  }

  if (fillW > 0 && fillW < innerW) {
    tft.drawFastVLine(x + fillW, y, h, COLOR_WHITE);
  }
}

// ================= 按鍵與狀態佈局渲染引擎 =================

// 1. 左右兩側翼指示：首頁 < >，分頁未選入 UP/DOWN 圖標，選入後變 -/+
void drawSideButtons() {
  tft.fillRect(0, 18, 12, 94, COLOR_BG);
  tft.drawLine(0, 22, 9, 18, COLOR_MIDGREY);
  tft.drawFastVLine(9, 18, 94, COLOR_MIDGREY);
  tft.drawLine(9, 111, 0, 107, COLOR_MIDGREY);
  tft.drawFastVLine(3, 34, 60, COLOR_DARKGREY);

  tft.fillRect(116, 18, 12, 94, COLOR_BG);
  tft.drawLine(127, 22, 118, 18, COLOR_MIDGREY);
  tft.drawFastVLine(118, 18, 94, COLOR_MIDGREY);
  tft.drawLine(118, 111, 127, 107, COLOR_MIDGREY);
  tft.drawFastVLine(124, 34, 60, COLOR_DARKGREY);

  if (currentState == STATE_HOME) {
    // 首頁：左右顯示 < 與 >
    drawTacticalChar(2, 58, '<', COLOR_CYAN, COLOR_BG, 1);
    drawTacticalChar(120, 58, '>', COLOR_CYAN, COLOR_BG, 1);
  } else if (currentState == STATE_SUBPAGE) {
    if (!isEditing) {
      // 分頁瀏覽狀態：左翼 UP，右翼 DOWN (依使用者操作反饋調整)
      int upX = (12 - ICON_UP_W) / 2;
      int upY = 58 - (ICON_UP_H / 2);
      tft.drawBitmap(upX, upY, icon_up, ICON_UP_W, ICON_UP_H, COLOR_AMBER,
                     COLOR_BG);

      int downX = 116 + (12 - ICON_DOWN_W) / 2;
      int downY = 58 - (ICON_DOWN_H / 2);
      tft.drawBitmap(downX, downY, icon_down, ICON_DOWN_W, ICON_DOWN_H,
                     COLOR_AMBER, COLOR_BG);
    } else {
      // 數值編輯中：左翼 -，右翼 +
      drawTacticalChar(2, 58, '-', COLOR_AMBER, COLOR_BG, 1);
      drawTacticalChar(120, 58, '+', COLOR_AMBER, COLOR_BG, 1);
    }
  }
}

// 2. 螢幕下排膠囊：BACK 與 ENTER 圖示
void drawPersistentButtonBar(uint16_t colLeft, uint16_t colRight) {
  tft.fillRect(0, 114, 128, 14, COLOR_BG);
  tft.drawFastHLine(0, 114, 128, COLOR_MIDGREY);

  // 左膠囊 (D: BACK)
  tft.drawRoundRect(8, 116, 50, 11, 2, colLeft);
  int bx = 8 + (50 - ICON_BACK_W) / 2;
  int by = 116 + (11 - ICON_BACK_H) / 2;
  tft.drawBitmap(bx, by, icon_back, ICON_BACK_W, ICON_BACK_H, colLeft,
                 COLOR_BG);

  // 右膠囊 (A: ENTER)
  tft.drawRoundRect(70, 116, 50, 11, 2, colRight);
  int ex = 70 + (50 - ICON_ENTER_W) / 2;
  int ey = 116 + (11 - ICON_ENTER_H) / 2;
  tft.drawBitmap(ex, ey, icon_enter, ICON_ENTER_W, ICON_ENTER_H, colRight,
                 COLOR_BG);
}

void updateGlobalButtonBar() {
  if (currentState == STATE_HOME) {
    // 首頁：BACK 鍵無效 (暗灰)，ENTER 鍵可選入 (琥珀色)
    drawPersistentButtonBar(COLOR_DARKGREY, COLOR_AMBER);
  } else if (currentState == STATE_SUBPAGE) {
    if (!isEditing) {
      // 分頁瀏覽：BACK 返回首頁，ENTER 進入編輯
      drawPersistentButtonBar(COLOR_AMBER, COLOR_AMBER);
    } else {
      // 數值編輯：BACK 取消還原，ENTER 確定儲存 (綠色)
      drawPersistentButtonBar(COLOR_AMBER, COLOR_GREEN);
    }
  }
}

// 頂部狀態列
void drawTopBar() {
  tft.fillRect(0, 0, 128, 16, COLOR_BG);

  // 電池外框與即時電量填充 (2S 鋰電池 GPIO 36)
  tft.drawRect(2, 4, 15, 8, COLOR_CYAN);
  tft.fillRect(17, 6, 2, 4, COLOR_CYAN);
  int fillW = map(constrain(sysBatteryPct, 0, 100), 0, 100, 0, 11);
  if (fillW > 0) {
    tft.fillRect(4, 6, fillW, 4,
                 sysBatteryPct > 20 ? COLOR_GREEN : COLOR_AMBER);
  }
  if (11 - fillW > 0) {
    tft.fillRect(4 + fillW, 6, 11 - fillW, 4, COLOR_BG);
  }
  drawTacticalPrintf(22, 5, COLOR_CYAN, COLOR_BG, 1, "%3d%%", sysBatteryPct);

  // 靜音圖標 (位於電池百分比與 WiFi 分隔線之間)
  if (isMuted) {
    tft.drawBitmap(42, 4, icon_mute_8x8, 8, 8, COLOR_AMBER, COLOR_BG);
  }

  tft.drawFastVLine(52, 3, 10, COLOR_DARKGREY);
  tft.drawPixel(52, 8, COLOR_AMBER);

  // WiFi AP 基地台在線狀態圖示
  tft.drawBitmap(58, 4, icon_wifi_8x8, 8, 8, COLOR_GREEN, COLOR_BG);
  tft.fillRect(68, 8, 2, 4, COLOR_GREEN);
  tft.fillRect(72, 6, 2, 6, COLOR_GREEN);
  tft.fillRect(76, 4, 2, 8, COLOR_GREEN);

  // 藍牙 / Web HUD 客戶端連線狀態
  tft.drawBitmap(88, 4, icon_bt_8x9, 8, 9, COLOR_CYAN, COLOR_BG);
  int clients = WiFi.softAPgetStationNum();
  if (clients > 0) {
    drawTacticalPrintf(98, 5, COLOR_GREEN, COLOR_BG, 1, "AP:%d", clients);
  } else {
    drawTacticalString(98, 5, "APON", COLOR_CYAN, COLOR_BG, 1);
  }

  tft.drawFastHLine(0, 16, 40, COLOR_MIDGREY);
  tft.drawFastHLine(44, 16, 40, COLOR_CYAN);
  tft.drawFastHLine(88, 16, 40, COLOR_MIDGREY);
  tft.drawPixel(42, 16, COLOR_AMBER);
  tft.drawPixel(86, 16, COLOR_AMBER);
}

// 雙層戰術外框
void drawTechCentralFrame(bool highlight) {
  int x = 12, y = 18, w = 104, h = 94;
  uint16_t cOuter = highlight ? COLOR_CYAN : COLOR_MIDGREY;
  uint16_t cInner = highlight ? COLOR_AMBER : 0x2165;

  tft.drawLine(x + 8, y, x + w - 9, y, cOuter);
  tft.drawLine(x + w - 1, y + 8, x + w - 1, y + h - 9, cOuter);
  tft.drawLine(x + w - 9, y + h - 1, x + 8, y + h - 1, cOuter);
  tft.drawLine(x, y + h - 9, x, y + 8, cOuter);

  tft.drawLine(x, y + 8, x + 8, y, cOuter);
  tft.drawLine(x + w - 9, y, x + w - 1, y + 8, cOuter);
  tft.drawLine(x + w - 1, y + h - 9, x + w - 9, y + h - 1, cOuter);
  tft.drawLine(x + 8, y + h - 1, x, y + h - 9, cOuter);

  tft.drawLine(x + 2, y + 8, x + 8, y + 2, cInner);
  tft.drawLine(x + w - 3, y + h - 9, x + w - 9, y + h - 3, cInner);

  tft.drawPixel(x + 3, y + 3, COLOR_CYAN);
  tft.drawPixel(x + w - 4, y + 3, COLOR_CYAN);
  tft.drawPixel(x + 3, y + h - 4, COLOR_AMBER);
  tft.drawPixel(x + w - 4, y + h - 4, COLOR_AMBER);
}

void drawRotatedArcGauge(int cx, int cy, int rIn, int rOut, int totalSegments,
                         int activeSegments, float startAngleDeg,
                         float totalDeg, uint16_t activeCol,
                         uint16_t inactiveCol) {
  float stepDeg = totalDeg / (totalSegments - 1);

  for (int i = 0; i < totalSegments; i++) {
    float deg = startAngleDeg + i * stepDeg;
    float rad = deg * 0.0174532925f;
    float cosA = cos(rad);
    float sinA = sin(rad);

    int x1 = (int)(cx + rIn * cosA + 0.5f);
    int y1 = (int)(cy + rIn * sinA + 0.5f);
    int x2 = (int)(cx + rOut * cosA + 0.5f);
    int y2 = (int)(cy + rOut * sinA + 0.5f);

    uint16_t col = (i < activeSegments) ? activeCol : inactiveCol;
    tft.drawLine(x1, y1, x2, y2, col);
  }
}

void drawSegmentedArcGauge(int cx, int cy, int rIn, int rOut, int totalSegments,
                           int activeSegments, uint16_t activeCol,
                           uint16_t inactiveCol) {
  drawRotatedArcGauge(cx, cy, rIn, rOut, totalSegments, activeSegments, 135.0f,
                      270.0f, activeCol, inactiveCol);
}

// ================= 三大分頁進場科技動畫 =================

void playLightEntranceAnim() {
  int cx = 14, cy = 19;
  tft.fillRect(cx + 1, cy + 1, 98, 90, COLOR_BG);

  drawTacticalString(cx + 6, cy + 3, ">LIGHT", COLOR_AMBER, COLOR_BG, 1);
  drawTacticalPrintf(cx + 66, cy + 3, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d'",
                     lightCfg.hue);

  int barX = cx + 8, barY = cy + 14, barW = 82, barH = 5;
  int ptrX = barX + map(lightCfg.hue, 0, 359, 0, barW - 1);
  tft.drawPixel(ptrX, barY - 1, COLOR_WHITE);
  tft.drawLine(ptrX - 2, barY - 3, ptrX + 2, barY - 3, COLOR_AMBER);
  tft.drawLine(ptrX - 1, barY - 2, ptrX + 1, barY - 2, COLOR_AMBER);

  for (int i = 0; i < barW; i++) {
    uint16_t sampleH = (uint16_t)((i * 359) / (barW - 1));
    tft.drawFastVLine(barX + i, barY, barH, hsvToRgb565(sampleH, 100, 100));
  }
  tft.drawRect(barX - 1, barY - 1, barW + 2, barH + 2, COLOR_MIDGREY);

  int rx = 63, ry = 62;
  uint16_t curCol = hsvToRgb565(lightCfg.hue, lightCfg.sat, lightCfg.val);

  for (int step = 0; step <= 5; step++) {
    int r = 6 + step * 2;
    float baseDeg = step * 36.0f;
    tft.fillRect(rx - 22, ry - 22, 45, 45, COLOR_BG);
    tft.drawCircle(rx, ry, r, COLOR_CYAN);
    for (int a = 0; a < 4; a++) {
      float rad = (baseDeg + a * 90.0f) * 0.0174532925f;
      int x1 = (int)(rx + (r - 2) * cos(rad) + 0.5f);
      int y1 = (int)(ry + (r - 2) * sin(rad) + 0.5f);
      int x2 = (int)(rx + (r + 4) * cos(rad) + 0.5f);
      int y2 = (int)(ry + (r + 4) * sin(rad) + 0.5f);
      tft.drawLine(x1, y1, x2, y2, COLOR_AMBER);
    }
    delay(16);
  }

  for (int step = 0; step <= 4; step++) {
    int r = 18 - step;
    float baseDeg = 180.0f + step * 45.0f;
    tft.fillRect(rx - 22, ry - 22, 45, 45, COLOR_BG);
    tft.drawCircle(rx, ry, r, COLOR_CYAN);
    for (int a = 0; a < 4; a++) {
      float rad = (baseDeg + a * 90.0f) * 0.0174532925f;
      int x1 = (int)(rx + (r - 2) * cos(rad) + 0.5f);
      int y1 = (int)(ry + (r - 2) * sin(rad) + 0.5f);
      int x2 = (int)(rx + (r + 3) * cos(rad) + 0.5f);
      int y2 = (int)(ry + (r + 3) * sin(rad) + 0.5f);
      tft.drawLine(x1, y1, x2, y2, COLOR_WHITE);
    }
    delay(15);
  }

  tft.fillRect(rx - 22, ry - 22, 45, 45, COLOR_BG);
  tft.drawCircle(rx, ry, 12, COLOR_CYAN);
  drawLightPreviewCircle(rx, ry, 8);

  tft.drawFastHLine(rx - 17, ry, 3, COLOR_CYAN);
  tft.drawFastHLine(rx + 15, ry, 3, COLOR_CYAN);
  tft.drawFastVLine(rx, ry - 17, 3, COLOR_CYAN);
  tft.drawFastVLine(rx, ry + 15, 3, COLOR_CYAN);

  int tagW = 76, tagH = 11;
  int tagX = cx + (98 - tagW) / 2, tagY = cy + 69;
  tft.drawRoundRect(tagX, tagY, tagW, tagH, 2, COLOR_AMBER);
  tft.fillRect(tagX + 1, tagY + 1, tagW - 2, tagH - 2, 0x18A2);
  int textW = getTacticalStringWidth(LIGHT_MODE_NAMES[lightCfg.style], 1);
  drawTacticalString(tagX + (tagW - textW) / 2, tagY + 2,
                     LIGHT_MODE_NAMES[lightCfg.style], COLOR_AMBER, 0x18A2, 1);
}

void playMotorEntranceAnim() {
  int cx = 14, cy = 19;
  tft.fillRect(cx + 1, cy + 1, 98, 90, COLOR_BG);

  drawTacticalString(cx + 6, cy + 3, ">MOTOR", COLOR_AMBER, COLOR_BG, 1);
  if (tofDistanceMm >= 0) {
    drawTacticalPrintf(cx + 52, cy + 3, COLOR_CYAN, COLOR_BG, 1, "%4dMM",
                       tofDistanceMm);
  } else {
    drawTacticalString(cx + 52, cy + 3, "--- MM", COLOR_DARKGREY, COLOR_BG, 1);
  }

  int dialX = 63, dialY = 56;
  int totalSegs = 16;
  int targetSegs = map(motorCfg.speed, 0, 100, 0, totalSegs);

  for (int seg = 2; seg <= 16; seg += 3) {
    drawSegmentedArcGauge(dialX, dialY, 14, 19, totalSegs, seg, COLOR_AMBER,
                          COLOR_DARKGREY);
    delay(15);
  }
  drawSegmentedArcGauge(dialX, dialY, 14, 19, totalSegs, 16, COLOR_WHITE,
                        COLOR_DARKGREY);
  delay(30);

  for (int deg = 0; deg <= 360; deg += 45) {
    tft.fillRect(dialX - 24, dialY - 24, 49, 49, COLOR_BG);
    drawRotatedArcGauge(dialX, dialY, 16, 22, totalSegs, 16, 135.0f + deg,
                        270.0f, COLOR_CYAN, COLOR_DARKGREY);
    delay(16);
  }

  tft.fillRect(dialX - 24, dialY - 24, 49, 49, COLOR_BG);
  for (int seg = 16; seg >= targetSegs; seg--) {
    drawSegmentedArcGauge(dialX, dialY, 14, 19, totalSegs, seg, COLOR_CYAN,
                          COLOR_DARKGREY);
    delay(12);
  }

  char pwrStr[8];
  snprintf(pwrStr, sizeof(pwrStr), "%d", motorCfg.speed);
  int numW = getTacticalStringWidth(pwrStr, 2);
  drawTacticalString(dialX - numW / 2 - 4, dialY - (FONT_CHAR_H), pwrStr,
                     COLOR_WHITE, COLOR_BG, 2);
  drawTacticalChar(dialX + numW / 2 + 1, dialY - (FONT_CHAR_H / 2), '%',
                   COLOR_CYAN, COLOR_BG, 1);

  int rpm = map(motorCfg.speed, 0, 100, 0, 4800);
  char rpmBuf[24];
  snprintf(rpmBuf, sizeof(rpmBuf), "VEL %4d RPM", rpm);
  int rpmW = getTacticalStringWidth(rpmBuf, 1);
  drawTacticalString(cx + (98 - rpmW) / 2, cy + 69, rpmBuf, COLOR_LIGHTGREY,
                     COLOR_BG, 1);
}

// 依當前系統溫度選取對應的真實色彩圖標 (T_low / T_norm / T_mid / T_high)
const uint16_t *getTemperatureIcon(float temp) {
  if (temp < 30.0f) {
    return icon_T_low;
  } else if (temp < 45.0f) {
    return icon_T_norm;
  } else if (temp < CFG_TEMP_WARNING_THRESHOLD) { // 預設 50.0°C
    return icon_T_mid;
  } else {
    return icon_T_high;
  }
}

void playSensorsEntranceAnim() {
  int cx = 14, cy = 19;
  tft.fillRect(cx + 1, cy + 1, 98, 90, COLOR_BG);

  drawTacticalString(cx + 6, cy + 3, ">THERMAL", COLOR_AMBER, COLOR_BG, 1);

  int boxX = cx + 8, boxY = cy + 15, boxW = 82, boxH = 28;
  tft.drawRoundRect(boxX, boxY, boxW, boxH, 2, COLOR_MIDGREY);
  tft.drawLine(boxX, boxY, boxX + 10, boxY, COLOR_AMBER);
  tft.drawLine(boxX, boxY, boxX, boxY + 10, COLOR_AMBER);

  int bY = cy + 49, bW = 39, bH = 26;
  int c1BoxX = cx + 8;
  tft.drawRect(c1BoxX, bY, bW, bH, COLOR_MIDGREY);
  tft.fillRect(c1BoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
  drawTacticalString(c1BoxX + 4, bY + 2, "CUR1", COLOR_CYAN, 0x11C5, 1);

  int c2BoxX = cx + 51;
  tft.drawRect(c2BoxX, bY, bW, bH, COLOR_MIDGREY);
  tft.fillRect(c2BoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
  drawTacticalString(c2BoxX + 4, bY + 2, "CUR2", COLOR_AMBER, 0x11C5, 1);
  delay(40);

  const uint16_t *tempFrames[4] = {icon_T_low, icon_T_norm, icon_T_mid,
                                   icon_T_high};
  int iconX = boxX + boxW - ICON_T_HIGH_W - 5;
  int iconY = boxY + (boxH - ICON_T_HIGH_H) / 2;

  // 循序展示低溫至高溫原圖色彩階梯動畫
  for (int f = 0; f < 4; f++) {
    tft.drawRGBBitmap(iconX, iconY, tempFrames[f], ICON_T_HIGH_W,
                      ICON_T_HIGH_H);
    delay(45);
  }
  // 動畫結束後恢復為當前溫度實際對應的原圖圖標
  tft.drawRGBBitmap(iconX, iconY, getTemperatureIcon(sysTemp), ICON_T_HIGH_W,
                    ICON_T_HIGH_H);

  // 數值特效
  tft.fillRect(boxX + 2, boxY + 2, boxW - ICON_T_HIGH_W - 10, boxH - 4,
               COLOR_CYAN);
  delay(18);
  tft.fillRect(boxX + 2, boxY + 2, boxW - ICON_T_HIGH_W - 10, boxH - 4,
               COLOR_BG);

  char tempBuf[12];
  snprintf(tempBuf, sizeof(tempBuf), "%2.1f", sysTemp);
  int tNumW = getTacticalStringWidth(tempBuf, 2);
  drawTacticalString(boxX + 4, boxY + 7, tempBuf, COLOR_WHITE, COLOR_BG, 2);
  drawTacticalString(boxX + 4 + tNumW + 3, boxY + 6, "'C", COLOR_WHITE,
                     COLOR_BG, 1);
  delay(25);

  drawTacticalString(boxX + 4, boxY + 7, tempBuf, COLOR_AMBER, COLOR_BG, 2);
  drawTacticalString(boxX + 4 + tNumW + 3, boxY + 6, "'C", COLOR_CYAN, COLOR_BG,
                     1);
  delay(35);

  tft.fillRect(c1BoxX + 1, bY + 11, bW - 2, 14, COLOR_CYAN);
  delay(15);
  tft.fillRect(c1BoxX + 1, bY + 11, bW - 2, 14, COLOR_BG);
  drawTacticalPrintf(c1BoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%2.2fA",
                     sysCurr1);
  delay(35);

  tft.fillRect(c2BoxX + 1, bY + 11, bW - 2, 14, COLOR_AMBER);
  delay(15);
  tft.fillRect(c2BoxX + 1, bY + 11, bW - 2, 14, COLOR_BG);
  drawTacticalPrintf(c2BoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%2.2fA",
                     sysCurr2);
}

void playPageEntranceAnimation(int pageIdx) {
  if (pageIdx == PAGE_LIGHT)
    playLightEntranceAnim();
  if (pageIdx == PAGE_MOTOR)
    playMotorEntranceAnim();
  if (pageIdx == PAGE_SENSORS)
    playSensorsEntranceAnim();
}

void drawCentralCardContent() {
  int cx = 14, cy = 19;
  tft.fillRect(cx + 1, cy + 1, 98, 90, COLOR_BG);

  if (carouselIndex == PAGE_LIGHT) {
    drawTacticalString(cx + 6, cy + 3, ">LIGHT", COLOR_AMBER, COLOR_BG, 1);
    if (lightCfg.style == MODE_RAINBOW) {
      drawTacticalPrintf(cx + 60, cy + 3, COLOR_LIGHTGREY, COLOR_BG, 1, "S:%3d",
                         lightCfg.speed);
    } else if (lightCfg.style == MODE_PARTY) {
      drawTacticalPrintf(cx + 60, cy + 3, COLOR_LIGHTGREY, COLOR_BG, 1, "B:%3d",
                         lightCfg.val);
    } else {
      drawTacticalPrintf(cx + 66, cy + 3, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d'",
                         lightCfg.hue);
    }

    int barX = cx + 8, barY = cy + 14, barW = 82, barH = 5;
    if (lightCfg.style == MODE_RAINBOW) {
      drawTechBar(barX, barY, barW, barH, lightCfg.speed, 100, COLOR_CYAN);
    } else if (lightCfg.style == MODE_PARTY) {
      drawTechBar(barX, barY, barW, barH, lightCfg.val, 100, COLOR_AMBER);
    } else {
      int ptrX = barX + map(lightCfg.hue, 0, 359, 0, barW - 1);
      tft.drawPixel(ptrX, barY - 1, COLOR_WHITE);
      tft.drawLine(ptrX - 2, barY - 3, ptrX + 2, barY - 3, COLOR_AMBER);
      tft.drawLine(ptrX - 1, barY - 2, ptrX + 1, barY - 2, COLOR_AMBER);

      for (int i = 0; i < barW; i++) {
        uint16_t sampleH = (uint16_t)((i * 359) / (barW - 1));
        tft.drawFastVLine(barX + i, barY, barH, hsvToRgb565(sampleH, 100, 100));
      }
      tft.drawRect(barX - 1, barY - 1, barW + 2, barH + 2, COLOR_MIDGREY);
    }

    int reactorX = 63, reactorY = 62;

    tft.drawCircle(reactorX, reactorY, 12,
                   blinkState ? COLOR_CYAN : COLOR_MIDGREY);
    drawLightPreviewCircle(reactorX, reactorY, 8);

    tft.drawFastHLine(reactorX - 17, reactorY, 3, COLOR_CYAN);
    tft.drawFastHLine(reactorX + 15, reactorY, 3, COLOR_CYAN);
    tft.drawFastVLine(reactorX, reactorY - 17, 3, COLOR_CYAN);
    tft.drawFastVLine(reactorX, reactorY + 15, 3, COLOR_CYAN);

    int tagW = 76, tagH = 11;
    int tagX = cx + (98 - tagW) / 2, tagY = cy + 69;
    tft.drawRoundRect(tagX, tagY, tagW, tagH, 2, COLOR_AMBER);
    tft.fillRect(tagX + 1, tagY + 1, tagW - 2, tagH - 2, 0x18A2);
    int textW = getTacticalStringWidth(LIGHT_MODE_NAMES[lightCfg.style], 1);
    drawTacticalString(tagX + (tagW - textW) / 2, tagY + 2,
                       LIGHT_MODE_NAMES[lightCfg.style], COLOR_AMBER, 0x18A2,
                       1);
  } else if (carouselIndex == PAGE_MOTOR) {
    drawTacticalString(cx + 6, cy + 3, ">MOTOR", COLOR_AMBER, COLOR_BG, 1);
    if (tofDistanceMm >= 0) {
      drawTacticalPrintf(cx + 52, cy + 3, COLOR_CYAN, COLOR_BG, 1, "%4dMM",
                         tofDistanceMm);
    } else {
      drawTacticalString(cx + 52, cy + 3, "--- MM", COLOR_DARKGREY, COLOR_BG,
                         1);
    }

    int dialX = 63, dialY = 56;
    int totalSegs = 16;
    int activeSegs = map(motorCfg.speed, 0, 100, 0, totalSegs);
    drawSegmentedArcGauge(dialX, dialY, 14, 19, totalSegs, activeSegs,
                          COLOR_CYAN, COLOR_DARKGREY);

    char pwrStr[8];
    snprintf(pwrStr, sizeof(pwrStr), "%d", motorCfg.speed);
    int numW = getTacticalStringWidth(pwrStr, 2);
    drawTacticalString(dialX - numW / 2 - 4, dialY - (FONT_CHAR_H), pwrStr,
                       COLOR_WHITE, COLOR_BG, 2);
    drawTacticalChar(dialX + numW / 2 + 1, dialY - (FONT_CHAR_H / 2), '%',
                     COLOR_CYAN, COLOR_BG, 1);

    int rpm = map(motorCfg.speed, 0, 100, 0, 4800);
    char rpmBuf[24];
    snprintf(rpmBuf, sizeof(rpmBuf), "VEL %4d RPM", rpm);
    int rpmW = getTacticalStringWidth(rpmBuf, 1);
    drawTacticalString(cx + (98 - rpmW) / 2, cy + 69, rpmBuf, COLOR_LIGHTGREY,
                       COLOR_BG, 1);
  } else if (carouselIndex == PAGE_SENSORS) {
    drawTacticalString(cx + 6, cy + 3, ">THERMAL", COLOR_AMBER, COLOR_BG, 1);

    int boxX = cx + 8, boxY = cy + 15, boxW = 82, boxH = 28;
    tft.drawRoundRect(boxX, boxY, boxW, boxH, 2, COLOR_MIDGREY);
    tft.drawLine(boxX, boxY, boxX + 10, boxY, COLOR_AMBER);
    tft.drawLine(boxX, boxY, boxX, boxY + 10, COLOR_AMBER);

    char tempBuf[12];
    snprintf(tempBuf, sizeof(tempBuf), "%2.1f", sysTemp);
    int tNumW = getTacticalStringWidth(tempBuf, 2);
    drawTacticalString(boxX + 4, boxY + 7, tempBuf, COLOR_AMBER, COLOR_BG, 2);
    drawTacticalString(boxX + 4 + tNumW + 3, boxY + 6, "'C", COLOR_CYAN,
                       COLOR_BG, 1);

    int iconX = boxX + boxW - ICON_T_HIGH_W - 5;
    int iconY = boxY + (boxH - ICON_T_HIGH_H) / 2;
    tft.drawRGBBitmap(iconX, iconY, getTemperatureIcon(sysTemp), ICON_T_HIGH_W,
                      ICON_T_HIGH_H);

    int bY = cy + 49, bW = 39, bH = 26;
    int c1BoxX = cx + 8;
    tft.drawRect(c1BoxX, bY, bW, bH, COLOR_MIDGREY);
    tft.fillRect(c1BoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
    drawTacticalString(c1BoxX + 4, bY + 2, "CUR1", COLOR_CYAN, 0x11C5, 1);
    drawTacticalPrintf(c1BoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%2.2fA",
                       sysCurr1);

    int c2BoxX = cx + 51;
    tft.drawRect(c2BoxX, bY, bW, bH, COLOR_MIDGREY);
    tft.fillRect(c2BoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
    drawTacticalString(c2BoxX + 4, bY + 2, "CUR2", COLOR_AMBER, 0x11C5, 1);
    drawTacticalPrintf(c2BoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%2.2fA",
                       sysCurr2);
  }
}

void playCyberTransition(bool toRight) {
  int xStart = 14, yStart = 18, w = 100, h = 94;
  int step = 14;

  if (toRight) {
    for (int x = xStart; x < xStart + w; x += step) {
      tft.drawFastVLine(x, yStart, h, COLOR_CYAN);
      tft.drawFastVLine(x + 1, yStart, h, COLOR_WHITE);
      if (x - step >= xStart)
        tft.fillRect(x - step, yStart, step, h, COLOR_BG);
      delay(8);
    }
  } else {
    for (int x = xStart + w - 1; x >= xStart; x -= step) {
      tft.drawFastVLine(x, yStart, h, COLOR_AMBER);
      tft.drawFastVLine(x - 1, yStart, h, COLOR_WHITE);
      if (x + step < xStart + w)
        tft.fillRect(x + 1, yStart, step, h, COLOR_BG);
      delay(8);
    }
  }
  tft.fillRect(xStart, yStart, w, h, COLOR_BG);
}

bool isLightRowSelectable(int row) {
  if (carouselIndex != PAGE_LIGHT)
    return true;
  if (row == 0) {
    if (lightCfg.style == MODE_RAINBOW || lightCfg.style == MODE_PARTY) {
      return false; // 灰底停用項目，導航自動跳過
    }
  } else if (row == 1) {
    if (lightCfg.style == MODE_PARTY) {
      return false; // 灰底停用項目，導航自動跳過
    }
  }
  return true;
}

void updateLightRow(int row) {
  int startY = 19;
  int y = startY + row * 18;
  bool sel = (cursorRow == row);

  // 1. 更新行游標 (# 或 >)
  drawTacticalChar(14, y + 2, sel ? (isEditing ? '#' : '>') : ' ',
                   isEditing ? COLOR_AMBER : COLOR_CYAN, COLOR_BG, 1);

  // 2. 動態標籤文字 (僅 STATIC 模式第 4 行為 CLR，其餘動態模式第 4 行統一為 SPD
  // 特效速度)
  const char *tag = "---";
  if (row == 0) {
    tag = "HUE";
  } else if (row == 1) {
    tag = "SAT";
  } else if (row == 2) {
    tag = "BRI";
  } else if (row == 3) {
    tag = "MOD";
  } else if (row == 4) {
    tag = (lightCfg.style == MODE_STATIC) ? "CLR" : "SPD";
  }

  uint16_t tagCol =
      sel ? (isEditing ? COLOR_AMBER : COLOR_WHITE) : COLOR_LIGHTGREY;
  // RAINBOW 與 PARTY 模式下 HUE 被忽略，顯示為暗灰色以提示停用
  if ((lightCfg.style == MODE_RAINBOW || lightCfg.style == MODE_PARTY) &&
      row == 0) {
    tagCol = COLOR_DARKGREY;
  }
  // PARTY 模式下 SAT 也被忽略
  if (lightCfg.style == MODE_PARTY && row == 1) {
    tagCol = COLOR_DARKGREY;
  }
  drawTacticalString(22, y + 2, tag, tagCol, COLOR_BG, 1);

  // 3. 各行內容渲染
  if (row == 0) {
    if (lightCfg.style == MODE_RAINBOW || lightCfg.style == MODE_PARTY) {
      // RAINBOW 與 PARTY 模式均灰底忽略 HUE
      tft.fillRect(44, y + 1, 40, 9, COLOR_BG);
      tft.drawRect(44, y + 1, 40, 9, COLOR_DARKGREY);
      drawTacticalString(88, y + 2, "---", COLOR_DARKGREY, COLOR_BG, 1);
    } else {
      drawHsvTrueColorBar(44, y + 1, 40, 9, 0, lightCfg.hue, 359, sel);
      drawTacticalPrintf(88, y + 2, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d",
                         lightCfg.hue);
    }
  } else if (row == 1) {
    if (lightCfg.style == MODE_PARTY) {
      // PARTY 模式忽略 SAT
      tft.fillRect(44, y + 1, 40, 9, COLOR_BG);
      tft.drawRect(44, y + 1, 40, 9, COLOR_DARKGREY);
      drawTacticalString(88, y + 2, "---", COLOR_DARKGREY, COLOR_BG, 1);
    } else {
      drawHsvTrueColorBar(44, y + 1, 40, 9, 1, lightCfg.sat, 100, sel);
      drawTacticalPrintf(88, y + 2, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d",
                         lightCfg.sat);
    }
  } else if (row == 2) {
    // BRI: 所有模式均受亮度控制 (受 18A 電流保護門檻守護)
    drawHsvTrueColorBar(44, y + 1, 40, 9, 2, lightCfg.val, 100, sel);
    if (sysCurr1 >= CFG_OVERCURRENT_LIMIT_A) {
      drawTacticalString(86, y + 2, "!18A!", COLOR_AMBER, COLOR_BG, 1);
    } else {
      drawTacticalPrintf(88, y + 2, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d",
                         lightCfg.val);
    }
  } else if (row == 3) {
    uint16_t boxBorder =
        (sel && isEditing) ? COLOR_AMBER : (sel ? COLOR_CYAN : COLOR_MIDGREY);
    tft.drawRect(44, y + 1, 68, 11, boxBorder);
    uint16_t boxBg = (sel && isEditing) ? 0x2104 : COLOR_BG;
    tft.fillRect(45, y + 2, 66, 9, boxBg);

    char mBuf[20];
    snprintf(mBuf, sizeof(mBuf), "[%s]", LIGHT_MODE_NAMES[lightCfg.style]);
    uint16_t mCol =
        (sel && isEditing) ? COLOR_AMBER : (sel ? COLOR_WHITE : COLOR_CYAN);
    int mW = getTacticalStringWidth(mBuf, 1);
    drawTacticalString(44 + (68 - mW) / 2, y + 2, mBuf, mCol, boxBg, 1);
  } else if (row == 4) {
    if (lightCfg.style == MODE_STATIC) {
      uint16_t curCol = hsvToRgb565(lightCfg.hue, lightCfg.sat, lightCfg.val);
      tft.fillRect(44, y + 1, 40, 9, curCol);
      tft.drawRect(44, y + 1, 40, 9,
                   sel ? (isEditing ? COLOR_AMBER : COLOR_CYAN) : COLOR_WHITE);
      drawTacticalString(88, y + 2, "VIEW", COLOR_LIGHTGREY, COLOR_BG, 1);
    } else if (lightCfg.style == MODE_RAINING) {
      // 雨滴滑行速度：支援負值 (-50 ~ +50) 雙向能量條
      tft.drawRect(44, y + 1, 40, 9,
                   sel ? (isEditing ? COLOR_AMBER : COLOR_CYAN)
                       : COLOR_MIDGREY);
      tft.fillRect(45, y + 2, 38, 7, COLOR_BG);
      tft.drawFastVLine(63, y + 2, 7, COLOR_MIDGREY); // 中心 0 刻度
      if (lightCfg.speed > 0) {
        int w = map(lightCfg.speed, 0, 50, 0, 19);
        tft.fillRect(64, y + 2, w, 7, isEditing ? COLOR_AMBER : COLOR_CYAN);
      } else if (lightCfg.speed < 0) {
        int w = map(-lightCfg.speed, 0, 50, 0, 18);
        tft.fillRect(63 - w, y + 2, w, 7, isEditing ? COLOR_AMBER : COLOR_CYAN);
      }
      char spdBuf[8];
      snprintf(spdBuf, sizeof(spdBuf), "%+3d", lightCfg.speed);
      drawTacticalString(88, y + 2, spdBuf, COLOR_LIGHTGREY, COLOR_BG, 1);
    } else {
      // RAINBOW, BREATHE, THUNDER, PARTY: 特效速度 SPD (1 ~ 100)
      drawTechBar(44, y + 1, 40, 9, lightCfg.speed, 100,
                  sel ? (isEditing ? COLOR_AMBER : COLOR_CYAN) : COLOR_AMBER);
      drawTacticalPrintf(88, y + 2, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d",
                         lightCfg.speed);
    }
  }
}

void updateMotorSubpage() {
  // 1. 即時行程與 ToF 測距區
  int yTof = 20;
  drawTacticalString(16, yTof, "DIST // TOF", COLOR_LIGHTGREY, COLOR_BG, 1);
  tft.fillRect(72, yTof, 40, 8, COLOR_BG);
  if (vl53l0xReady && currentTofDistMm > 0.0f) {
    drawTacticalPrintf(76, yTof, COLOR_CYAN, COLOR_BG, 1, "%4.1fMM",
                       currentTofDistMm);
    int strokeMm =
        constrain((int)(currentTofDistMm - CFG_DEFAULT_Y_MIN_MM + 0.5f), 0,
                  (int)CFG_DEFAULT_STROKE_SPAN_MM);
    drawTechBar(16, yTof + 9, 96, 6, strokeMm, (int)CFG_DEFAULT_STROKE_SPAN_MM,
                COLOR_CYAN);
  } else {
    drawTacticalString(76, yTof, "---.-MM", COLOR_DARKGREY, COLOR_BG, 1);
    drawTechBar(16, yTof + 9, 96, 6, 0, (int)CFG_DEFAULT_STROKE_SPAN_MM,
                COLOR_DARKGREY);
  }

  // 2. 指定位置 (Row 0: TARGET POS)
  int yTgt = 38;
  bool isRow0 = (cursorRow == 0);
  uint16_t tgtTitleCol =
      isRow0 ? (isEditing ? COLOR_AMBER : COLOR_CYAN) : COLOR_LIGHTGREY;
  uint16_t tgtBarCol =
      isRow0 ? (isEditing ? COLOR_AMBER : COLOR_CYAN) : COLOR_MIDGREY;
  const char *tgtPrefix =
      isRow0 ? (isEditing ? "# TARGET" : "> TARGET") : "  TARGET";
  tft.fillRect(16, yTgt, 56, 8, COLOR_BG);
  drawTacticalString(16, yTgt, tgtPrefix, tgtTitleCol, COLOR_BG, 1);
  tft.fillRect(72, yTgt, 40, 8, COLOR_BG);
  drawTacticalPrintf(80, yTgt, tgtTitleCol, COLOR_BG, 1, "%3d%%",
                     motorCfg.targetPos);
  drawTechBar(16, yTgt + 9, 96, 8, motorCfg.targetPos, 100, tgtBarCol);

  // 3. 手動轉速 (Row 1: VELOCITY)
  int yVel = 58;
  bool isRow1 = (cursorRow == 1);
  uint16_t velTitleCol =
      isRow1 ? (isEditing ? COLOR_AMBER : COLOR_CYAN) : COLOR_LIGHTGREY;
  uint16_t velBarCol =
      isRow1 ? (isEditing ? COLOR_AMBER : COLOR_CYAN) : COLOR_MIDGREY;
  const char *velPrefix =
      isRow1 ? (isEditing ? "# VELOCITY" : "> VELOCITY") : "  VELOCITY";
  tft.fillRect(16, yVel, 60, 8, COLOR_BG);
  drawTacticalString(16, yVel, velPrefix, velTitleCol, COLOR_BG, 1);
  tft.fillRect(72, yVel, 40, 8, COLOR_BG);
  drawTacticalPrintf(80, yVel, velTitleCol, COLOR_BG, 1, "%3d%%",
                     motorCfg.speed);
  drawTechBar(16, yVel + 9, 96, 8, motorCfg.speed, 100, velBarCol);

  // 4. 動態狀態指示線與遙測數據
  tft.drawFastHLine(16, 78, 96, COLOR_MIDGREY);

  int yStat = 82;
  tft.fillRect(16, yStat, 96, 20, COLOR_BG);

  if (motorGuardStatus != GUARD_OK) {
    drawTacticalString(16, yStat, "! GUARD TRIPPED !", COLOR_AMBER, COLOR_BG,
                       1);
    if (motorGuardStatus == GUARD_TRIPPED_OOB) {
      drawTacticalString(16, yStat + 10, "OUT OF BOUNDS", COLOR_WHITE, COLOR_BG,
                         1);
    } else if (motorGuardStatus == GUARD_TRIPPED_STALL) {
      drawTacticalString(16, yStat + 10, "MOTOR STALLED", COLOR_WHITE, COLOR_BG,
                         1);
    } else {
      drawTacticalString(16, yStat + 10, "SENSOR TIMEOUT", COLOR_WHITE,
                         COLOR_BG, 1);
    }
  } else if (motorServoActive) {
    drawTacticalPrintf(16, yStat, COLOR_AMBER, COLOR_BG, 1, "MOVING.. P:%3d",
                       (int)motorCurrentOutPwm);
    float err = motorTargetMm - currentTofDistMm;
    drawTacticalPrintf(16, yStat + 10, COLOR_CYAN, COLOR_BG, 1,
                       "E:%+4.1f V:%+4.1f", err, motorFilteredVelocityMmS);
  } else if (motorServoArrived) {
    drawTacticalString(16, yStat, "ARRIVED! [PWM:0]", COLOR_GREEN, COLOR_BG, 1);
    drawTacticalPrintf(16, yStat + 10, COLOR_LIGHTGREY, COLOR_BG, 1,
                       "HOLD AT %4.1fMM", currentTofDistMm);
  } else if (motorCfg.speed > 0) {
    drawTacticalPrintf(16, yStat, COLOR_CYAN, COLOR_BG, 1, "MANUAL PWM:%3d",
                       (int)motorCurrentOutPwm);
    int rpm = map(motorCfg.speed, 0, 100, 0, 4800);
    drawTacticalPrintf(16, yStat + 10, COLOR_LIGHTGREY, COLOR_BG, 1,
                       "SPEED %4d RPM", rpm);
  } else {
    drawTacticalString(16, yStat, "IDLE [PWM: 0]", COLOR_LIGHTGREY, COLOR_BG,
                       1);
    float targetPhysMm = calcTargetMm(motorCfg.targetPos);
    drawTacticalPrintf(16, yStat + 10, COLOR_LIGHTGREY, COLOR_BG, 1,
                       "TARGET: %4.1fMM", targetPhysMm);
  }
}

void updateSensorsSubpage() {
  drawTacticalPrintf(16, 24, COLOR_LIGHTGREY, COLOR_BG, 1, "SYS TEMP: %2.1fC",
                     sysTemp);
  drawTechBar(16, 36, 96, 7, (int)sysTemp, 80, COLOR_AMBER);

  drawTacticalPrintf(16, 50, COLOR_LIGHTGREY, COLOR_BG, 1, "CURR 1  : %2.2fA",
                     sysCurr1);
  drawTechBar(16, 62, 96, 7, (int)(sysCurr1 * 10), 200, COLOR_CYAN);

  drawTacticalPrintf(16, 76, COLOR_LIGHTGREY, COLOR_BG, 1, "CURR 2  : %2.2fA",
                     sysCurr2);
  drawTechBar(16, 88, 96, 7, (int)(sysCurr2 * 10), 50, COLOR_AMBER);
}

const char *getSubpageTitle() {
  if (carouselIndex == PAGE_LIGHT)
    return "LIGHT";
  if (carouselIndex == PAGE_MOTOR)
    return "MOTOR";
  if (carouselIndex == PAGE_SENSORS)
    return "THERMAL";
  return "";
}

const char *getHomeCardTitle() {
  if (carouselIndex == PAGE_LIGHT)
    return ">LIGHT";
  if (carouselIndex == PAGE_MOTOR)
    return ">MOTOR";
  if (carouselIndex == PAGE_SENSORS)
    return ">THERMAL";
  return "";
}

// 繪製子分頁上排戰術標題列 (純大標題 LIGHT / MOTOR / THERMAL，無多餘小字與雜圖)
void drawSubpageTopBar() {
  tft.fillRect(0, 0, 128, 16, COLOR_BG);

  // 戰術風格分隔線 (y=16) 附琥珀色定位點
  tft.drawFastHLine(0, 16, 40, COLOR_MIDGREY);
  tft.drawFastHLine(44, 16, 40, COLOR_CYAN);
  tft.drawFastHLine(88, 16, 40, COLOR_MIDGREY);
  tft.drawPixel(42, 16, COLOR_AMBER);
  tft.drawPixel(86, 16, COLOR_AMBER);

  drawTacticalString(6, 4, getSubpageTitle(), COLOR_AMBER, COLOR_BG, 1);
}

// 進場動畫：中樞標題平順向上滑動取代上排，徹底抹除八邊形外框
void playSubpageEnterTransition() {
  const char *homeTitle = getHomeCardTitle();
  const char *subTitle = getSubpageTitle();

  // 1. 中樞圖表內容向下掃描平順淡出 (y: 34 -> 112)
  for (int y = 34; y < 112; y += 12) {
    int h = min(12, 114 - y);
    tft.fillRect(10, y, 108, h, COLOR_BG);
    tft.drawFastHLine(10, y + h - 1, 108, COLOR_CYAN);
    delay(5);
    tft.drawFastHLine(10, y + h - 1, 108, COLOR_BG);
  }

  // 2. 徹底抹除中央八邊形科技外框 (包含切角與內部裝飾，分頁不使用八邊形框)
  tft.fillRect(10, 17, 108, 97, COLOR_BG);

  // 3. 大標平順向上滑動取代上排 (y: 22 -> 4, x: 20 -> 6)
  int startX = 20, startY = 22;
  int targetX = 6, targetY = 4;
  int steps = 5;

  for (int i = 1; i <= steps; i++) {
    int curX = map(i, 0, steps, startX, targetX);
    int curY = map(i, 0, steps, startY, targetY);
    int prevY = map(i - 1, 0, steps, startY, targetY);

    // 抹除上一幀位置
    tft.fillRect(0, prevY, 128, 10, COLOR_BG);

    // 跨越至頂部區域 (y <= 14) 時清空舊的 topBar (電池/WiFi/藍牙)
    if (curY <= 14) {
      tft.fillRect(0, 0, 128, 16, COLOR_BG);
    }

    // 滑動過程中由 >LIGHT 過渡為純大標 LIGHT
    drawTacticalString(curX, curY, (i >= 3) ? subTitle : homeTitle, COLOR_AMBER,
                       COLOR_BG, 1);
    delay(8);
  }

  // 4. 抵達頂部，繪製正式頂部分隔線與大標
  drawSubpageTopBar();

  // 5. 更新兩翼操作圖示 (UP/DOWN) 與底部藥丸列
  drawSideButtons();
  updateGlobalButtonBar();

  // 6. 子分頁各項目平順展開 (無外框包住，簡約乾淨)
  if (carouselIndex == PAGE_LIGHT) {
    for (int i = 0; i < 5; i++) {
      updateLightRow(i);
      delay(10);
    }
  } else if (carouselIndex == PAGE_MOTOR) {
    updateMotorSubpage();
  } else if (carouselIndex == PAGE_SENSORS) {
    updateSensorsSubpage();
  }
}

// 退場動畫：大標由上排平順滑回中樞，還原首頁頂部狀態列與八邊形外框
void playSubpageExitTransition() {
  const char *homeTitle = getHomeCardTitle();
  const char *subTitle = getSubpageTitle();

  // 1. 子分頁內部各列由下往上快速抹除
  for (int y = 110; y >= 18; y -= 16) {
    int h = min(16, y - 17);
    tft.fillRect(10, y - h + 1, 108, h, COLOR_BG);
    delay(5);
  }

  // 2. 標題由上排平順滑回中樞位置 (y: 4 -> 22, x: 6 -> 20)
  int startX = 6, startY = 4;
  int targetX = 20, targetY = 22;
  int steps = 5;

  for (int i = 1; i <= steps; i++) {
    int curX = map(i, 0, steps, startX, targetX);
    int curY = map(i, 0, steps, startY, targetY);
    int prevY = map(i - 1, 0, steps, startY, targetY);

    tft.fillRect(0, prevY, 128, 10, COLOR_BG);
    drawTacticalString(curX, curY, (i <= 2) ? subTitle : homeTitle, COLOR_AMBER,
                       COLOR_BG, 1);
    delay(8);
  }

  // 3. 還原首頁頂部狀態列 (電池、WiFi、藍牙) 與中樞八邊形科技框
  drawTopBar();
  drawTechCentralFrame(blinkState);
  drawCentralCardContent();

  // 4. 還原左右切換翼鍵與按鈕列
  drawSideButtons();
  updateGlobalButtonBar();
}

// 子分頁介面 (初次進入或切換頁面時全畫)
void drawSubpageScreen(bool animate = false) {
  if (animate) {
    playSubpageEnterTransition();
    return;
  }

  tft.fillScreen(COLOR_BG);
  drawSubpageTopBar();

  if (carouselIndex == PAGE_LIGHT) {
    for (int i = 0; i < 5; i++) {
      updateLightRow(i);
    }
  } else if (carouselIndex == PAGE_MOTOR) {
    updateMotorSubpage();
  } else if (carouselIndex == PAGE_SENSORS) {
    updateSensorsSubpage();
  }

  drawSideButtons();
  updateGlobalButtonBar();
}

// ==========================================================
// 電源管理與 NVS 偏好設定 (Power Management & Preferences)
// ==========================================================

bool isAnyButtonPressed() {
  return (digitalRead(PIN_KEY_A) == LOW || digitalRead(PIN_KEY_B) == LOW ||
          digitalRead(PIN_KEY_C) == LOW || digitalRead(PIN_KEY_D) == LOW);
}

void loadPreferences() {
  lightCfg.style = preferences.getUChar("light_style", MODE_STATIC);
  if (lightCfg.style >= TOTAL_LIGHT_MODES)
    lightCfg.style = MODE_STATIC;

  lightCfg.hue = preferences.getUShort("light_hue", CFG_DEFAULT_HUE);
  if (lightCfg.hue >= 360)
    lightCfg.hue = CFG_DEFAULT_HUE;

  lightCfg.sat = preferences.getUChar("light_sat", CFG_DEFAULT_SAT);
  if (lightCfg.sat > 100)
    lightCfg.sat = CFG_DEFAULT_SAT;

  lightCfg.val = preferences.getUChar("light_val", CFG_DEFAULT_VAL);
  if (lightCfg.val > 100)
    lightCfg.val = CFG_DEFAULT_VAL;

  lightCfg.speed = preferences.getChar("light_spd", CFG_DEFAULT_LIGHT_SPEED);
  if (lightCfg.speed < -50 || lightCfg.speed > 100)
    lightCfg.speed = CFG_DEFAULT_LIGHT_SPEED;

  motorCfg.speed = preferences.getUChar("motor_spd", CFG_DEFAULT_MOTOR_SPEED);
  if (motorCfg.speed > 100)
    motorCfg.speed = CFG_DEFAULT_MOTOR_SPEED;

  motorCfg.targetPos =
      preferences.getUChar("motor_pos", CFG_DEFAULT_TARGET_POS);
  if (motorCfg.targetPos > 100)
    motorCfg.targetPos = CFG_DEFAULT_TARGET_POS;

  isMuted = preferences.getBool("is_muted", false);

  lightBackup = lightCfg;
  motorBackup = motorCfg;
}

void savePreferences(bool setSleepFlag) {
  preferences.putBool("is_sleep", setSleepFlag);
  preferences.putUChar("light_style", lightCfg.style);
  preferences.putUShort("light_hue", lightCfg.hue);
  preferences.putUChar("light_sat", lightCfg.sat);
  preferences.putUChar("light_val", lightCfg.val);
  preferences.putChar("light_spd", lightCfg.speed);
  preferences.putUChar("motor_spd", motorCfg.speed);
  preferences.putUChar("motor_pos", motorCfg.targetPos);
  preferences.putBool("is_muted", isMuted);
}

void enterDeepSleep(bool saveToPrefs = true) {
  if (saveToPrefs) {
    sfxPowerOff();
    savePreferences(true);
  }

  // 關閉螢幕背光/畫面 (全黑)
  tft.fillScreen(COLOR_BG);

  // 關閉 WS2812 燈條與 NMOS 電源 (GPIO 19 = 0 徹底阻斷待機漏電)
  strip.clear();
  strip.show();
  digitalWrite(PIN_WS2812_EN, LOW);

  // 關閉電池電壓檢測 NMOS (GPIO 2 = 0 徹底阻斷分壓漏電)
  digitalWrite(PIN_BATTERY_EN, LOW);
  pinMode(PIN_BATTERY_EN, OUTPUT);

  // 蜂鳴器確保靜音
  sfxMute();

  // 保持 RTC 外設電源開啟以維持內部上拉
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  rtc_gpio_pullup_en((gpio_num_t)PIN_KEY_A);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_KEY_A);
  rtc_gpio_pullup_en((gpio_num_t)PIN_KEY_B);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_KEY_B);
  rtc_gpio_pullup_en((gpio_num_t)PIN_KEY_C);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_KEY_C);
  rtc_gpio_pullup_en((gpio_num_t)PIN_KEY_D);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_KEY_D);

  // EXT0 喚醒：BACK (Button D, GPIO 27) 立即喚醒
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_KEY_D, 0);
  // 定時器定週期 (50ms) 喚醒檢測其餘按鍵 (Button A, B, C)
  esp_sleep_enable_timer_wakeup(50ULL * 1000ULL);

  // 進入深度睡眠
  esp_deep_sleep_start();
}

void triggerSystemReset() {
  Serial.println("\n[SYSTEM] === 4-BUTTON SIMULTANEOUS PRESS DETECTED: "
                 "HARDWARE SYSTEM RESET ===");

  // 1. 硬體級別重置 TFT 螢幕 (拉低 TFT_RST 重啟 LCD 控制器，修復短路白屏)
  if (TFT_RST >= 0) {
    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, LOW);
    delay(50);
    digitalWrite(TFT_RST, HIGH);
    delay(100);
  }

  // 2. 重新初始化 SPI 匯流排與 ST7735 螢幕晶片
  SPI.end();
  delay(10);
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_144GREENTAB);
  tft.setRotation(TFT_ROTATION);

  // 3. 戰術重設提示畫面
  tft.fillScreen(COLOR_BG);
  tft.drawRect(8, 20, 112, 88, COLOR_AMBER);
  tft.fillRect(9, 21, 110, 86, 0x2104);
  drawTacticalString(20, 36, "SYSTEM RESET", COLOR_AMBER, 0x2104, 1);
  drawTacticalString(16, 54, "RESTORING DEFAULTS", COLOR_WHITE, 0x2104, 1);

  // 播放重置音效 (快速階梯音)
  sfxReset();

  // 進度條動畫
  for (int p = 0; p <= 90; p += 15) {
    drawTechBar(18, 74, 92, 10, p, 90, COLOR_CYAN);
    delay(30);
  }
  delay(100);

  // 4. 恢復所有預設參數
  lightCfg.hue = CFG_DEFAULT_HUE;
  lightCfg.sat = CFG_DEFAULT_SAT;
  lightCfg.val = CFG_DEFAULT_VAL;
  lightCfg.style = MODE_STATIC;
  lightCfg.speed = CFG_DEFAULT_LIGHT_SPEED;
  lightBackup = lightCfg;

  rainAngle = 0.0f;
  thunderState = THUNDER_IDLE;

  stopMotorAll();
  motorCfg.speed = CFG_DEFAULT_MOTOR_SPEED;
  motorCfg.targetPos = CFG_DEFAULT_TARGET_POS;
  motorBackup = motorCfg;
  motorGuardStatus = GUARD_OK;
  tofDistanceMm = -1;

  // 儲存重設後的預設值至 Preferences 並解除靜音
  isMuted = false;
  savePreferences(false);

  // 5. 重置狀態機
  currentState = STATE_HOME;
  carouselIndex = PAGE_LIGHT;
  cursorRow = 0;
  isEditing = false;
  stateSwitchLockout = millis() + 800;

  // 6. 重設溫度感測器請求狀態
  tempConversionPending = false;
  lastTempRequestTime = millis();

  // 7. 清除所有按鍵狀態避免釋放時誤觸
  for (int i = 0; i < 4; i++) {
    btns[i].justPressed = false;
    btns[i].repeatTriggered = false;
    btns[i].stableState = HIGH;
    btns[i].lastReading = HIGH;
  }

  // 8. 重繪 HUD 畫面
  tft.fillScreen(COLOR_BG);
  drawTopBar();
  drawSideButtons();
  drawTechCentralFrame(true);
  updateGlobalButtonBar();
  playPageEntranceAnimation(carouselIndex);
}

void renderFullHomeScreen() {
  tft.fillScreen(COLOR_BG);
  drawTopBar();
  drawSideButtons();
  drawTechCentralFrame(blinkState);
  drawCentralCardContent();
  updateGlobalButtonBar();
}

void calibrateAcs712Zeros() {
  if (!CFG_ACS712_AUTO_ZERO)
    return;

  Serial.println(F("[ACS712] Calibrating 0A Quiescent Baseline..."));

  // 預先讀取以穩定 ADC 內部取樣電容
  for (int i = 0; i < 20; i++) {
    analogReadMilliVolts(PIN_CURRENT_ACS712_1);
    delay(2);
    analogReadMilliVolts(PIN_CURRENT_ACS712_2);
    delay(2);
  }

  uint32_t sum1 = 0, sum2 = 0;
  const int TARE_SAMPLES = 80;
  for (int i = 0; i < TARE_SAMPLES; i++) {
    sum1 += analogReadMilliVolts(PIN_CURRENT_ACS712_1);
    delayMicroseconds(50);
    sum2 += analogReadMilliVolts(PIN_CURRENT_ACS712_2);
    delayMicroseconds(50);
  }

  float avg1 = ((float)sum1 / TARE_SAMPLES) * CFG_ACS712_1_DIVIDER_RATIO;
  float avg2 = ((float)sum2 / TARE_SAMPLES) * CFG_ACS712_2_DIVIDER_RATIO;

  // 驗證讀數是否在合理範圍 (500mV ~ 3100mV) 避免接線斷路時誤歸零
  if (avg1 >= 500.0f && avg1 <= 3100.0f) {
    zeroCurrentMv1 = avg1;
  }
  if (avg2 >= 500.0f && avg2 <= 3100.0f) {
    zeroCurrentMv2 = avg2;
  }

  Serial.printf("[ACS712] Calibrated Zero 1 (GPIO 35): %1.1f mV\n",
                zeroCurrentMv1);
  Serial.printf("[ACS712] Calibrated Zero 2 (GPIO 34): %1.1f mV\n",
                zeroCurrentMv2);
}

// I2C 匯流排主動解鎖：若感測器卡在 ACK/Data 狀態將 SDA 咬死，送出 9 個 SCL
// 脈衝強制釋放
void recoverI2cBus(uint8_t sdaPin, uint8_t sclPin) {
  pinMode(sdaPin, INPUT_PULLUP);
  pinMode(sclPin, OUTPUT);
  digitalWrite(sclPin, HIGH);
  delayMicroseconds(50);

  if (digitalRead(sdaPin) == LOW) {
    Serial.println(F("[I2C RECOVERY] SDA locked LOW! Sending 9 SCL pulses..."));
    for (int i = 0; i < 9; i++) {
      digitalWrite(sclPin, LOW);
      delayMicroseconds(20);
      digitalWrite(sclPin, HIGH);
      delayMicroseconds(20);
      if (digitalRead(sdaPin) == HIGH)
        break;
    }
    pinMode(sdaPin, OUTPUT);
    digitalWrite(sdaPin, LOW);
    delayMicroseconds(20);
    digitalWrite(sclPin, HIGH);
    delayMicroseconds(20);
    digitalWrite(sdaPin, HIGH);
    delayMicroseconds(20);
    pinMode(sdaPin, INPUT_PULLUP);
  }
}

void setup() {
  // 0. 初始化馬達驅動引腳與 PWM 通道
  pinMode(PIN_MOTOR_DIR, OUTPUT);
  digitalWrite(PIN_MOTOR_DIR, LOW);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_MOTOR_PWM, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
  ledcWrite(PIN_MOTOR_PWM, 0);
#else
  ledcSetup(MOTOR_PWM_CH, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
  ledcAttachPin(PIN_MOTOR_PWM, MOTOR_PWM_CH);
  ledcWrite(MOTOR_PWM_CH, 0);
#endif

  // 1. 初始化所有戰術按鍵引腳為內部上拉輸入
  for (int i = 0; i < 4; i++) {
    pinMode(btns[i].pin, INPUT_PULLUP);
  }

  // 2. 開啟 Preferences NVS 偏好設定儲存空間
  preferences.begin("wing_sys", false);

  // 3. 檢查系統先前是否被手動置入待機深度睡眠
  bool wasSleep = preferences.getBool("is_sleep", false);

  if (wasSleep) {
    // 處於待機沉睡狀態：唯有長按任意鍵持續 2 秒 (CFG_WAKE_HOLD_MS)
    // 才喚醒進入活躍模式
    bool isHeldFor2Sec = false;
    unsigned long pressStart = millis();

    while (isAnyButtonPressed()) {
      if (millis() - pressStart >= CFG_WAKE_HOLD_MS) {
        isHeldFor2Sec = true;
        break;
      }
      delay(10);
    }

    if (isHeldFor2Sec) {
      // 成功長按 2 秒：解除待機睡眠狀態
      preferences.putBool("is_sleep", false);
      // 等待使用者放開按鍵，避免開機後立即誤觸選單
      while (isAnyButtonPressed()) {
        delay(10);
      }
    } else {
      // 任意按鍵未按住滿 2
      // 秒（包含插拔電冷開機、誤碰、定時器週期喚醒）：立即重回待機睡眠，不開啟螢幕與外設
      enterDeepSleep(false);
      return;
    }
  }

  // 4. 載入保存的各項參數 (若為意外斷電重開，自動回復上一次的參數)
  loadPreferences();

  Serial.begin(115200);
  delay(100);
  Serial.println("\n==============================================");
  Serial.println("  WING UI - ESP32 Tactical HUD (Active Mode)  ");
  Serial.println("==============================================");
  Serial.println(
      "Display: ST7735 1.44\" (CS:5, DC:22, RES:EN, SCK:18, MOSI:23)");
  Serial.println("Serial Input Simulation Active:");
  Serial.println("  [Q] : Left  (< / UP / -)   -> Button C");
  Serial.println("  [W] : Right (> / DOWN / +) -> Button B");
  Serial.println("  [E] : ENTER (Confirm)      -> Button A");
  Serial.println("  [R] : BACK  (Return)       -> Button D");
  Serial.println("==============================================\n");

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_144GREENTAB);
  tft.setRotation(TFT_ROTATION);

  tft.fillScreen(COLOR_BG);
  drawTopBar();
  drawSideButtons();
  drawTechCentralFrame(true);
  // 初始化 DS18B20 溫度感測器 (設為非同步模式，避免卡頓)
  tempSensor.begin();
  tempSensor.setWaitForConversion(false);
  tempSensor.requestTemperatures();
  lastTempRequestTime = millis();
  tempConversionPending = true;

  // 初始化 VL53L0X 飛行時間測距感測器 (I2C: SDA=GPIO21, SCL=GPIO15)
  recoverI2cBus(PIN_TOF_SDA, PIN_TOF_SCL);
  Wire.begin(PIN_TOF_SDA, PIN_TOF_SCL);
  Wire.setClock(100000);
  delay(50);

  vl53l0xReady = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (lox.begin(VL53L0X_I2C_ADDR, false, &Wire)) {
      vl53l0xReady = true;
      Serial.println(F("[SENSOR] VL53L0X ToF Initialized OK (SDA:21, SCL:15)"));
      break;
    }
    delay(100);
  }
  if (!vl53l0xReady) {
    Serial.println(F("[SENSOR] VL53L0X ToF Init Failed / Not Detected!"));
  } else {
    // 預熱中值與 IIR 濾波器
    for (int i = 0; i < 6; i++) {
      handleTofSampling();
      delay(30);
    }
    lastMotorMeasuredDistMm = currentTofDistMm;
    motorActiveTargetMm = currentTofDistMm;
    motorTargetMm = currentTofDistMm;
  }

  // 初始化 ACS712 電流感測器輸入 (CURR 1: GPIO 35 [20A], CURR 2: GPIO 34 [05B
  // 5A])
  pinMode(PIN_CURRENT_ACS712_1, INPUT);
  analogSetPinAttenuation(PIN_CURRENT_ACS712_1, ADC_11db);
  pinMode(PIN_CURRENT_ACS712_2, INPUT);
  analogSetPinAttenuation(PIN_CURRENT_ACS712_2, ADC_11db);

  // 執行 ACS712 零電流基準校準 (Tare)
  calibrateAcs712Zeros();

  // 初始化 2S 電池電壓檢測 (GPIO 36, 180k:36k 分壓) 與 NMOS 測量致能 (GPIO 2)
  pinMode(PIN_BATTERY_EN, OUTPUT);
  digitalWrite(PIN_BATTERY_EN, LOW); // 預設關閉防漏電
  pinMode(PIN_BATTERY_ADC, INPUT);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  for (int i = 0; i < 6; i++) {
    handleBatterySampling();
    delay(15);
  }

  // 初始化 WS2812 幻彩燈條 (GPIO 16) 與 NMOS 供電開關 (GPIO 19)
  pinMode(PIN_WS2812_EN, OUTPUT);
  if (lightCfg.val > 0) {
    digitalWrite(PIN_WS2812_EN, HIGH);
    delay(50); // 延長開機延時讓電解電容與燈條完全上電穩定
  } else {
    digitalWrite(PIN_WS2812_EN, LOW);
  }
  strip.begin();
  strip.setBrightness(255);
  gpio_set_drive_capability((gpio_num_t)PIN_WS2812, GPIO_DRIVE_CAP_3); // 最大驅動電流 (40mA) 增強長導線上升沿邊緣銳利度
  strip.show();

  // 初始化 WiFi AP 模式與手機 Web 戰術控制台
  initWebHud();

  // 初始化蜂鳴器音效引擎並播放開機未來戰術音
  sfxInit();
  sfxPowerOn();

  updateGlobalButtonBar();
  playPageEntranceAnimation(carouselIndex);
}

void handleTemperatureSampling() {
  unsigned long now = millis();

  // 若已有請求正在進行中，且等待時間超過 750ms（DS18B20 12-bit 轉換所需時間）
  if (tempConversionPending && (now - lastTempRequestTime >= 750)) {
    float readT = tempSensor.getTempCByIndex(0);
    // 檢查數值有效性 (DS18B20 離線或讀取失敗時會回傳 -127 或 85)
    if (readT > -50.0f && readT < 125.0f && readT != 85.0f) {
      if (fabs(readT - sysTemp) >= 0.1f) {
        sysTemp = readT;
        // 如果當前正在看感測器頁面或首頁 THERMAL 卡片，微幅即時更新
        if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_SENSORS) {
          drawSubpageTopBar();
          updateSensorsSubpage();
        } else if (currentState == STATE_HOME &&
                   carouselIndex == PAGE_SENSORS) {
          drawCentralCardContent();
        }
      }
    }
    tempConversionPending = false;
    lastTempRequestTime = now;
  }
  // 每隔 CFG_TEMP_SAMPLE_INTERVAL_MS 發起一次新的溫度轉換請求
  else if (!tempConversionPending &&
           (now - lastTempRequestTime >= CFG_TEMP_SAMPLE_INTERVAL_MS)) {
    tempSensor.requestTemperatures();
    lastTempRequestTime = now;
    tempConversionPending = true;
  }

  // 超溫警報音效循環：若當前溫度超過門檻，每隔 CFG_TEMP_ALARM_INTERVAL_MS
  // 重複播放警告音效
  if (sysTemp >= CFG_TEMP_WARNING_THRESHOLD) {
    if (now - lastThermalAlarmTime >= CFG_TEMP_ALARM_INTERVAL_MS) {
      sfxWarning();
      lastThermalAlarmTime = now;
    }
  }
}

void handleTofSampling() {
  if (!vl53l0xReady)
    return;

  unsigned long now = millis();
  if (now - lastTofSampleTime < CFG_TOF_SAMPLE_INTERVAL_MS)
    return;
  lastTofSampleTime = now;

  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);

  if (measure.RangeStatus != 4) {
    float raw = (float)measure.RangeMilliMeter;
    if (raw >= 20.0f && raw <= 400.0f) {
      // 1. 5-Tap 中值濾波 (消滅離群光學毛刺)
      tofMedianBuf[tofMedianIdx] = raw;
      tofMedianIdx = (tofMedianIdx + 1) % TOF_MEDIAN_WINDOW;
      if (tofMedianIdx == 0)
        tofMedianFilled = true;
      int count = tofMedianFilled ? TOF_MEDIAN_WINDOW : tofMedianIdx;
      float med = getTofMedian(tofMedianBuf, count);

      // 2. 一階 IIR 低通濾波 (撫平毫米級隨機噪訊)
      if (!iirTofInitialized) {
        iirTofDistanceMm = med;
        iirTofInitialized = true;
      } else {
        iirTofDistanceMm =
            TOF_IIR_ALPHA * iirTofDistanceMm + (1.0f - TOF_IIR_ALPHA) * med;
      }

      currentTofDistMm = iirTofDistanceMm;
      lastTofValidReadTime = now;
      int16_t newDist = (int16_t)(currentTofDistMm + 0.5f);

      // 數值有變動時局部更新畫面
      if (newDist != tofDistanceMm) {
        tofDistanceMm = newDist;
        if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR) {
          updateMotorSubpage();
        } else if (currentState == STATE_HOME && carouselIndex == PAGE_MOTOR) {
          int cx = 14, cy = 19;
          drawTacticalPrintf(cx + 52, cy + 3, COLOR_CYAN, COLOR_BG, 1, "%4dMM",
                             tofDistanceMm);
        }
      }
    }
  }
}

// 讀取 ACS712 感測器原始輸出電壓 (mV)
float readAcs712RawMv(uint8_t pin, float dividerRatio) {
  // 切換通道時先執行一次讀取以釋放/充放內部取樣電容，徹底消除多通道串擾
  analogReadMilliVolts(pin);
  delayMicroseconds(30);

  uint32_t sumMv = 0;
  const int SAMPLES = 64;
  for (int i = 0; i < SAMPLES; i++) {
    sumMv += analogReadMilliVolts(pin);
    delayMicroseconds(10);
  }
  float pinMv = (float)sumMv / SAMPLES;
  return pinMv * dividerRatio;
}

void handleCurrentSampling() {
  unsigned long now = millis();
  if (now - lastCurrentSampleTime < CFG_CURRENT_SAMPLE_INTERVAL_MS)
    return;
  lastCurrentSampleTime = now;

  // 採樣感測器 1 (ACS712-20A, GPIO 35) 與感測器 2 (ACS712T-05B, GPIO 34) 原始毫伏
  float rawMv1 = readAcs712RawMv(PIN_CURRENT_ACS712_1, CFG_ACS712_1_DIVIDER_RATIO);
  float rawMv2 = readAcs712RawMv(PIN_CURRENT_ACS712_2, CFG_ACS712_2_DIVIDER_RATIO);

  // --- C1 (WS2812 燈條 20A) 動態零點追蹤與電流計算 ---
  float rawCurr1 = 0.0f;
  bool lightIsOff = (lightCfg.val == 0 || digitalRead(PIN_WS2812_EN) == LOW);
  if (lightIsOff) {
    // 燈條關閉狀態下物理電流必為 0A，主動動態追蹤溫漂與零點電壓
    if (rawMv1 >= 500.0f && rawMv1 <= 3200.0f) {
      zeroCurrentMv1 = (zeroCurrentMv1 * 0.90f) + (rawMv1 * 0.10f);
    }
    rawCurr1 = 0.0f;
  } else {
    // 燈條亮起時凍結零點基準，計算即時電流
    rawCurr1 = fabs(rawMv1 - zeroCurrentMv1) / CFG_ACS712_1_SENSITIVITY_MV_PER_A;
    if (rawCurr1 < CFG_ACS712_1_DEADBAND_AMPS) {
      rawCurr1 = 0.0f;
    }
  }

  // --- C2 (馬達驅動/H橋 S09 12V 05B) 動態零點追蹤與電流計算 ---
  float rawCurr2 = 0.0f;
  bool motorIsIdle = (motorCurrentOutPwm <= 0.01f && !motorServoActive && motorCfg.speed == 0);
  if (motorIsIdle) {
    // 馬達怠速靜止且無 PWM 輸出時物理電流必為 0A，主動動態追蹤溫漂與零點電壓
    if (rawMv2 >= 500.0f && rawMv2 <= 3200.0f) {
      zeroCurrentMv2 = (zeroCurrentMv2 * 0.90f) + (rawMv2 * 0.10f);
    }
    rawCurr2 = 0.0f;
  } else {
    // 馬達運轉時凍結零點基準，精準計算負載電流
    rawCurr2 = fabs(rawMv2 - zeroCurrentMv2) / CFG_ACS712_2_SENSITIVITY_MV_PER_A;
    if (rawCurr2 < CFG_ACS712_2_DEADBAND_AMPS) {
      rawCurr2 = 0.0f;
    }
  }

  // 低通濾波穩定讀數 (70% 舊值 + 30% 新值)
  float filt1 = lightIsOff ? 0.0f : ((sysCurr1 * 0.7f) + (rawCurr1 * 0.3f));
  if (filt1 < 0.04f)
    filt1 = 0.0f;

  float filt2 = motorIsIdle ? 0.0f : ((sysCurr2 * 0.7f) + (rawCurr2 * 0.3f));
  if (filt2 < 0.03f)
    filt2 = 0.0f;

  bool c1Changed = (fabs(filt1 - sysCurr1) >= 0.03f);
  bool c2Changed = (fabs(filt2 - sysCurr2) >= 0.02f);

  if (c1Changed)
    sysCurr1 = filt1;
  if (c2Changed)
    sysCurr2 = filt2;

  // 定期輸出 Serial 監控數值 (包含原始毫伏與零點基準，方便即時觀察)
  static unsigned long lastCurrLogTime = 0;
  if (now - lastCurrLogTime >= 1500) {
    lastCurrLogTime = now;
    Serial.printf("[CURR] C1 (20A, GPIO35): %2.2fA (Raw: %.0fmV, Zero: %.0fmV) | C2 (05B, GPIO34): %2.2fA (Raw: %.0fmV, Zero: %.0fmV)\n",
                  sysCurr1, rawMv1, zeroCurrentMv1, sysCurr2, rawMv2, zeroCurrentMv2);
  }

  if (c1Changed || c2Changed) {
    if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_SENSORS) {
      updateSensorsSubpage();
    } else if (currentState == STATE_HOME && carouselIndex == PAGE_SENSORS) {
      int cx = 14, cy = 19;
      int bY = cy + 49, bW = 39;

      if (c1Changed) {
        int c1BoxX = cx + 8;
        tft.fillRect(c1BoxX + 1, bY + 11, bW - 2, 14, COLOR_BG);
        drawTacticalPrintf(c1BoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1,
                           "%2.2fA", sysCurr1);
      }
      if (c2Changed) {
        int c2BoxX = cx + 51;
        tft.fillRect(c2BoxX + 1, bY + 11, bW - 2, 14, COLOR_BG);
        drawTacticalPrintf(c2BoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1,
                           "%2.2fA", sysCurr2);
      }
    }
  }
}

void handleBatterySampling() {
  unsigned long now = millis();
  if (now - lastBatSampleTime < CFG_BAT_SAMPLE_INTERVAL_MS)
    return;
  lastBatSampleTime = now;

  // 1. 打通 NMOS 導通分壓電路 (GPIO 2 = HIGH)
  digitalWrite(PIN_BATTERY_EN, HIGH);
  delay(15); // 給予充足時間讓 RC 濾波電容 (180k//36k) 與 NMOS 完全導通穩定

  // 預讀一次釋放內部取樣電容，消除多通道取樣串擾
  analogReadMilliVolts(PIN_BATTERY_ADC);
  delayMicroseconds(50);

  // 2. 讀取 GPIO 36 類比電壓 (mV)
  uint32_t sumMv = 0;
  const int SAMPLES = 32;
  for (int i = 0; i < SAMPLES; i++) {
    sumMv += analogReadMilliVolts(PIN_BATTERY_ADC);
    delayMicroseconds(50);
  }

  // 3. 採樣完成立即關閉 NMOS，阻斷漏電 (GPIO 2 = LOW)
  digitalWrite(PIN_BATTERY_EN, LOW);

  float pinMv = (float)sumMv / SAMPLES;
  float batVolt = (pinMv * CFG_BAT_DIVIDER_RATIO) / 1000.0f; // 換算實際電池電壓

  // 若為開機初次採樣，直接賦值避免 7.4V 平滑延遲
  static bool firstSample = true;
  if (firstSample) {
    firstSample = false;
    sysBatteryVolt = batVolt;
  } else {
    // 80% 歷史平滑 + 20% 當前測量
    sysBatteryVolt = (sysBatteryVolt * 0.8f) + (batVolt * 0.2f);
  }

  // 映射至 2S 電池百分比 (6.4V ~ 8.4V)
  float pct = ((sysBatteryVolt - CFG_BAT_VOLT_MIN) /
               (CFG_BAT_VOLT_MAX - CFG_BAT_VOLT_MIN)) *
              100.0f;
  pct = constrain(pct, 0.0f, 100.0f);
  int newPct = (int)(pct + 0.5f);

  if (newPct != sysBatteryPct) {
    sysBatteryPct = newPct;
    if (currentState == STATE_HOME) {
      drawTopBar();
    }
  }

  // 每 2 秒輸出電池除錯數據至 Serial
  static unsigned long lastBatLogTime = 0;
  if (now - lastBatLogTime >= 2000) {
    lastBatLogTime = now;
    Serial.printf("[BATTERY] Raw ADC: %1.1fmV | BatVolt: %1.2fV | Filtered: %1.2fV (%d%%)\n",
                  pinMv, batVolt, sysBatteryVolt, sysBatteryPct);
  }
}

void handleMotorControl() {
  unsigned long now = millis();
  if (now - lastMotorCtrlTimeMs < MOTOR_CTRL_PERIOD_MS)
    return;
  float dt = (now - lastMotorCtrlTimeMs) / 1000.0f;
  lastMotorCtrlTimeMs = now;

  // 速度估算 (微分阻尼濾波)
  if (currentTofDistMm > 0.0f) {
    float rawVel = (currentTofDistMm - lastMotorMeasuredDistMm) / dt;
    lastMotorMeasuredDistMm = currentTofDistMm;
    motorFilteredVelocityMmS = 0.7f * motorFilteredVelocityMmS + 0.3f * rawVel;
  }

  // 1. 伺服移動模式 (Closed-Loop Servo)
  if (motorServoActive) {
    // 感測器失聯保護
    if (now - lastTofValidReadTime > SENSOR_TIMEOUT_MS) {
      emergencyStopMotor(GUARD_TRIPPED_SENSOR_TIMEOUT);
      if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR)
        updateMotorSubpage();
      return;
    }

    // 物理行程超限保護
    if (currentTofDistMm < (CFG_DEFAULT_Y_MIN_MM - GUARDRAIL_MARGIN_MM) ||
        currentTofDistMm > (CFG_DEFAULT_Y_MAX_MM + GUARDRAIL_MARGIN_MM)) {
      emergencyStopMotor(GUARD_TRIPPED_OOB);
      if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR)
        updateMotorSubpage();
      return;
    }

    // 堵轉保護
    if (motorCurrentOutPwm >= GUARDRAIL_STALL_PWM) {
      if (motorHighEffortStartTime == 0) {
        motorHighEffortStartTime = now;
        motorStallCheckStartDist = currentTofDistMm;
      } else if (now - motorHighEffortStartTime > GUARDRAIL_STALL_MS) {
        if (fabs(currentTofDistMm - motorStallCheckStartDist) <
            GUARDRAIL_STALL_MIN_DY) {
          emergencyStopMotor(GUARD_TRIPPED_STALL);
          if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR)
            updateMotorSubpage();
          return;
        }
      }
    } else {
      motorHighEffortStartTime = 0;
    }

    // 平滑軌跡步進 (30 mm/s)
    float maxStep = maxProfileVelMmS * dt;
    if (motorActiveTargetMm < motorTargetMm) {
      motorActiveTargetMm += maxStep;
      if (motorActiveTargetMm > motorTargetMm)
        motorActiveTargetMm = motorTargetMm;
    } else if (motorActiveTargetMm > motorTargetMm) {
      motorActiveTargetMm -= maxStep;
      if (motorActiveTargetMm < motorTargetMm)
        motorActiveTargetMm = motorTargetMm;
    }

    // Profile Anti-Windup (最多超前/落後 6.0mm)
    const float MAX_PROFILE_LAG_MM = 6.0f;
    motorActiveTargetMm =
        constrain(motorActiveTargetMm, currentTofDistMm - MAX_PROFILE_LAG_MM,
                  currentTofDistMm + MAX_PROFILE_LAG_MM);

    float error = motorActiveTargetMm - currentTofDistMm;
    float absErr = fabs(error);
    float targetErr = fabs(motorTargetMm - currentTofDistMm);

    // 到達判斷：若距離最終目標 <= 2.0mm 且速度已降至靜止範圍 (< 6.0mm/s)
    // 【核心要求】：到定點後切回一般模式的 pwm0
    if (targetErr <= 2.0f && fabs(motorFilteredVelocityMmS) < 6.0f) {
      setMotorRaw(true, 0);
      motorCurrentOutPwm = 0.0f;
      motorIntegralErr = 0.0f;
      motorServoActive = false; // 關閉伺服狀態
      motorServoArrived = true; // 標記已成功抵達
      motorCfg.speed = 0;       // 切回一般模式的 pwm 0
      sfxEnter();               // 抵達戰術提示音
      Serial.printf("[SERVO ARRIVED] Target reached: %0.1f mm. Power cut to "
                    "PWM: 0 (Normal mode idle)\n",
                    currentTofDistMm);
      if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR) {
        updateMotorSubpage();
      }
      return;
    }

    // PID + 前饋計算
    float baseFf = getScotchYokeFeedforward(currentTofDistMm);
    float ffWeight = 1.0f;
    if (absErr < 8.0f) {
      ffWeight = (absErr - 2.0f) / 6.0f;
      if (ffWeight < 0.0f)
        ffWeight = 0.0f;
    }
    float effectiveFf = baseFf * ffWeight;

    float pOut = motorKp * absErr;
    bool forward = (error > 0.0f);

    if (absErr > 2.0f && absErr < 6.0f) {
      motorIntegralErr += (forward ? 1.0f : -1.0f) * absErr * dt;
      motorIntegralErr = constrain(motorIntegralErr, -15.0f, 15.0f);
    } else {
      motorIntegralErr = 0.0f;
    }
    float iOut = motorKi * motorIntegralErr;

    float dDamping = forward ? (-motorKd * motorFilteredVelocityMmS)
                             : (+motorKd * motorFilteredVelocityMmS);

    float desiredCmd = effectiveFf + pOut + (forward ? iOut : -iOut) + dDamping;
    desiredCmd = constrain(desiredCmd, 30.0f, 220.0f);

    // 斜率限制
    if (desiredCmd > motorCurrentOutPwm + MAX_PWM_SLEW_UP) {
      motorCurrentOutPwm += MAX_PWM_SLEW_UP;
    } else if (desiredCmd < motorCurrentOutPwm - MAX_PWM_SLEW_DOWN) {
      motorCurrentOutPwm -= MAX_PWM_SLEW_DOWN;
    } else {
      motorCurrentOutPwm = desiredCmd;
    }

    uint8_t outDuty = (uint8_t)constrain(motorCurrentOutPwm, 0.0f, 255.0f);

    // 頂點翻轉死點保護
    if (forward && currentTofDistMm > (CFG_DEFAULT_Y_MAX_MM - 8.0f) &&
        motorFilteredVelocityMmS < -8.0f) {
      setMotorRaw(true, 0);
      motorCurrentOutPwm = 0.0f;
      motorIntegralErr = 0.0f;
      motorActiveTargetMm = currentTofDistMm;
    } else {
      setMotorRaw(forward, outDuty);
    }

    // 在子分頁中動態刷新狀態與即時 PWM
    static unsigned long lastMotorUiRefresh = 0;
    if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR &&
        (now - lastMotorUiRefresh >= 120)) {
      lastMotorUiRefresh = now;
      updateMotorSubpage();
    }
  }
  // 2. 一般手動模式 (Normal Mode)
  else {
    if (motorCfg.speed > 0 && motorGuardStatus == GUARD_OK) {
      uint8_t duty = map(motorCfg.speed, 0, 100, 0, 255);
      setMotorRaw(true, duty);
      motorCurrentOutPwm = duty;
    } else {
      setMotorRaw(true, 0);
      motorCurrentOutPwm = 0.0f;
    }
  }
}

void loop() {
  scanButtons();
  handleTemperatureSampling();
  handleTofSampling();
  handleCurrentSampling();
  handleBatterySampling();
  handleLightAnimation();
  handleMotorControl();
  handleWebHud();

  unsigned long now = millis();

  // 四顆按鍵同時持續按住 1 秒 (CFG_RESET_PRESS_DURATION_MS) 檢測 System Reset
  bool allFourDown =
      (btns[0].stableState == LOW && btns[1].stableState == LOW &&
       btns[2].stableState == LOW && btns[3].stableState == LOW);

  if (allFourDown && now > stateSwitchLockout) {
    if (resetPressStartTime == 0) {
      resetPressStartTime = now;
    } else if (now - resetPressStartTime >= CFG_RESET_PRESS_DURATION_MS) {
      resetPressStartTime = 0;
      triggerSystemReset();
      return;
    }
  } else {
    resetPressStartTime = 0;
  }

  // 左右雙鍵 (Button B: 右, Button C: 左) 同時長按 1.5 秒 (CFG_MUTE_HOLD_MS)
  // 切換靜音
  bool dualLeftRightDown = (btns[1].stableState == LOW &&
                            btns[2].stableState == LOW && !allFourDown);
  if (dualLeftRightDown && now > stateSwitchLockout) {
    if (mutePressStartTime == 0) {
      mutePressStartTime = now;
      muteToggleTriggered = false;
    } else if (!muteToggleTriggered &&
               (now - mutePressStartTime >= CFG_MUTE_HOLD_MS)) {
      muteToggleTriggered = true;
      isMuted = !isMuted;
      if (!isMuted) {
        // 解除靜音提示音
        sfxTone(3500, 30);
      }
      if (currentState == STATE_HOME) {
        if (isMuted) {
          tft.drawBitmap(42, 4, icon_mute_8x8, 8, 8, COLOR_AMBER, COLOR_BG);
        } else {
          tft.fillRect(42, 4, 8, 8, COLOR_BG);
        }
      }
    }
  } else {
    mutePressStartTime = 0;
    muteToggleTriggered = false;
  }

  // ================= 狀態 1：首頁 (HUD HOME) =================
  if (currentState == STATE_HOME) {
    // 檢測在首頁長按 BACK (Button D, PIN_KEY_D) 3 秒進入待機深度睡眠
    if (btns[3].stableState == LOW && !allFourDown && !dualLeftRightDown &&
        now > stateSwitchLockout) {
      if (sleepPressStartTime == 0) {
        sleepPressStartTime = now;
      } else if (now - sleepPressStartTime >= CFG_SLEEP_HOLD_MS) {
        sleepPressStartTime = 0;
        enterDeepSleep(true); // 存入當前設定至 Preferences 並沉睡
        return;
      }
    } else {
      sleepPressStartTime = 0;
    }

    // [右翼 >] 按鍵 B (W / B)：向右切換 (roll left)
    if (btns[1].justPressed && btns[2].stableState == HIGH &&
        !dualLeftRightDown) {
      sfxNavigate();
      playCyberTransition(false);
      carouselIndex = (carouselIndex + 1) % TOTAL_CAROUSEL;
      drawSideButtons();
      drawTechCentralFrame(blinkState);
      playPageEntranceAnimation(carouselIndex);
    }
    // [左翼 <] 按鍵 C (Q / C)：向左切換 (roll right)
    else if (btns[2].justPressed && btns[1].stableState == HIGH &&
             !dualLeftRightDown) {
      sfxNavigate();
      playCyberTransition(true);
      carouselIndex = (carouselIndex - 1 + TOTAL_CAROUSEL) % TOTAL_CAROUSEL;
      drawSideButtons();
      drawTechCentralFrame(blinkState);
      playPageEntranceAnimation(carouselIndex);
    }
    // [底右 ENTER] 按鍵 A (E / A)：選入分頁
    else if (btns[0].justPressed && !allFourDown) {
      sfxEnter();
      currentState = STATE_SUBPAGE;
      cursorRow = 0;
      if (carouselIndex == PAGE_LIGHT) {
        while (!isLightRowSelectable(cursorRow)) {
          cursorRow = (cursorRow + 1) % 5;
        }
      }
      isEditing = false;
      lightBackup = lightCfg;
      motorBackup = motorCfg;
      stateSwitchLockout = millis() + 200;
      drawSubpageScreen(true);
    }

    if (millis() - lastBlinkTime > 350) {
      blinkState = !blinkState;
      lastBlinkTime = millis();
      drawTechCentralFrame(blinkState);
    }
  }

  // ================= 狀態 2：子分頁 (SUBPAGE) =================
  else if (currentState == STATE_SUBPAGE) {
    if (carouselIndex == PAGE_LIGHT) {
      if (!isEditing) {
        // [左翼 UP] 按鍵 C (Q): 游標上移 (Navigate 音效，自動跳過灰底停用項目)
        if (btns[2].justPressed) {
          sfxNavigate();
          int oldRow = cursorRow;
          do {
            cursorRow = (cursorRow - 1 + 5) % 5;
          } while (!isLightRowSelectable(cursorRow));
          updateLightRow(oldRow);
          updateLightRow(cursorRow);
        }
        // [右翼 DOWN] 按鍵 B (W): 游標下移 (Navigate
        // 音效，自動跳過灰底停用項目)
        if (btns[1].justPressed) {
          sfxNavigate();
          int oldRow = cursorRow;
          do {
            cursorRow = (cursorRow + 1) % 5;
          } while (!isLightRowSelectable(cursorRow));
          updateLightRow(oldRow);
          updateLightRow(cursorRow);
        }
        // [底右 ENTER] 按鍵 A (E): 確定進入編輯
        if (btns[0].justPressed) {
          if (!isLightRowSelectable(cursorRow)) {
            // 停用項目不進入編輯
          } else if (lightCfg.style == MODE_STATIC && cursorRow == 4) {
            // STATIC 模式下 CLR 為純 VIEW 視窗，不進入編輯
          } else {
            sfxEnter();
            isEditing = true;
            lightBackup = lightCfg;
            updateLightRow(cursorRow);
            drawSideButtons();
            updateGlobalButtonBar();
          }
        }
        // [底左 BACK] 按鍵 D (R): 返回首頁
        if (btns[3].justPressed) {
          sfxBack();
          currentState = STATE_HOME;
          stateSwitchLockout = millis() + 200;
          playSubpageExitTransition();
        }
      } else {
        // [右翼 +] 按鍵 B (W): 數值增加 (靜音調節)
        bool bTrigger =
            btns[1].justPressed ||
            (btns[1].repeatTriggered && (cursorRow == 0 || cursorRow == 1 ||
                                         cursorRow == 2 || cursorRow == 4));
        if (bTrigger) {
          if (cursorRow == 0) {
            if (lightCfg.style != MODE_RAINBOW &&
                lightCfg.style != MODE_PARTY) {
              lightCfg.hue = (lightCfg.hue + CFG_STEP_HUE) % 360;
            }
          }
          if (cursorRow == 1 && lightCfg.style != MODE_PARTY &&
              (btns[1].justPressed || btns[1].repeatTriggered))
            lightCfg.sat =
                constrain(lightCfg.sat + CFG_STEP_SATURATION, 0, 100);
          if (cursorRow == 2 &&
              (btns[1].justPressed || btns[1].repeatTriggered)) {
            if (sysCurr1 >= CFG_OVERCURRENT_LIMIT_A) {
              sfxWarning(); // 18A 電流防護：阻止調高亮度並發出警報音
            } else {
              lightCfg.val =
                  constrain(lightCfg.val + CFG_STEP_BRIGHTNESS, 0, 100);
            }
          }
          if (cursorRow == 3 && btns[1].justPressed) {
            lightCfg.style = (lightCfg.style + 1) % TOTAL_LIGHT_MODES;
            for (int r = 0; r < 5; r++)
              updateLightRow(r);
          }
          if (cursorRow == 4) {
            if (lightCfg.style == MODE_RAINING) {
              lightCfg.speed =
                  constrain(lightCfg.speed + CFG_STEP_SPEED, -50, 50);
            } else if (lightCfg.style != MODE_STATIC) {
              lightCfg.speed =
                  constrain(lightCfg.speed + CFG_STEP_SPEED, 1, 100);
            }
          }
          if (cursorRow != 3) {
            updateLightRow(cursorRow);
            if (cursorRow == 0 && lightCfg.style != MODE_RAINBOW &&
                lightCfg.style != MODE_PARTY) {
              updateLightRow(1);
              updateLightRow(2);
            }
          }
          drawSubpageTopBar();
        }
        // [左翼 -] 按鍵 C (Q): 數值減少 (靜音調節)
        bool cTrigger =
            btns[2].justPressed ||
            (btns[2].repeatTriggered && (cursorRow == 0 || cursorRow == 1 ||
                                         cursorRow == 2 || cursorRow == 4));
        if (cTrigger) {
          if (cursorRow == 0) {
            if (lightCfg.style != MODE_RAINBOW &&
                lightCfg.style != MODE_PARTY) {
              lightCfg.hue = (lightCfg.hue - CFG_STEP_HUE + 360) % 360;
            }
          }
          if (cursorRow == 1 && lightCfg.style != MODE_PARTY &&
              (btns[2].justPressed || btns[2].repeatTriggered))
            lightCfg.sat =
                constrain(lightCfg.sat - CFG_STEP_SATURATION, 0, 100);
          if (cursorRow == 2 &&
              (btns[2].justPressed || btns[2].repeatTriggered))
            lightCfg.val =
                constrain(lightCfg.val - CFG_STEP_BRIGHTNESS, 0, 100);
          if (cursorRow == 3 && btns[2].justPressed) {
            lightCfg.style =
                (lightCfg.style - 1 + TOTAL_LIGHT_MODES) % TOTAL_LIGHT_MODES;
            for (int r = 0; r < 5; r++)
              updateLightRow(r);
          }
          if (cursorRow == 4) {
            if (lightCfg.style == MODE_RAINING) {
              lightCfg.speed =
                  constrain(lightCfg.speed - CFG_STEP_SPEED, -50, 50);
            } else if (lightCfg.style != MODE_STATIC) {
              lightCfg.speed =
                  constrain(lightCfg.speed - CFG_STEP_SPEED, 1, 100);
            }
          }
          if (cursorRow != 3) {
            updateLightRow(cursorRow);
            if (cursorRow == 0 && lightCfg.style != MODE_RAINBOW &&
                lightCfg.style != MODE_PARTY) {
              updateLightRow(1);
              updateLightRow(2);
            }
          }
          drawSubpageTopBar();
        }
        // [底右 ENTER] 按鍵 A (E) 儲存 或 [底左 BACK] 按鍵 D (R) 鎖定退出
        if (btns[0].justPressed) {
          sfxEnter();
          isEditing = false;
          updateLightRow(cursorRow);
          drawSideButtons();
          updateGlobalButtonBar();
          drawSubpageTopBar();
        } else if (btns[3].justPressed) {
          sfxBack();
          lightCfg = lightBackup;
          isEditing = false;
          updateLightRow(cursorRow);
          drawSideButtons();
          updateGlobalButtonBar();
          drawSubpageTopBar();
        }
      }
    } else if (carouselIndex == PAGE_MOTOR) {
      if (!isEditing) {
        // [左翼 UP] 按鍵 C (Q): 游標切換到 TARGET (0) 或 VELOCITY (1)
        if (btns[2].justPressed) {
          sfxNavigate();
          cursorRow = (cursorRow - 1 + 2) % 2;
          updateMotorSubpage();
        }
        // [右翼 DOWN] 按鍵 B (W): 游標切換到 TARGET (0) 或 VELOCITY (1)
        if (btns[1].justPressed) {
          sfxNavigate();
          cursorRow = (cursorRow + 1) % 2;
          updateMotorSubpage();
        }
        // [底右 ENTER] 按鍵 A (E): 進入編輯模式 (若防護欄觸發則先清除故障)
        if (btns[0].justPressed) {
          if (motorGuardStatus != GUARD_OK) {
            motorGuardStatus = GUARD_OK;
            sfxEnter();
            updateMotorSubpage();
          } else {
            sfxEnter();
            isEditing = true;
            motorBackup = motorCfg;
            updateMotorSubpage();
            drawSideButtons();
            updateGlobalButtonBar();
          }
        }
        // [底左 BACK] 按鍵 D (R):
        // 若馬達正在移動中，按 BACK 立即緊急煞車停機；
        // 若馬達已處於停止狀態，按 BACK 返回 HUD 首頁。
        if (btns[3].justPressed) {
          if (motorServoActive || motorCfg.speed > 0) {
            stopMotorAll();
            sfxWarning();
            updateMotorSubpage();
          } else {
            sfxBack();
            currentState = STATE_HOME;
            stateSwitchLockout = millis() + 200;
            playSubpageExitTransition();
          }
        }
      } else {
        // === 編輯模式 (isEditing == true) ===
        // Row 0: 指定位置調整 (調整時馬達不移動，按確定才開始移動)
        if (cursorRow == 0) {
          if (btns[1].justPressed || btns[1].repeatTriggered) {
            motorCfg.targetPos =
                constrain(motorCfg.targetPos + CFG_STEP_TARGET_POS, 0, 100);
            updateMotorSubpage();
            drawSubpageTopBar();
          }
          if (btns[2].justPressed || btns[2].repeatTriggered) {
            motorCfg.targetPos = constrain(
                (int)motorCfg.targetPos - (int)CFG_STEP_TARGET_POS, 0, 100);
            updateMotorSubpage();
            drawSubpageTopBar();
          }
          // [底右 ENTER] 按鍵 A (E): 確定！儲存設定並啟動伺服移動到定點！
          if (btns[0].justPressed) {
            sfxEnter();
            isEditing = false;
            savePreferences(false);
            startMotorServoToPct((float)motorCfg.targetPos);
            updateMotorSubpage();
            drawSideButtons();
            updateGlobalButtonBar();
            drawSubpageTopBar();
          }
          // [底左 BACK] 按鍵 D (R): 取消修改並退出編輯 (不移動馬達)
          else if (btns[3].justPressed) {
            sfxBack();
            motorCfg = motorBackup;
            isEditing = false;
            updateMotorSubpage();
            drawSideButtons();
            updateGlobalButtonBar();
            drawSubpageTopBar();
          }
        }
        // Row 1: 手動速度調節
        else if (cursorRow == 1) {
          if (btns[1].justPressed || btns[1].repeatTriggered) {
            motorCfg.speed = constrain(motorCfg.speed + CFG_STEP_MOTOR, 0, 100);
            updateMotorSubpage();
            drawSubpageTopBar();
          }
          if (btns[2].justPressed || btns[2].repeatTriggered) {
            motorCfg.speed =
                constrain((int)motorCfg.speed - (int)CFG_STEP_MOTOR, 0, 100);
            updateMotorSubpage();
            drawSubpageTopBar();
          }
          // [底右 ENTER] 按鍵 A (E): 確定手動轉速 (若 > 0 則切換至手動運轉)
          if (btns[0].justPressed) {
            sfxEnter();
            isEditing = false;
            motorServoActive = false;
            motorServoArrived = false;
            savePreferences(false);
            updateMotorSubpage();
            drawSideButtons();
            updateGlobalButtonBar();
            drawSubpageTopBar();
          }
          // [底左 BACK] 按鍵 D (R): 取消修改並退出編輯
          else if (btns[3].justPressed) {
            sfxBack();
            motorCfg = motorBackup;
            isEditing = false;
            updateMotorSubpage();
            drawSideButtons();
            updateGlobalButtonBar();
            drawSubpageTopBar();
          }
        }
      }
    } else if (carouselIndex == PAGE_SENSORS) {
      if (btns[3].justPressed || btns[0].justPressed) {
        sfxBack();
        currentState = STATE_HOME;
        stateSwitchLockout = millis() + 200;
        playSubpageExitTransition();
      }
    }
  }

  delay(10);
}