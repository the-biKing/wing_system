// ==========================================================
// WING SYSTEM - CENTRAL CONFIGURATION (config.h)
// All hardware pins, timing constants, sound, and thresholds
// ==========================================================
#pragma once
#include <Arduino.h>

// ==========================================================
// 1. 硬體引腳配置 (GPIO Pins - ESP32 NodeMCU-32S)
// ==========================================================

// --- SPI 顯示螢幕 (ST7735 1.44" 128x128) ---
#define TFT_CS    5   // 螢幕晶片選擇 (Chip Select)
#define TFT_DC    22  // 螢幕數據/命令選擇 (Data/Command)
#define TFT_RST   14  // 螢幕硬體重置腳位 (Reset)
#define TFT_SCLK  18  // VSPI 時脈 (SCK)
#define TFT_MOSI  23  // VSPI 主機輸出 (MOSI / SDA)

// --- 戰術按鍵 (四顆實體按鈕，內部上拉電阻，按至 GND 觸發) ---
#define PIN_KEY_A 26  // 按鍵 A [底右 ENTER]: 確定 / 進入編輯 / 儲存鎖定
#define PIN_KEY_B 25  // 按鍵 B [右翼 > / DOWN / +]: 下一頁 / 游標下移 / 數值增加
#define PIN_KEY_C 33  // 按鍵 C [左翼 < / UP / -]: 上一頁 / 游標上移 / 數值減少
#define PIN_KEY_D 27  // 按鍵 D [底左 BACK]: 返回 / 退出編輯鎖定

// --- 數位溫度感測器 (DS18B20 1-Wire) ---
#define PIN_TEMP_DS18B20 4   // 1-Wire 雙向通訊資料腳位 (需接 4.7kΩ 上拉電阻)

// --- 無源蜂鳴器 (Passive Buzzer - 低電平觸發模組) ---
#define BUZZER_PIN       13  // 蜂鳴器 PWM 驅動訊號腳位 (可依硬體自由更動為 2 或 13)
#define BUZZER_TIMER_RES 8   // LEDC PWM 計時器解析度 (8-bit)

// --- 飛行時間測距感測器 (VL53L0X ToF - I2C) ---
#define PIN_TOF_SDA      21  // I2C 資料線 (SDA)
#define PIN_TOF_SCL      15  // I2C 時脈線 (SCL)


// ==========================================================
// 2. 按鍵彈跳與連發行為設定 (Button Debounce & Repeat Timing)
// ==========================================================

// 按鍵去彈跳時間 (毫秒)：過濾機械接點雜訊
const unsigned long CFG_BUTTON_DEBOUNCE_MS = 45;

// 長按連發啟動延遲 (毫秒)：按住按鈕多久後開始自動快速增加/減少數值
const unsigned long CFG_BUTTON_REPEAT_START_MS = 1000;

// 連發觸發間隔 (毫秒)：長按時每隔多久步進一次 (40ms = 每秒約 25 階)
const unsigned long CFG_BUTTON_REPEAT_RATE_MS = 40;

// 四鍵同時長按系統重置延遲 (毫秒)：四顆鍵同時按住多久觸發硬體重置
const unsigned long CFG_RESET_PRESS_DURATION_MS = 1000;


// ==========================================================
// 3. 數值步進調節幅度 (Value Step Sizes for + / -)
// ==========================================================

// 單擊增加/減少的步進量
const uint16_t CFG_STEP_HUE        = 1;   // 色相 (0 ~ 359°) 每點一次增減度數
const uint8_t  CFG_STEP_SATURATION = 1;   // 飽和度 (0 ~ 100%) 每點一次增減%
const uint8_t  CFG_STEP_BRIGHTNESS = 1;   // 亮度 (0 ~ 100%) 每點一次增減%
const uint8_t  CFG_STEP_MOTOR      = 5;   // 馬達速度 (0 ~ 100%) 每點一次增減%
const int8_t   CFG_STEP_SPEED      = 1;   // 特效速度 (SPD) 每點一次增減幅度


// ==========================================================
// 4. 感測器採樣與警告門檻 (Sensors & Thermal Alarm)
// ==========================================================

// 溫度讀取週期 (毫秒)：發起下一輪 DS18B20 取樣的間隔
const unsigned long CFG_TEMP_SAMPLE_INTERVAL_MS = 1500;

// 超溫警告門檻 (°C)：高於此溫度觸發警告音效與警報畫面
const float CFG_TEMP_WARNING_THRESHOLD = 50.0f;

// 警報重複間隔 (毫秒)：溫度超標時，警報音效每隔多久重複響一次
const unsigned long CFG_TEMP_ALARM_INTERVAL_MS = 1500;

// ToF 測距感測器採樣間隔 (毫秒)：每隔多久向 VL53L0X 讀取一次距離 (mm)
const unsigned long CFG_TOF_SAMPLE_INTERVAL_MS = 60;


// ==========================================================
// 5. 系統預設參數 (Default Fallback Parameters)
// ==========================================================

#define CFG_DEFAULT_HUE         180
#define CFG_DEFAULT_SAT         100
#define CFG_DEFAULT_VAL         80
#define CFG_DEFAULT_MOTOR_SPEED 65
#define CFG_DEFAULT_LIGHT_SPEED 50

