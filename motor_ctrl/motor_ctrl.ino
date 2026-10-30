#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <Preferences.h>
#include "config_motor.h"
#include "adrc_core.h"

// ==========================================================
// 系統實例與變數宣告
// ==========================================================
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
Preferences motorPrefs;

ADRC_Controller_t adrc;
DelayBuffer_t delayBuf;

bool tofReady = false;
float currentDistanceMm = 0.0f;
unsigned long lastSensorReadTime = 0;

// 控制模式定義
enum ControlMode {
  MODE_IDLE,           // 待機停機
  MODE_STATION_HOLD,   // 定點精確伺服保持 (Station-Keeping)
  MODE_SWEEP_CYCLE     // 週期性往復往返掃描 (Reciprocating Flapping)
};

ControlMode ctrlMode = MODE_IDLE;

// 安全防護欄狀態
enum GuardrailState {
  GUARD_OK = 0,
  GUARD_TRIPPED_OOB,            // 距離超出物理行程安全容許範圍
  GUARD_TRIPPED_SENSOR_TIMEOUT, // 感測器失聯或持續無回波
  GUARD_TRIPPED_STALL           // 輸出高 PWM 但無位移 (機械卡死/堵轉)
};

GuardrailState guardStatus = GUARD_OK;

// 設定點變數
float targetPct = 50.0f;        // 目標百分比行程 [0.0 ~ 100.0%]
float targetMm = 0.0f;          // 目標物理位置 [mm]

// 週期掃描模式參數 (Sweep Mode)
float sweepMinPct = 10.0f;
float sweepMaxPct = 90.0f;
float sweepFreqHz = 0.8f;       // 0.8 Hz 循環頻率
unsigned long sweepStartTime = 0;

// 堵轉檢測計時器
unsigned long highEffortStartTime = 0;
float stallCheckStartDist = 0.0f;

// 控制迴圈排程 (100 Hz / 10ms)
const unsigned long CTRL_PERIOD_US = 10000;
unsigned long lastCtrlTimeUs = 0;

// Serial Plotter 繪圖模式
bool plotterOutput = false;

// ==========================================================
// 底層馬達驅動函式 (H-Bridge)
// ==========================================================
void setMotorCommand(float u_cmd) {
  // u_cmd 範圍為 -1.0 ~ +1.0
  u_cmd = adrc_clamp(u_cmd, -1.0f, 1.0f);

  if (fabs(u_cmd) < 0.01f || guardStatus != GUARD_OK || !adrc.is_armed) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_MOTOR_PWM, 0);
#else
    ledcWrite(MOTOR_PWM_CH, 0);
#endif
    return;
  }

  bool forward = (u_cmd >= 0.0f);
  uint8_t duty = (uint8_t)(fabs(u_cmd) * 255.0f);

  digitalWrite(PIN_MOTOR_DIR, forward ? HIGH : LOW);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, duty);
#else
  ledcWrite(MOTOR_PWM_CH, duty);
#endif
}

void emergencyStop(GuardrailState reason) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, 0);
#else
  ledcWrite(MOTOR_PWM_CH, 0);
#endif

  adrc.is_armed = false;
  ctrlMode = MODE_IDLE;
  guardStatus = reason;

  Serial.println(F("\n========================================================"));
  Serial.println(F(">> [CRITICAL SAFETY GUARDRAIL TRIGGERED - MOTOR HALTED] <<"));
  Serial.println(F("========================================================"));
  if (reason == GUARD_TRIPPED_OOB) {
    Serial.printf("  * REASON: Out-of-Bounds! Measured: %0.1f mm (Allowed: %0.1f ~ %0.1f mm)\n",
                  currentDistanceMm, adrc.y_min - GUARDRAIL_MARGIN_MM, adrc.y_max + GUARDRAIL_MARGIN_MM);
    Serial.println(F("    Mechanical stroke or sensor mounting has shifted!"));
  } else if (reason == GUARD_TRIPPED_SENSOR_TIMEOUT) {
    Serial.println(F("  * REASON: Sensor Timeout / Disconnected! No valid ToF readings."));
  } else if (reason == GUARD_TRIPPED_STALL) {
    Serial.println(F("  * REASON: Motor Jam / Stall Detected! High effort with zero motion."));
  }
  Serial.println(F(">> Send 'reset' in Serial Monitor after clearing fault to re-arm.\n"));
}

// 讀取 VL53L0X 距離 (mm)
float readTofMm() {
  if (!tofReady) return -1.0f;
  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);
  if (measure.RangeStatus != 4) {
    lastSensorReadTime = millis();
    return (float)measure.RangeMilliMeter;
  }
  return -1.0f;
}

