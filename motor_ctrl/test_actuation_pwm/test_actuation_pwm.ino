#include "../config_motor.h"
#include <Adafruit_VL53L0X.h>
#include <Arduino.h>
#include <Wire.h>

Adafruit_VL53L0X lox = Adafruit_VL53L0X();
bool tofReady = false;

// 5-Tap 中值濾波環形緩衝區
const int MEDIAN_WINDOW = 5;
float medianBuf[MEDIAN_WINDOW] = {0};
uint8_t medianIdx = 0;
bool medianFilled = false;

// IIR 一階低通濾波器
float iirDistanceMm = 0.0f;
bool iirInitialized = false;
const float TOF_IIR_ALPHA = 0.70f;

// 測試狀態機
enum MultiTestState {
  STATE_IDLE,
  STATE_RAMP_UP,   // 緩加 PWM 測 Breakaway
  STATE_RAMP_DOWN, // 緩降 PWM 測 Kinetic
  STATE_ALL_COMPLETE
};

MultiTestState testState = STATE_IDLE;

int lastBreakawayPwm = 0;
int lastKineticPwm = 0;

int currentTestPwm = 0;
float startDistMm = 0.0f;
float lastDistForVel = 0.0f;
unsigned long lastStepTime = 0;
unsigned long steadyRunningStartTime = 0;

void setMotor(bool forward, uint8_t speed) {
  if (speed == 0) {
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
  delay(10);

  digitalWrite(PIN_MOTOR_DIR, forward ? HIGH : LOW);
  delayMicroseconds(50);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, speed);
#else
  ledcWrite(PWM_CH, speed);
#endif
}

void brakeMotor() { setMotor(true, 0); }

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

float readDistanceMm() {
  if (!tofReady)
    return -1.0f;
  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);
  if (measure.RangeStatus == 4)
    return -1.0f;

  float raw = (float)measure.RangeMilliMeter;
  if (raw < 20.0f || raw > 400.0f)
    return -1.0f;

  medianBuf[medianIdx] = raw;
  medianIdx = (medianIdx + 1) % MEDIAN_WINDOW;
  if (medianIdx == 0)
    medianFilled = true;

  int count = medianFilled ? MEDIAN_WINDOW : medianIdx;
  float med = getMedian(medianBuf, count);

  if (!iirInitialized) {
    iirDistanceMm = med;
    iirInitialized = true;
  } else {
    iirDistanceMm = TOF_IIR_ALPHA * iirDistanceMm + (1.0f - TOF_IIR_ALPHA) * med;
  }

  return iirDistanceMm;
}

float getAveragedDistance(int samples = 5) {
  float sum = 0.0f;
  int valid = 0;
  for (int i = 0; i < samples; i++) {
    float d = readDistanceMm();
    if (d > 0.0f) {
      sum += d;
      valid++;
    }
    delay(20);
  }
  return valid > 0 ? (sum / valid) : -1.0f;
}

