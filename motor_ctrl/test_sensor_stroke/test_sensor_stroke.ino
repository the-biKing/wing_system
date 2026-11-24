#include <Adafruit_VL53L0X.h>
#include <Arduino.h>
#include <Wire.h>

// ==========================================================
// 硬體引腳定義 (ESP32 NodeMCU-32S)
// ==========================================================
const int PIN_MOTOR_DIR = 17; // GPIO 17: 馬達正反轉 (DIR)
const int PIN_MOTOR_PWM = 32; // GPIO 32: 馬達調速 (PWM)

const int PIN_TOF_SDA = 21; // GPIO 21: I2C 資料線 (SDA)
const int PIN_TOF_SCL = 15; // GPIO 15: I2C 時脈線 (SCL)

// ==========================================================
// PWM 設定 (降為 1000Hz，配合 MOSFET 10k 洩放電阻，避免無法關斷)
// ==========================================================
const int PWM_FREQ = 1000;
const int PWM_RES = 8;
const int PWM_CH = 4;

Adafruit_VL53L0X lox = Adafruit_VL53L0X();
bool tofReady = false;

// 測試狀態機
enum TestStage {
  STAGE_STATIC_0PWM,    // 0 PWM 靜態噪訊與穩定度測試
  STAGE_DYNAMIC_STROKE, // 動態運轉與擺幅收斂鎖定測試
  STAGE_COMPLETED,      // 校準完成，輸出報告
  STAGE_STOPPED         // 手動煞停
};

TestStage currentStage = STAGE_STATIC_0PWM;

// 5-Tap 中值濾波環形緩衝區 (消滅光學 FoV 多徑跳動突波)
const int MEDIAN_WINDOW = 5;
float medianBuf[MEDIAN_WINDOW] = {0};
uint8_t medianIdx = 0;
bool medianFilled = false;

// 0 PWM 靜態採樣變數
const int STATIC_SAMPLES_TARGET = 60;
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
float lastFilteredDist = 0.0f;

// 歷史循環擺幅 (計算收斂穩定度)
const int CYCLE_HISTORY_LEN = 6;
float amplitudeHistory[CYCLE_HISTORY_LEN] = {0};
float yMinHistory[CYCLE_HISTORY_LEN] = {0};
float yMaxHistory[CYCLE_HISTORY_LEN] = {0};
int completedCycles = 0;
bool amplitudeLocked = false;

// 最終鎖定基線
float lockedYMin = 0.0f;
float lockedYMax = 0.0f;
float lockedAmplitude = 0.0f;

bool plotterMode = false;
unsigned long lastSampleTime = 0;
const unsigned long SAMPLE_INTERVAL_MS = 30; // ~33 Hz 取樣

// 馬達驅動控制
void setMotor(bool forward, uint8_t speed) {
  if (speed == 0) {
    // 完全煞停：PWM 歸零，兩腳拉低，確保 MOSFET 閘極電荷洩放完全
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_MOTOR_PWM, 0);
#else
    ledcWrite(PWM_CH, 0);
#endif
    digitalWrite(PIN_MOTOR_DIR, LOW);
    return;
  }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, 0);
#else
  ledcWrite(PWM_CH, 0);
#endif
  delay(10); // 換向保護延遲

  digitalWrite(PIN_MOTOR_DIR, forward ? HIGH : LOW);
  delayMicroseconds(50);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, speed);
#else
  ledcWrite(PWM_CH, speed);
#endif
}

void brakeMotor() { setMotor(true, 0); }

// 快速浮點數排序求中值
float getMedian(float *arr, int n) {
  float temp[MEDIAN_WINDOW];
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

// 讀取經 5-Tap 中值濾波後的純淨距離 (mm)
float readFilteredDistanceMm() {
  if (!tofReady)
    return -1.0f;
  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);
  if (measure.RangeStatus == 4)
    return -1.0f; // Phase Fail

  float raw = (float)measure.RangeMilliMeter;
  if (raw < 20.0f || raw > 400.0f)
    return -1.0f; // 排除不合理極端值

  medianBuf[medianIdx] = raw;
  medianIdx = (medianIdx + 1) % MEDIAN_WINDOW;
  if (medianIdx == 0)
    medianFilled = true;

  int count = medianFilled ? MEDIAN_WINDOW : medianIdx;
  return getMedian(medianBuf, count);
}

