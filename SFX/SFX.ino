// ==========================================================
// SFX Testbed - Serial Interactive Acoustic Console
// Test individual tactical sound effects via Serial Monitor
// ==========================================================
#include "sfx.h"

void printMenu() {
  Serial.println("\n==============================================");
  Serial.println("     TACTICAL HUD - SFX SOUNDBOARD (GPIO 2)   ");
  Serial.println("==============================================");
  Serial.println("  [1] Navigate   (Next / Prev page / Tab scroll)");
  Serial.println("  [2] Enter      (Confirm / Save / Select)");
  Serial.println("  [3] Back       (Cancel / Return to Home)");
  Serial.println("  [4] Reset      (4-Button Factory System Reset)");
  Serial.println("  [5] Warning    (Thermal / Overload Alert)");
  Serial.println("  [6] Power On   (Futuristic HUD Boot SFX)");
  Serial.println("  [7] Power Off  (Energy Discharge / Shutdown)");
  Serial.println("  [A] Play ALL Sequentially");
  Serial.println("==============================================");
  Serial.print("Select SFX (1-7, A) > ");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  // 初始化無源蜂鳴器
  sfxInit();

  Serial.println("\n[INIT] Buzzer Engine Ready on GPIO 2.");
  Serial.println("[INIT] Testing boot power-on chime...");
  sfxPowerOn();

  printMenu();
}

void loop() {
  if (Serial.available() > 0) {
    char raw = Serial.read();
    if (raw == '\r' || raw == '\n' || raw == ' ' || raw == '\t')
      return;

    char cmd = toupper(raw);
    Serial.printf("%c\n", cmd);

    switch (cmd) {
    case '1':
      Serial.println(">> Playing SFX: [NAVIGATE]");
      sfxNavigate();
      break;

    case '2':
      Serial.println(">> Playing SFX: [ENTER / CONFIRM]");
      sfxEnter();
      break;

    case '3':
      Serial.println(">> Playing SFX: [BACK / CANCEL]");
      sfxBack();
      break;

    case '4':
      Serial.println(">> Playing SFX: [SYSTEM RESET]");
      sfxReset();
      break;

    case '5':
      Serial.println(">> Playing SFX: [WARNING]");
      sfxWarning();
      break;

    case '6':
      Serial.println(">> Playing SFX: [POWER ON]");
      sfxPowerOn();
      break;

    case '7':
      Serial.println(">> Playing SFX: [POWER OFF]");
      sfxPowerOff();
      break;

    case 'A':
      Serial.println(">> Demo: Playing ALL sound effects sequentially...");
      delay(300);
      Serial.println("  1. Power On");
      sfxPowerOn();
      delay(600);
      Serial.println("  2. Navigate");
      sfxNavigate();
      delay(400);
      Serial.println("  3. Enter");
      sfxEnter();
      delay(400);
      Serial.println("  4. Back");
      sfxBack();
      delay(400);
      Serial.println("  5. Warning");
      sfxWarning();
      delay(600);
      Serial.println("  6. Reset");
      sfxReset();
      delay(600);
      Serial.println("  7. Power Off");
      sfxPowerOff();
      Serial.println(">> Demo finished.");
      break;

    default:
      Serial.printf("Unknown command '%c'. Enter 1-7 or A.\n", cmd);
      break;
    }

    Serial.print("\nSelect SFX (1-7, A) > ");
  }
}
