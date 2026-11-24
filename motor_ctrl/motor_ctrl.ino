#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <Preferences.h>
#include "config_motor.h"

// ==========================================================
// 系統實例與變數宣告
// ==========================================================
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
Preferences motorPrefs;

bool tofReady = false;
float currentDistanceMm = 0.0f;
unsigned long lastSensorReadTime = 0;

// 控制模式定義
enum ControlMode {
  MODE_IDLE,           // 待機停機
  MODE_STATION_HOLD,   // 定點伺服保持
  MODE_SWEEP_CYCLE     // 週期性往復往返掃描
};

ControlMode ctrlMode = MODE_IDLE;
bool isArmed = false;

// 安全防護欄狀態
enum GuardrailState {
  GUARD_OK = 0,
  GUARD_TRIPPED_OOB,            // 距離超出物理行程安全容許範圍
  GUARD_TRIPPED_SENSOR_TIMEOUT, // 感測器失聯或持續無回波
  GUARD_TRIPPED_STALL           // 輸出高 PWM 但無位移 (堵轉)
};

GuardrailState guardStatus = GUARD_OK;

// 物理行程極限 (由 Test 1 實測鎖定)
float y_min = CFG_DEFAULT_Y_MIN_MM; // 53.7 mm
float y_max = CFG_DEFAULT_Y_MAX_MM; // 101.7 mm
float stroke_span = CFG_DEFAULT_STROKE_SPAN_MM; // 48.0 mm

// 蘇格蘭軛機構伺服定位安全邊界 (Soft Limits)
// 幾何死點 (0% 與 100%) 為曲柄頂點，越過頂點運動方向會物理顛倒。
// 為確保負回授單調性，將 0%~100% 映射至安全單調區間：[y_min + 2.0mm, y_max - 2.5mm]
const float SOFT_LIMIT_LOW_OFFSET_MM  = 2.0f; // 下死點保留 2.0mm 淨空
const float SOFT_LIMIT_HIGH_OFFSET_MM = 2.5f; // 上死點保留 2.5mm 淨空

float calcTargetMm(float pct) {
  float safe_min = y_min + SOFT_LIMIT_LOW_OFFSET_MM;
  float safe_max = y_max - SOFT_LIMIT_HIGH_OFFSET_MM;
  pct = constrain(pct, 0.0f, 100.0f);
  return safe_min + (pct / 100.0f) * (safe_max - safe_min);
}

// 設定點變數與平滑軌跡生成器 (Trajectory Generator)
float targetPct = 50.0f;          // 最終目標百分比 [0.0 ~ 100.0%]
float targetMm = 77.4f;           // 最終目標物理位置 [mm] (安全工作區中心)
float activeTargetMm = 77.4f;     // 目前動態追蹤設定點 [mm] (經速度斜率限制)
float maxProfileVelMmS = 30.0f;   // 軌跡最大位移速度 (30 mm/s，全行程約 1.6 秒)

// 輸出斜率限制器 (Slew Rate Limiter)
const float MAX_PWM_SLEW_UP = 15.0f;   // 每個控制週期 (30ms) 最大升速 PWM 步階 (~500 PWM/s)
const float MAX_PWM_SLEW_DOWN = 35.0f; // 每個控制週期最大減速 PWM 步階

// 週期掃描模式參數 (Sweep Mode)
float sweepMinPct = 15.0f;
float sweepMaxPct = 85.0f;
float sweepFreqHz = 0.5f;         // 0.5 Hz (每 2 秒一個完整來回)
unsigned long sweepStartTime = 0;

// 堵轉檢測計時器
unsigned long highEffortStartTime = 0;
float stallCheckStartDist = 0.0f;

// 速度估算 (微分阻尼濾波)
float lastMeasuredDistMm = 77.7f;
float filteredVelocityMmS = 0.0f;

// 5-Tap 中值濾波環形緩衝區 (消滅光學突波)
const int MEDIAN_WINDOW = 5;
float medianBuf[MEDIAN_WINDOW] = {0};
uint8_t medianIdx = 0;
bool medianFilled = false;

