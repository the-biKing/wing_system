# Wing System - 馬達與距離感測器校準與 ADRC 控制指南
**Hardware & Firmware Calibration Manual for Scotch Yoke Linear Stroke Mechanism**

---

## 1. 系統架構與設計概念 (Design Philosophy)

本系統針對 **Scotch Yoke (蘇格蘭軛機構) / 往復式連桿機構** 所驅動之線性往復位移設計，結合 **自校準擴展狀態觀測器 (ADRC + ESO + CFDL)** 實現無硬編碼參數、抗擾動能力極高的伺服級位置與行程控制。

### 為什麼先進行離線腳本校準（Warm-Start Baseline）？
原論文中提出「每次開機自動冷校準（Cold Calibration）」，但在實際機械硬體中：
1. **安裝時需要實體對齊反饋**：VL53L0X 飛行時間測距感測器若離目標太近（< 30mm）會產生光學飽和，或若角度偏移會收不到回波。我們透過專門的測試腳本，能在馬達運轉時即時監控擺幅，讓您邊微調固定螺絲邊觀察數值，直到擺幅完全穩定鎖定。
2. **無負載基線（No-load Baseline）是自適應控制的最佳起點**：
   - 蘇格蘭軛機構的幾何全行程擺幅（$y_{max} - y_{min} = 2r$）由曲柄半徑 $r$ 決定，**無論有無負載，行程極限與擺幅都是物理固定不變的**。
   - 在無負載下測得的最低啟動 PWM（Actuation PWM）代表機構純機械靜摩擦（Stiction）的下限基準。
   - 當未來加上外部負載（風阻、拉力、自重）時，負載與動摩擦的變化將全權由 **擴展狀態觀測器 (ESO $z_3$) 與動態線性化 (CFDL $b_0(t)$)** 在運行中即時補償，大幅提升啟動響應與穩定性。
3. **固若金湯的安全防護欄（Guardrails）**：
   獲得穩態物理邊界後，韌體可在行程兩端建立安全防護牆。一旦機構脫扣、感測器鬆脫或數值異常超限，系統**立即切斷 PWM 急停**，保護機構齒輪不被強行打壞。

---

## 2. 硬體引腳與接線配置

| 功能模組 | ESP32 GPIO 引腳 | 連接對象 / 說明 |
| :--- | :--- | :--- |
| **馬達調速 (PWM)** | **GPIO 32** | H-Bridge 驅動板 PWM (IN2) |
| **馬達換向 (DIR)** | **GPIO 17** | H-Bridge 驅動板 DIR (IN1) |
| **ToF 感測器 SDA** | **GPIO 21** | VL53L0X I2C 資料線 (SDA) |
| **ToF 感測器 SCL** | **GPIO 15** | VL53L0X I2C 時脈線 (SCL) |
| **感測器供電** | **3.3V & GND** | 直接接 ESP32 3.3V 軌 (共地共壓) |

> [!TIP]
> **感測器安裝最佳距距建議**：建議感測器安裝在機構移動到「最靠近感測器」的位置時，仍保留 **35mm ~ 45mm** 的物理淨空，以避開 VL53L0X 的近場非線性盲區。

---

## 3. 校準流程第一步：感測器對齊與擺幅鎖定 (Test 1)

