// --- 腳位定義 ---
const int PIN_MOTOR_PWM = 0; // GPIO0: 馬達調速 PWM
const int PIN_MOTOR_DIR = 1; // GPIO1: 馬達正反轉 (motor)

// PWM 參數設定
const int PWM_FREQ =
    1000;              // 1kHz (初期測試用較低頻率，避免 10k 電阻關斷太慢發燙)
const int PWM_RES = 8; // 8 位元解析度 (數值範圍 0 ~ 255)
const int PWM_CH = 0;  // PWM 通道 (ESP32 Core 2.x 使用)

// 安全死區停頓時間 (ms)
const int DEAD_TIME_MS = 50;

// 安全煞車與換向函式
void setMotor(bool forward, uint8_t speed) {
// 1. 如果要換向或停機，先將 PWM 歸零
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, 0);
#else
  ledcWrite(PWM_CH, 0);
#endif

  // 2. 強制等待死區時間，確保所有 MOSFET 內部電荷完全洩放
  delay(DEAD_TIME_MS);

  // 3. 切換方向
  digitalWrite(PIN_MOTOR_DIR, forward ? HIGH : LOW);
  delay(2);

// 4. 寫入目標速度
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_MOTOR_PWM, speed);
#else
  ledcWrite(PWM_CH, speed);
#endif
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP32-C3 H-Bridge Motor Test ===");

  pinMode(PIN_MOTOR_DIR, OUTPUT);
  digitalWrite(PIN_MOTOR_DIR, LOW);

// 設定 PWM 腳位 (相容 ESP32 Arduino Core 2.x 與 3.x)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_MOTOR_PWM, PWM_FREQ, PWM_RES);
  ledcWrite(PIN_MOTOR_PWM, 0);
#else
  ledcSetup(PWM_CH, PWM_FREQ, PWM_RES);
  ledcAttachPin(PIN_MOTOR_PWM, PWM_CH);
  ledcWrite(PWM_CH, 0);
#endif

  Serial.println("System Ready. Starting test cycle in 2 seconds...");
  delay(2000);
}

void loop() {
  // --- 測試 1：正轉漸速 (0 -> 100% -> 0) ---
  Serial.println(">> [FORWARD] Speed Ramp Up...");
  for (int duty = 0; duty <= 255; duty += 15) {
    setMotor(true, duty);
    Serial.printf("Forward PWM: %d/255\n", duty);
    delay(100);
  }
  delay(1000);

  Serial.println(">> [FORWARD] Speed Ramp Down...");
  for (int duty = 255; duty >= 0; duty -= 15) {
    setMotor(true, duty);
    delay(50);
  }

  // 完全煞停休息
  setMotor(true, 0);
  Serial.println(">> Coasting / Idle...");
  delay(1500);

  // --- 測試 2：反轉漸速 (0 -> 100% -> 0) ---
  Serial.println(">> [REVERSE] Speed Ramp Up...");
  for (int duty = 0; duty <= 255; duty += 15) {
    setMotor(false, duty);
    Serial.printf("Reverse PWM: %d/255\n", duty);
    delay(100);
  }
  delay(1000);

  Serial.println(">> [REVERSE] Speed Ramp Down...");
  for (int duty = 255; duty >= 0; duty -= 15) {
    setMotor(false, duty);
    delay(50);
  }

  // 完全煞停休息
  setMotor(false, 0);
  Serial.println(">> Test Round Finished. Cooling down for 3 seconds...\n");
  delay(3000);
}