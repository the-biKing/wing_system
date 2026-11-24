// ==========================================================
// WING SYSTEM - CENTRAL CONFIGURATION (config.h)
// All hardware pins, timing constants, sound, and thresholds
// ==========================================================
#pragma once
#include <Arduino.h>

// ==========================================================
// 1. 硬體引腳配置總表 (GPIO Master Pinout - ESP32 NodeMCU-32S)
// ==========================================================

// --- SPI 顯示螢幕 (ST7735 1.44" 128x128) ---
#define TFT_CS    5   // 螢幕晶片選擇 (Chip Select / SS)
#define TFT_DC    22  // 螢幕數據/命令選擇 (Data / Command)
#define TFT_RST   14  // 螢幕硬體重置腳位 (Reset)
#define TFT_SCLK  18  // VSPI 時脈 (SCK)
#define TFT_MOSI  23  // VSPI 主機輸出 (MOSI / SDA)
#define TFT_ROTATION 2 // 螢幕方向 (0: 正向, 2: 旋轉 180度 上下顛倒安裝)

// --- 戰術按鍵 (四顆實體按鈕，內部上拉電阻，按至 GND 觸發) ---
#define PIN_KEY_A 26  // 按鍵 A [底右 ENTER]: 確定 / 進入編輯 / 儲存鎖定
#define PIN_KEY_B 25  // 按鍵 B [右翼 > / DOWN / +]: 下一頁 / 游標下移 / 數值增加
#define PIN_KEY_C 33  // 按鍵 C [左翼 < / UP / -]: 上一頁 / 游標上移 / 數值減少
#define PIN_KEY_D 27  // 按鍵 D [底左 BACK]: 返回 / 退出編輯鎖定

// --- 直流馬達驅動 (H-Bridge PWM / DIR - 離散式 MOSFET H橋) ---
#define PIN_MOTOR_DIR         17  // 馬達正反轉方向控制 (H-Bridge DIR / IN1)
#define PIN_MOTOR_PWM         32  // 馬達轉速調速 (H-Bridge PWM / IN2)

// --- 飛行時間測距感測器 (VL53L0X ToF - I2C) ---
#define PIN_TOF_SDA           21  // I2C 資料線 (SDA)
#define PIN_TOF_SCL           15  // I2C 時脈線 (SCL)

// --- 數位溫度感測器 (DS18B20 1-Wire) ---
#define PIN_TEMP_DS18B20      4   // 1-Wire 雙向通訊資料腳位 (需接 4.7kΩ 上拉電阻)

// --- 無源蜂鳴器 (Passive Buzzer - 低電平觸發模組) ---
#define BUZZER_PIN            13   // 蜂鳴器 PWM 驅動訊號腳位 (可依硬體自由更動為 2 或 13)
#define BUZZER_TIMER_RES      8    // LEDC PWM 計時器解析度 (8-bit)
#define BUZZER_IDLE_LEVEL     HIGH // 靜態空載電平：低電平觸發模組預設 HIGH 徹底截止 PNP 三極管防雜音

// --- ACS712 電流感測器 (Current Sensors - 類比輸入) ---
#define PIN_CURRENT_ACS712_1  35  // 電流感測器 1 (ACS712-20A, GPIO 35)
#define PIN_CURRENT_ACS712_2  34  // 電流感測器 2 (ACS712T-05B, GPIO 34)
#define PIN_CURRENT_ACS712    PIN_CURRENT_ACS712_1 // 向後相容別名

// --- WS2812 幻彩 RGB 燈條 (NeoPixel 實體輸出) ---
#define PIN_WS2812            16  // WS2812 資料訊號腳位 (Data Line)
#define PIN_WS2812_EN         19  // WS2812 電源開關致能腳位 (NMOS Gate: HIGH=供電, LOW=關閉阻斷靜態漏電)
#define CFG_WS2812_NUM_LEDS   400 // 實體燈珠數量 (400顆 WS2812 RGB)

// --- 2S 鋰電池電量檢測 (GPIO 36, 180kΩ : 36kΩ 分壓) ---
#define PIN_BATTERY_ADC       36  // 電池分壓類比讀取腳位 (SENSOR_VP / ADC1_CH0)
#define PIN_BATTERY_EN        2   // 電池分壓量測致能腳位 (NMOS Gate: HIGH=導通測量, LOW=關閉阻斷漏電)


// ==========================================================
// 2. 馬達 PWM 驅動設定 (Motor PWM & Dead-time Config)
// ==========================================================
const uint32_t MOTOR_PWM_FREQ       = 1000; // 1 kHz PWM 頻率 (配合 MOSFET 10k 洩放電阻)
const uint8_t  MOTOR_PWM_RES        = 8;    // 8-bit 解析度 (0 ~ 255)
const uint8_t  MOTOR_PWM_CH         = 4;    // LEDC 通道 (ESP32 Core 2.x 使用)
const uint32_t MOTOR_DEAD_TIME_MS   = 10;   // 換向保護延遲 (ms)，防止 H 橋直通短路


