#pragma once
#include <Arduino.h>

// ==========================================================
// 1. 硬體引腳定義 (Hardware Pin Assignments)
// ==========================================================
#define PIN_MOTOR_DIR       17  // 馬達正反轉方向控制 (H-Bridge DIR / IN1)
#define PIN_MOTOR_PWM       32  // 馬達轉速調速 (H-Bridge PWM / IN2)

#define PIN_TOF_SDA         21  // VL53L0X I2C 資料線 (SDA)
#define PIN_TOF_SCL         15  // VL53L0X I2C 時脈線 (SCL)

// ==========================================================
// 2. 馬達 PWM 驅動設定 (Motor PWM & Dead-time Config)
// ==========================================================
const uint32_t MOTOR_PWM_FREQ       = 5000; // 5 kHz PWM 頻率 (消除人耳尖銳嘯叫聲)
const uint8_t  MOTOR_PWM_RES        = 8;    // 8-bit 解析度 (0 ~ 255)
const uint8_t  MOTOR_PWM_CH         = 4;    // LEDC 通道 (ESP32 Core 2.x 使用)
const uint32_t MOTOR_DEAD_TIME_MS   = 15;   // 換向保護延遲 (ms)，防止 H 橋直通短路

// ==========================================================
// 3. 校準基線預設值 (Calibrated Baseline Defaults)
//    這些數值可由 test_sensor_stroke 與 test_actuation_pwm
//    實測獲得，並可寫入 NVS Preferences 持久化儲存。
// ==========================================================
#define CFG_DEFAULT_Y_MIN_MM        35.0f   // 機械下死點極限 (mm)
#define CFG_DEFAULT_Y_MAX_MM        85.0f   // 機械上死點極限 (mm)
#define CFG_DEFAULT_STROKE_SPAN_MM  50.0f   // 行程全幅 (y_max - y_min) (mm)

// --- 雙閥值摩擦補償 (Dual-Threshold Friction Compensation) ---
// (1) 靜摩擦突破前饋 (Breakaway / Stiction)：靜止時克服咬合啟動所需 (例如 100/255 ≈ 0.39)
#define CFG_DEFAULT_U_BREAKAWAY_POS 0.39f   // 正向靜摩擦突破 PWM
#define CFG_DEFAULT_U_BREAKAWAY_NEG 0.39f   // 反向靜摩擦突破 PWM

// (2) 動摩擦維持前饋 (Kinetic / Running)：轉動中維持運動最低所需 (例如 50/255 ≈ 0.20)
#define CFG_DEFAULT_U_KINETIC_POS   0.20f   // 正向動摩擦維持 PWM
#define CFG_DEFAULT_U_KINETIC_NEG   0.20f   // 反向動摩擦維持 PWM

// 運動速度切換過渡門檻 (mm/s)：低於此速度視為起步/微速，高於此速度平滑過渡至動摩擦補償
const float CFG_STICTION_VELOCITY_THRESH_MM_S = 6.0f;

// ADRC 控制增益基線
#define CFG_DEFAULT_B0_INIT         12.0f   // 初始系統控制權重 b0
#define CFG_DEFAULT_B0_MIN          0.8f    // Scotch Yoke 死點奇異值正規化下限 clamp
#define CFG_DEFAULT_POLARITY        1       // 1: 正 PWM 增加距離, -1: 正 PWM 減少距離

// ==========================================================
// 4. 安全防護欄門檻 (Guardrails & Safety Limits)
// ==========================================================
// 行程超限容許邊界 (mm)：若距離小於 (y_min - MARGIN) 或大於 (y_max + MARGIN)，觸發急停
const float    GUARDRAIL_MARGIN_MM    = 7.0f;

// 堵轉保護 (Stall Protection)：若控制輸出超過閾值且距離無變化超過此時間 (ms)，急停保護
const float    GUARDRAIL_STALL_PWM    = 0.45f; // 45% PWM
const float    GUARDRAIL_STALL_MIN_DY = 1.2f;  // 最小應移動距離 (mm)
const uint32_t GUARDRAIL_STALL_MS     = 600;   // 600ms 堵轉超時

// 感測器失聯/失效超時 (ms)
const uint32_t SENSOR_TIMEOUT_MS      = 250;

// ==========================================================
// 5. ADRC 控制器與擴展狀態觀測器 (ESO) 頻寬參數
// ==========================================================
const float ADRC_OMEGA_O  = 35.0f;  // 觀測器頻寬 (Observer Bandwidth, rad/s)
const float ADRC_OMEGA_C  = 7.0f;   // 控制器頻寬 (Controller Bandwidth, rad/s)

// CFDL 動態線性化參數
const float CFDL_RHO      = 0.25f;  // 自適應步長學習率
const float CFDL_MU       = 0.02f;  // 正規化防除以零常數
const float CFDL_EPS_U    = 0.015f; // 激勵輸入變動閾值
const float CFDL_EPS_A    = 0.008f; // 激勵加速度變動閾值