// 載入基線參數 (優先讀取 NVS Preferences，若無則載入 config_motor.h 預設值)
void loadBaselineParameters() {
  motorPrefs.begin("wing_motor", false);

  adrc.y_min           = motorPrefs.getFloat("y_min", CFG_DEFAULT_Y_MIN_MM);
  adrc.y_max           = motorPrefs.getFloat("y_max", CFG_DEFAULT_Y_MAX_MM);
  adrc.stroke_span     = adrc.y_max - adrc.y_min;

  adrc.u_breakaway_pos = motorPrefs.getFloat("u_break_p", CFG_DEFAULT_U_BREAKAWAY_POS);
  adrc.u_breakaway_neg = motorPrefs.getFloat("u_break_n", CFG_DEFAULT_U_BREAKAWAY_NEG);
  adrc.u_kinetic_pos   = motorPrefs.getFloat("u_kin_p",   CFG_DEFAULT_U_KINETIC_POS);
  adrc.u_kinetic_neg   = motorPrefs.getFloat("u_kin_n",   CFG_DEFAULT_U_KINETIC_NEG);

  adrc.polarity        = motorPrefs.getInt("polarity", CFG_DEFAULT_POLARITY);
  adrc.b0_init         = motorPrefs.getFloat("b0_init", CFG_DEFAULT_B0_INIT);
  adrc.b0              = adrc.b0_init;

  motorPrefs.end();

  Serial.println(F(">> Loaded Baseline Calibration Profile:"));
  Serial.printf("   - Physical Stroke Range : %0.1f mm ~ %0.1f mm (Span: %0.1f mm)\n",
                adrc.y_min, adrc.y_max, adrc.stroke_span);
  Serial.printf("   - Breakaway Stiction    : Pos: %0.3f | Neg: %0.3f\n",
                adrc.u_breakaway_pos, adrc.u_breakaway_neg);
  Serial.printf("   - Kinetic Running       : Pos: %0.3f | Neg: %0.3f\n",
                adrc.u_kinetic_pos, adrc.u_kinetic_neg);
  Serial.printf("   - Baseline Gain b0      : %0.1f | Polarity: %d\n",
                adrc.b0, adrc.polarity);
  Serial.printf("   - Guardrail Limits      : Low: %0.1f mm | High: %0.1f mm\n",
                adrc.y_min - GUARDRAIL_MARGIN_MM, adrc.y_max + GUARDRAIL_MARGIN_MM);
}

// 儲存當前校準參數至 NVS Preferences
void saveBaselineParameters() {
  motorPrefs.begin("wing_motor", false);
  motorPrefs.putFloat("y_min", adrc.y_min);
  motorPrefs.putFloat("y_max", adrc.y_max);
  motorPrefs.putFloat("u_break_p", adrc.u_breakaway_pos);
  motorPrefs.putFloat("u_break_n", adrc.u_breakaway_neg);
  motorPrefs.putFloat("u_kin_p", adrc.u_kinetic_pos);
  motorPrefs.putFloat("u_kin_n", adrc.u_kinetic_neg);
  motorPrefs.putInt("polarity", adrc.polarity);
  motorPrefs.putFloat("b0_init", adrc.b0);
  motorPrefs.end();
  Serial.println(F(">> [SUCCESS] Baseline calibration saved to ESP32 Flash Preferences!"));
}