void printBanner() {
  Serial.println(
      F("\n========================================================"));
  Serial.println(F("   WING SYSTEM - SENSOR & STROKE CALIBRATION (V2-MEDIAN)"));
  Serial.println(F("   [Anti-Jitter Median Filter + Dynamic Stroke Lock]"));
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

  Wire.begin(PIN_TOF_SDA, PIN_TOF_SCL);
  Wire.setClock(400000);

  if (lox.begin(VL53L0X_I2C_ADDR, false, &Wire)) {
    tofReady = true;
    Serial.println(F("[OK] VL53L0X ToF Initialized (SDA:21, SCL:15)"));
  } else {
    tofReady = false;
    Serial.println(F("[ERROR] VL53L0X Not Found! Check connections."));
  }

  Serial.println(F("\n[STEP 1] Starting 0-PWM Filtered Noise & Drift Test..."));
  Serial.println(F("Please keep mechanism completely still. Motor is OFF."));
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'p' || c == 'P') {
      plotterMode = !plotterMode;
      Serial.printf(">> Plotter Mode: %s\n", plotterMode ? "ON" : "OFF");
    } else if (c == '+') {
      if (testPwm <= 240)
        testPwm += 15;
      if (currentStage == STAGE_DYNAMIC_STROKE)
        setMotor(testForward, testPwm);
      Serial.printf(">> Test PWM set to: %d/255\n", testPwm);
    } else if (c == '-') {
      if (testPwm >= 40)
        testPwm -= 15;
      if (currentStage == STAGE_DYNAMIC_STROKE)
        setMotor(testForward, testPwm);
      Serial.printf(">> Test PWM set to: %d/255\n", testPwm);
    } else if (c == 'r' || c == 'R') {
      brakeMotor();
      currentStage = STAGE_STATIC_0PWM;
      staticSampleCount = 0;
      staticSum = 0;
      staticSumSq = 0;
      staticMin = 9999;
      staticMax = -9999;
      completedCycles = 0;
      amplitudeLocked = false;
      medianFilled = false;
      medianIdx = 0;
      Serial.println(F("\n>> Calibration sequence restarted."));
    } else if (c == 's' || c == 'S') {
      brakeMotor();
      currentStage = STAGE_STOPPED;
      Serial.println(F("\n>> Motor Stopped by user. Send 'r' to restart."));
    }
  }

  unsigned long now = millis();
  if (now - lastSampleTime < SAMPLE_INTERVAL_MS)
    return;
  lastSampleTime = now;

  float d = readFilteredDistanceMm();
  if (d <= 0.0f)
    return;
  currentDist = d;

  // ========================================================
  // 階段 1：0 PWM 靜態噪訊與基線檢驗
  // ========================================================
  if (currentStage == STAGE_STATIC_0PWM) {
    brakeMotor();
    staticSum += d;
    staticSumSq += (d * d);
    if (d < staticMin)
      staticMin = d;
    if (d > staticMax)
      staticMax = d;
    staticSampleCount++;

    if (staticSampleCount % 15 == 0 && !plotterMode) {
      Serial.printf("[0-PWM Sampling] %d/%d samples... Filtered: %0.1f mm\n",
                    staticSampleCount, STATIC_SAMPLES_TARGET, d);
    }

    if (staticSampleCount >= STATIC_SAMPLES_TARGET) {
      staticMean = staticSum / (float)STATIC_SAMPLES_TARGET;
      float variance = (staticSumSq / (float)STATIC_SAMPLES_TARGET) -
                       (staticMean * staticMean);
      staticStdDev = (variance > 0.0f) ? sqrt(variance) : 0.0f;
      float peakToPeakNoise = staticMax - staticMin;

      Serial.println(
          F("\n--------------------------------------------------------"));
      Serial.println(
          F(">> [STEP 1 RESULT] 0-PWM Filtered Stability Evaluation:"));
      Serial.printf("   - Filtered Mean Position (y0) : %0.2f mm\n",
                    staticMean);
      Serial.printf("   - Min / Max Reading           : %0.1f mm / %0.1f mm\n",
                    staticMin, staticMax);
      Serial.printf("   - Peak-to-Peak Noise (P-P)    : %0.2f mm\n",
                    peakToPeakNoise);
      Serial.printf("   - Standard Deviation (s)      : %0.2f mm\n",
                    staticStdDev);

      if (peakToPeakNoise <= 5.0f && staticStdDev <= 2.5f) {
        Serial.println(
            F("   -> [STATUS: PASS] Filtered 0-PWM baseline is rock-solid!"));
      } else {
        Serial.println(F("   -> [ADVICE] Noticeable optical jitter remains."));
        Serial.println(F("      Add a black tape shroud around ToF or enlarge "
                         "reflector target."));
      }
      Serial.println(
          F("--------------------------------------------------------"));

      // 進入階段 2：動態運轉與擺幅收斂鎖定
      Serial.println(
          F("\n[STEP 2] Starting Dynamic Stroke & Amplitude Locking..."));
      Serial.printf(
          "Turning motor ON at PWM: %d/255. Observing stroke cycles...\n",
          testPwm);
      Serial.println(F("Noise rejection enabled: Only full strokes (>35mm) "
                       "will be counted.\n"));

      cycleMinDist = d;
      cycleMaxDist = d;
      lastFilteredDist = d;
      completedCycles = 0;
      setMotor(testForward, testPwm);
      currentStage = STAGE_DYNAMIC_STROKE;
    }
    return;
  }

  // ========================================================
  // 階段 2：動態運轉與行程擺幅鎖定 (基於極值回落的穩健峰谷偵測)
  // ========================================================
  if (currentStage == STAGE_DYNAMIC_STROKE) {
    if (d < cycleMinDist) cycleMinDist = d;
    if (d > cycleMaxDist) cycleMaxDist = d;

    // 回落門檻：距離當前極值回調 8.0mm 即確認換向，完美適應 3.5s/cycle 低速運轉
    const float PEAK_DROP_THRESH = 8.0f;

    // 狀態 1：正在往上走（向上死點前進）
    if (cycleDirection >= 0) {
      if (d < (cycleMaxDist - PEAK_DROP_THRESH)) {
        // 從上死點（Max）回落超過 8mm -> 確認抵達波峰並開始折返向下！
        cycleDirection = -1;
        cycleMinDist = d; // 開始追蹤下一個波谷
      }
    }
    // 狀態 -1：正在往下走（向下死點前進）
    else if (cycleDirection < 0) {
      if (d > (cycleMinDist + PEAK_DROP_THRESH)) {
        // 從下死點（Min）回彈超過 8mm -> 確認抵達波谷，完成一個完整往復循環！
        cycleDirection = 1;

        float currentAmp = cycleMaxDist - cycleMinDist;

        // 只認可大於 30mm 的真實機械全擺幅
        if (currentAmp >= 30.0f) {
          int idx = completedCycles % CYCLE_HISTORY_LEN;
          amplitudeHistory[idx] = currentAmp;
          yMinHistory[idx] = cycleMinDist;
          yMaxHistory[idx] = cycleMaxDist;
          completedCycles++;

          int validCount = completedCycles < CYCLE_HISTORY_LEN
                               ? completedCycles
                               : CYCLE_HISTORY_LEN;
          float ampSum = 0.0f, yMinSum = 0.0f, yMaxSum = 0.0f;
          for (int i = 0; i < validCount; i++) {
            ampSum += amplitudeHistory[i];
            yMinSum += yMinHistory[i];
            yMaxSum += yMaxHistory[i];
          }
          float ampMean = ampSum / validCount;
          float yMinMean = yMinSum / validCount;
          float yMaxMean = yMaxSum / validCount;

          float ampVar = 0.0f;
          for (int i = 0; i < validCount; i++) {
            float err = amplitudeHistory[i] - ampMean;
            ampVar += err * err;
          }
          float ampStdDev = sqrt(ampVar / validCount);

          if (!plotterMode) {
            Serial.printf("[Stroke #%02d] Min: %4.1f mm | Max: %4.1f mm | Amplitude: %4.1f mm | StdDev: %0.2f mm\n",
                          completedCycles, yMinMean, yMaxMean, ampMean, ampStdDev);
          }

          // 連續 6 個完整循環擺幅標準差 <= 2.0mm 時自動鎖定
          if (validCount >= CYCLE_HISTORY_LEN && ampStdDev <= 2.0f && !amplitudeLocked) {
            amplitudeLocked = true;
            lockedYMin = yMinMean;
            lockedYMax = yMaxMean;
            lockedAmplitude = ampMean;

            brakeMotor();
            currentStage = STAGE_COMPLETED;

            Serial.println(F("\n========================================================"));
            Serial.println(F(">> [AMPLITUDE LOCKED & CALIBRATION COMPLETE] <<"));
            Serial.println(F("========================================================"));
            Serial.printf(" 1. Lower Mechanical Bound (y_min)  : %0.2f mm\n", lockedYMin);
            Serial.printf(" 2. Upper Mechanical Bound (y_max)  : %0.2f mm\n", lockedYMax);
            Serial.printf(" 3. Physical Stroke Span            : %0.2f mm\n", lockedAmplitude);
            Serial.printf(" 4. Stroke Consistency StdDev (s)   : %0.2f mm\n", ampStdDev);
            Serial.println(F("--------------------------------------------------------"));
            Serial.println(F(">> [CONFIG CODE TO COPY INTO config_motor.h] :"));
            Serial.printf("   #define CFG_DEFAULT_Y_MIN_MM       %0.1ff\n", lockedYMin);
            Serial.printf("   #define CFG_DEFAULT_Y_MAX_MM       %0.1ff\n", lockedYMax);
            Serial.printf("   #define CFG_DEFAULT_STROKE_SPAN_MM %0.1ff\n", lockedAmplitude);
            Serial.printf("   // Safety Guardrail Trigger Limits:\n");
            Serial.printf("   // Guardrail Low  = %0.1f mm (y_min - 7.0mm)\n", lockedYMin - 7.0f);
            Serial.printf("   // Guardrail High = %0.1f mm (y_max + 7.0mm)\n", lockedYMax + 7.0f);
            Serial.println(F("========================================================\n"));
            Serial.println(F("Calibration locked successfully! You can now proceed to Test 2."));
          }
        }

        // 開始追蹤下一個波峰
        cycleMaxDist = d;
      }
    }

    if (plotterMode) {
      Serial.printf("Dist:%0.1f,Min:%0.1f,Max:%0.1f,Span:%0.1f\n", d,
                    cycleMinDist, cycleMaxDist, cycleMaxDist - cycleMinDist);
    }
  }
}