// IIR 一階低通濾波器狀態 (消弭毫米級高頻光學噪訊)
float iirDistanceMm = 0.0f;
bool iirInitialized = false;
const float TOF_IIR_ALPHA = 0.70f; // 70% 歷史平滑 + 30% 當前測量 (完美兼顧抗噪與低延遲)

// ==========================================================
// 增強型前饋 PID 參數 (Feedforward + PID)
// ==========================================================
// 比例 P: 誤差 10mm 時產生約 40 PWM 驅動力
float Kp = 4.0f;

// 積分 I: 誤差進入 3mm 內時緩步累積消除微小靜差 (含防飽和 Anti-Windup)
float Ki = 0.8f;

// 微分 D: 煞車阻尼防衝過頭
float Kd = 1.2f;

float integralErr = 0.0f;
float currentOutPwm = 0.0f;

// 控制排程計時器 (30 Hz / 33ms，完美同步 ToF 測距頻率)
const unsigned long CTRL_PERIOD_MS = 30;
unsigned long lastCtrlTimeMs = 0;

// 繪圖串流開關
bool plotterOutput = false;

// 快速浮點數排序求中值
float getMedian(float *arr, int n) {
  float temp[MEDIAN_WINDOW];
  for (int i = 0; i < n; i++) temp[i] = arr[i];
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

float readTofMm() {
  if (!tofReady) return -1.0f;
  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);
  if (measure.RangeStatus == 4) return -1.0f;

  float raw = (float)measure.RangeMilliMeter;
  if (raw < 20.0f || raw > 400.0f) return -1.0f;

  // 1. 中值濾波 (Median Filter): 消除突發離群光學毛刺與反射突波
  medianBuf[medianIdx] = raw;
  medianIdx = (medianIdx + 1) % MEDIAN_WINDOW;
  if (medianIdx == 0) medianFilled = true;

  lastSensorReadTime = millis();
  int count = medianFilled ? MEDIAN_WINDOW : medianIdx;
  float med = getMedian(medianBuf, count);

  // 2. 一階 IIR 低通濾波 (IIR Low-Pass Filter): 撫平毫米級高頻隨機光學白噪訊
  if (!iirInitialized) {
    iirDistanceMm = med;
    iirInitialized = true;
  } else {
    iirDistanceMm = TOF_IIR_ALPHA * iirDistanceMm + (1.0f - TOF_IIR_ALPHA) * med;
  }

  return iirDistanceMm;
}

// ==========================================================
// 底層馬達驅動函式 (H-Bridge 含換向死區與安全洩放)
// ==========================================================
static bool lastForwardState = true;

void setMotorRaw(bool forward, uint8_t duty) {
  if (duty == 0 || guardStatus != GUARD_OK || !isArmed) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_MOTOR_PWM, 0);
#else
    ledcWrite(MOTOR_PWM_CH, 0);
#endif
    digitalWrite(PIN_MOTOR_DIR, LOW);
    return;
  }

  // 換向保護：等待 10ms 確保 MOSFET 電荷完全洩放
  if (forward != lastForwardState) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_MOTOR_PWM, 0);
#else
    ledcWrite(MOTOR_PWM_CH, 0);
