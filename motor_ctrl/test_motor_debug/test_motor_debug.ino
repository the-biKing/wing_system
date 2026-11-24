#include <Arduino.h>

// ==========================================================
// 硬體引腳定義 (ESP32 NodeMCU-32S)
// ==========================================================
const int PIN_MOTOR_DIR = 17; // GPIO 17: 方向控制
const int PIN_MOTOR_PWM = 32; // GPIO 32: PWM 調速

const int PWM_FREQ = 1000;    // 1kHz (配合 10k 洩放電阻)
const int PWM_RES  = 8;       // 8-bit (0 ~ 255)
const int PWM_CH   = 4;

void writePwmDuty(uint8_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, duty);
#else
  ledcWrite(PWM_CH, duty);
#endif
}

void printHelp() {
  Serial.println(F("\n========================================================"));
  Serial.println(F("       MOTOR STOP & PWM DEBUG DIAGNOSTIC SUITE"));
  Serial.println(F("========================================================"));
  Serial.println(F("Send single character commands in Serial Monitor (115200):"));
  Serial.println(F("  '0' : Hard Stop Mode 1 -> DIR=LOW , PWM=0 (Both LOW)"));
  Serial.println(F("  '1' : Hard Stop Mode 2 -> DIR=HIGH, PWM=255 (Both HIGH)"));
  Serial.println(F("  '2' : Inverted PWM Stop -> DIR=LOW , PWM=255"));
  Serial.println(F("  'f' : Forward Slow (PWM = 80)"));
  Serial.println(F("  'r' : Reverse Slow (PWM = 80)"));
  Serial.println(F("  'p <value>' : Set raw PWM value (0~255), e.g., 'p 50' or 'p 0'"));
  Serial.println(F("  'd' : Toggle DIR pin (HIGH <-> LOW)"));
  Serial.println(F("========================================================\n"));
}

bool currentDir = LOW;

void setup() {
  Serial.begin(115200);
  delay(1000);
  printHelp();

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

  Serial.println(F("[INITIAL BOOT] Applied default: DIR=LOW, PWM=0."));
  Serial.println(F(">> IS THE MOTOR RUNNING RIGHT NOW?"));
  Serial.println(F("   - If YES -> Send '1' or '2' to test inverted stop modes!"));
  Serial.println(F("   - If NO  -> It is stopped normally.\n"));
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) return;

    if (cmd == "0") {
      // 模式 0：兩腳皆拉 LOW (最標準的低電平剎車)
      digitalWrite(PIN_MOTOR_DIR, LOW);
      writePwmDuty(0);
      currentDir = LOW;
      Serial.println(F(">> [MODE 0 APPLIED] DIR = LOW (0V), PWM Duty = 0 (0V)"));
      Serial.println(F("   Status check: Did motor stop?"));
    } 
    else if (cmd == "1") {
      // 模式 1：兩腳皆拉 HIGH (高電平煞車 / 某些 H 橋下臂直通剎車)
      digitalWrite(PIN_MOTOR_DIR, HIGH);
      writePwmDuty(255);
      currentDir = HIGH;
      Serial.println(F(">> [MODE 1 APPLIED] DIR = HIGH (3.3V), PWM Duty = 255 (3.3V)"));
      Serial.println(F("   Status check: Did motor stop?"));
    } 
    else if (cmd == "2") {
      // 模式 2：PWM 腳為反相邏輯 (0=全速, 255=停止)
      digitalWrite(PIN_MOTOR_DIR, LOW);
      writePwmDuty(255);
      currentDir = LOW;
      Serial.println(F(">> [MODE 2 APPLIED] DIR = LOW, PWM Duty = 255 (Active-Low Inverted Stop)"));
      Serial.println(F("   Status check: Did motor stop?"));
    } 
    else if (cmd == "f" || cmd == "F") {
      digitalWrite(PIN_MOTOR_DIR, HIGH);
      writePwmDuty(80);
      currentDir = HIGH;
      Serial.println(F(">> [FORWARD RUN] DIR = HIGH, PWM = 80/255"));
    } 
    else if (cmd == "r" || cmd == "R") {
      digitalWrite(PIN_MOTOR_DIR, LOW);
      writePwmDuty(80);
      currentDir = LOW;
      Serial.println(F(">> [REVERSE RUN] DIR = LOW, PWM = 80/255"));
    } 
    else if (cmd.startsWith("p ") || cmd.startsWith("P ")) {
      int val = cmd.substring(2).toInt();
      val = constrain(val, 0, 255);
      writePwmDuty(val);
      Serial.printf(">> [RAW PWM SET] Duty = %d/255 on GPIO 32\n", val);
    } 
    else if (cmd == "d" || cmd == "D") {
      currentDir = !currentDir;
      digitalWrite(PIN_MOTOR_DIR, currentDir);
      Serial.printf(">> [DIR TOGGLED] GPIO 17 is now: %s\n", currentDir ? "HIGH" : "LOW");
    }
  }
}