// ==========================================================
// 3. 蘇格蘭軛機構與行程校準基準 (Calibrated Stroke Baseline)
// ==========================================================
#define CFG_DEFAULT_Y_MIN_MM        53.7f   // 機械下死點極限 (mm) - 實測校準鎖定
#define CFG_DEFAULT_Y_MAX_MM        101.7f  // 機械上死點極限 (mm) - 實測校準鎖定
#define CFG_DEFAULT_STROKE_SPAN_MM  48.0f   // 行程全幅 (y_max - y_min) (mm)

// 安全軟體限位 (Soft Limits): 預留死點緩衝避免頂死點造成翻轉失控
const float SOFT_LIMIT_LOW_OFFSET_MM  = 2.0f; // 下死點保留 2.0mm 淨空 (55.7 mm)
const float SOFT_LIMIT_HIGH_OFFSET_MM = 2.5f; // 上死點保留 2.5mm 淨空 (99.2 mm)

// Scotch Yoke 前饋扭矩摩擦常數 (Position-Dependent Friction)
#define CFG_DEFAULT_U_MID_BREAKAWAY  0.118f   // 行程中段起步 PWM ~30
#define CFG_DEFAULT_U_DEAD_BREAKAWAY 0.400f   // 死點端點起步 PWM ~102


// ==========================================================
// 4. 安全防護欄門檻 (Guardrails & Safety Limits)
// ==========================================================
// 安全防護欄狀態
enum GuardrailState {
  GUARD_OK = 0,
  GUARD_TRIPPED_OOB,            // 距離超出物理行程安全容許範圍
  GUARD_TRIPPED_SENSOR_TIMEOUT, // 感測器失聯或持續無回波
  GUARD_TRIPPED_STALL           // 輸出高 PWM 但無位移 (堵轉)
};

const float    GUARDRAIL_MARGIN_MM    = 7.0f;   // 容許超出實體死點裕度 (mm)
const float    GUARDRAIL_STALL_PWM    = 210.0f; // 堵轉 PWM 輸出門檻 (210/255 ≈ 82% 功率)
const float    GUARDRAIL_STALL_MIN_DY = 1.5f;   // 最小位移距離 (mm)
const uint32_t GUARDRAIL_STALL_MS     = 2000;   // 堵轉保護判定時間 (2.0s)
const uint32_t SENSOR_TIMEOUT_MS      = 300;    // 感測器失聯超時 (ms)


// ==========================================================
// 5. 感測器採樣與電源參數 (Sensors, ADC & Power)
// ==========================================================

// 2S 鋰電池電壓分壓參數 (180kΩ : 36kΩ)
const float CFG_BAT_R_HIGH    = 180.0f; // 上分壓電阻 (kΩ)
const float CFG_BAT_R_LOW     = 36.0f;  // 下分壓電阻 (kΩ)
// 分壓比 = (180 + 36) / 36 = 216 / 36 = 6.0
const float CFG_BAT_DIVIDER_RATIO = (CFG_BAT_R_HIGH + CFG_BAT_R_LOW) / CFG_BAT_R_LOW;
const float CFG_BAT_VOLT_MAX  = 8.4f;   // 滿電電壓 (4.2V x 2)
const float CFG_BAT_VOLT_MIN  = 6.4f;   // 截止電壓 (3.2V x 2, 保護鋰電池不過放)
const unsigned long CFG_BAT_SAMPLE_INTERVAL_MS = 1000; // 電池取樣間隔 (1秒)

// 溫度讀取週期 (毫秒)：發起下一輪 DS18B20 取樣的間隔
const unsigned long CFG_TEMP_SAMPLE_INTERVAL_MS = 1500;
const float CFG_TEMP_WARNING_THRESHOLD = 50.0f;        // 超溫警告門檻 (°C)
const unsigned long CFG_TEMP_ALARM_INTERVAL_MS = 1500; // 警報重複間隔 (毫秒)

// ToF 測距感測器採樣間隔 (毫秒)
const unsigned long CFG_TOF_SAMPLE_INTERVAL_MS = 60;

// ACS712-20A (電流感測器 1, GPIO 35)
const float CFG_ACS712_1_SENSITIVITY_MV_PER_A = 100.0f;
const float CFG_ACS712_1_ZERO_MV              = 1663.0f;
const float CFG_ACS712_1_DIVIDER_RATIO        = 1.0f;
const float CFG_ACS712_1_DEADBAND_AMPS        = 0.06f;