void printBanner() {
  Serial.println(
      F("\n========================================================"));
  Serial.println(F("   WING SYSTEM - MULTI-POSITION ACTUATION PROFILER"));
  Serial.println(F("   [Scotch Yoke Non-Linear Stiction & Torque Mapper]"));
  Serial.println(F("========================================================"));
  Serial.println(F("Calibrated Stroke Bounds (from config_motor.h):"));
  Serial.printf(
      "  * y_min = %0.1f mm  |  y_max = %0.1f mm  |  Span = %0.1f mm\n",
      CFG_DEFAULT_Y_MIN_MM, CFG_DEFAULT_Y_MAX_MM, CFG_DEFAULT_STROKE_SPAN_MM);
  Serial.println(F("--------------------------------------------------------"));
  Serial.println(F("Commands:"));
  Serial.println(F("  'g' : Start test at current position"));
  Serial.println(
      F("  'f' : Jog forward slowly (PWM 60 for 250ms) to position mechanism"));
  Serial.println(F("  'r' : Jog reverse slowly (PWM 60 for 250ms)"));
  Serial.println(F("  's' : Stop / Cancel\n"));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  printBanner();

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
    Serial.println(F("[OK] VL53L0X ToF Initialized OK."));
    float cur = getAveragedDistance(6);
    float pct =
        ((cur - CFG_DEFAULT_Y_MIN_MM) / CFG_DEFAULT_STROKE_SPAN_MM) * 100.0f;
    Serial.printf("Current Rest Position: %0.1f mm (Stroke: %0.1f%%)\n", cur,
                  pct);
  } else {
    tofReady = false;
    Serial.println(F("[ERROR] VL53L0X Not Found!"));
  }
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("g") || cmd.equalsIgnoreCase("start")) {
      float cur = getAveragedDistance(6);
      float pct =
          ((cur - CFG_DEFAULT_Y_MIN_MM) / CFG_DEFAULT_STROKE_SPAN_MM) * 100.0f;
      startDistMm = cur;
      lastDistForVel = cur;

      Serial.println(
          F("\n========================================================"));
      Serial.printf(">> Starting Actuation Sweep at Current Position: %0.1f mm "
                    "(%0.1f%%)\n",
                    cur, pct);
      Serial.println(
          F("========================================================"));

      currentTestPwm = 10;
      testState = STATE_RAMP_UP;
      lastStepTime = millis();
    } else if (cmd.equalsIgnoreCase("f")) {
      // 點動前進
      setMotor(true, 70);
      delay(200);
      brakeMotor();
      delay(200);
      float d = getAveragedDistance(4);
      float pct =
          ((d - CFG_DEFAULT_Y_MIN_MM) / CFG_DEFAULT_STROKE_SPAN_MM) * 100.0f;
      Serial.printf(">> Jog FWD -> Current: %0.1f mm (%0.1f%%)\n", d, pct);
    } else if (cmd.equalsIgnoreCase("r")) {
      // 點動後退
      setMotor(false, 70);
      delay(200);
      brakeMotor();
      delay(200);
      float d = getAveragedDistance(4);
      float pct =
          ((d - CFG_DEFAULT_Y_MIN_MM) / CFG_DEFAULT_STROKE_SPAN_MM) * 100.0f;
      Serial.printf(">> Jog REV -> Current: %0.1f mm (%0.1f%%)\n", d, pct);
    } else if (cmd.equalsIgnoreCase("s")) {
      brakeMotor();
      testState = STATE_IDLE;
      Serial.println(F(">> Stopped."));
    }
  }

  unsigned long now = millis();

  // 1. 緩加 PWM 測 Breakaway
  if (testState == STATE_RAMP_UP) {
    if (now - lastStepTime >= 100) {
      lastStepTime = now;
      currentTestPwm += 2;
      setMotor(true, currentTestPwm);

      float cur = readDistanceMm();
      float delta = fabs(cur - startDistMm);

      Serial.printf(
          "  [RAMP UP] PWM: %3d/255 | Pos: %5.1f mm | Delta: %4.1f mm\n",
          currentTestPwm, cur, delta);

      if (delta >= 2.0f) {
        lastBreakawayPwm = currentTestPwm;
        Serial.printf("\n>> [MOTION DETECTED] Motor broke free at PWM = %d! "
                      "(Delta: %0.1f mm)\n",
                      lastBreakawayPwm, delta);
        Serial.println(F(">> Letting motor cruise for 500ms before ramping "
                         "down to find Kinetic Limit..."));
        steadyRunningStartTime = millis();
        testState = STATE_RAMP_DOWN;
      } else if (currentTestPwm >= 250) {
        brakeMotor();
        Serial.println(
            F(">> [TIMEOUT] Reached PWM 250 without motion. Check mechanism."));
        testState = STATE_IDLE;
      }
    }
  }

  // 2. 緩降 PWM 測 Kinetic
  else if (testState == STATE_RAMP_DOWN) {
    if (now - steadyRunningStartTime >= 500) {
      if (now - lastStepTime >= 100) {
        unsigned long dt = now - lastStepTime;
        lastStepTime = now;

        float cur = readDistanceMm();
        float speed = fabs(cur - lastDistForVel) / ((float)dt / 1000.0f);
        lastDistForVel = cur;

        Serial.printf("  [RAMP DOWN] PWM: %3d/255 | Speed: %5.1f mm/s\n",
                      currentTestPwm, speed);

        if (speed < 3.0f || currentTestPwm <= 15) {
          lastKineticPwm = currentTestPwm;
          brakeMotor();
          testState = STATE_ALL_COMPLETE;

          Serial.println(
              F("\n========================================================"));
          Serial.println(F(">> [TEST COMPLETED FOR THIS POSITION] <<"));
          Serial.println(
              F("========================================================"));
          Serial.printf(" * Position Tested : %0.1f mm\n", startDistMm);
          Serial.printf(" * Breakaway PWM   : %d / 255 (u = %0.3f)\n",
                        lastBreakawayPwm, lastBreakawayPwm / 255.0f);
          Serial.printf(" * Kinetic PWM     : %d / 255 (u = %0.3f)\n",
                        lastKineticPwm, lastKineticPwm / 255.0f);
          Serial.println(
              F("--------------------------------------------------------"));
          Serial.println(F("Tip: Use 'f' or 'r' to jog to another position "
                           "(e.g. mid vs end),"));
          Serial.println(
              F("     then send 'g' to compare stiction differences!"));
          Serial.println(
              F("========================================================\n"));
        } else {
          currentTestPwm -= 2;
          if (currentTestPwm < 10)
            currentTestPwm = 10;
          setMotor(true, currentTestPwm);
        }
      }
    }
  }
}
