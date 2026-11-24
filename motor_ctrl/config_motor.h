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
const uint32_t MOTOR_PWM_FREQ       = 1000; // 1 kHz PWM 頻率 (配合 MOSFET 10k 洩放電阻)
const uint8_t  MOTOR_PWM_RES        = 8;    // 8-bit 解析度 (0 ~ 255)
const uint8_t  MOTOR_PWM_CH         = 4;    // LEDC 通道 (ESP32 Core 2.x 使用)
const uint32_t MOTOR_DEAD_TIME_MS   = 15;   // 換向保護延遲 (ms)，防止 H 橋直通短路

// ==========================================================
// 3. 校準基線預設值 (Calibrated Baseline Defaults)
//    這些數值可由 test_sensor_stroke 與 test_actuation_pwm
//    實測獲得，並可寫入 NVS Preferences 持久化儲存。
// ==========================================================
#define CFG_DEFAULT_Y_MIN_MM        53.7f   // 機械下死點極限 (mm) - 實測校準鎖定 (最新校準)
#define CFG_DEFAULT_Y_MAX_MM        101.7f  // 機械上死點極限 (mm) - 實測校準鎖定 (最新校準)
#define CFG_DEFAULT_STROKE_SPAN_MM  48.0f   // 行程全幅 (y_max - y_min) (mm)

// --- 基於實測之 Scotch Yoke 位置相依摩擦補償 (Position-Dependent Friction) ---
// (1) 行程中央滑動摩擦 (Mid-Stroke ~50%): 實測只需要 PWM 22~30
#define CFG_DEFAULT_U_MID_BREAKAWAY  0.118f   // 行程中段起步 PWM ~30
#define CFG_DEFAULT_U_MID_KINETIC    0.086f   // 行程中段維持 PWM ~22

// (2) 幾何死點極限摩擦 (Dead-Center 0% & 100%): 實測需要 PWM 100~104
#define CFG_DEFAULT_U_DEAD_BREAKAWAY 0.400f   // 死點端點起步 PWM ~102
#define CFG_DEFAULT_U_DEAD_KINETIC   0.380f   // 死點端點維持 PWM ~97

// 運動速度切換過渡門檻 (mm/s)
const float CFG_STICTION_VELOCITY_THRESH_MM_S = 6.0f;

// ADRC 控制增益基線 (實測動態響應換算)
#define CFG_DEFAULT_B0_INIT         94.4f   // 初始系統控制權重 b0
#define CFG_DEFAULT_B0_MIN          3.0f    // Scotch Yoke 死點奇異值正規化下限 clamp
#define CFG_DEFAULT_POLARITY        1       // 1: 正 PWM 增加距離, -1: 正 PWM 減少距離

// ==========================================================
// 4. 安全防護欄門檻 (Guardrails & Safety Limits)
// ==========================================================
// 行程超限容許邊界 (mm)：若距離小於 (y_min - MARGIN) 或大於 (y_max + MARGIN)，觸發急停
const float    GUARDRAIL_MARGIN_MM    = 7.0f;

// 堵轉保護 (Stall Protection)：若控制輸出超過閾值且距離無變化超過此時間 (ms)，急停保護
const float    GUARDRAIL_STALL_PWM    = 210.0f; // PWM 輸出門檻 (210/255 ≈ 82% 功率)
const float    GUARDRAIL_STALL_MIN_DY = 1.5f;   // 最小應移動距離 (mm)
const uint32_t GUARDRAIL_STALL_MS     = 2000;   // 放寬至 2000ms (2.0 秒)，允許大機構啟動克服慣性

// 感測器失聯/失效超時 (ms)
const uint32_t SENSOR_TIMEOUT_MS      = 250;

// ==========================================================
// 5. ADRC 控制器與擴展狀態觀測器 (ESO) 頻寬參數 (數值收斂調優)
// ==========================================================
const float ADRC_OMEGA_O  = 12.0f;  // 觀測器頻寬 (rad/s) - 確保 10ms 離散尤拉積分數值收斂
const float ADRC_OMEGA_C  = 4.0f;   // 控制器頻寬 (rad/s) - Kp=16, Kd=8

// CFDL 動態線性化參數
const float CFDL_RHO      = 0.15f;  // 自適應步長學習率
const float CFDL_MU       = 0.05f;  // 正規化防除以零常數
const float CFDL_EPS_U    = 0.02f;  // 激勵輸入變動閾值
const float CFDL_EPS_A    = 0.01f;  // 激勵加速度變動閾值
