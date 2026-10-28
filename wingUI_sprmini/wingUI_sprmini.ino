#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <math.h>

#include "custom_font.h"
#include "hud_icons.h" // ◄◄ 引入全新打包的圖標庫！

// --- 引腳定義 ---
#define TFT_CS    7
#define TFT_RST   1
#define TFT_DC    0
#define TFT_SCLK  4
#define TFT_MOSI  6

// 四顆戰術按鍵
#define PIN_KEY_A  10  // A: [底右 ENTER] 確定 / 進入編輯 / 儲存
#define PIN_KEY_B  3   // B: [右翼 > / DOWN / +] 下一個 / 游標下移 / 數值增加
#define PIN_KEY_C  5   // C: [左翼 < / UP / -] 上一個 / 游標上移 / 數值減少
#define PIN_KEY_D  20  // D: [底左 BACK] 返回 / 取消還原

// --- 戰術 HUD 16-bit RGB565 色盤 ---
#define COLOR_BG        0x0000  // 深空黑底
#define COLOR_CYAN      0x07FF  // 霓虹青 (主瞄準線)
#define COLOR_AMBER     0xFD20  // 戰術琥珀橘 (警示/切角高光)
#define COLOR_GREEN     0x07E0  // 矩陣綠 (在線/正常)
#define COLOR_DARKGREY  0x18E3  // 底層未激活刻度暗灰
#define COLOR_MIDGREY   0x3A4E  // 輔助結構線框灰
#define COLOR_LIGHTGREY 0xAD55  // 標籤字體白灰
#define COLOR_WHITE     0xFFFF  // 強光核心點

Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_DC, TFT_RST);

// --- 點陣狀態圖標 ---
const uint8_t PROGMEM icon_wifi_8x8[] = {
  0b00111100, 0b01000010, 0b10011001, 0b00100100,
  0b01000010, 0b00011000, 0b00000000, 0b00011000
};

const uint8_t PROGMEM icon_bt_8x9[] = {
  0b00010000, 0b00011000, 0b01010100, 0b00111000,
  0b00010000, 0b00111000, 0b01010100, 0b00011000, 0b00010000
};

// ================= 自訂戰術點陣字型渲染核心 =================

const uint8_t* getCustomGlyph(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;

  switch (c) {
    case '0': return font_0; case '1': return font_1; case '2': return font_2;
    case '3': return font_3; case '4': return font_4; case '5': return font_5;
    case '6': return font_6; case '7': return font_7; case '8': return font_8;
    case '9': return font_9;
    case 'A': return font_A; case 'B': return font_B; case 'C': return font_C;
    case 'D': return font_D; case 'E': return font_E; case 'F': return font_F;
    case 'G': return font_G; case 'H': return font_H; case 'I': return font_I;
    case 'J': return font_J; case 'K': return font_K; case 'L': return font_L;
    case 'M': return font_M; case 'N': return font_N; case 'O': return font_O;
    case 'P': return font_P; case 'Q': return font_Q; case 'R': return font_R;
    case 'S': return font_S; case 'T': return font_T; case 'U': return font_U;
    case 'V': return font_V; case 'W': return font_W; case 'X': return font_X;
    case 'Y': return font_Y; case 'Z': return font_Z;
    default: return nullptr;
  }
}

void drawTacticalChar(int x, int y, char c, uint16_t color, uint16_t bg, uint8_t size = 1) {
  if (c == ' ') {
    if (bg != color) tft.fillRect(x, y, FONT_CHAR_W * size, FONT_CHAR_H * size, bg);
    return;
  }

  const uint8_t* glyph = getCustomGlyph(c);
  if (!glyph) {
    tft.drawChar(x, y, c, color, bg, size);
    return;
  }

  for (int r = 0; r < FONT_CHAR_H; r++) {
    uint8_t line = pgm_read_byte(&glyph[r]);
    for (int col = 0; col < FONT_CHAR_W; col++) {
      if (line & (0x80 >> col)) {
        if (size == 1) tft.drawPixel(x + col, y + r, color);
        else tft.fillRect(x + col * size, y + r * size, size, size, color);
      } else if (bg != color) {
        if (size == 1) tft.drawPixel(x + col, y + r, bg);
        else tft.fillRect(x + col * size, y + r * size, size, size, bg);
      }
    }
  }
}

int getTacticalStringWidth(const char* str, uint8_t size = 1) {
  int len = strlen(str);
  if (len == 0) return 0;
  int spacing = (size == 1) ? 1 : 2;
  return (len * FONT_CHAR_W * size) + ((len - 1) * spacing);
}

int drawTacticalString(int x, int y, const char* str, uint16_t color, uint16_t bg, uint8_t size = 1) {
  int curX = x;
  int spacing = (size == 1) ? 1 : 2;
  while (*str) {
    drawTacticalChar(curX, y, *str, color, bg, size);
    curX += (FONT_CHAR_W * size) + spacing;
    str++;
  }
  return curX;
}

