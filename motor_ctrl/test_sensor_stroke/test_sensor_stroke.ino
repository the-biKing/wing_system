#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_VL53L0X.h>

// ==========================================================
// 硬體引腳定義 (ESP32 NodeMCU-32S)
// ==========================================================
const int PIN_MOTOR_DIR = 17; // GPIO 17: 馬達正反轉 (DIR)
const int PIN_MOTOR_PWM = 32; // GPIO 32: 馬達調速 (PWM)

const int PIN_TOF_SDA   = 21; // GPIO 21: I2C 資料線 (SDA)
const int PIN_TOF_SCL   = 15; // GPIO 15: I2C 時脈線 (SCL)

// ==========================================================
// PWM 設定 (相容 ESP32 Core 2.x 與 3.x)
// ==========================================================
const int PWM_FREQ = 5000;
const int PWM_RES  = 8;
const int PWM_CH   = 4;

Adafruit_VL53L0X lox = Adafruit_VL53L0X();
bool tofReady = false;

// 測試狀態機
enum TestStage {
  STAGE_STATIC_0PWM,       // 0 PWM 靜態噪訊與穩定度測試
  STAGE_DYNAMIC_STROKE,    // 動態運轉與擺幅收斂鎖定測試
  STAGE_COMPLETED,         // 校準完成，輸出報告
  STAGE_STOPPED            // 手動煞停
};

TestStage currentStage = STAGE_STATIC_0PWM;

// 0 PWM 靜態採樣變數
const int STATIC_SAMPLES_TARGET = 80;
int staticSampleCount = 0;
float staticSum = 0.0f;
float staticSumSq = 0.0f;
float staticMin = 9999.0f;
float staticMax = -9999.0f;
float staticMean = 0.0f;
float staticStdDev = 0.0f;

// 動態擺幅偵測變數 (Scotch Yoke 週期性運動)
uint8_t testPwm = 130; // 預設測試運轉 PWM (約 50% 轉速)
bool testForward = true;

float currentDist = 0.0f;
float cycleMinDist = 9999.0f;
float cycleMaxDist = -9999.0f;
int cycleDirection = 0; // +1 增長中, -1 縮小中
float lastDist = 0.0f;

// 歷史循環擺幅 (計算收斂穩定度)
const int CYCLE_HISTORY_LEN = 6;
float amplitudeHistory[CYCLE_HISTORY_LEN] = {0};
int completedCycles = 0;
bool amplitudeLocked = false;

// 最終鎖定基線
float lockedYMin = 0.0f;
float lockedYMax = 0.0f;
float lockedAmplitude = 0.0f;

bool plotterMode = false;
unsigned long lastSampleTime = 0;
const unsigned long SAMPLE_INTERVAL_MS = 25; // 40 Hz 測距

// 馬達驅動控制
void setMotor(bool forward, uint8_t speed) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, 0);
#else
  ledcWrite(PWM_CH, 0);
#endif

  digitalWrite(PIN_MOTOR_DIR, forward ? HIGH : LOW);
  delayMicroseconds(50);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, speed);
#else
  ledcWrite(PWM_CH, speed);
#endif
}

void brakeMotor() {
  setMotor(true, 0);
}

// 讀取 VL53L0X 距離 (mm)
float readDistanceMm() {
  if (!tofReady) return -1.0f;
  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);
  if (measure.RangeStatus != 4) { // 4 = Phase Fail (超出範圍或無回波)
    return (float)measure.RangeMilliMeter;
  }
  return -1.0f;
}

void printBanner() {
  Serial.println(F("\n========================================================"));
  Serial.println(F("   WING SYSTEM - SENSOR & STROKE CALIBRATION SUITE"));
  Serial.println(F("   [Test 1: 0-PWM Stability & Cycle Amplitude Locking]"));
  Serial.println(F("========================================================"));
  Serial.println(F("Commands:"));
  Serial.println(F("  'p' : Toggle Serial Plotter mode / Text mode"));
  Serial.println(F("  '+' : Increase motor test PWM"));
  Serial.println(F("  '-' : Decrease motor test PWM"));
  Serial.println(F("  'r' : Restart calibration sequence"));
  Serial.println(F("  's' : Stop motor immediately (E-Stop)\n"));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  printBanner();

  pinMode(PIN_MOTOR_DIR, OUTPUT);
  digitalWrite(PIN_MOTOR_DIR, LOW);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_MOTOR_PWM, PWM_FREQ, PWM_RES);
  ledcWrite(PIN_MOTOR_PWM, 0);
#else
  ledcSetup(PWM_CH, PWM_FREQ, PWM_RES);
  ledcAttachPin(PIN_MOTOR_PWM, PWM_CH);
  ledcWrite(PWM_CH, 0);