void printHelp() {
  Serial.println(F("\n========================================================"));
  Serial.println(F("   WING SYSTEM - ADRC SCOTCH YOKE POSITION CONTROLLER"));
  Serial.println(F("========================================================"));
  Serial.println(F("Available Serial Commands:"));
  Serial.println(F("  'arm'                 : Enable motor controller"));
  Serial.println(F("  'stop' / 'disarm'     : Immediately disable motor"));
  Serial.println(F("  'pos <0..100>'        : Set stroke target percentage (e.g. 'pos 50')"));
  Serial.println(F("  'sweep <min> <max> <f>: Start continuous sweep (e.g. 'sweep 15 85 0.8')"));
  Serial.println(F("  'status'              : Display real-time controller & ESO states"));
  Serial.println(F("  'plot'                : Toggle CSV output for Arduino Serial Plotter"));
  Serial.println(F("  'save'                : Commit current baseline to NVS Flash"));
  Serial.println(F("  'reset'               : Clear tripped safety guardrails and re-arm\n"));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  printHelp();

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

  Wire.begin(PIN_TOF_SDA, PIN_TOF_SCL);
  Wire.setClock(400000);

  if (lox.begin(VL53L0X_I2C_ADDR, false, &Wire)) {
    tofReady = true;
    Serial.println(F("[OK] VL53L0X Distance Sensor Initialized."));
  } else {
    tofReady = false;
    Serial.println(F("[ERROR] VL53L0X Not Found! Guardrail will prevent arming."));
  }

  // 初始化 ADRC 控制器與延遲補償環形緩衝區
  float dt_seconds = (float)CTRL_PERIOD_US / 1000000.0f; // 0.01s (100 Hz)
  ADRC_Init(&adrc, dt_seconds);
  DelayBuffer_Init(&delayBuf, 2); // 補償約 2 個控制週期感測器傳輸延遲

  loadBaselineParameters();

  // 取得初始測距以暖啟動觀測器狀態 z1
  delay(100);
  float d_init = readTofMm();
  if (d_init > 0.0f) {
    currentDistanceMm = d_init;
    adrc.z1 = d_init;
    adrc.z2 = 0.0f;
    adrc.z3 = 0.0f;
    targetMm = d_init;
    targetPct = ADRC_MmToPct(&adrc, d_init);
    Serial.printf("[INIT] System Resting at Position: %0.1f mm (%0.1f%%)\n", currentDistanceMm, targetPct);
  }

  lastCtrlTimeUs = micros();
  lastSensorReadTime = millis();
}

