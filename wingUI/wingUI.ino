#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <math.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include "config.h"
#include "sfx.h"
#include "custom_font.h"
#include "hud_icons.h"

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
const char *const LIGHT_MODE_NAMES[TOTAL_LIGHT_MODES] = {
    "STATIC", "RAINBOW", "BREATHE", "THUNDER", "RAINING", "PARTY"};

struct LightConfig {
  uint16_t hue = CFG_DEFAULT_HUE;
  uint8_t sat = CFG_DEFAULT_SAT;
  uint8_t val = CFG_DEFAULT_VAL;
  uint8_t style = MODE_STATIC;
  int8_t speed = CFG_DEFAULT_LIGHT_SPEED; // 特效速度 (支援負值: RAINING 模式 -50 ~ +50, 其他 1 ~ 100)
} lightCfg, lightBackup;

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

// ================= 動態燈效動畫引擎 (Dynamic Light Mode Engine) =================
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
        unsigned long baseWait = map(constrain((int)lightCfg.speed, 1, 100), 1, 100, 3200, 250);
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
    unsigned long interval = map(constrain((int)lightCfg.speed, 1, 100), 1, 100, 700, 60);
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
    uint16_t cCol = hsvToRgb565((int)animRainbowPhase % 360, lightCfg.sat, lightCfg.val);
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
    uint16_t tCol = hsvToRgb565(lightCfg.hue, (lightCfg.sat * 2) / 3, thunderBrightness);
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

void handleLightAnimation() {
  unsigned long now = millis();
  if (now - lastLightAnimTick < 33)
    return;
  float dt = (now - lastLightAnimTick) / 1000.0f;
  lastLightAnimTick = now;

  float rSpeed = (float)lightCfg.speed;
  animRainbowPhase += rSpeed * 1.8f * dt * 60.0f;
  if (animRainbowPhase >= 360.0f) animRainbowPhase -= 360.0f;

  float bSpeed = (float)lightCfg.speed;
  animBreathePhase += bSpeed * 0.05f * dt * 60.0f;
  if (animBreathePhase >= 6.283185f) animBreathePhase -= 6.283185f;

  updateThunderAnimation(now);

  float rainSpeed = (float)lightCfg.speed;
  rainAngle += rainSpeed * 2.5f * dt * 60.0f;
  if (rainAngle >= 360.0f) rainAngle -= 360.0f;
  if (rainAngle < 0.0f) rainAngle += 360.0f;

  updatePartyAnimation(now);

  if (currentState == STATE_HOME && carouselIndex == PAGE_LIGHT) {
    drawLightPreviewCircle(63, 62, 8);
  }
}

struct MotorConfig {
  uint8_t speed = CFG_DEFAULT_MOTOR_SPEED;
} motorCfg, motorBackup;

// DS18B20 1-Wire 實例
OneWire oneWire(PIN_TEMP_DS18B20);
DallasTemperature tempSensor(&oneWire);

// VL53L0X ToF 飛行時間測距感測器實例 (I2C: SDA=GPIO21, SCL=GPIO15)
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
bool vl53l0xReady = false;
int16_t tofDistanceMm = -1; // -1 表示感測器未就緒或超出測量範圍
unsigned long lastTofSampleTime = 0;

float sysTemp = 25.0; // 預設室溫初值，背景自動更新
float sysVolt = 3.31;
int sysCurr = 142;

