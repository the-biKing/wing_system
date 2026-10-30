#pragma once
#include <Arduino.h>
#include <math.h>
#include "config_motor.h"

#define DELAY_BUFFER_SIZE 16

// 延遲補償環形緩衝區
typedef struct {
  float buffer[DELAY_BUFFER_SIZE];
  uint8_t head;
  uint8_t delay_samples;
} DelayBuffer_t;

// ADRC 核心控制結構體
typedef struct {
  // --- 物理行程邊界 (mm) ---
  float y_min;
  float y_max;
  float stroke_span;

  // --- 雙閥值摩擦補償 (正規化 0.0 ~ 1.0) ---
  // 1. 靜摩擦起步突破閥值 (Breakaway Stiction)
  float u_breakaway_pos;
  float u_breakaway_neg;
  // 2. 動摩擦滑動維持閥值 (Kinetic Running Friction)
  float u_kinetic_pos;
  float u_kinetic_neg;
  // 速度過渡門檻
  float v_stiction_thresh;

  // --- 動態控制權重與極性 ---
  float b0_init;
  float b0;
  float b0_min;
  int8_t polarity; // +1 或 -1

  // --- 觀測器與控制器頻寬 ---
  float omega_o;
  float omega_c;
  float kp;
  float kd;

  // --- 擴展狀態觀測器 (ESO) 狀態估計值 ---
  float z1; // 位置估計 [mm]
  float z2; // 速度估計 [mm/s]
  float z3; // 總和擾動估計 [mm/s^2] (包含負載、動態非線性、反電勢)

  // --- CFDL 自適應參數 ---
  float rho;
  float mu;
  float eps_u;
  float eps_a;

  // --- 歷史狀態暫存暫存器 ---
  float u_prev1;
  float u_prev2;
  float a_prev;

  // --- 控制週期 (秒) ---
  float dt;

  // --- 系統旗標 ---
  bool is_calibrated;
  bool is_armed;
} ADRC_Controller_t;

// --- 輔助數值函式 ---
static inline float adrc_sign(float val) {
  if (val > 0.00001f) return 1.0f;
  if (val < -0.00001f) return -1.0f;
  return 0.0f;
}

static inline float adrc_abs(float val) {
  return (val < 0.0f) ? -val : val;
}

static inline float adrc_clamp(float val, float min_v, float max_v) {
  if (val < min_v) return min_v;
  if (val > max_v) return max_v;
  return val;
}

// 初始化延遲環形緩衝區
static inline void DelayBuffer_Init(DelayBuffer_t *db, uint8_t delay_samples) {
  db->head = 0;
  db->delay_samples = delay_samples < DELAY_BUFFER_SIZE ? delay_samples : (DELAY_BUFFER_SIZE - 1);
  for (int i = 0; i < DELAY_BUFFER_SIZE; i++) {
    db->buffer[i] = 0.0f;
  }
}

static inline void DelayBuffer_Push(DelayBuffer_t *db, float val) {
  db->buffer[db->head] = val;
  db->head = (db->head + 1) % DELAY_BUFFER_SIZE;
}

static inline float DelayBuffer_Get(DelayBuffer_t *db) {
  int idx = (int)db->head - 1 - (int)db->delay_samples;
  while (idx < 0) {
    idx += DELAY_BUFFER_SIZE;
  }
  return db->buffer[idx % DELAY_BUFFER_SIZE];
}

// 初始化 ADRC 控制器
inline void ADRC_Init(ADRC_Controller_t *adrc, float dt_seconds) {
  adrc->dt = dt_seconds;
  adrc->omega_o = ADRC_OMEGA_O;
  adrc->omega_c = ADRC_OMEGA_C;

  adrc->kp = adrc->omega_c * adrc->omega_c;
  adrc->kd = 2.0f * adrc->omega_c;

  // 載入預設基線參數 (後續可由校準數據覆蓋)
  adrc->y_min = CFG_DEFAULT_Y_MIN_MM;
  adrc->y_max = CFG_DEFAULT_Y_MAX_MM;
  adrc->stroke_span = adrc->y_max - adrc->y_min;

  adrc->u_breakaway_pos = CFG_DEFAULT_U_BREAKAWAY_POS;
  adrc->u_breakaway_neg = CFG_DEFAULT_U_BREAKAWAY_NEG;
  adrc->u_kinetic_pos   = CFG_DEFAULT_U_KINETIC_POS;
  adrc->u_kinetic_neg   = CFG_DEFAULT_U_KINETIC_NEG;
  adrc->v_stiction_thresh = CFG_STICTION_VELOCITY_THRESH_MM_S;

  adrc->b0_init = CFG_DEFAULT_B0_INIT;
  adrc->b0 = adrc->b0_init;
  adrc->b0_min = CFG_DEFAULT_B0_MIN;
  adrc->polarity = CFG_DEFAULT_POLARITY;

  adrc->rho = CFDL_RHO;
  adrc->mu = CFDL_MU;
  adrc->eps_u = CFDL_EPS_U;
  adrc->eps_a = CFDL_EPS_A;

  adrc->z1 = (adrc->y_min + adrc->y_max) * 0.5f;
  adrc->z2 = 0.0f;
  adrc->z3 = 0.0f;

  adrc->u_prev1 = 0.0f;
  adrc->u_prev2 = 0.0f;
  adrc->a_prev = 0.0f;

  adrc->is_calibrated = false;
  adrc->is_armed = false;
}

