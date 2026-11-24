#include <Arduino.h>

// ==========================================================
// 螢幕 SPI 相關腳位清單
// ==========================================================
struct PinInfo {
  uint8_t pin;
  const char* name;
  const char* desc;
};

PinInfo tftPins[] = {
  { 5,  "CS",   "Chip Select (片選)" },
  { 22, "DC",   "Data / Command (資料/命令)" },
  { 14, "RST",  "Reset (硬體重置)" },
  { 18, "SCLK", "SPI Clock (時脈)" },
  { 23, "MOSI", "SPI Data / SDA (資料輸入)" }
};

const int NUM_PINS = sizeof(tftPins) / sizeof(tftPins[0]);
bool blinkMode = false;

void setAllHigh() {
  for (int i = 0; i < NUM_PINS; i++) {
    pinMode(tftPins[i].pin, OUTPUT);
    digitalWrite(tftPins[i].pin, HIGH);
  }
}

void setAllLow() {
  for (int i = 0; i < NUM_PINS; i++) {
    digitalWrite(tftPins[i].pin, LOW);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n========================================================"));
  Serial.println(F("   TFT SCREEN PIN CONTINUITY & VOLTAGE TESTER (ALL HIGH) "));
  Serial.println(F("========================================================"));
  Serial.println(F("All 5 display pins are now set to OUTPUT + HIGH (3.3V)!\n"));
  
  for (int i = 0; i < NUM_PINS; i++) {
    pinMode(tftPins[i].pin, OUTPUT);
    digitalWrite(tftPins[i].pin, HIGH);
    Serial.printf("  [ON] GPIO %2d  -->  %-5s : %s\n", 
                  tftPins[i].pin, tftPins[i].name, tftPins[i].desc);
  }

  Serial.println(F("\n--------------------------------------------------------"));
  Serial.println(F("測試方式:"));
  Serial.println(F("  1. 將 LED 陽極(+) 接各腳位焊點，陰極(-) 接 ESP32 GND。"));
  Serial.println(F("  2. 若某隻腳 LED 【完全不亮】，代表該 GPIO 或焊點斷路/虛焊。"));
  Serial.println(F("--------------------------------------------------------"));
  Serial.println(F("Serial Commands:"));
  Serial.println(F("  'high'  : 全部輸出 HIGH (恆亮量測)"));
  Serial.println(F("  'low'   : 全部輸出 LOW (熄滅)"));
  Serial.println(F("  'blink' : 依序輪流閃爍各腳位 (檢查是否腳位焊在一起短路)"));
  Serial.println(F("========================================================\n"));
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("blink")) {
      blinkMode = true;
      Serial.println(F(">> Switched to Sequential BLINK mode."));
    } else if (cmd.equalsIgnoreCase("high")) {
      blinkMode = false;
      setAllHigh();
      Serial.println(F(">> All pins set to HIGH."));
    } else if (cmd.equalsIgnoreCase("low")) {
      blinkMode = false;
      setAllLow();
      Serial.println(F(">> All pins set to LOW."));
    }
  }

  if (blinkMode) {
    for (int i = 0; i < NUM_PINS; i++) {
      if (!blinkMode) break;
      setAllLow();
      digitalWrite(tftPins[i].pin, HIGH);
      Serial.printf("--> ONLY GPIO %2d (%s) is HIGH now\n", tftPins[i].pin, tftPins[i].name);
      delay(800);
    }
  } else {
    // 確保維持全 HIGH
    setAllHigh();
    delay(200);
  }
}