void drawTacticalPrintf(int x, int y, uint16_t color, uint16_t bg, uint8_t size, const char* format, ...) {
  char buf[36];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  drawTacticalString(x, y, buf, color, bg, size);
}

// ================= 狀態機與參數結構 =================

enum SysState { STATE_HOME, STATE_SUBPAGE };
SysState currentState = STATE_HOME;

enum CarouselItem { PAGE_LIGHT = 0, PAGE_MOTOR = 1, PAGE_SENSORS = 2 };
int carouselIndex = PAGE_LIGHT;
const int TOTAL_CAROUSEL = 3;

enum LightMode {
  MODE_STATIC = 0,
  MODE_RAINBOW = 1,
  MODE_BREATHING = 2,
  MODE_THUNDER = 3,
  MODE_RAINING = 4,
  MODE_PARTY = 5
};
const int TOTAL_LIGHT_MODES = 6;
const char* const LIGHT_MODE_NAMES[TOTAL_LIGHT_MODES] = {
  "STATIC", "RAINBOW", "BREATHE", "THUNDER", "RAINING", "PARTY"
};

struct LightConfig {
  uint16_t hue = 180;
  uint8_t  sat = 100;
  uint8_t  val = 80;
  uint8_t  style = MODE_STATIC;
} lightCfg, lightBackup;

void hsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
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
  if (h < 60)       { rf = c; gf = x; bf = 0; }
  else if (h < 120) { rf = x; gf = c; bf = 0; }
  else if (h < 180) { rf = 0; gf = c; bf = x; }
  else if (h < 240) { rf = 0; gf = x; bf = c; }
  else if (h < 300) { rf = x; gf = 0; bf = c; }
  else              { rf = c; gf = 0; bf = x; }

  r = (uint8_t)((rf + m) * 255.0f + 0.5f);
  g = (uint8_t)((gf + m) * 255.0f + 0.5f);
  b = (uint8_t)((bf + m) * 255.0f + 0.5f);
}

uint16_t hsvToRgb565(uint16_t h, uint8_t s, uint8_t v) {
  uint8_t r, g, b;
  hsvToRgb(h, s, v, r, g, b);
  return tft.color565(r, g, b);
}

struct MotorConfig {
  uint8_t speed = 65;
} motorCfg, motorBackup;

float sysTemp = 41.8;
float sysVolt = 3.31;
int   sysCurr = 142;

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
} btns[4] = {
  {PIN_KEY_A, HIGH, HIGH, 0, false},
  {PIN_KEY_B, HIGH, HIGH, 0, false},
  {PIN_KEY_C, HIGH, HIGH, 0, false},
  {PIN_KEY_D, HIGH, HIGH, 0, false}
};

const unsigned long DEBOUNCE_DELAY = 45;

void scanButtons() {
  unsigned long now = millis();
  for (int i = 0; i < 4; i++) {
    btns[i].justPressed = false;
    bool reading = digitalRead(btns[i].pin);
    if (reading != btns[i].lastReading) btns[i].lastDebounceTime = now;
    if ((now - btns[i].lastDebounceTime) > DEBOUNCE_DELAY) {
      if (reading != btns[i].stableState) {
        btns[i].stableState = reading;
        if (btns[i].stableState == LOW && now > stateSwitchLockout) {
          btns[i].justPressed = true;
        }
      }
    }
    btns[i].lastReading = reading;
  }
}

void drawTechBar(int x, int y, int w, int h, int val, int maxVal, uint16_t activeColor) {
  tft.drawRect(x, y, w, h, COLOR_MIDGREY);
  int fillW = map(constrain(val, 0, maxVal), 0, maxVal, 0, w - 2);
  if (fillW > 0) tft.fillRect(x + 1, y + 1, fillW, h - 2, activeColor);
  if (w - 2 - fillW > 0) tft.fillRect(x + 1 + fillW, y + 1, w - 2 - fillW, h - 2, COLOR_BG);
}

void drawHsvTrueColorBar(int x, int y, int w, int h, int mode, int currentVal, int maxVal, bool isSelected) {
  tft.drawRect(x, y, w, h, isSelected ? (isEditing ? COLOR_AMBER : COLOR_CYAN) : COLOR_MIDGREY);
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
      barCol = hsvToRgb565(lightCfg.hue, sampleS, lightCfg.val > 20 ? lightCfg.val : 80);
    } else {
      uint8_t sampleV = (uint8_t)((i * 100) / (innerW - 1));
      barCol = hsvToRgb565(lightCfg.hue, lightCfg.sat, sampleV);
    }

    if (i < fillW) tft.drawFastVLine(x + 1 + i, y + 1, innerH, barCol);
    else          tft.drawFastVLine(x + 1 + i, y + 1, innerH, COLOR_BG);
  }

  if (fillW > 0 && fillW < innerW) {
    tft.drawFastVLine(x + fillW, y, h, COLOR_WHITE);
  }
}