// 百分比行程轉換為物理距離 [mm]
static inline float ADRC_PctToMm(const ADRC_Controller_t *adrc, float pct) {
  pct = adrc_clamp(pct, 0.0f, 100.0f);
  return adrc->y_min + (pct / 100.0f) * adrc->stroke_span;
}

// 物理距離 [mm] 轉換為百分比行程 [0.0 ~ 100.0%]
static inline float ADRC_MmToPct(const ADRC_Controller_t *adrc, float mm) {
  if (adrc->stroke_span < 0.1f) return 0.0f;
  float pct = ((mm - adrc->y_min) / adrc->stroke_span) * 100.0f;
  return adrc_clamp(pct, 0.0f, 100.0f);
}

// 執行一次 ADRC 控制器迭代計算 (返回 -1.0 ~ +1.0 正規化控制量)
inline float ADRC_Update(ADRC_Controller_t *adrc, float y_ref_mm, float y_meas_mm, DelayBuffer_t *db) {
  // 1. 存入新量測值並取得延遲對齊值
  DelayBuffer_Push(db, y_meas_mm);
  float y_delayed = DelayBuffer_Get(db);

  // 2. 擴展狀態觀測器 (ESO) 迭代更新
  float beta1 = 3.0f * adrc->omega_o;
  float beta2 = 3.0f * adrc->omega_o * adrc->omega_o;
  float beta3 = adrc->omega_o * adrc->omega_o * adrc->omega_o;

  float e = adrc->z1 - y_delayed;

  // 尤拉積分推進
  adrc->z1 += adrc->dt * (adrc->z2 - beta1 * e);
  adrc->z2 += adrc->dt * (adrc->z3 - beta2 * e + adrc->b0 * adrc->u_prev1);
  adrc->z3 += adrc->dt * (-beta3 * e);

  // 3. CFDL 動態即時控制權重 b0(t) 自適應更新
  float a_curr = (adrc->z2 - adrc->a_prev) / adrc->dt;
  float delta_a = a_curr - adrc->a_prev;
  float delta_u = adrc->u_prev1 - adrc->u_prev2;

  // 激勵持久性檢查 (PE 條件)：靜止時凍結自適應更新
  if (adrc_abs(delta_u) > adrc->eps_u && adrc_abs(delta_a) > adrc->eps_a) {
    float den = adrc->mu + (delta_u * delta_u);
    float b0_new = adrc->b0 + (adrc->rho * delta_u / den) * (delta_a - adrc->b0 * delta_u);

    // Scotch Yoke 死點正規化防護：防止 cos(theta)->0 時 b0 崩塌導致 u 暴增
    if (adrc_abs(b0_new) >= adrc->b0_min) {
      adrc->b0 = b0_new;
    }
  }
  adrc->a_prev = a_curr;
  adrc->u_prev2 = adrc->u_prev1;

  // 4. ADRC 虛擬控制律合成與總和擾動消除
  float u0 = adrc->kp * (y_ref_mm - adrc->z1) - adrc->kd * adrc->z2;

  // 確保 b0 不為零
  float eff_b0 = adrc->b0;
  if (adrc_abs(eff_b0) < adrc->b0_min) {
    eff_b0 = (eff_b0 >= 0.0f) ? adrc->b0_min : -adrc->b0_min;
  }

  float u_raw = (u0 - adrc->z3) / eff_b0;

  // 動態極性校正
  u_raw *= (float)adrc->polarity;

  // 5. 雙閥值摩擦補償 (Dual-Threshold Friction Compensation with Stribeck Transition)
  //    依據觀測器估計之速度 z2，平滑切換「靜摩擦突破前饋」與「動摩擦滑行前饋」
  float speed = adrc_abs(adrc->z2);
  float stiction_factor = 1.0f;
  if (speed < adrc->v_stiction_thresh) {
    // 速度低於門檻時：隨速度平滑下降 (1.0 -> 0.0)
    stiction_factor = 1.0f - (speed / adrc->v_stiction_thresh);
  } else {
    // 已經順暢滑行：完全過渡到純動摩擦
    stiction_factor = 0.0f;
  }

  float eff_dead_pos = adrc->u_kinetic_pos + stiction_factor * (adrc->u_breakaway_pos - adrc->u_kinetic_pos);
  float eff_dead_neg = adrc->u_kinetic_neg + stiction_factor * (adrc->u_breakaway_neg - adrc->u_kinetic_neg);

  float u_cmd = 0.0f;
  const float EPS_ZERO = 0.005f;

  if (u_raw > EPS_ZERO) {
    u_cmd = u_raw + eff_dead_pos;
  } else if (u_raw < -EPS_ZERO) {
    u_cmd = u_raw - eff_dead_neg;
  } else {
    u_cmd = 0.0f;
  }

  // 6. 硬體邊界鉗位 [-1.0, 1.0]
  u_cmd = adrc_clamp(u_cmd, -1.0f, 1.0f);

  adrc->u_prev1 = u_cmd;
  return u_cmd;
}