### 程式位置：
📂 [`motor_ctrl/test_sensor_stroke/test_sensor_stroke.ino`](file:///c:/Users/jim94/wing_system/motor_ctrl/test_sensor_stroke/test_sensor_stroke.ino)

### 測試目的：
1. **驗證 0 PWM 靜態噪訊**：馬達完全停轉時，確認感測器是否穩定，檢驗「0 PWM = 0 位移」與噪訊標準差 $\sigma$。
2. **安裝位置微調與擺幅自動鎖定**：馬達以中速平穩旋轉，程式持續追蹤每個往復循環的極大值（上死點）與極小值（下死點），計算週期擺幅。當連續 6 個循環的擺幅標準差 $< 1.5\text{ mm}$ 時，自動鎖定幾何極限！

### 操作步驟：
1. 將 `test_sensor_stroke.ino` 燒錄至 ESP32。
2. 打開 Arduino IDE 序列埠監控視窗（Baud rate: **115200**）。
3. **靜態階段**：保持機構不動約 3 秒，程式會收集 80 筆靜態數據，輸出噪訊評估報告。
   - 噪訊標準差 $\sigma \le 1.5\text{ mm}$：代表感測器安裝穩固、反光良好。
4. **動態階段**：馬達會自動以適當轉速啟動，監控視窗會即時顯示每一圈的 `Min`、`Max` 與 `Amplitude`。
   - **實體微調**：此時您可以微調感測器的安裝角度與支架位置，觀察序列埠數值是否平穩連貫。
   - 您可按鍵盤 `+` 或 `-` 調節馬達運轉轉速。
   - 輸入 `p` 可切換至 **序列埠繪圖家 (Serial Plotter)**，觀察正弦往復波形。
5. **自動鎖定 (Amplitude Locked)**：
   當擺幅穩定後，程式會自動煞停馬達，並印出如下報告：
   ```text
   ========================================================
   >> [AMPLITUDE LOCKED & CALIBRATION COMPLETE] <<
   ========================================================
    1. Lower Mechanical Bound (y_min)  : 34.50 mm
    2. Upper Mechanical Bound (y_max)  : 85.20 mm
    3. Physical Stroke Span            : 50.70 mm
    4. 0-PWM Sensor Noise Floor (s)    : 0.85 mm
   --------------------------------------------------------
   >> [RECOMMENDED SAFETY GUARDRAIL CONFIGURATION] :
      #define CFG_DEFAULT_Y_MIN_MM       34.5f
      #define CFG_DEFAULT_Y_MAX_MM       85.2f
      #define CFG_DEFAULT_STROKE_SPAN_MM 50.7f
      // Guard Low  = 27.5 mm (y_min - 7.0mm)
      // Guard High = 92.2 mm (y_max + 7.0mm)
   ========================================================
   ```
6. **結果用途**：記錄此處測出的 `y_min`、`y_max` 與行程全幅，這將成為主程式的物理邊界與防護欄基線！

---

## 4. 校準流程第二步：雙閥值摩擦模型測試 (Test 2)

### 程式位置：
📂 [`motor_ctrl/test_actuation_pwm/test_actuation_pwm.ino`](file:///c:/Users/jim94/wing_system/motor_ctrl/test_actuation_pwm/test_actuation_pwm.ino)

### 測試目的（針對「PWM=100 才啟動，但 PWM=50 仍可維持」之摩擦遲滯）：
1. **靜摩擦起步突破閥值（Breakaway Stiction，約 PWM 100）**：
   - 馬達由 0 緩速往上加 PWM，測量「瞬間掙脫靜摩擦咬合、產生實質位移」的臨界點。
   - 用於控制器靜止起步時的前饋加速，消除響應遲滯。
2. **動摩擦滑動維持極限（Kinetic Running Limit，約 PWM 50）**：
   - 馬達開始旋轉進入滑動狀態後，轉速進入階梯式向下緩降，測量「速度掉至停機臨界」的最低維持 PWM。
   - 用於控制器運動滑行時的前饋值，**徹底防止給予過大補償導致目標點附近的狂暴震盪與發燙**。
3. **探測馬達極性與初始加速度控制權重 $b_0$**：
   - 發送短脈衝，確認正轉時距離是增加還是減少，自動配置方向極性。

### 操作步驟：
1. 將 `test_actuation_pwm.ino` 燒錄至 ESP32。
2. 開啟序列埠監控視窗（**115200**）。
3. 輸入 `g` 或 `start` 並送出，開始自動化 3 輪測試：
   - **Phase 1 [爬坡]**：緩慢向上加 PWM，測得 Breakaway Stiction PWM。
   - **Phase 2 [滑動]**：讓馬達順暢滑動 600ms。
   - **Phase 3 [緩降]**：階梯向下減少 PWM，直到測出 Kinetic Running PWM。
   - 雙向（正向與反向）各執行 3 輪求精準平均。
   - 最後施加短脈衝，自動判斷極性與 $b_0$。
4. **輸出代碼報告**：
   ```text
   ========================================================
   >> [TEST 2 COMPLETED: DUAL-THRESHOLD FRICTION PROFILE] <<
   ========================================================
     * FWD Breakaway Stiction : 98.3 / 255 (u = 0.385)
     * FWD Kinetic Running    : 52.0 / 255 (u = 0.204)
     * REV Breakaway Stiction : 96.0 / 255 (u = 0.376)
     * REV Kinetic Running    : 50.3 / 255 (u = 0.197)
     * Motor Polarity         : 1 (Normal: FWD increases distance)
     * Estimated Baseline b0  : 14.2 mm/(s^2 * PWM)
   --------------------------------------------------------
   >> [READY-TO-USE CODE FOR config_motor.h] :
      #define CFG_DEFAULT_U_BREAKAWAY_POS 0.385f  // PWM ~98
      #define CFG_DEFAULT_U_BREAKAWAY_NEG 0.376f  // PWM ~96
      #define CFG_DEFAULT_U_KINETIC_POS   0.204f  // PWM ~52
      #define CFG_DEFAULT_U_KINETIC_NEG   0.197f  // PWM ~50
      #define CFG_DEFAULT_POLARITY        1
      #define CFG_DEFAULT_B0_INIT         14.2f
   ========================================================
   ```
5. **結果用途**：將產生的參數填入 [`motor_ctrl/config_motor.h`](file:///c:/Users/jim94/wing_system/motor_ctrl/config_motor.h) 的預設值區段中。

---

## 5. 正式運行：ADRC 位置控制與安全防護欄 (Main Controller)

### 程式位置：
📂 [`motor_ctrl/motor_ctrl.ino`](file:///c:/Users/jim94/wing_system/motor_ctrl/motor_ctrl.ino)

### 主控制器關鍵優化特點：
1. **多重安全防護欄 (Active Guardrails)**：
   - **行程超限急停 (OOB)**：若實測距離超出 $[y_{min} - 7\text{mm}, y_{max} + 7\text{mm}]$，瞬間切斷 PWM 並解除致動（Disarm）。
   - **感測器離線保護 (Sensor Timeout)**：若 VL53L0X 超過 250ms 未收到有效數據，立即急停。
   - **堵轉保護 (Stall Jam Protection)**：若控制輸出超過 45% PWM 持續超過 600ms 但位移小於 1.2mm，判定為機械異物卡死，立即斷電保護馬達。
2. **Scotch Yoke 死點奇異值正規化 (Dead-Center Regularization)**：
   - 蘇格蘭軛機構接近死點時，$\cos\theta \to 0$，機構增益銳減。傳統控制器會在此處輸出無限大 PWM 造成劇烈抖動；本系統已實作數值鉗位保護（`adrc.b0 >= b0_min`），平順過渡。
3. **擴展狀態觀測器 (ESO) 自適應抗負載**：
   - 即使無負載測試時啟動 PWM 較低，一旦裝上負載或遇到強大阻力，觀測器狀態 $z_3$ 會在數十毫秒內將外部阻力估算出來並於前饋端直接抵消（Disturbance Decoupling）！

### 序列埠操作指令指南：
燒錄 `motor_ctrl.ino` 後，可在 Serial Monitor 輸入以下指令：

| 指令 | 範例 | 說明 |
| :--- | :--- | :--- |
| `arm` | `arm` | 啟用馬達閉迴路控制器（若感測器正常且無故障則進入運轉） |
| `stop` / `s` | `stop` | 立即關閉馬達輸出，進入待機模式 |
| `pos <百分比>` | `pos 50` | 定點伺服：移動並精準保持在行程 50% 位置（0% 為下死點，100% 為上死點） |
| `sweep <min> <max> <freq>` | `sweep 10 90 0.8` | 週期掃描：以 0.8 Hz 頻率在 10% ~ 90% 行程間進行平滑正弦往復擺動 |
| `status` | `status` | 顯示當前觀測器狀態 ($z_1, z_2, z_3$)、自適應權重 $b_0$、目標與實測距離 |
| `plot` | `plot` | 切換為 Serial Plotter 高速 CSV 串流，直觀查看響應曲線 |
| `save` | `save` | 將目前參數永久寫入 ESP32 內部 Flash (NVS Preferences) |
| `reset` | `reset` | 解除防護欄警報（例如排除卡死後重新復歸） |

---

## 6. 整合檔案清單

- [`motor_ctrl/config_motor.h`](file:///c:/Users/jim94/wing_system/motor_ctrl/config_motor.h) - 引腳定義、安全閾值與基線參數中心設定檔。
- [`motor_ctrl/adrc_core.h`](file:///c:/Users/jim94/wing_system/motor_ctrl/adrc_core.h) - ADRC 核心演算函式庫（ESO 觀測器、CFDL 自適應、死區前饋補償、死點鉗位）。
- [`motor_ctrl/test_sensor_stroke/test_sensor_stroke.ino`](file:///c:/Users/jim94/wing_system/motor_ctrl/test_sensor_stroke/test_sensor_stroke.ino) - **Test 1**：感測器對齊、0-PWM 穩定度與擺幅鎖定腳本。
- [`motor_ctrl/test_actuation_pwm/test_actuation_pwm.ino`](file:///c:/Users/jim94/wing_system/motor_ctrl/test_actuation_pwm/test_actuation_pwm.ino) - **Test 2**：雙向啟動 PWM、靜摩擦死區與初始增益探測腳本。
- [`motor_ctrl/motor_ctrl.ino`](file:///c:/Users/jim94/wing_system/motor_ctrl/motor_ctrl.ino) - **正式主控制器**：具備三重安全防護欄、定點保持與往復擺動之 ADRC 控制韌體。