// ACS712T-05B (電流感測器 2, GPIO 34 - 監測 BMS 至 H橋 S09 12V 供電路徑)
// 05B (5A) 規格靈敏度為 185 mV/A (0.185 V/A)
const float CFG_ACS712_2_SENSITIVITY_MV_PER_A = 185.0f;
// 實測無負載靜態中點電壓約 1750mV (先前 1232mV 偏差導致計算出 2.8A)
const float CFG_ACS712_2_ZERO_MV              = 1750.0f;
const float CFG_ACS712_2_DIVIDER_RATIO        = 1.0f;
const float CFG_ACS712_2_DEADBAND_AMPS        = 0.08f;
// 開啟開機自動採樣校準 (Tare)，通電無負載時自動校正 0A 基準
const bool  CFG_ACS712_AUTO_ZERO              = true;
const unsigned long CFG_CURRENT_SAMPLE_INTERVAL_MS = 60;

// --- 系統過電流防護門檻 (Overcurrent Protection) ---
const float CFG_OVERCURRENT_LIMIT_A = 18.0f; // 18A 電流安全上限 (阻止發出更高亮度並限流)
const float CFG_OVERCURRENT_HYST_A  = 1.5f;  // 1.5A 遲滯恢復區間 (降至 16.5A 以下才允許恢復)


// ==========================================================
// 6. WiFi AP 與 Web 戰術控制台設定 (WiFi & Mobile Web HUD)
// ==========================================================
#define CFG_WIFI_AP_SSID      "WING-SYSTEM-HUD" // 基地台名稱 (手機連線)
#define CFG_WIFI_AP_PASS      "12345678"        // 基地台密碼 (8位數以上)
#define CFG_WIFI_AP_CHANNEL   1
#define CFG_WIFI_MDNS_HOST    "wing"            // mDNS: http://wing.local


// ==========================================================
// 7. 按鍵彈跳與連發行為設定 (Button Debounce & Repeat Timing)
// ==========================================================
const unsigned long CFG_BUTTON_DEBOUNCE_MS = 45;
const unsigned long CFG_BUTTON_REPEAT_START_MS = 1000;
const unsigned long CFG_BUTTON_REPEAT_RATE_MS = 40;
const unsigned long CFG_RESET_PRESS_DURATION_MS = 1000;
const unsigned long CFG_SLEEP_HOLD_MS = 3000;
const unsigned long CFG_WAKE_HOLD_MS = 2000;
const unsigned long CFG_MUTE_HOLD_MS = 1500;


// ==========================================================
// 8. 數值步進調節幅度 (Value Step Sizes for + / -)
// ==========================================================
const uint16_t CFG_STEP_HUE        = 1;   // 色相 (0 ~ 359°) 每點一次增減度數
const uint8_t  CFG_STEP_SATURATION = 1;   // 飽和度 (0 ~ 100%) 每點一次增減%
const uint8_t  CFG_STEP_BRIGHTNESS = 1;   // 亮度 (0 ~ 100%) 每點一次增減%
const uint8_t  CFG_STEP_MOTOR      = 5;   // 馬達速度 (0 ~ 100%) 每點一次增減%
const uint8_t  CFG_STEP_TARGET_POS = 5;   // 目標指定位置 (0 ~ 100%) 每點一次增減%
const int8_t   CFG_STEP_SPEED      = 1;   // 特效速度 (SPD) 每點一次增減幅度


// ==========================================================
// 9. 系統預設參數與資料結構 (Default Parameters & Structures)
// ==========================================================
#define CFG_DEFAULT_HUE         180
#define CFG_DEFAULT_SAT         100
#define CFG_DEFAULT_VAL         80
#define CFG_DEFAULT_MOTOR_SPEED 0
#define CFG_DEFAULT_TARGET_POS  50
#define CFG_DEFAULT_LIGHT_SPEED 50

// 系統狀態機定義
enum SysState { STATE_HOME, STATE_SUBPAGE };
enum CarouselItem { PAGE_LIGHT = 0, PAGE_MOTOR = 1, PAGE_SENSORS = 2 };

// 燈效模式定義
enum LightMode {
  MODE_STATIC = 0,
  MODE_RAINBOW = 1,
  MODE_BREATHING = 2,
  MODE_THUNDER = 3,
  MODE_RAINING = 4,
  MODE_PARTY = 5,
  TOTAL_LIGHT_MODES = 6
};

struct LightConfig {
  uint16_t hue = CFG_DEFAULT_HUE;
  uint8_t sat = CFG_DEFAULT_SAT;
  uint8_t val = CFG_DEFAULT_VAL;
  uint8_t style = MODE_STATIC;
  int8_t speed = CFG_DEFAULT_LIGHT_SPEED;
};

struct MotorConfig {
  uint8_t targetPos = CFG_DEFAULT_TARGET_POS; // 指定位置 (0 ~ 100%)
  uint8_t speed = 0;                          // 手動模式轉速 (0 ~ 100%)
};