#endif
    delay(10);
    digitalWrite(PIN_MOTOR_DIR, forward ? HIGH : LOW);
    delayMicroseconds(50);
    lastForwardState = forward;
  } else {
    digitalWrite(PIN_MOTOR_DIR, forward ? HIGH : LOW);
  }

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

  isArmed = false;
  ctrlMode = MODE_IDLE;
  guardStatus = reason;
  currentOutPwm = 0.0f;

  Serial.println(F("\n========================================================"));
  Serial.println(F(">> [CRITICAL SAFETY GUARDRAIL TRIGGERED - MOTOR HALTED] <<"));
  Serial.println(F("========================================================"));
  if (reason == GUARD_TRIPPED_OOB) {
    Serial.printf("  * REASON: Out-of-Bounds! Measured: %0.1f mm (Allowed: %0.1f ~ %0.1f mm)\n",
                  currentDistanceMm, y_min - GUARDRAIL_MARGIN_MM, y_max + GUARDRAIL_MARGIN_MM);
  } else if (reason == GUARD_TRIPPED_SENSOR_TIMEOUT) {
    Serial.println(F("  * REASON: Sensor Timeout / Disconnected! No valid ToF readings."));
  } else if (reason == GUARD_TRIPPED_STALL) {
    Serial.println(F("  * REASON: Motor Jam / Stall Detected! High effort with zero motion."));
  }
  Serial.println(F(">> Send 'reset' in Serial Monitor after clearing fault to re-arm.\n"));
}

// 根據當前機構位置，計算出 Scotch Yoke 克服靜摩擦所需的前饋 PWM (U形力矩曲線)
float getScotchYokeFeedforward(float posMm) {
  float norm_pos = (posMm - y_min) / stroke_span;
  norm_pos = constrain(norm_pos, 0.0f, 1.0f);

  // eta: 中心(50%)時為 0.0，兩端死點(0%或100%)時為 1.0
  float eta = fabs(norm_pos - 0.5f) * 2.0f;
  float eta3 = eta * eta * eta; // 三次方塑形

  // 實測數據：中段只需 PWM ~30 (u=0.118)，兩端死點需 PWM ~102 (u=0.400)
  float u_ff = CFG_DEFAULT_U_MID_BREAKAWAY + (CFG_DEFAULT_U_DEAD_BREAKAWAY - CFG_DEFAULT_U_MID_BREAKAWAY) * eta3;
  return u_ff * 255.0f; // 換算為 0~255 PWM 基礎前饋
}

void printHelp() {
  Serial.println(F("\n========================================================"));
  Serial.println(F("   WING SYSTEM - PID + TRAJECTORY PROFILER CONTROLLER"));
  Serial.println(F("========================================================"));
  Serial.println(F("Available Serial Commands:"));
  Serial.println(F("  'arm'                 : Enable motor closed-loop controller"));
  Serial.println(F("  'stop' / 'disarm'     : Immediately disable motor"));
  Serial.println(F("  'pos <0..100>'        : Set stroke target percentage (e.g. 'pos 50')"));
  Serial.println(F("  'speed <mm/s>'        : Set max profile tracking speed (e.g. 'speed 30')"));
  Serial.println(F("  'sweep <min> <max> <f>: Start continuous sweep (e.g. 'sweep 20 80 0.5')"));
  Serial.println(F("  'tune <p> <i> <d>'    : Real-time PID tuning (e.g. 'tune 4.0 0.8 1.2')"));
  Serial.println(F("  'status'              : Display real-time telemetry"));
  Serial.println(F("  'reinit' / 'scan'     : Re-detect & initialize VL53L0X sensor"));
  Serial.println(F("  'plot'                : Toggle CSV output for Arduino Serial Plotter"));
  Serial.println(F("  'reset'               : Clear tripped safety guardrails and re-arm\n"));
}

// I2C 匯流排主動解鎖：若感測器卡在 ACK/Data 狀態將 SDA 咬死，送出 9 個 SCL 脈衝強制釋放
void recoverI2cBus(uint8_t sdaPin, uint8_t sclPin) {
  pinMode(sdaPin, INPUT_PULLUP);
  pinMode(sclPin, OUTPUT);
  digitalWrite(sclPin, HIGH);
  delayMicroseconds(50);

  // 若 SDA 被咬死在 LOW
  if (digitalRead(sdaPin) == LOW) {
    Serial.println(F("[I2C RECOVERY] SDA is locked LOW by slave! Sending 9 SCL pulses..."));
    for (int i = 0; i < 9; i++) {
      digitalWrite(sclPin, LOW);
      delayMicroseconds(20);
      digitalWrite(sclPin, HIGH);
      delayMicroseconds(20);
      if (digitalRead(sdaPin) == HIGH) break;
    }
    // 發送 STOP 訊號
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

void scanI2cDevices() {
  Serial.println(F(">> Scanning I2C bus for devices..."));
  int count = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("   - Found I2C Device at 0x%02X", addr);
      if (addr == 0x29) Serial.print(F(" (VL53L0X Default)"));
      Serial.println();
      count++;
    }
  }
  if (count == 0) {
    Serial.println(F("   [WARNING] No I2C devices found on bus!"));
  }
}