// 非阻塞感測器取樣計時器
unsigned long lastTempRequestTime = 0;
bool tempConversionPending = false;
unsigned long lastThermalAlarmTime = 0; // 超溫警報重複計時器
unsigned long resetPressStartTime = 0;   // 四鍵長按計時器

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
    char raw = Serial.read();
    if (raw == '\r' || raw == '\n' || raw == ' ' || raw == '\t')
      continue;

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
    Serial.println(
        "  >> Keys: [Q] Left/Up/-  [W] Right/Down/+  [E] ENTER  [R] BACK  [X] System Reset");
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

    // 長按連發檢測：按住超過 CFG_BUTTON_REPEAT_START_MS 後，每隔 CFG_BUTTON_REPEAT_RATE_MS 觸發一次 repeatTriggered
    if (btns[i].stableState == LOW && !btns[i].justPressed && now > stateSwitchLockout) {
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
      tft.drawBitmap(upX, upY, icon_up, ICON_UP_W, ICON_UP_H,
                     COLOR_AMBER, COLOR_BG);

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
    drawTacticalPrintf(cx + 52, cy + 3, COLOR_CYAN, COLOR_BG, 1, "%4dMM", tofDistanceMm);
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
  int vBoxX = cx + 8;
  tft.drawRect(vBoxX, bY, bW, bH, COLOR_MIDGREY);
  tft.fillRect(vBoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
  drawTacticalString(vBoxX + 4, bY + 2, "VOLT", COLOR_CYAN, 0x11C5, 1);

  int cBoxX = cx + 51;
  tft.drawRect(cBoxX, bY, bW, bH, COLOR_MIDGREY);
  tft.fillRect(cBoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
  drawTacticalString(cBoxX + 4, bY + 2, "CURR", COLOR_AMBER, 0x11C5, 1);
  delay(40);

  const uint16_t *tempFrames[4] = {icon_T_low, icon_T_norm, icon_T_mid,
                                   icon_T_high};
  int iconX = boxX + boxW - ICON_T_HIGH_W - 5;
  int iconY = boxY + (boxH - ICON_T_HIGH_H) / 2;

  // 循序展示低溫至高溫原圖色彩階梯動畫
  for (int f = 0; f < 4; f++) {
    tft.drawRGBBitmap(iconX, iconY, tempFrames[f], ICON_T_HIGH_W, ICON_T_HIGH_H);
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
  drawTacticalString(boxX + 4 + tNumW + 3, boxY + 6, "'C", COLOR_WHITE, COLOR_BG, 1);
  delay(25);

  drawTacticalString(boxX + 4, boxY + 7, tempBuf, COLOR_AMBER, COLOR_BG, 2);
  drawTacticalString(boxX + 4 + tNumW + 3, boxY + 6, "'C", COLOR_CYAN, COLOR_BG, 1);
  delay(35);

  tft.fillRect(vBoxX + 1, bY + 11, bW - 2, 14, COLOR_CYAN);
  delay(15);
  tft.fillRect(vBoxX + 1, bY + 11, bW - 2, 14, COLOR_BG);
  drawTacticalPrintf(vBoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%1.2fV",
                     sysVolt);
  delay(35);

  tft.fillRect(cBoxX + 1, bY + 11, bW - 2, 14, COLOR_AMBER);
  delay(15);
  tft.fillRect(cBoxX + 1, bY + 11, bW - 2, 14, COLOR_BG);
  drawTacticalPrintf(cBoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%3dMA",
                     sysCurr);
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
      drawTacticalPrintf(cx + 60, cy + 3, COLOR_LIGHTGREY, COLOR_BG, 1, "S:%3d", lightCfg.speed);
    } else if (lightCfg.style == MODE_PARTY) {
      drawTacticalPrintf(cx + 60, cy + 3, COLOR_LIGHTGREY, COLOR_BG, 1, "B:%3d", lightCfg.val);
    } else {
      drawTacticalPrintf(cx + 66, cy + 3, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d'", lightCfg.hue);
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
      drawTacticalPrintf(cx + 52, cy + 3, COLOR_CYAN, COLOR_BG, 1, "%4dMM", tofDistanceMm);
    } else {
      drawTacticalString(cx + 52, cy + 3, "--- MM", COLOR_DARKGREY, COLOR_BG, 1);
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
    drawTacticalString(boxX + 4 + tNumW + 3, boxY + 6, "'C", COLOR_CYAN, COLOR_BG, 1);

    int iconX = boxX + boxW - ICON_T_HIGH_W - 5;
    int iconY = boxY + (boxH - ICON_T_HIGH_H) / 2;
    tft.drawRGBBitmap(iconX, iconY, getTemperatureIcon(sysTemp), ICON_T_HIGH_W,
                      ICON_T_HIGH_H);

    int bY = cy + 49, bW = 39, bH = 26;
    int vBoxX = cx + 8;
    tft.drawRect(vBoxX, bY, bW, bH, COLOR_MIDGREY);
    tft.fillRect(vBoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
    drawTacticalString(vBoxX + 4, bY + 2, "VOLT", COLOR_CYAN, 0x11C5, 1);
    drawTacticalPrintf(vBoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%1.2fV",
                       sysVolt);

    int cBoxX = cx + 51;
    tft.drawRect(cBoxX, bY, bW, bH, COLOR_MIDGREY);
    tft.fillRect(cBoxX + 1, bY + 1, bW - 2, 9, 0x11C5);
    drawTacticalString(cBoxX + 4, bY + 2, "CURR", COLOR_AMBER, 0x11C5, 1);
    drawTacticalPrintf(cBoxX + 4, bY + 14, COLOR_WHITE, COLOR_BG, 1, "%3dMA",
                       sysCurr);
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
  if (carouselIndex != PAGE_LIGHT) return true;
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

  // 2. 動態標籤文字 (僅 STATIC 模式第 4 行為 CLR，其餘動態模式第 4 行統一為 SPD 特效速度)
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

  uint16_t tagCol = sel ? (isEditing ? COLOR_AMBER : COLOR_WHITE) : COLOR_LIGHTGREY;
  // RAINBOW 與 PARTY 模式下 HUE 被忽略，顯示為暗灰色以提示停用
  if ((lightCfg.style == MODE_RAINBOW || lightCfg.style == MODE_PARTY) && row == 0) {
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
      drawTacticalPrintf(88, y + 2, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d", lightCfg.hue);
    }
  } else if (row == 1) {
    if (lightCfg.style == MODE_PARTY) {
      // PARTY 模式忽略 SAT
      tft.fillRect(44, y + 1, 40, 9, COLOR_BG);
      tft.drawRect(44, y + 1, 40, 9, COLOR_DARKGREY);
      drawTacticalString(88, y + 2, "---", COLOR_DARKGREY, COLOR_BG, 1);
    } else {
      drawHsvTrueColorBar(44, y + 1, 40, 9, 1, lightCfg.sat, 100, sel);
      drawTacticalPrintf(88, y + 2, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d", lightCfg.sat);
    }
  } else if (row == 2) {
    // BRI: 所有模式均受亮度控制 (守護最高亮度)
    drawHsvTrueColorBar(44, y + 1, 40, 9, 2, lightCfg.val, 100, sel);
    drawTacticalPrintf(88, y + 2, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d", lightCfg.val);
  } else if (row == 3) {
    uint16_t boxBorder = (sel && isEditing) ? COLOR_AMBER : (sel ? COLOR_CYAN : COLOR_MIDGREY);
    tft.drawRect(44, y + 1, 68, 11, boxBorder);
    uint16_t boxBg = (sel && isEditing) ? 0x2104 : COLOR_BG;
    tft.fillRect(45, y + 2, 66, 9, boxBg);

    char mBuf[20];
    snprintf(mBuf, sizeof(mBuf), "[%s]", LIGHT_MODE_NAMES[lightCfg.style]);
    uint16_t mCol = (sel && isEditing) ? COLOR_AMBER : (sel ? COLOR_WHITE : COLOR_CYAN);
    int mW = getTacticalStringWidth(mBuf, 1);
    drawTacticalString(44 + (68 - mW) / 2, y + 2, mBuf, mCol, boxBg, 1);
  } else if (row == 4) {
    if (lightCfg.style == MODE_STATIC) {
      uint16_t curCol = hsvToRgb565(lightCfg.hue, lightCfg.sat, lightCfg.val);
      tft.fillRect(44, y + 1, 40, 9, curCol);
      tft.drawRect(44, y + 1, 40, 9, sel ? (isEditing ? COLOR_AMBER : COLOR_CYAN) : COLOR_WHITE);
      drawTacticalString(88, y + 2, "VIEW", COLOR_LIGHTGREY, COLOR_BG, 1);
    } else if (lightCfg.style == MODE_RAINING) {
      // 雨滴滑行速度：支援負值 (-50 ~ +50) 雙向能量條
      tft.drawRect(44, y + 1, 40, 9, sel ? (isEditing ? COLOR_AMBER : COLOR_CYAN) : COLOR_MIDGREY);
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
      drawTechBar(44, y + 1, 40, 9, lightCfg.speed, 100, sel ? (isEditing ? COLOR_AMBER : COLOR_CYAN) : COLOR_AMBER);
      drawTacticalPrintf(88, y + 2, COLOR_LIGHTGREY, COLOR_BG, 1, "%3d", lightCfg.speed);
    }
  }
}

void updateMotorSubpage() {
  // 1. ToF (VL53L0X) 測距讀數區
  int yTof = 22;
  drawTacticalString(16, yTof, "DIST // TOF", COLOR_LIGHTGREY, COLOR_BG, 1);
  if (tofDistanceMm >= 0) {
    drawTacticalPrintf(80, yTof, COLOR_CYAN, COLOR_BG, 1, "%4dMM", tofDistanceMm);
    drawTechBar(16, yTof + 11, 96, 7, constrain((int)tofDistanceMm, 0, 1200), 1200, COLOR_CYAN);
  } else {
    drawTacticalString(80, yTof, "--- MM", COLOR_DARKGREY, COLOR_BG, 1);
    drawTechBar(16, yTof + 11, 96, 7, 0, 1200, COLOR_DARKGREY);
  }

  // 2. 馬達速度 Velocity 設定區
  int yMot = 48;
  uint16_t titleCol = isEditing ? COLOR_AMBER : COLOR_CYAN;
  drawTacticalString(16, yMot, isEditing ? "# VELOCITY" : "> VELOCITY", titleCol, COLOR_BG, 1);
  drawTacticalPrintf(84, yMot, titleCol, COLOR_BG, 1, "%3d%%", motorCfg.speed);

  uint16_t bCol = isEditing ? COLOR_AMBER : COLOR_CYAN;
  drawTechBar(16, yMot + 13, 96, 12, motorCfg.speed, 100, bCol);

  // 3. 預估轉速 RPM
  int rpm = map(motorCfg.speed, 0, 100, 0, 4800);
  char rpmBuf[24];
  snprintf(rpmBuf, sizeof(rpmBuf), "VEL %4d RPM", rpm);
  int rpmW = getTacticalStringWidth(rpmBuf, 1);
  drawTacticalString(16 + (96 - rpmW) / 2, yMot + 31, rpmBuf, COLOR_LIGHTGREY, COLOR_BG, 1);
}

void updateSensorsSubpage() {
  drawTacticalPrintf(16, 24, COLOR_LIGHTGREY, COLOR_BG, 1, "SYS TEMP: %2.1fC",
                     sysTemp);
  drawTechBar(16, 36, 96, 7, (int)sysTemp, 80, COLOR_AMBER);

  drawTacticalPrintf(16, 50, COLOR_LIGHTGREY, COLOR_BG, 1, "BUS VOLT: %1.2fV",
                     sysVolt);
  drawTechBar(16, 62, 96, 7, (int)(sysVolt * 10), 50, COLOR_GREEN);

  drawTacticalPrintf(16, 76, COLOR_LIGHTGREY, COLOR_BG, 1, "TOT CURR: %3dMA",
                     sysCurr);
  drawTechBar(16, 88, 96, 7, sysCurr, 300, COLOR_CYAN);
}

const char *getSubpageTitle() {
  if (carouselIndex == PAGE_LIGHT)   return "LIGHT";
  if (carouselIndex == PAGE_MOTOR)   return "MOTOR";
  if (carouselIndex == PAGE_SENSORS) return "THERMAL";
  return "";
}

const char *getHomeCardTitle() {
  if (carouselIndex == PAGE_LIGHT)   return ">LIGHT";
  if (carouselIndex == PAGE_MOTOR)   return ">MOTOR";
  if (carouselIndex == PAGE_SENSORS) return ">THERMAL";
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
    drawTacticalString(curX, curY, (i >= 3) ? subTitle : homeTitle, COLOR_AMBER, COLOR_BG, 1);
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
    drawTacticalString(curX, curY, (i <= 2) ? subTitle : homeTitle, COLOR_AMBER, COLOR_BG, 1);
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

void triggerSystemReset() {
  Serial.println("\n[SYSTEM] === 4-BUTTON SIMULTANEOUS PRESS DETECTED: HARDWARE SYSTEM RESET ===");

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
  tft.setRotation(0);

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

  motorCfg.speed = CFG_DEFAULT_MOTOR_SPEED;
  motorBackup = motorCfg;
  tofDistanceMm = -1;

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

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n==============================================");
  Serial.println("  WING UI - ESP32 NodeMCU-32S Tactical HUD    ");
  Serial.println("==============================================");
  Serial.println(
      "Display: ST7735 1.44\" (CS:5, DC:22, RES:EN, SCK:18, MOSI:23)");
  Serial.println("Serial Input Simulation Active:");
  Serial.println("  [Q] : Left  (< / UP / -)   -> Button C");
  Serial.println("  [W] : Right (> / DOWN / +) -> Button B");
  Serial.println("  [E] : ENTER (Confirm)      -> Button A");
  Serial.println("  [R] : BACK  (Return)       -> Button D");
  Serial.println("==============================================\n");

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
  // 初始化 DS18B20 溫度感測器 (設為非同步模式，避免卡頓)
  tempSensor.begin();
  tempSensor.setWaitForConversion(false);
  tempSensor.requestTemperatures();
  lastTempRequestTime = millis();
  tempConversionPending = true;

  // 初始化 VL53L0X 飛行時間測距感測器 (I2C: SDA=GPIO21, SCL=GPIO15)
  Wire.begin(PIN_TOF_SDA, PIN_TOF_SCL);
  if (lox.begin(VL53L0X_I2C_ADDR, false, &Wire)) {
    vl53l0xReady = true;
    Serial.println(F("[SENSOR] VL53L0X ToF Initialized OK (SDA:21, SCL:15)"));
  } else {
    vl53l0xReady = false;
    Serial.println(F("[SENSOR] VL53L0X ToF Init Failed / Not Detected!"));
  }

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
        } else if (currentState == STATE_HOME && carouselIndex == PAGE_SENSORS) {
          drawCentralCardContent();
        }
      }
    }
    tempConversionPending = false;
    lastTempRequestTime = now;
  }
  // 每隔 CFG_TEMP_SAMPLE_INTERVAL_MS 發起一次新的溫度轉換請求
  else if (!tempConversionPending && (now - lastTempRequestTime >= CFG_TEMP_SAMPLE_INTERVAL_MS)) {
    tempSensor.requestTemperatures();
    lastTempRequestTime = now;
    tempConversionPending = true;
  }

  // 超溫警報音效循環：若當前溫度超過門檻，每隔 CFG_TEMP_ALARM_INTERVAL_MS 重複播放警告音效
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

  int16_t newDist = -1;
  // RangeStatus 4 = Phase Fail (超出範圍或無障礙物)
  if (measure.RangeStatus != 4) {
    newDist = (int16_t)measure.RangeMilliMeter;
  }

  // 數值有變動時局部更新畫面
  if (newDist != tofDistanceMm) {
    tofDistanceMm = newDist;
    if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR) {
      drawSubpageTopBar();
      updateMotorSubpage();
    } else if (currentState == STATE_HOME && carouselIndex == PAGE_MOTOR) {
      // 僅即時更新右上角測距文字，避免重畫中央錶盤造成微抖動
      int cx = 14, cy = 19;
      if (tofDistanceMm >= 0) {
        drawTacticalPrintf(cx + 52, cy + 3, COLOR_CYAN, COLOR_BG, 1, "%4dMM", tofDistanceMm);
      } else {
        drawTacticalString(cx + 52, cy + 3, "--- MM", COLOR_DARKGREY, COLOR_BG, 1);
      }
    }
  }
}

void loop() {
  scanButtons();
  handleTemperatureSampling();
  handleTofSampling();
  handleLightAnimation();

  unsigned long now = millis();

  // 四顆按鍵同時持續按住 1 秒 (CFG_RESET_PRESS_DURATION_MS) 檢測 System Reset
  bool allFourDown = (btns[0].stableState == LOW &&
                      btns[1].stableState == LOW &&
                      btns[2].stableState == LOW &&
                      btns[3].stableState == LOW);

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

  // ================= 狀態 1：首頁 (HUD HOME) =================
  if (currentState == STATE_HOME) {
    // [右翼 >] 按鍵 B (W / B)：向右切換 (roll left)
    if (btns[1].justPressed) {
      sfxNavigate();
      playCyberTransition(false);
      carouselIndex = (carouselIndex + 1) % TOTAL_CAROUSEL;
      drawSideButtons();
      drawTechCentralFrame(blinkState);
      playPageEntranceAnimation(carouselIndex);
    }
    // [左翼 <] 按鍵 C (Q / C)：向左切換 (roll right)
    else if (btns[2].justPressed) {
      sfxNavigate();
      playCyberTransition(true);
      carouselIndex = (carouselIndex - 1 + TOTAL_CAROUSEL) % TOTAL_CAROUSEL;
      drawSideButtons();
      drawTechCentralFrame(blinkState);
      playPageEntranceAnimation(carouselIndex);
    }
    // [底右 ENTER] 按鍵 A (E / A)：選入分頁
    else if (btns[0].justPressed) {
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
        // [右翼 DOWN] 按鍵 B (W): 游標下移 (Navigate 音效，自動跳過灰底停用項目)
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
        bool bTrigger = btns[1].justPressed || (btns[1].repeatTriggered && (cursorRow == 0 || cursorRow == 1 || cursorRow == 2 || cursorRow == 4));
        if (bTrigger) {
          if (cursorRow == 0) {
            if (lightCfg.style != MODE_RAINBOW && lightCfg.style != MODE_PARTY) {
              lightCfg.hue = (lightCfg.hue + CFG_STEP_HUE) % 360;
            }
          }
          if (cursorRow == 1 && lightCfg.style != MODE_PARTY && (btns[1].justPressed || btns[1].repeatTriggered))
            lightCfg.sat = constrain(lightCfg.sat + CFG_STEP_SATURATION, 0, 100);
          if (cursorRow == 2 && (btns[1].justPressed || btns[1].repeatTriggered))
            lightCfg.val = constrain(lightCfg.val + CFG_STEP_BRIGHTNESS, 0, 100);
          if (cursorRow == 3 && btns[1].justPressed) {
            lightCfg.style = (lightCfg.style + 1) % TOTAL_LIGHT_MODES;
            for (int r = 0; r < 5; r++) updateLightRow(r);
          }
          if (cursorRow == 4) {
            if (lightCfg.style == MODE_RAINING) {
              lightCfg.speed = constrain(lightCfg.speed + CFG_STEP_SPEED, -50, 50);
            } else if (lightCfg.style != MODE_STATIC) {
              lightCfg.speed = constrain(lightCfg.speed + CFG_STEP_SPEED, 1, 100);
            }
          }
          if (cursorRow != 3) {
            updateLightRow(cursorRow);
            if (cursorRow == 0 && lightCfg.style != MODE_RAINBOW && lightCfg.style != MODE_PARTY) {
              updateLightRow(1);
              updateLightRow(2);
            }
          }
          drawSubpageTopBar();
        }
        // [左翼 -] 按鍵 C (Q): 數值減少 (靜音調節)
        bool cTrigger = btns[2].justPressed || (btns[2].repeatTriggered && (cursorRow == 0 || cursorRow == 1 || cursorRow == 2 || cursorRow == 4));
        if (cTrigger) {
          if (cursorRow == 0) {
            if (lightCfg.style != MODE_RAINBOW && lightCfg.style != MODE_PARTY) {
              lightCfg.hue = (lightCfg.hue - CFG_STEP_HUE + 360) % 360;
            }
          }
          if (cursorRow == 1 && lightCfg.style != MODE_PARTY && (btns[2].justPressed || btns[2].repeatTriggered))
            lightCfg.sat = constrain(lightCfg.sat - CFG_STEP_SATURATION, 0, 100);
          if (cursorRow == 2 && (btns[2].justPressed || btns[2].repeatTriggered))
            lightCfg.val = constrain(lightCfg.val - CFG_STEP_BRIGHTNESS, 0, 100);
          if (cursorRow == 3 && btns[2].justPressed) {
            lightCfg.style =
                (lightCfg.style - 1 + TOTAL_LIGHT_MODES) % TOTAL_LIGHT_MODES;
            for (int r = 0; r < 5; r++) updateLightRow(r);
          }
          if (cursorRow == 4) {
            if (lightCfg.style == MODE_RAINING) {
              lightCfg.speed = constrain(lightCfg.speed - CFG_STEP_SPEED, -50, 50);
            } else if (lightCfg.style != MODE_STATIC) {
              lightCfg.speed = constrain(lightCfg.speed - CFG_STEP_SPEED, 1, 100);
            }
          }
          if (cursorRow != 3) {
            updateLightRow(cursorRow);
            if (cursorRow == 0 && lightCfg.style != MODE_RAINBOW && lightCfg.style != MODE_PARTY) {
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
        if (btns[0].justPressed) {
          sfxEnter();
          isEditing = true;
          motorBackup = motorCfg;
          updateMotorSubpage();
          drawSideButtons();
          updateGlobalButtonBar();
        }
        if (btns[3].justPressed) {
          sfxBack();
          currentState = STATE_HOME;
          stateSwitchLockout = millis() + 200;
          playSubpageExitTransition();
        }
      } else {
        if (btns[1].justPressed) {
          motorCfg.speed = constrain(motorCfg.speed + CFG_STEP_MOTOR, 0, 100);
          updateMotorSubpage();
          drawSubpageTopBar();
        }
        if (btns[2].justPressed) {
          motorCfg.speed = constrain(motorCfg.speed - CFG_STEP_MOTOR, 0, 100);
          updateMotorSubpage();
          drawSubpageTopBar();
        }
        // [底右 ENTER] 按鍵 A (E) 儲存 或 [底左 BACK] 按鍵 D (R) 鎖定退出
        if (btns[0].justPressed) {
          sfxEnter();
          isEditing = false;
          updateMotorSubpage();
          drawSideButtons();
          updateGlobalButtonBar();
          drawSubpageTopBar();
        } else if (btns[3].justPressed) {
          sfxBack();
          motorCfg = motorBackup;
          isEditing = false;
          updateMotorSubpage();
          drawSideButtons();
          updateGlobalButtonBar();
          drawSubpageTopBar();
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