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
enum ActuationTestState {
  STATE_IDLE,
  STATE_WAIT_USER,
  // 正向測試：先測突破 PWM，再測動摩擦維持 PWM
  STATE_FWD_RAMP_UP,      // 0 -> 往上加，測 Breakaway (啟動點，例如 100)
  STATE_FWD_RUNNING,      // 短暫維持穩態滑動
  STATE_FWD_RAMP_DOWN,    // 往下減，測 Kinetic (熄火點，例如 50)
  STATE_FWD_COOLDOWN,

  // 反向測試：先測突破 PWM，再測動摩擦維持 PWM
  STATE_REV_RAMP_UP,
  STATE_REV_RUNNING,
  STATE_REV_RAMP_DOWN,
  STATE_REV_COOLDOWN,

  STATE_DYNAMIC_PROBE,
  STATE_COMPLETE
};

ActuationTestState testState = STATE_WAIT_USER;

// 測試門檻與步進參數
const float MOTION_THRESHOLD_MM = 2.5f;     // 判定克服靜摩擦開始動的位移門檻 (mm)
const float STOP_VELOCITY_THRESH_MM_S = 3.0f; // 判定動摩擦停止滑動的速度門檻 (mm/s)
const int RAMP_STEP = 2;                    // 每次增減的 PWM 值 (0 ~ 255)
const unsigned long STEP_INTERVAL_MS = 100; // 階梯停留時間 (ms)

int currentTestPwm = 0;
float startDistMm = 0.0f;
float lastDistForVel = 0.0f;
unsigned long lastStepTime = 0;
unsigned long cooldownStartTime = 0;
unsigned long steadyRunningStartTime = 0;

// 多輪測試統計
const int TOTAL_ROUNDS = 3;
int currentRound = 0;

// 雙閥值記錄列表
int fwdBreakawayList[TOTAL_ROUNDS] = {0}; // 靜摩擦起步 PWM
int fwdKineticList[TOTAL_ROUNDS]   = {0}; // 動摩擦維持最低 PWM
int revBreakawayList[TOTAL_ROUNDS] = {0};
int revKineticList[TOTAL_ROUNDS]   = {0};

float avgFwdBreakaway = 0.0f, avgFwdKinetic = 0.0f;
float avgRevBreakaway = 0.0f, avgRevKinetic = 0.0f;

int8_t identifiedPolarity = 1;
float estimatedB0 = 12.0f;

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