bool initTofSensor() {
  Serial.println(F("\n>> Initializing VL53L0X Distance Sensor..."));
  // 1. 先主動排除 I2C 卡死
  recoverI2cBus(PIN_TOF_SDA, PIN_TOF_SCL);

  // 2. 初始化 I2C (降為 100kHz 標準速，提高抗干擾能力)
  Wire.begin(PIN_TOF_SDA, PIN_TOF_SCL);
  Wire.setClock(100000);
  delay(100);

  // 3. 掃描確認
  scanI2cDevices();

  // 4. 重試初始化 VL53L0X (最多試 4 次)
  tofReady = false;
  for (int attempt = 1; attempt <= 4; attempt++) {
    if (lox.begin(VL53L0X_I2C_ADDR, false, &Wire)) {
      tofReady = true;
      Serial.println(F("[OK] VL53L0X Distance Sensor Initialized OK."));
      break;
    }
    Serial.printf(">> VL53L0X Init attempt %d/4 failed, retrying in 250ms...\n", attempt);
    delay(250);
  }

  if (tofReady) {
    // 預熱中值緩衝區與 IIR 濾波器
    iirInitialized = false;
    medianFilled = false;
    medianIdx = 0;
    for (int i = 0; i < 6; i++) {
      readTofMm();
      delay(35);
    }
    float d_init = readTofMm();
    if (d_init > 0.0f) {
      currentDistanceMm = d_init;
      targetMm = d_init;
      activeTargetMm = d_init;
      lastMeasuredDistMm = d_init;
      targetPct = ((d_init - y_min) / stroke_span) * 100.0f;
      Serial.printf("[INIT] System Resting at Position: %0.1f mm (%0.1f%%) [IIR Filter Active]\n", currentDistanceMm, targetPct);
    }
  } else {
    Serial.println(F("[ERROR] VL53L0X Not Found after multiple attempts!"));
    Serial.println(F("Tip: If ToF is stuck from a previous flash, unplug USB for 3 seconds to power-cycle."));
  }
  return tofReady;
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

  initTofSensor();

  lastCtrlTimeMs = millis();
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
          integralErr = 0.0f;
          currentOutPwm = 0.0f;
          highEffortStartTime = 0;
          isArmed = true;
          ctrlMode = MODE_STATION_HOLD;
          // 動態目標先對齊當前真實位置，就地接管保持
          activeTargetMm = currentDistanceMm;
          targetMm = currentDistanceMm;
          float safe_min = y_min + SOFT_LIMIT_LOW_OFFSET_MM;
          float safe_max = y_max - SOFT_LIMIT_HIGH_OFFSET_MM;
          targetPct = ((currentDistanceMm - safe_min) / (safe_max - safe_min)) * 100.0f;
          targetPct = constrain(targetPct, 0.0f, 100.0f);
          Serial.printf(">> [ARMED] Controller Active. Holding target: %0.1f mm (%0.1f%%)\n", targetMm, targetPct);
        } else {
          Serial.printf(">> [DENIED] Cannot arm! Guardrail status: %d, ToF: %d\n", guardStatus, tofReady);
        }
      } else if (line.equalsIgnoreCase("stop") || line.equalsIgnoreCase("disarm") || line.equalsIgnoreCase("s")) {
        setMotorRaw(true, 0);
        isArmed = false;
        ctrlMode = MODE_IDLE;
        currentOutPwm = 0.0f;
        Serial.println(F(">> [DISARMED] Motor Stopped. Idle mode."));
      } else if (line.startsWith("pos ") || line.startsWith("POS ")) {
        float p = line.substring(4).toFloat();
        targetPct = constrain(p, 0.0f, 100.0f);
        targetMm = calcTargetMm(targetPct);
        ctrlMode = MODE_STATION_HOLD;
        integralErr = 0.0f; // 重設積分避免累積
        Serial.printf(">> Setpoint Target: %0.1f%% (%0.2f mm) [Safe Working Range: %0.1f ~ %0.1f mm]\n",
                      targetPct, targetMm, y_min + SOFT_LIMIT_LOW_OFFSET_MM, y_max - SOFT_LIMIT_HIGH_OFFSET_MM);
      } else if (line.startsWith("speed ") || line.startsWith("SPEED ")) {
        float v = line.substring(6).toFloat();
        if (v >= 5.0f && v <= 100.0f) {
          maxProfileVelMmS = v;
          Serial.printf(">> [SPEED SET] Max Profile Velocity: %0.1f mm/s\n", maxProfileVelMmS);
        }
      } else if (line.startsWith("sweep ") || line.startsWith("SWEEP ")) {
        int idx1 = line.indexOf(' ', 6);
        int idx2 = line.indexOf(' ', idx1 + 1);
        if (idx1 > 0 && idx2 > 0) {
          sweepMinPct = line.substring(6, idx1).toFloat();
          sweepMaxPct = line.substring(idx1 + 1, idx2).toFloat();
          sweepFreqHz = line.substring(idx2 + 1).toFloat();
          sweepStartTime = millis();
          ctrlMode = MODE_SWEEP_CYCLE;
          integralErr = 0.0f;
          Serial.printf(">> Starting Sweep: %0.1f%% ~ %0.1f%% @ %0.2f Hz\n",
                        sweepMinPct, sweepMaxPct, sweepFreqHz);
        }
      } else if (line.startsWith("tune ") || line.startsWith("TUNE ")) {
        int idx1 = line.indexOf(' ', 5);
        int idx2 = line.indexOf(' ', idx1 + 1);
        if (idx1 > 0 && idx2 > 0) {
          Kp = line.substring(5, idx1).toFloat();
          Ki = line.substring(idx1 + 1, idx2).toFloat();
          Kd = line.substring(idx2 + 1).toFloat();
          Serial.printf(">> [PID TUNED] Kp=%0.2f, Ki=%0.2f, Kd=%0.2f\n", Kp, Ki, Kd);
        }
      } else if (line.equalsIgnoreCase("plot")) {
        plotterOutput = !plotterOutput;
        Serial.printf(">> Plotter Stream: %s\n", plotterOutput ? "ON" : "OFF");
      } else if (line.equalsIgnoreCase("status")) {
        float pct = ((currentDistanceMm - y_min) / stroke_span) * 100.0f;
        Serial.printf(">> Status: Mode=%d, Armed=%d, Pos=%0.1fmm (%0.1f%%), Target=%0.1fmm, PWM=%d, Guard=%d\n",
                      ctrlMode, isArmed, currentDistanceMm, pct, targetMm, (int)currentOutPwm, guardStatus);
      } else if (line.equalsIgnoreCase("reinit") || line.equalsIgnoreCase("scan")) {
        initTofSensor();
      } else if (line.equalsIgnoreCase("reset")) {
        guardStatus = GUARD_OK;
        integralErr = 0.0f;
        currentOutPwm = 0.0f;
        highEffortStartTime = 0;
        if (!tofReady) initTofSensor();
        Serial.println(F(">> Guardrail reset. Send 'arm' to re-engage."));
      }
    }
  }

  // ========================================================
  // 2. 非同步感測器讀取
  // ========================================================
  float freshDist = readTofMm();
  if (freshDist > 0.0f) {
    currentDistanceMm = freshDist;
  }

  if (millis() - lastSensorReadTime > SENSOR_TIMEOUT_MS) {
    if (isArmed) {
      emergencyStop(GUARD_TRIPPED_SENSOR_TIMEOUT);
      return;
    }
  }

  // ========================================================
  // 3. 閉迴路 PID 控制計算 (30 Hz / 33ms)
  // ========================================================
  unsigned long now = millis();
  if (now - lastCtrlTimeMs >= CTRL_PERIOD_MS) {
    float dt = (now - lastCtrlTimeMs) / 1000.0f;
    lastCtrlTimeMs = now;

    // 速度估算 (微分阻尼濾波)
    float rawVel = (currentDistanceMm - lastMeasuredDistMm) / dt;
    lastMeasuredDistMm = currentDistanceMm;
    filteredVelocityMmS = 0.7f * filteredVelocityMmS + 0.3f * rawVel;

    // --- 安全防護欄檢查 ---
    if (isArmed) {
      // (A) 行程邊界
      if (currentDistanceMm < (y_min - GUARDRAIL_MARGIN_MM) || currentDistanceMm > (y_max + GUARDRAIL_MARGIN_MM)) {
        emergencyStop(GUARD_TRIPPED_OOB);
        return;
      }

      // (B) 堵轉保護 (已放寬至 2000ms 與 210 PWM)
      if (currentOutPwm >= GUARDRAIL_STALL_PWM) {
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

    // --- 平滑目標軌跡生成器 (Trajectory Generator) ---
    if (ctrlMode == MODE_SWEEP_CYCLE) {
      float t = (millis() - sweepStartTime) / 1000.0f;
      float sweepMidPct = (sweepMinPct + sweepMaxPct) * 0.5f;
      float sweepAmpPct = (sweepMaxPct - sweepMinPct) * 0.5f;
      targetPct = sweepMidPct + sweepAmpPct * sin(2.0f * PI * sweepFreqHz * t);
      targetMm = calcTargetMm(targetPct);
      activeTargetMm = targetMm; // 正弦曲線本身即為連續平滑
    } else if (ctrlMode == MODE_STATION_HOLD) {
      // 速度斜率限制 (Rate-Limiter): 避免大階躍造成瞬間暴衝拉垮供電
      float maxStep = maxProfileVelMmS * dt;
      if (activeTargetMm < targetMm) {
        activeTargetMm += maxStep;
        if (activeTargetMm > targetMm) activeTargetMm = targetMm;
      } else if (activeTargetMm > targetMm) {
        activeTargetMm -= maxStep;
        if (activeTargetMm < targetMm) activeTargetMm = targetMm;
      }

      // 動態追隨防飽和 (Profile Anti-Windup):
      // 若馬達在靜摩擦下尚未起步，動態軌跡最多領先當前實際位置 6mm，不讓誤差累積暴衝
      const float MAX_PROFILE_LAG_MM = 6.0f;
      activeTargetMm = constrain(activeTargetMm, currentDistanceMm - MAX_PROFILE_LAG_MM, currentDistanceMm + MAX_PROFILE_LAG_MM);
    }

    // --- 核心 PID + 前饋計算 ---
    if (isArmed && guardStatus == GUARD_OK) {
      // 誤差以平滑軌跡 activeTargetMm 為基準
      float error = activeTargetMm - currentDistanceMm;
      float absErr = fabs(error);

      // 死區容差：若動態誤差小於 2.0mm，且已非常接近最終目標 (誤差小於 2.5mm)，判定到位鎖死靜止
      if (absErr <= 2.0f && fabs(targetMm - currentDistanceMm) <= 2.5f) {
        setMotorRaw(true, 0);
        currentOutPwm = 0.0f;
        integralErr = 0.0f;
      } else {
        // 1. Scotch Yoke 前饋扭矩 (死點 ~102, 中間 ~30)
        float baseFf = getScotchYokeFeedforward(currentDistanceMm);

        // 平滑著陸 (Soft-Landing):
        // 當距離目標 > 8mm 時前饋全力輸出；<= 8mm 時線性衰減
        float ffWeight = 1.0f;
        if (absErr < 8.0f) {
          ffWeight = (absErr - 2.0f) / 6.0f; // 2mm->0.0, 8mm->1.0
          if (ffWeight < 0.0f) ffWeight = 0.0f;
        }
        float effectiveFf = baseFf * ffWeight;

        // 2. 比例項 (P)
        float pOut = Kp * absErr;

        // 3. 積分項 (I) - 僅在 2mm ~ 6mm 之間微幅補償靜差
        bool forward = (error > 0.0f);
        if (absErr > 2.0f && absErr < 6.0f) {
          integralErr += (forward ? 1.0f : -1.0f) * absErr * dt;
          integralErr = constrain(integralErr, -15.0f, 15.0f);
        } else {
          integralErr = 0.0f;
        }
        float iOut = Ki * integralErr;

        // 4. 微分阻尼項 (D on Measurement) - 有效抑制衝過頭
        // filteredVelocityMmS: 正值表示機構往遠端 (FWD) 運動
        float dDamping = 0.0f;
        if (forward) {
          dDamping = -Kd * filteredVelocityMmS; // 往前衝越快，阻尼越重 (減少 PWM)
        } else {
          dDamping = +Kd * filteredVelocityMmS; // 往後衝越快 (Vel為負)，阻尼越重 (減少 PWM)
        }

        // 5. 合成目標 PWM
        float desiredCmd = effectiveFf + pOut + (forward ? iOut : -iOut) + dDamping;
        desiredCmd = constrain(desiredCmd, 30.0f, 220.0f);

        // 6. PWM 升降壓斜率限制 (Slew Rate Limiter) - 防止大電流衝擊
        if (desiredCmd > currentOutPwm + MAX_PWM_SLEW_UP) {
          currentOutPwm += MAX_PWM_SLEW_UP;
        } else if (desiredCmd < currentOutPwm - MAX_PWM_SLEW_DOWN) {
          currentOutPwm -= MAX_PWM_SLEW_DOWN;
        } else {
          currentOutPwm = desiredCmd;
        }

        uint8_t outDuty = (uint8_t)constrain(currentOutPwm, 0.0f, 255.0f);

        // 頂點翻轉死點保護 (Crest-Crossing Inversion Guard)
        // 若輸出正轉但機構高速往回滑落 (Vel < -8mm/s 且已接近頂點)，代表曲柄越過了頂死點進入反向區
        if (forward && currentDistanceMm > (y_max - 8.0f) && filteredVelocityMmS < -8.0f) {
          setMotorRaw(true, 0);
          currentOutPwm = 0.0f;
          integralErr = 0.0f;
          activeTargetMm = currentDistanceMm;
        } else {
          setMotorRaw(forward, outDuty);
        }
      }

      // --- 即時診斷印出 ---
      static unsigned long lastDiagLogTime = 0;
      if (!plotterOutput && (millis() - lastDiagLogTime >= 350)) {
        lastDiagLogTime = millis();
        float err = targetMm - currentDistanceMm;
        Serial.printf("[PID] Target:%5.1fmm (Trk:%5.1f) | Cur:%5.1fmm | Err:%+5.1fmm | PWM:%3d/255 | Dir:%s | Vel:%+5.1fmm/s\n",
                      targetMm, activeTargetMm, currentDistanceMm, err, (int)currentOutPwm,
                      (activeTargetMm >= currentDistanceMm) ? "FWD" : "REV", filteredVelocityMmS);
      }
    } else {
      setMotorRaw(true, 0);
      currentOutPwm = 0.0f;
    }

    if (plotterOutput) {
      Serial.printf("Target:%0.1f,Track:%0.1f,Measured:%0.1f,Error:%0.1f,OutPWM:%0.1f\n",
                    targetMm, activeTargetMm, currentDistanceMm, targetMm - currentDistanceMm, currentOutPwm);
    }
  }
}