// ================= 全新按鍵佈局渲染引擎 =================

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
      // 分頁瀏覽狀態：使用全新 UP 與 DOWN 圖示！
      int upX = (12 - ICON_UP_W) / 2;
      int upY = 58 - (ICON_UP_H / 2);
      tft.drawBitmap(upX, upY, icon_up, ICON_UP_W, ICON_UP_H, COLOR_AMBER, COLOR_BG);

      int downX = 116 + (12 - ICON_DOWN_W) / 2;
      int downY = 58 - (ICON_DOWN_H / 2);
      tft.drawBitmap(downX, downY, icon_down, ICON_DOWN_W, ICON_DOWN_H, COLOR_AMBER, COLOR_BG);
    } else {
      // 數值編輯中：左右切換為 - 與 +
      drawTacticalChar(2, 58, '-', COLOR_AMBER, COLOR_BG, 1);
      drawTacticalChar(120, 58, '+', COLOR_AMBER, COLOR_BG, 1);
    }
  }
}

// 2. 螢幕下排膠囊：使用全新 BACK 與 ENTER 圖示[cite: 8]
void drawPersistentButtonBar(uint16_t colLeft, uint16_t colRight) {
  tft.fillRect(0, 114, 128, 14, COLOR_BG);
  tft.drawFastHLine(0, 114, 128, COLOR_MIDGREY);

  // 左膠囊 (D: BACK)
  tft.drawRoundRect(8, 116, 50, 11, 2, colLeft);
  int bx = 8 + (50 - ICON_BACK_W) / 2;
  int by = 116 + (11 - ICON_BACK_H) / 2;
  tft.drawBitmap(bx, by, icon_back, ICON_BACK_W, ICON_BACK_H, colLeft, COLOR_BG);

  // 右膠囊 (A: ENTER)
  tft.drawRoundRect(70, 116, 50, 11, 2, colRight);
  int ex = 70 + (50 - ICON_ENTER_W) / 2;
  int ey = 116 + (11 - ICON_ENTER_H) / 2;
  tft.drawBitmap(ex, ey, icon_enter, ICON_ENTER_W, ICON_ENTER_H, colRight, COLOR_BG);
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

  tft.drawRect(2, 4, 15, 8, COLOR_CYAN);
  tft.fillRect(17, 6, 2, 4, COLOR_CYAN);
  tft.fillRect(4, 6, 9, 4, COLOR_GREEN);
  drawTacticalString(22, 5, "85%", COLOR_CYAN, COLOR_BG, 1);

  tft.drawFastVLine(52, 3, 10, COLOR_DARKGREY);
  tft.drawPixel(52, 8, COLOR_AMBER);

  tft.drawBitmap(58, 4, icon_wifi_8x8, 8, 8, COLOR_GREEN, COLOR_BG);
  tft.fillRect(68, 8, 2, 4, COLOR_GREEN);
  tft.fillRect(72, 6, 2, 6, COLOR_GREEN);
  tft.fillRect(76, 4, 2, 8, COLOR_GREEN);

  tft.drawBitmap(88, 4, icon_bt_8x9, 8, 9, COLOR_CYAN, COLOR_BG);
  drawTacticalString(98, 5, "LINK", COLOR_CYAN, COLOR_BG, 1);

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

void drawRotatedArcGauge(int cx, int cy, int rIn, int rOut, int totalSegments, int activeSegments, float startAngleDeg, float totalDeg, uint16_t activeCol, uint16_t inactiveCol) {
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

void drawSegmentedArcGauge(int cx, int cy, int rIn, int rOut, int totalSegments, int activeSegments, uint16_t activeCol, uint16_t inactiveCol) {
  drawRotatedArcGauge(cx, cy, rIn, rOut, totalSegments, activeSegments, 135.0f, 270.0f, activeCol, inactiveCol);
}

// ================= 三大分頁進場科技動畫 =================

void playLightEntranceAnim() {
  int cx = 14, cy = 19;
  tft.fillRect(cx + 1, cy + 1, 98, 90, COLOR_BG);

  drawTacticalString(cx + 6, cy + 3, ">LIGHT", COLOR_AMBER, COLOR_BG, 1);
  drawTacticalPrintf(cx + 66, cy + 3, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d'", lightCfg.hue);

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
  tft.fillCircle(rx, ry, 8, curCol);
  tft.drawCircle(rx, ry, 8, COLOR_WHITE);

  tft.drawFastHLine(rx - 17, ry, 3, COLOR_CYAN);
  tft.drawFastHLine(rx + 15, ry, 3, COLOR_CYAN);
  tft.drawFastVLine(rx, ry - 17, 3, COLOR_CYAN);
  tft.drawFastVLine(rx, ry + 15, 3, COLOR_CYAN);

  int tagW = 76, tagH = 11;
  int tagX = cx + (98 - tagW) / 2, tagY = cy + 69;
  tft.drawRoundRect(tagX, tagY, tagW, tagH, 2, COLOR_AMBER);
  tft.fillRect(tagX + 1, tagY + 1, tagW - 2, tagH - 2, 0x18A2);
  int textW = getTacticalStringWidth(LIGHT_MODE_NAMES[lightCfg.style], 1);
  drawTacticalString(tagX + (tagW - textW) / 2, tagY + 2, LIGHT_MODE_NAMES[lightCfg.style], COLOR_AMBER, 0x18A2, 1);
}

void playMotorEntranceAnim() {
  int cx = 14, cy = 19;
  tft.fillRect(cx + 1, cy + 1, 98, 90, COLOR_BG);

  drawTacticalString(cx + 6, cy + 3, ">MOTOR", COLOR_AMBER, COLOR_BG, 1);

  int dialX = 63, dialY = 56;
  int totalSegs = 16;
  int targetSegs = map(motorCfg.speed, 0, 100, 0, totalSegs);

  for (int seg = 2; seg <= 16; seg += 3) {
    drawSegmentedArcGauge(dialX, dialY, 14, 19, totalSegs, seg, COLOR_AMBER, COLOR_DARKGREY);
    delay(15);
  }
  drawSegmentedArcGauge(dialX, dialY, 14, 19, totalSegs, 16, COLOR_WHITE, COLOR_DARKGREY);
  delay(30);

  for (int deg = 0; deg <= 360; deg += 45) {
    tft.fillRect(dialX - 24, dialY - 24, 49, 49, COLOR_BG);
    drawRotatedArcGauge(dialX, dialY, 16, 22, totalSegs, 16, 135.0f + deg, 270.0f, COLOR_CYAN, COLOR_DARKGREY);
    delay(16);
  }

  tft.fillRect(dialX - 24, dialY - 24, 49, 49, COLOR_BG);
  for (int seg = 16; seg >= targetSegs; seg--) {
    drawSegmentedArcGauge(dialX, dialY, 14, 19, totalSegs, seg, COLOR_CYAN, COLOR_DARKGREY);
    delay(12);
  }

  char pwrStr[8];
  snprintf(pwrStr, sizeof(pwrStr), "%d", motorCfg.speed);
  int numW = getTacticalStringWidth(pwrStr, 2);
  drawTacticalString(dialX - numW / 2 - 4, dialY - (FONT_CHAR_H), pwrStr, COLOR_WHITE, COLOR_BG, 2);
  drawTacticalChar(dialX + numW / 2 + 1, dialY - (FONT_CHAR_H / 2), '%', COLOR_CYAN, COLOR_BG, 1);

  int rpm = map(motorCfg.speed, 0, 100, 0, 4800);
  char rpmBuf[24];
  snprintf(rpmBuf, sizeof(rpmBuf), "VEL %4d RPM", rpm);
  int rpmW = getTacticalStringWidth(rpmBuf, 1);
  drawTacticalString(cx + (98 - rpmW) / 2, cy + 69, rpmBuf, COLOR_LIGHTGREY, COLOR_BG, 1);
}

const uint16_t *getTemperatureIcon(float temp) {
  if (temp < 30.0f) {
    return icon_T_low;
  } else if (temp < 45.0f) {
    return icon_T_norm;
  } else if (temp < 50.0f) {
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
  int vBoxX = cx + 8;
  tft.drawRect(vBoxX, bY, bW, bH, COLOR_MIDGREY);
  tft.fillRect(vBoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
  drawTacticalString(vBoxX + 4, bY + 2, "VOLT", COLOR_CYAN, 0x11C5, 1);

  int cBoxX = cx + 51;
  tft.drawRect(cBoxX, bY, bW, bH, COLOR_MIDGREY);
  tft.fillRect(cBoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
  drawTacticalString(cBoxX + 4, bY + 2, "CURR", COLOR_AMBER, 0x11C5, 1);
  delay(40);

  // 升溫動畫[cite: 7]
  const uint16_t* tempFrames[4] = {icon_T_low, icon_T_norm, icon_T_mid, icon_T_high};
  int iconX = boxX + boxW - ICON_T_HIGH_W - 5;
  int iconY = boxY + (boxH - ICON_T_HIGH_H) / 2;

  for (int f = 0; f < 4; f++) {
    tft.drawRGBBitmap(iconX, iconY, tempFrames[f], ICON_T_HIGH_W, ICON_T_HIGH_H);
    delay(45);
  }
  tft.drawRGBBitmap(iconX, iconY, getTemperatureIcon(sysTemp), ICON_T_HIGH_W, ICON_T_HIGH_H);

  // 數值砸入
  tft.fillRect(boxX + 2, boxY + 2, boxW - ICON_T_HIGH_W - 10, boxH - 4, COLOR_CYAN);
  delay(18);
  tft.fillRect(boxX + 2, boxY + 2, boxW - ICON_T_HIGH_W - 10, boxH - 4, COLOR_BG);

  char tempBuf[12];
  snprintf(tempBuf, sizeof(tempBuf), "%2.1f", sysTemp);
  drawTacticalString(boxX + 6, boxY + 6, tempBuf, COLOR_WHITE, COLOR_BG, 2);
  drawTacticalString(boxX + 48, boxY + 5, "'C", COLOR_WHITE, COLOR_BG, 1);
  delay(25);

  drawTacticalString(boxX + 6, boxY + 6, tempBuf, COLOR_AMBER, COLOR_BG, 2);
  drawTacticalString(boxX + 48, boxY + 5, "'C", COLOR_CYAN, COLOR_BG, 1);
  delay(35);

  tft.fillRect(vBoxX + 1, bY + 11, bW - 2, 14, COLOR_CYAN);
  delay(15);
  tft.fillRect(vBoxX + 1, bY + 11, bW - 2, 14, COLOR_BG);
  drawTacticalPrintf(vBoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%1.2fV", sysVolt);
  delay(35);

  tft.fillRect(cBoxX + 1, bY + 11, bW - 2, 14, COLOR_AMBER);
  delay(15);
  tft.fillRect(cBoxX + 1, bY + 11, bW - 2, 14, COLOR_BG);
  drawTacticalPrintf(cBoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%3dMA", sysCurr);
}

void playPageEntranceAnimation(int pageIdx) {
  if (pageIdx == PAGE_LIGHT)   playLightEntranceAnim();
  if (pageIdx == PAGE_MOTOR)   playMotorEntranceAnim();
  if (pageIdx == PAGE_SENSORS) playSensorsEntranceAnim();
}

void drawCentralCardContent() {
  int cx = 14, cy = 19;
  tft.fillRect(cx + 1, cy + 1, 98, 90, COLOR_BG);

  if (carouselIndex == PAGE_LIGHT) {
    drawTacticalString(cx + 6, cy + 3, ">LIGHT", COLOR_AMBER, COLOR_BG, 1);
    drawTacticalPrintf(cx + 66, cy + 3, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d'", lightCfg.hue);

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

    uint16_t curCol = hsvToRgb565(lightCfg.hue, lightCfg.sat, lightCfg.val);
    int reactorX = 63, reactorY = 62;

    tft.drawCircle(reactorX, reactorY, 12, blinkState ? COLOR_CYAN : COLOR_MIDGREY);
    tft.fillCircle(reactorX, reactorY, 8, curCol);
    tft.drawCircle(reactorX, reactorY, 8, COLOR_WHITE);

    tft.drawFastHLine(reactorX - 17, reactorY, 3, COLOR_CYAN);
    tft.drawFastHLine(reactorX + 15, reactorY, 3, COLOR_CYAN);
    tft.drawFastVLine(reactorX, reactorY - 17, 3, COLOR_CYAN);
    tft.drawFastVLine(reactorX, reactorY + 15, 3, COLOR_CYAN);

    int tagW = 76, tagH = 11;
    int tagX = cx + (98 - tagW) / 2, tagY = cy + 69;
    tft.drawRoundRect(tagX, tagY, tagW, tagH, 2, COLOR_AMBER);
    tft.fillRect(tagX + 1, tagY + 1, tagW - 2, tagH - 2, 0x18A2);
    int textW = getTacticalStringWidth(LIGHT_MODE_NAMES[lightCfg.style], 1);
    drawTacticalString(tagX + (tagW - textW) / 2, tagY + 2, LIGHT_MODE_NAMES[lightCfg.style], COLOR_AMBER, 0x18A2, 1);
  }
  else if (carouselIndex == PAGE_MOTOR) {
    drawTacticalString(cx + 6, cy + 3, ">MOTOR", COLOR_AMBER, COLOR_BG, 1);

    int dialX = 63, dialY = 56;
    int totalSegs = 16;
    int activeSegs = map(motorCfg.speed, 0, 100, 0, totalSegs);
    drawSegmentedArcGauge(dialX, dialY, 14, 19, totalSegs, activeSegs, COLOR_CYAN, COLOR_DARKGREY);

    char pwrStr[8];
    snprintf(pwrStr, sizeof(pwrStr), "%d", motorCfg.speed);
    int numW = getTacticalStringWidth(pwrStr, 2);
    drawTacticalString(dialX - numW / 2 - 4, dialY - (FONT_CHAR_H), pwrStr, COLOR_WHITE, COLOR_BG, 2);
    drawTacticalChar(dialX + numW / 2 + 1, dialY - (FONT_CHAR_H / 2), '%', COLOR_CYAN, COLOR_BG, 1);

    int rpm = map(motorCfg.speed, 0, 100, 0, 4800);
    char rpmBuf[24];
    snprintf(rpmBuf, sizeof(rpmBuf), "VEL %4d RPM", rpm);
    int rpmW = getTacticalStringWidth(rpmBuf, 1);
    drawTacticalString(cx + (98 - rpmW) / 2, cy + 69, rpmBuf, COLOR_LIGHTGREY, COLOR_BG, 1);
  }
  else if (carouselIndex == PAGE_SENSORS) {
    drawTacticalString(cx + 6, cy + 3, ">THERMAL", COLOR_AMBER, COLOR_BG, 1);

    int boxX = cx + 8, boxY = cy + 15, boxW = 82, boxH = 28;
    tft.drawRoundRect(boxX, boxY, boxW, boxH, 2, COLOR_MIDGREY);
    tft.drawLine(boxX, boxY, boxX + 10, boxY, COLOR_AMBER);
    tft.drawLine(boxX, boxY, boxX, boxY + 10, COLOR_AMBER);

    char tempBuf[12];
    snprintf(tempBuf, sizeof(tempBuf), "%2.1f", sysTemp);
    drawTacticalString(boxX + 6, boxY + 6, tempBuf, COLOR_AMBER, COLOR_BG, 2);
    drawTacticalString(boxX + 48, boxY + 5, "'C", COLOR_CYAN, COLOR_BG, 1);

    int iconX = boxX + boxW - ICON_T_HIGH_W - 5;
    int iconY = boxY + (boxH - ICON_T_HIGH_H) / 2;
    tft.drawRGBBitmap(iconX, iconY, getTemperatureIcon(sysTemp), ICON_T_HIGH_W, ICON_T_HIGH_H);

    int bY = cy + 49, bW = 39, bH = 26;
    int vBoxX = cx + 8;
    tft.drawRect(vBoxX, bY, bW, bH, COLOR_MIDGREY);
    tft.fillRect(vBoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
    drawTacticalString(vBoxX + 4, bY + 2, "VOLT", COLOR_CYAN, 0x11C5, 1);
    drawTacticalPrintf(vBoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%1.2fV", sysVolt);

    int cBoxX = cx + 51;
    tft.drawRect(cBoxX, bY, bW, bH, COLOR_MIDGREY);
    tft.fillRect(cBoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
    drawTacticalString(cBoxX + 4, bY + 2, "CURR", COLOR_AMBER, 0x11C5, 1);
    drawTacticalPrintf(cBoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%3dMA", sysCurr);
  }
}

void playCyberTransition(bool toRight) {
  int xStart = 14, yStart = 18, w = 100, h = 94;
  int step = 14;

  if (toRight) {
    for (int x = xStart; x < xStart + w; x += step) {
      tft.drawFastVLine(x, yStart, h, COLOR_CYAN);
      tft.drawFastVLine(x + 1, yStart, h, COLOR_WHITE);
      if (x - step >= xStart) tft.fillRect(x - step, yStart, step, h, COLOR_BG);
      delay(8);
    }
  } else {
    for (int x = xStart + w - 1; x >= xStart; x -= step) {
      tft.drawFastVLine(x, yStart, h, COLOR_AMBER);
      tft.drawFastVLine(x - 1, yStart, h, COLOR_WHITE);
      if (x + step < xStart + w) tft.fillRect(x + 1, yStart, step, h, COLOR_BG);
      delay(8);
    }
  }
  tft.fillRect(xStart, yStart, w, h, COLOR_BG);
}

// 子分頁介面
void drawSubpageScreen() {
  tft.fillScreen(COLOR_BG);

  tft.drawFastHLine(0, 16, 40, COLOR_MIDGREY);
  tft.drawFastHLine(44, 16, 40, COLOR_CYAN);
  tft.drawFastHLine(88, 16, 40, COLOR_MIDGREY);
  tft.drawPixel(42, 16, COLOR_AMBER);
  tft.drawPixel(86, 16, COLOR_AMBER);

  const char *title = "";
  if (carouselIndex == PAGE_LIGHT)   title = "LIGHT";
  if (carouselIndex == PAGE_MOTOR)   title = "MOTOR";
  if (carouselIndex == PAGE_SENSORS) title = "THERMAL";
  drawTacticalString(6, 4, title, COLOR_AMBER, COLOR_BG, 1);

  int startY = 19;

  if (carouselIndex == PAGE_LIGHT) {
    const char* tags[5] = {"HUE", "SAT", "BRI", "MOD", "CLR"};
    int vals[4] = {lightCfg.hue, lightCfg.sat, lightCfg.val, lightCfg.style};
    int maxV[4] = {359, 100, 100, 3};

    for (int i = 0; i < 5; i++) {
      int y = startY + i * 18;
      bool sel = (cursorRow == i);

      if (sel) {
        drawTacticalChar(14, y + 2, isEditing ? '#' : '>', isEditing ? COLOR_AMBER : COLOR_CYAN, COLOR_BG, 1);
      } else {
        drawTacticalChar(14, y + 2, ' ', COLOR_BG, COLOR_BG, 1);
      }

      uint16_t tagCol = sel ? (isEditing ? COLOR_AMBER : COLOR_WHITE) : COLOR_LIGHTGREY;
      drawTacticalString(22, y + 2, tags[i], tagCol, COLOR_BG, 1);

      if (i < 3) {
        drawHsvTrueColorBar(44, y + 1, 40, 9, i, vals[i], maxV[i], sel);
        drawTacticalPrintf(88, y + 2, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d", vals[i]);
      } else if (i == 3) {
        uint16_t boxBorder = (sel && isEditing) ? COLOR_AMBER : (sel ? COLOR_CYAN : COLOR_MIDGREY);
        tft.drawRect(44, y + 1, 68, 11, boxBorder);
        uint16_t boxBg = (sel && isEditing) ? 0x2104 : COLOR_BG;
        tft.fillRect(45, y + 2, 66, 9, boxBg);

        char mBuf[20];
        snprintf(mBuf, sizeof(mBuf), "[%s]", LIGHT_MODE_NAMES[lightCfg.style]);
        uint16_t mCol = (sel && isEditing) ? COLOR_AMBER : (sel ? COLOR_WHITE : COLOR_CYAN);
        int mW = getTacticalStringWidth(mBuf, 1);
        drawTacticalString(44 + (68 - mW) / 2, y + 2, mBuf, mCol, boxBg, 1);
      } else {
        uint16_t curCol = hsvToRgb565(lightCfg.hue, lightCfg.sat, lightCfg.val);
        tft.fillRect(44, y + 1, 40, 9, curCol);
        tft.drawRect(44, y + 1, 40, 9, sel ? (isEditing ? COLOR_AMBER : COLOR_CYAN) : COLOR_WHITE);
        drawTacticalString(88, y + 2, "VIEW", COLOR_LIGHTGREY, COLOR_BG, 1);
      }
    }
  }
  else if (carouselIndex == PAGE_MOTOR) {
    int y = 44;
    uint16_t titleCol = isEditing ? COLOR_AMBER : COLOR_CYAN;
    drawTacticalString(16, y, isEditing ? "# VELOCITY" : "> VELOCITY", titleCol, COLOR_BG, 1);
    drawTacticalPrintf(84, y, titleCol, COLOR_BG, 1, "%3d%%", motorCfg.speed);

    uint16_t bCol = isEditing ? COLOR_AMBER : COLOR_CYAN;
    drawTechBar(16, y + 16, 96, 12, motorCfg.speed, 100, bCol);
  }
  else if (carouselIndex == PAGE_SENSORS) {
    drawTacticalPrintf(16, 24, COLOR_LIGHTGREY, COLOR_BG, 1, "SYS TEMP: %2.1fC", sysTemp);
    drawTechBar(16, 36, 96, 7, (int)sysTemp, 80, COLOR_AMBER);

    drawTacticalPrintf(16, 50, COLOR_LIGHTGREY, COLOR_BG, 1, "BUS VOLT: %1.2fV", sysVolt);
    drawTechBar(16, 62, 96, 7, (int)(sysVolt * 10), 50, COLOR_GREEN);

    drawTacticalPrintf(16, 76, COLOR_LIGHTGREY, COLOR_BG, 1, "TOT CURR: %3dMA", sysCurr);
    drawTechBar(16, 88, 96, 7, sysCurr, 300, COLOR_CYAN);
  }

  drawSideButtons();
  updateGlobalButtonBar();
}

void renderFullHomeScreen() {
  tft.fillScreen(COLOR_BG);
  drawTopBar();
  drawSideButtons();
  drawTechCentralFrame(blinkState);
  drawCentralCardContent();
  updateGlobalButtonBar();
}

void setup() {
  for (int i = 0; i < 4; i++) {
    pinMode(btns[i].pin, INPUT_PULLUP);
  }

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_144GREENTAB);
  tft.setRotation(0);

  tft.fillScreen(COLOR_BG);
  drawTopBar();
  drawSideButtons();
  drawTechCentralFrame(true);
  updateGlobalButtonBar();
  playPageEntranceAnimation(carouselIndex);
}

void loop() {
  scanButtons();

  // ================= 狀態 1：首頁 (HUD HOME) =================
  if (currentState == STATE_HOME) {
    // [右翼 >] 按鍵 B：向右切換並觸發進場動畫
    if (btns[1].justPressed) {
      playCyberTransition(true);
      carouselIndex = (carouselIndex + 1) % TOTAL_CAROUSEL;
      drawSideButtons();
      drawTechCentralFrame(blinkState);
      playPageEntranceAnimation(carouselIndex);
    }
    // [左翼 <] 按鍵 C：向左切換並觸發進場動畫
    else if (btns[2].justPressed) {
      playCyberTransition(false);
      carouselIndex = (carouselIndex - 1 + TOTAL_CAROUSEL) % TOTAL_CAROUSEL;
      drawSideButtons();
      drawTechCentralFrame(blinkState);
      playPageEntranceAnimation(carouselIndex);
    }
    // [底右 ENTER] 按鍵 A：選入分頁
    else if (btns[0].justPressed) {
      currentState = STATE_SUBPAGE;
      cursorRow = 0;
      isEditing = false;
      lightBackup = lightCfg;
      motorBackup = motorCfg;
      stateSwitchLockout = millis() + 200;
      drawSubpageScreen();
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
        // [右翼 DOWN] 按鍵 B：游標下移
        if (btns[1].justPressed) { cursorRow = (cursorRow + 1) % 5; drawSubpageScreen(); }
        // [左翼 UP] 按鍵 C：游標上移
        if (btns[2].justPressed) { cursorRow = (cursorRow - 1 + 5) % 5; drawSubpageScreen(); }
        // [底右 ENTER] 按鍵 A：確定進入編輯
        if (btns[0].justPressed) { isEditing = true; lightBackup = lightCfg; drawSubpageScreen(); }
        // [底左 BACK] 按鍵 D：返回首頁
        if (btns[3].justPressed) {
          currentState = STATE_HOME;
          stateSwitchLockout = millis() + 200;
          renderFullHomeScreen();
        }
      } else {
        // [右翼 +] 按鍵 B：數值增加
        if (btns[1].justPressed) {
          if (cursorRow == 0) lightCfg.hue = (lightCfg.hue + 15) % 360;
          if (cursorRow == 1) lightCfg.sat = constrain(lightCfg.sat + 5, 0, 100);
          if (cursorRow == 2) lightCfg.val = constrain(lightCfg.val + 5, 0, 100);
          if (cursorRow == 3) lightCfg.style = (lightCfg.style + 1) % TOTAL_LIGHT_MODES;
          drawSubpageScreen();
        }
        // [左翼 -] 按鍵 C：數值減少
        if (btns[2].justPressed) {
          if (cursorRow == 0) lightCfg.hue = (lightCfg.hue - 15 + 360) % 360;
          if (cursorRow == 1) lightCfg.sat = constrain(lightCfg.sat - 5, 0, 100);
          if (cursorRow == 2) lightCfg.val = constrain(lightCfg.val - 5, 0, 100);
          if (cursorRow == 3) lightCfg.style = (lightCfg.style - 1 + TOTAL_LIGHT_MODES) % TOTAL_LIGHT_MODES;
          drawSubpageScreen();
        }
        // [底右 ENTER] 按鍵 A：確定儲存
        if (btns[0].justPressed) { isEditing = false; drawSubpageScreen(); }
        // [底左 BACK] 按鍵 D：取消還原
        if (btns[3].justPressed) { lightCfg = lightBackup; isEditing = false; drawSubpageScreen(); }
      }
    }
    else if (carouselIndex == PAGE_MOTOR) {
      if (!isEditing) {
        if (btns[0].justPressed) { isEditing = true; motorBackup = motorCfg; drawSubpageScreen(); }
        if (btns[3].justPressed) {
          currentState = STATE_HOME;
          stateSwitchLockout = millis() + 200;
          renderFullHomeScreen();
        }
      } else {
        if (btns[1].justPressed) { motorCfg.speed = constrain(motorCfg.speed + 5, 0, 100); drawSubpageScreen(); }
        if (btns[2].justPressed) { motorCfg.speed = constrain(motorCfg.speed - 5, 0, 100); drawSubpageScreen(); }
        if (btns[0].justPressed) { isEditing = false; drawSubpageScreen(); }
        if (btns[3].justPressed) { motorCfg = motorBackup; isEditing = false; drawSubpageScreen(); }
      }
    }
    else if (carouselIndex == PAGE_SENSORS) {
      if (btns[3].justPressed || btns[0].justPressed) {
        currentState = STATE_HOME;
        stateSwitchLockout = millis() + 200;
        renderFullHomeScreen();
      }
    }
  }

  delay(10);
}