float readDistanceMm() {
  if (!tofReady) return -1.0f;
  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);
  if (measure.RangeStatus != 4) {
    return (float)measure.RangeMilliMeter;
  }
  return -1.0f;
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
  Serial.println(F("\n========================================================"));
  Serial.println(F("   WING SYSTEM - DUAL-THRESHOLD FRICTION TEST SUITE"));
  Serial.println(F("   [Test 2: Breakaway Stiction & Kinetic Running PWM]"));
  Serial.println(F("========================================================"));
  Serial.println(F("Features:"));
  Serial.println(F("  1. Ramp-Up   : Finds Breakaway Stiction PWM (Motor starts moving)"));
  Serial.println(F("  2. Ramp-Down : Finds Kinetic Sustaining PWM (Motor halts sliding)"));
  Serial.println(F("  Prevents limit-cycle hunting and stick-slip oscillations!\n"));
  Serial.println(F("Commands:"));
  Serial.println(F("  'g' or 'start' : Start automated 3-round dual test"));
  Serial.println(F("  's'            : Emergency Stop\n"));
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
    Serial.println(F("[OK] VL53L0X ToF Initialized OK."));
    float initDist = getAveragedDistance(8);
    Serial.printf("Current Initial Distance: %0.1f mm\n", initDist);
  } else {
    tofReady = false;
    Serial.println(F("[ERROR] VL53L0X Not Found! Check connections."));
  }
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("g") || cmd.equalsIgnoreCase("start")) {
      if (testState == STATE_WAIT_USER || testState == STATE_COMPLETE || testState == STATE_IDLE) {
        Serial.println(F("\n>> Starting Dual-Threshold Friction Profiling (3 Rounds)..."));
        currentRound = 0;
        testState = STATE_FWD_RAMP_UP;
        currentTestPwm = 0;
        brakeMotor();
        delay(300);
        startDistMm = getAveragedDistance(6);
        lastDistForVel = startDistMm;
        lastStepTime = millis();
        Serial.printf("\n--- Round 1/%d [FORWARD] Phase 1: Seeking Breakaway Stiction ---\n", TOTAL_ROUNDS);
      }
    } else if (cmd.equalsIgnoreCase("s") || cmd.equalsIgnoreCase("stop")) {
      brakeMotor();
      testState = STATE_IDLE;
      Serial.println(F("\n>> [EMERGENCY STOP] Motor Halted. Send 'g' to restart."));
    }
  }

  unsigned long now = millis();

  // ========================================================
  // 1. 正向向上爬坡：尋找靜摩擦突破點 (Breakaway Stiction)
  // ========================================================
  if (testState == STATE_FWD_RAMP_UP) {
    if (now - lastStepTime >= STEP_INTERVAL_MS) {
      lastStepTime = now;
      currentTestPwm += RAMP_STEP;
      setMotor(true, currentTestPwm);

      float currDist = readDistanceMm();
      float delta = fabs(currDist - startDistMm);

      Serial.printf("  [FWD Up] PWM: %3d/255 | Dist: %5.1f mm | Delta: %4.1f mm\n",
                    currentTestPwm, currDist, delta);

      if (delta >= MOTION_THRESHOLD_MM) {
        fwdBreakawayList[currentRound] = currentTestPwm;
        Serial.printf("  >> [BREAKAWAY DETECTED] Motor broke free at PWM = %d!\n", currentTestPwm);
        Serial.println(F("  >> Letting motor spin smoothly for 600ms before ramping down..."));

        steadyRunningStartTime = millis();
        testState = STATE_FWD_RUNNING;
      } else if (currentTestPwm >= 250) {
        brakeMotor();
        Serial.println(F("  >> [TIMEOUT] Reached max PWM without motion! Check mechanism."));
        testState = STATE_IDLE;
      }
    }
  }

  // ========================================================
  // 2. 正向平穩滑動過渡 (600ms)
  // ========================================================
  else if (testState == STATE_FWD_RUNNING) {
    if (now - steadyRunningStartTime >= 600) {
      lastDistForVel = readDistanceMm();
      lastStepTime = millis();
      testState = STATE_FWD_RAMP_DOWN;
      Serial.println(F("  --- Phase 2: Ramping Down to Find Kinetic Sustaining Limit ---"));
    }
  }

  // ========================================================
  // 3. 正向向下遞減：尋找動摩擦維持極限 (Kinetic Sustaining)
  // ========================================================
  else if (testState == STATE_FWD_RAMP_DOWN) {
    if (now - lastStepTime >= 120) {
      unsigned long dtMs = now - lastStepTime;
      lastStepTime = now;

      float currDist = readDistanceMm();
      float speed = fabs(currDist - lastDistForVel) / ((float)dtMs / 1000.0f); // mm/s
      lastDistForVel = currDist;

      Serial.printf("  [FWD Down] PWM: %3d/255 | Speed: %5.1f mm/s\n", currentTestPwm, speed);

      // 當速度掉到低於門檻，表示動摩擦阻力大於扭矩，馬達停止滑動
      if (speed < STOP_VELOCITY_THRESH_MM_S || currentTestPwm <= 15) {
        brakeMotor();
        fwdKineticList[currentRound] = currentTestPwm;
        Serial.printf("  >> [KINETIC LIMIT DETECTED] Motor stopped sliding at PWM = %d!\n", currentTestPwm);

        cooldownStartTime = millis();
        testState = STATE_FWD_COOLDOWN;
        Serial.println(F("  Cooling down and resting mechanism for 2.0s..."));
      } else {
        currentTestPwm -= RAMP_STEP;
        if (currentTestPwm < 10) currentTestPwm = 10;
        setMotor(true, currentTestPwm);
      }
    }
  }

  // ========================================================
  // 4. 正向冷卻休息
  // ========================================================
  else if (testState == STATE_FWD_COOLDOWN) {
    if (now - cooldownStartTime >= 2000) {
      currentTestPwm = 0;
      brakeMotor();
      delay(300);
      startDistMm = getAveragedDistance(6);
      lastDistForVel = startDistMm;
      lastStepTime = millis();
      testState = STATE_REV_RAMP_UP;
      Serial.printf("\n--- Round %d/%d [REVERSE] Phase 1: Seeking Breakaway Stiction ---\n",
                    currentRound + 1, TOTAL_ROUNDS);
    }
  }

  // ========================================================
  // 5. 反向向上爬坡 (REVERSE BREAKAWAY)
  // ========================================================
  else if (testState == STATE_REV_RAMP_UP) {
    if (now - lastStepTime >= STEP_INTERVAL_MS) {
      lastStepTime = now;
      currentTestPwm += RAMP_STEP;
      setMotor(false, currentTestPwm);

      float currDist = readDistanceMm();
      float delta = fabs(currDist - startDistMm);

      Serial.printf("  [REV Up] PWM: %3d/255 | Dist: %5.1f mm | Delta: %4.1f mm\n",
                    currentTestPwm, currDist, delta);

      if (delta >= MOTION_THRESHOLD_MM) {
        revBreakawayList[currentRound] = currentTestPwm;
        Serial.printf("  >> [BREAKAWAY DETECTED] Motor broke free at PWM = %d!\n", currentTestPwm);
        Serial.println(F("  >> Letting motor spin smoothly for 600ms before ramping down..."));

        steadyRunningStartTime = millis();
        testState = STATE_REV_RUNNING;
      } else if (currentTestPwm >= 250) {
        brakeMotor();
        Serial.println(F("  >> [TIMEOUT] Reached max PWM without motion! Check mechanism."));
        testState = STATE_IDLE;
      }
    }
  }

  // ========================================================
  // 6. 反向平穩滑動過渡
  // ========================================================
  else if (testState == STATE_REV_RUNNING) {
    if (now - steadyRunningStartTime >= 600) {
      lastDistForVel = readDistanceMm();
      lastStepTime = millis();
      testState = STATE_REV_RAMP_DOWN;
      Serial.println(F("  --- Phase 2: Ramping Down to Find Kinetic Sustaining Limit ---"));
    }
  }

  // ========================================================
  // 7. 反向向下遞減 (REVERSE KINETIC LIMIT)
  // ========================================================
  else if (testState == STATE_REV_RAMP_DOWN) {
    if (now - lastStepTime >= 120) {
      unsigned long dtMs = now - lastStepTime;
      lastStepTime = now;

      float currDist = readDistanceMm();
      float speed = fabs(currDist - lastDistForVel) / ((float)dtMs / 1000.0f);
      lastDistForVel = currDist;

      Serial.printf("  [REV Down] PWM: %3d/255 | Speed: %5.1f mm/s\n", currentTestPwm, speed);

      if (speed < STOP_VELOCITY_THRESH_MM_S || currentTestPwm <= 15) {
        brakeMotor();
        revKineticList[currentRound] = currentTestPwm;
        Serial.printf("  >> [KINETIC LIMIT DETECTED] Motor stopped sliding at PWM = %d!\n", currentTestPwm);

        cooldownStartTime = millis();
        testState = STATE_REV_COOLDOWN;
        Serial.println(F("  Cooling down and resting mechanism for 2.0s..."));
      } else {
        currentTestPwm -= RAMP_STEP;
        if (currentTestPwm < 10) currentTestPwm = 10;
        setMotor(false, currentTestPwm);
      }
    }
  }

  // ========================================================
  // 8. 反向冷卻休息 / 輪次判斷
  // ========================================================
  else if (testState == STATE_REV_COOLDOWN) {
    if (now - cooldownStartTime >= 2000) {
      currentRound++;
      if (currentRound < TOTAL_ROUNDS) {
        currentTestPwm = 0;
        brakeMotor();
        delay(300);
        startDistMm = getAveragedDistance(6);
        lastDistForVel = startDistMm;
        lastStepTime = millis();
        testState = STATE_FWD_RAMP_UP;
        Serial.printf("\n--- Round %d/%d [FORWARD] Phase 1: Seeking Breakaway Stiction ---\n",
                      currentRound + 1, TOTAL_ROUNDS);
      } else {
        testState = STATE_DYNAMIC_PROBE;
      }
    }
  }

  // ========================================================
  // 9. 動態脈衝探測極性與 b0
  // ========================================================
  else if (testState == STATE_DYNAMIC_PROBE) {
    Serial.println(F("\n[STEP 3] Probing Polarity and Baseline Control Effectiveness (b0)..."));

    float sumFwdBreak = 0, sumFwdKin = 0;
    float sumRevBreak = 0, sumRevKin = 0;
    for (int i = 0; i < TOTAL_ROUNDS; i++) {
      sumFwdBreak += fwdBreakawayList[i];
      sumFwdKin   += fwdKineticList[i];
      sumRevBreak += revBreakawayList[i];
      sumRevKin   += revKineticList[i];
    }
    avgFwdBreakaway = sumFwdBreak / TOTAL_ROUNDS;
    avgFwdKinetic   = sumFwdKin   / TOTAL_ROUNDS;
    avgRevBreakaway = sumRevBreak / TOTAL_ROUNDS;
    avgRevKinetic   = sumRevKin   / TOTAL_ROUNDS;

    // 探測脈衝：略高於克服靜摩擦之 PWM
    uint8_t probePwm = (uint8_t)(avgFwdBreakaway + 20.0f);
    if (probePwm > 220) probePwm = 220;

    brakeMotor();
    delay(500);
    float y_start = getAveragedDistance(5);

    setMotor(true, probePwm);
    delay(200);
    float y_mid = getAveragedDistance(3);

    setMotor(false, probePwm);
    delay(200);
    brakeMotor();
    delay(400);
    float y_end = getAveragedDistance(5);

    float deltaProbe = y_mid - y_start;
    if (deltaProbe > 0.8f) {
      identifiedPolarity = 1;  // 正轉 = 距離增大
    } else if (deltaProbe < -0.8f) {
      identifiedPolarity = -1; // 正轉 = 距離縮小
    } else {
      identifiedPolarity = 1;
    }

    float accelProxy = fabs(y_end - 2.0f * y_mid + y_start) / (0.2f * 0.2f);
    float normEffort = (float)probePwm / 255.0f;
    estimatedB0 = accelProxy / normEffort;
    if (estimatedB0 < 2.0f) estimatedB0 = 12.0f;

    testState = STATE_COMPLETE;

    Serial.println(F("\n========================================================"));
    Serial.println(F(">> [TEST 2 COMPLETED: DUAL-THRESHOLD FRICTION PROFILE] <<"));
    Serial.println(F("========================================================"));
    Serial.println(F("Multi-Round Breakdown (Stiction Breakaway vs Kinetic Sustaining):"));
    for (int i = 0; i < TOTAL_ROUNDS; i++) {
      Serial.printf("  Round %d: FWD [Break:%3d | Kin:%3d]  ||  REV [Break:%3d | Kin:%3d]\n",
                    i + 1, fwdBreakawayList[i], fwdKineticList[i], revBreakawayList[i], revKineticList[i]);
    }
    Serial.println(F("--------------------------------------------------------"));
    Serial.printf("  * FWD Breakaway Stiction : %0.1f / 255 (u = %0.3f)\n", avgFwdBreakaway, avgFwdBreakaway / 255.0f);
    Serial.printf("  * FWD Kinetic Running    : %0.1f / 255 (u = %0.3f)\n", avgFwdKinetic, avgFwdKinetic / 255.0f);
    Serial.printf("  * REV Breakaway Stiction : %0.1f / 255 (u = %0.3f)\n", avgRevBreakaway, avgRevBreakaway / 255.0f);
    Serial.printf("  * REV Kinetic Running    : %0.1f / 255 (u = %0.3f)\n", avgRevKinetic, avgRevKinetic / 255.0f);
    Serial.printf("  * Motor Polarity         : %d (%s)\n",
                  identifiedPolarity, identifiedPolarity > 0 ? "Normal: FWD increases distance" : "Inverted: FWD decreases distance");
    Serial.printf("  * Estimated Baseline b0  : %0.1f mm/(s^2 * PWM)\n", estimatedB0);
    Serial.println(F("--------------------------------------------------------"));
    Serial.println(F(">> [READY-TO-USE CODE FOR config_motor.h] :"));
    Serial.printf("   #define CFG_DEFAULT_U_BREAKAWAY_POS %0.3ff  // PWM ~%0.0f\n", avgFwdBreakaway / 255.0f, avgFwdBreakaway);
    Serial.printf("   #define CFG_DEFAULT_U_BREAKAWAY_NEG %0.3ff  // PWM ~%0.0f\n", avgRevBreakaway / 255.0f, avgRevBreakaway);
    Serial.printf("   #define CFG_DEFAULT_U_KINETIC_POS   %0.3ff  // PWM ~%0.0f\n", avgFwdKinetic / 255.0f, avgFwdKinetic);
    Serial.printf("   #define CFG_DEFAULT_U_KINETIC_NEG   %0.3ff  // PWM ~%0.0f\n", avgRevKinetic / 255.0f, avgRevKinetic);
    Serial.printf("   #define CFG_DEFAULT_POLARITY        %d\n", identifiedPolarity);
    Serial.printf("   #define CFG_DEFAULT_B0_INIT         %0.1ff\n", estimatedB0);
    Serial.println(F("========================================================\n"));
    Serial.println(F("Test finished! Send 'g' to re-test or run 'motor_ctrl.ino'."));
  }
}
