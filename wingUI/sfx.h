// ==========================================================
// SFX - Tactical UI Sound Effects Engine (Passive Buzzer)
// Optimized for 2kHz - 5kHz Low-Level Trigger Buzzer Modules
// ==========================================================
#pragma once
#include <Arduino.h>
#include "config.h"

// 全域靜音狀態旗標 (定義於主程式 wingUI.ino)
extern bool isMuted;

// 基礎單音播放 (Tone & Silence) - 靜音模式下自動不發聲
inline void sfxTone(uint32_t freq, uint32_t durationMs) {
  if (isMuted) return;
  if (freq == 0) {
    ledcWriteTone(BUZZER_PIN, 0);
  } else {
    ledcWriteTone(BUZZER_PIN, freq);
  }
  delay(durationMs);
  ledcWriteTone(BUZZER_PIN, 0);
}

// 快速靜音
inline void sfxMute() {
  ledcWriteTone(BUZZER_PIN, 0);
}

// 初始化蜂鳴器
inline void sfxInit() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH); // 低電平觸發模組：預設 HIGH 保持靜音
  ledcAttach(BUZZER_PIN, 2000, BUZZER_TIMER_RES);
  sfxMute();
}

// ==================== 音效定義庫 ====================

// 1. NAVIGATE (切換首頁分頁 / 列表滾動，短促俐落清脆 Blip，2.8kHz -> 3.5kHz)
inline void sfxNavigate() {
  sfxTone(2800, 18);
  delay(4);
  sfxTone(3500, 22);
  sfxMute();
}

// 2. ENTER (確定 / 進入編輯 / 鎖定儲存，上揚科技確認音，2.4kHz -> 3.2kHz -> 4.2kHz)
inline void sfxEnter() {
  sfxTone(2400, 25);
  delay(6);
  sfxTone(3200, 25);
  delay(6);
  sfxTone(4200, 45);
  sfxMute();
}

// 3. BACK (返回 / 退出，下沉科技取消音，3.8kHz -> 2.6kHz)
inline void sfxBack() {
  sfxTone(3800, 28);
  delay(8);
  sfxTone(2600, 40);
  sfxMute();
}

// 4. RESET (四鍵全重置 / 恢復原廠，快速階梯音)
inline void sfxReset() {
  for (int f = 1800; f <= 4200; f += 400) {
    sfxTone(f, 15);
    delay(5);
  }
  delay(30);
  sfxTone(3800, 35);
  delay(15);
  sfxTone(4500, 70);
  sfxMute();
}

// 5. WARNING (警示音 / 超溫超載，緊迫刺耳顫音)
inline void sfxWarning() {
  for (int i = 0; i < 3; i++) {
    sfxTone(3600, 60);
    delay(25);
    sfxTone(4400, 70);
    delay(30);
  }
  sfxMute();
}

// 6. POWER ON (開機科技音效，未來戰術啟動旋律，充滿科幻儀式感)
inline void sfxPowerOn() {
  sfxTone(1800, 35);
  delay(12);
  sfxTone(2400, 35);
  delay(12);
  sfxTone(3000, 45);
  delay(15);
  sfxTone(3800, 50);
  delay(20);
  sfxTone(4800, 110);
  sfxMute();
}

// 7. POWER OFF (關機 / 待機沉睡音效，能量衰退遞減)
inline void sfxPowerOff() {
  sfxTone(4500, 45);
  delay(15);
  sfxTone(3600, 45);
  delay(15);
  sfxTone(2800, 55);
  delay(20);
  sfxTone(2000, 75);
  delay(25);
  sfxTone(1400, 120);
  sfxMute();
}