void loop() {
  // ========================================================
  // 1. 處理 Serial 輸入命令
  // ========================================================
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      if (line.equalsIgnoreCase("arm")) {
        if (guardStatus == GUARD_OK && tofReady) {
          adrc.is_armed = true;
          ctrlMode = MODE_STATION_HOLD;
          targetMm = ADRC_PctToMm(&adrc, targetPct);
          Serial.println(F(">> [ARMED] Controller Active. Holding target position."));
        } else {
          Serial.printf(">> [DENIED] Cannot arm! Guardrail status: %d, ToF: %d\n", guardStatus, tofReady);
        }
      } else if (line.equalsIgnoreCase("stop") || line.equalsIgnoreCase("disarm") || line.equalsIgnoreCase("s")) {
        setMotorCommand(0.0f);
        adrc.is_armed = false;
        ctrlMode = MODE_IDLE;
        Serial.println(F(">> [DISARMED] Motor Stopped. Idle mode."));
      } else if (line.startsWith("pos ") || line.startsWith("POS ")) {
        float p = line.substring(4).toFloat();
        targetPct = adrc_clamp(p, 0.0f, 100.0f);
        targetMm = ADRC_PctToMm(&adrc, targetPct);
        ctrlMode = MODE_STATION_HOLD;
        Serial.printf(">> Setpoint Target: %0.1f%% (%0.2f mm)\n", targetPct, targetMm);
      } else if (line.startsWith("sweep ") || line.startsWith("SWEEP ")) {
        // 格式: sweep <min_pct> <max_pct> <freq_hz>
        int idx1 = line.indexOf(' ', 6);
        int idx2 = line.indexOf(' ', idx1 + 1);
        if (idx1 > 0 && idx2 > 0) {
          sweepMinPct = line.substring(6, idx1).toFloat();
          sweepMaxPct = line.substring(idx1 + 1, idx2).toFloat();
          sweepFreqHz = line.substring(idx2 + 1).toFloat();
          sweepStartTime = millis();
          ctrlMode = MODE_SWEEP_CYCLE;
          Serial.printf(">> Starting Sweep: %0.1f%% ~ %0.1f%% @ %0.2f Hz\n",
                        sweepMinPct, sweepMaxPct, sweepFreqHz);
        }
      } else if (line.equalsIgnoreCase("status")) {
        Serial.println(F("\n--- ADRC CONTROLLER TELEMETRY ---"));
        Serial.printf("Armed: %s | Mode: %d | Guard: %d\n",
                      adrc.is_armed ? "YES" : "NO", ctrlMode, guardStatus);
        Serial.printf("Target Position   : %0.1f mm (%0.1f%%)\n", targetMm, targetPct);
        Serial.printf("Sensor Measured   : %0.1f mm\n", currentDistanceMm);
        Serial.printf("ESO Estimates     : z1 (Pos)=%0.1f mm | z2 (Vel)=%0.1f mm/s | z3 (Disturb)=%0.1f mm/s^2\n",
                      adrc.z1, adrc.z2, adrc.z3);
        Serial.printf("Adaptive Gain b0  : %0.2f (Base: %0.2f, Clamp: %0.2f)\n",
                      adrc.b0, adrc.b0_init, adrc.b0_min);
        Serial.printf("Last Command u    : %0.3f\n\n", adrc.u_prev1);
      } else if (line.equalsIgnoreCase("plot")) {
        plotterOutput = !plotterOutput;
        Serial.printf(">> Plotter Stream: %s\n", plotterOutput ? "ON" : "OFF");
      } else if (line.equalsIgnoreCase("save")) {
        saveBaselineParameters();
      } else if (line.equalsIgnoreCase("reset")) {
        guardStatus = GUARD_OK;
        adrc.z1 = currentDistanceMm;
        adrc.z2 = 0.0f;
        adrc.z3 = 0.0f;
        Serial.println(F(">> Guardrail reset. Send 'arm' to re-engage."));
      }
    }
  }

  // ========================================================
  // 2. 非同步感測器讀取 (Asynchronous ToF Sampling)
  // ========================================================
  float freshDist = readTofMm();
  if (freshDist > 0.0f) {
    currentDistanceMm = freshDist;
  }

  // 感測器逾時失聯防護
  if (millis() - lastSensorReadTime > SENSOR_TIMEOUT_MS) {
    if (adrc.is_armed) {
      emergencyStop(GUARD_TRIPPED_SENSOR_TIMEOUT);
      return;
    }
  }

  // ========================================================
  // 3. 高頻 ADRC 控制迴圈 (100 Hz / 10ms 週期排程)
  // ========================================================
  unsigned long nowUs = micros();
  if (nowUs - lastCtrlTimeUs >= CTRL_PERIOD_US) {
    lastCtrlTimeUs = nowUs;

    // --- 嚴格安全防護欄檢測 (Guardrails Check) ---
    if (adrc.is_armed) {
      // (A) 行程物理邊界檢查
      float y_min_guard = adrc.y_min - GUARDRAIL_MARGIN_MM;
      float y_max_guard = adrc.y_max + GUARDRAIL_MARGIN_MM;

      if (currentDistanceMm < y_min_guard || currentDistanceMm > y_max_guard) {
        emergencyStop(GUARD_TRIPPED_OOB);
        return;
      }

      // (B) 馬達堵轉保護 (Stall Protection)
      if (fabs(adrc.u_prev1) >= GUARDRAIL_STALL_PWM) {
        if (highEffortStartTime == 0) {
          highEffortStartTime = millis();
          stallCheckStartDist = currentDistanceMm;
        } else if (millis() - highEffortStartTime > GUARDRAIL_STALL_MS) {
          if (fabs(currentDistanceMm - stallCheckStartDist) < GUARDRAIL_STALL_MIN_DY) {
            emergencyStop(GUARD_TRIPPED_STALL);
            return;
          }
        }
      } else {
        highEffortStartTime = 0;
      }
    }

    // --- 設定點軌跡生成 ---
    if (ctrlMode == MODE_SWEEP_CYCLE) {
      float t = (millis() - sweepStartTime) / 1000.0f;
      // 正弦往復行程軌跡: y(t) = y_mid + r * sin(2*pi*f*t)
      float sweepMidPct = (sweepMinPct + sweepMaxPct) * 0.5f;
      float sweepAmpPct = (sweepMaxPct - sweepMinPct) * 0.5f;
      targetPct = sweepMidPct + sweepAmpPct * sin(2.0f * PI * sweepFreqHz * t);
      targetMm = ADRC_PctToMm(&adrc, targetPct);
    } else if (ctrlMode == MODE_STATION_HOLD) {
      targetMm = ADRC_PctToMm(&adrc, targetPct);
    }

    // --- 執行 ADRC 核心控制計算 ---
    if (adrc.is_armed && guardStatus == GUARD_OK) {
      float u_cmd = ADRC_Update(&adrc, targetMm, currentDistanceMm, &delayBuf);
      setMotorCommand(u_cmd);
    } else {
      setMotorCommand(0.0f);
    }

    // --- Serial Plotter 格式化輸出 ---
    if (plotterOutput) {
      // 輸出: Target, Measured, ESO_Pos, ESO_Vel, Disturbance(z3/10), Control_Effort(u*100)
      Serial.printf("Target:%0.1f,Meas:%0.1f,Z1:%0.1f,Z2:%0.1f,Z3_x0.1:%0.1f,Effort_pct:%0.1f\n",
                    targetMm, currentDistanceMm, adrc.z1, adrc.z2, adrc.z3 * 0.1f, adrc.u_prev1 * 100.0f);
    }
  }
}