#endif

  // 初始化 I2C 與 VL53L0X
  Wire.begin(PIN_TOF_SDA, PIN_TOF_SCL);
  Wire.setClock(400000); // 400kHz Fast I2C

  if (lox.begin(VL53L0X_I2C_ADDR, false, &Wire)) {
    tofReady = true;
    Serial.println(F("[OK] VL53L0X ToF Initialized (SDA:21, SCL:15)"));
  } else {
    tofReady = false;
    Serial.println(F("[ERROR] VL53L0X Not Found! Check SDA/SCL & 3.3V Power."));
  }

  Serial.println(F("\n[STEP 1] Starting 0-PWM Static Noise & Drift Test..."));
  Serial.println(F("Please keep mechanism completely still. Motor is OFF (0 PWM)."));
}

void loop() {
  // 處理 Serial 命令
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'p' || c == 'P') {
      plotterMode = !plotterMode;
      Serial.printf(">> Plotter Mode: %s\n", plotterMode ? "ON" : "OFF");
    } else if (c == '+') {
      if (testPwm <= 240) testPwm += 15;
      if (currentStage == STAGE_DYNAMIC_STROKE) setMotor(testForward, testPwm);
      Serial.printf(">> Test PWM set to: %d/255\n", testPwm);
    } else if (c == '-') {
      if (testPwm >= 40) testPwm -= 15;
      if (currentStage == STAGE_DYNAMIC_STROKE) setMotor(testForward, testPwm);
      Serial.printf(">> Test PWM set to: %d/255\n", testPwm);
    } else if (c == 'r' || c == 'R') {
      brakeMotor();
      currentStage = STAGE_STATIC_0PWM;
      staticSampleCount = 0;
      staticSum = 0; staticSumSq = 0;
      staticMin = 9999; staticMax = -9999;
      completedCycles = 0;
      amplitudeLocked = false;
      Serial.println(F("\n>> Calibration sequence restarted."));
    } else if (c == 's' || c == 'S') {
      brakeMotor();
      currentStage = STAGE_STOPPED;
      Serial.println(F("\n>> Motor Stopped by user. Send 'r' to restart."));
    }
  }

  unsigned long now = millis();
  if (now - lastSampleTime < SAMPLE_INTERVAL_MS) return;
  lastSampleTime = now;

  float d = readDistanceMm();
  if (d <= 0.0f) return; // 略過無效量測
  currentDist = d;

  // ========================================================
  // 階段 1：0 PWM 靜態噪訊與基線檢驗
  // ========================================================
  if (currentStage == STAGE_STATIC_0PWM) {
    brakeMotor(); // 確保馬達不動
    staticSum += d;
    staticSumSq += (d * d);
    if (d < staticMin) staticMin = d;
    if (d > staticMax) staticMax = d;
    staticSampleCount++;

    if (staticSampleCount % 20 == 0 && !plotterMode) {
      Serial.printf("[0-PWM Sampling] %d/%d samples... Current: %0.1f mm\n",
                    staticSampleCount, STATIC_SAMPLES_TARGET, d);
    }

    if (staticSampleCount >= STATIC_SAMPLES_TARGET) {
      staticMean = staticSum / (float)STATIC_SAMPLES_TARGET;
      float variance = (staticSumSq / (float)STATIC_SAMPLES_TARGET) - (staticMean * staticMean);
      staticStdDev = (variance > 0.0f) ? sqrt(variance) : 0.0f;
      float peakToPeakNoise = staticMax - staticMin;

      Serial.println(F("\n--------------------------------------------------------"));
      Serial.println(F(">> [STEP 1 RESULT] 0-PWM Static Stability Evaluation:"));
      Serial.printf("   - Mean Position (y0)     : %0.2f mm\n", staticMean);
      Serial.printf("   - Min / Max Reading      : %0.1f mm / %0.1f mm\n", staticMin, staticMax);
      Serial.printf("   - Peak-to-Peak Noise     : %0.2f mm\n", peakToPeakNoise);
      Serial.printf("   - Standard Deviation (s) : %0.2f mm\n", staticStdDev);

      if (peakToPeakNoise <= 3.5f && staticStdDev <= 1.5f) {
        Serial.println(F("   -> [STATUS: PASS] 0 PWM = 0 Motion. Sensor is highly stable!"));
      } else {
        Serial.println(F("   -> [WARNING] Significant jitter detected while stationary!"));
        Serial.println(F("      Check sensor mounting rigidity and optical alignment."));
      }
      Serial.println(F("--------------------------------------------------------"));

      // 進入階段 2：動態運轉與擺幅收斂鎖定
      Serial.println(F("\n[STEP 2] Starting Motor Dynamic Stroke & Amplitude Locking..."));
      Serial.printf("Turning motor ON at PWM: %d/255. Observing stroke cycles...\n", testPwm);
      Serial.println(F("TIP: You can physically adjust sensor mounting bracket NOW."));
      Serial.println(F("The script will automatically detect when the cycle amplitude locks in!\n"));

      cycleMinDist = d;
      cycleMaxDist = d;
      lastDist = d;
      completedCycles = 0;
      setMotor(testForward, testPwm);
      currentStage = STAGE_DYNAMIC_STROKE;
    }
    return;
  }

  // ========================================================
  // 階段 2：動態運轉與行程擺幅鎖定 (Stroke Discovery)
  // ========================================================
  if (currentStage == STAGE_DYNAMIC_STROKE) {
    if (d < cycleMinDist) cycleMinDist = d;
    if (d > cycleMaxDist) cycleMaxDist = d;

    float diff = d - lastDist;
    const float HYSTERESIS = 2.0f; // 2mm 遲滯防止雜訊誤觸反向

    // 方向檢測 (偵測 Scotch Yoke 上下死點折返)
    if (cycleDirection <= 0 && diff > HYSTERESIS) {
      // 從下死點 (Trough) 開始往上走，記錄剛才抓到的極小值
      cycleDirection = 1;
    } else if (cycleDirection >= 0 && diff < -HYSTERESIS) {
      // 從上死點 (Peak) 開始往下走，代表完成一個完整單向擺幅
      cycleDirection = -1;

      float currentAmp = cycleMaxDist - cycleMinDist;
      if (currentAmp > 5.0f) { // 有效擺幅必須大於 5mm
        amplitudeHistory[completedCycles % CYCLE_HISTORY_LEN] = currentAmp;
        completedCycles++;

        // 計算最近 N 個循環擺幅的均值與標準差
        int validCount = completedCycles < CYCLE_HISTORY_LEN ? completedCycles : CYCLE_HISTORY_LEN;
        float ampSum = 0.0f;
        for (int i = 0; i < validCount; i++) ampSum += amplitudeHistory[i];
        float ampMean = ampSum / validCount;

        float ampVar = 0.0f;
        for (int i = 0; i < validCount; i++) {
          float err = amplitudeHistory[i] - ampMean;
          ampVar += err * err;
        }
        float ampStdDev = sqrt(ampVar / validCount);

        if (!plotterMode) {
          Serial.printf("[Cycle #%02d] Min: %0.1f mm | Max: %0.1f mm | Amplitude: %0.1f mm | StdDev: %0.2f mm\n",
                        completedCycles, cycleMinDist, cycleMaxDist, currentAmp, ampStdDev);
        }

        // 當收集滿足夠循環且擺幅標準差小於 1.5mm (代表機構幾何擺幅已穩定固化)
        if (validCount >= CYCLE_HISTORY_LEN && ampStdDev < 1.5f && !amplitudeLocked) {
          amplitudeLocked = true;
          lockedYMin = cycleMinDist;
          lockedYMax = cycleMaxDist;
          lockedAmplitude = ampMean;

          brakeMotor();
          currentStage = STAGE_COMPLETED;

          Serial.println(F("\n========================================================"));
          Serial.println(F(">> [AMPLITUDE LOCKED & CALIBRATION COMPLETE] <<"));
          Serial.println(F("========================================================"));
          Serial.printf(" 1. Lower Mechanical Bound (y_min)  : %0.2f mm\n", lockedYMin);
          Serial.printf(" 2. Upper Mechanical Bound (y_max)  : %0.2f mm\n", lockedYMax);
          Serial.printf(" 3. Physical Stroke Span            : %0.2f mm\n", lockedAmplitude);
          Serial.printf(" 4. 0-PWM Sensor Noise Floor (s)    : %0.2f mm\n", staticStdDev);
          Serial.println(F("--------------------------------------------------------"));
          Serial.println(F(">> [RECOMMENDED SAFETY GUARDRAIL CONFIGURATION] :"));
          Serial.printf("   #define CFG_DEFAULT_Y_MIN_MM       %0.1ff\n", lockedYMin);
          Serial.printf("   #define CFG_DEFAULT_Y_MAX_MM       %0.1ff\n", lockedYMax);
          Serial.printf("   #define CFG_DEFAULT_STROKE_SPAN_MM %0.1ff\n", lockedAmplitude);
          Serial.printf("   // Safety bounds (E-Stop if exceeded by margin):\n");
          Serial.printf("   // Guard Low  = %0.1f mm (y_min - 7.0mm)\n", lockedYMin - 7.0f);
          Serial.printf("   // Guard High = %0.1f mm (y_max + 7.0mm)\n", lockedYMax + 7.0f);
          Serial.println(F("========================================================\n"));
          Serial.println(F("Copy these values into 'config_motor.h' or proceed to Test 2!"));
        }
      }

      // 重設尋找下一個循環的極值
      cycleMinDist = d;
      cycleMaxDist = d;
    }
    lastDist = d;

    // Serial Plotter 格式輸出
    if (plotterMode) {
      Serial.printf("Dist:%0.1f,Min:%0.1f,Max:%0.1f,Amp:%0.1f\n",
                    d, cycleMinDist, cycleMaxDist, cycleMaxDist - cycleMinDist);
    }
  }
}
