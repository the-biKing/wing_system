#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "config.h"

// 引用主程式宣告之全域變數與控制函式
extern MotorConfig motorCfg;
extern GuardrailState motorGuardStatus;
extern bool motorServoActive;
extern bool motorServoArrived;
extern float motorIntegralErr;
extern float motorTargetMm;
extern float motorCurrentOutPwm;
extern float motorFilteredVelocityMmS;
extern float currentTofDistMm;
extern void startMotorServoToPct(float pct);
extern void stopMotorAll();
extern float calcTargetMm(float pct);

extern LightConfig lightCfg;

extern float sysTemp;
extern float sysCurr1;
extern float sysCurr2;
extern float sysBatteryVolt;
extern int sysBatteryPct;
extern bool isMuted;
extern SysState currentState;
extern int carouselIndex;

extern void savePreferences(bool setSleepFlag);
extern void enterDeepSleep(bool saveToPrefs);
extern void triggerSystemReset();
extern void updateMotorSubpage();
extern void updateLightRow(int r);
extern void updatePhysicalWs2812();
extern void drawTopBar();

// Web 伺服器實例 (Port 80)
extern WebServer webServer;

// 靜態 HTML / CSS / JS 戰術網頁 (儲存於 Flash PROGMEM，免外網即可使用)
const char PAGE_INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-TW">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>WING SYSTEM // MOBILE HUD</title>
  <style>
    :root {
      --bg: #070a0f;
      --card-bg: #0f1622;
      --border: #1e2c40;
      --cyan: #00f0ff;
      --amber: #ffaa00;
      --green: #00ff66;
      --red: #ff3344;
      --text: #c0d0e0;
      --text-dim: #5a7088;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace; }
    body { background: var(--bg); color: var(--text); padding: 12px; font-size: 14px; -webkit-tap-highlight-color: transparent; }
    .header { display: flex; justify-content: space-between; align-items: center; padding-bottom: 10px; border-bottom: 1px solid var(--border); margin-bottom: 12px; }
    .title { color: var(--cyan); font-weight: 900; font-size: 16px; letter-spacing: 1px; display: flex; align-items: center; gap: 6px; }
    .badge { font-size: 11px; padding: 2px 6px; border-radius: 4px; font-weight: bold; background: rgba(0, 240, 255, 0.15); color: var(--cyan); border: 1px solid var(--cyan); }
    .telemetry-bar { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px; margin-bottom: 12px; }
    .telem-box { background: var(--card-bg); border: 1px solid var(--border); border-radius: 6px; padding: 8px 6px; text-align: center; }
    .telem-label { font-size: 10px; color: var(--text-dim); text-transform: uppercase; margin-bottom: 2px; }
    .telem-val { font-size: 14px; font-weight: bold; color: #fff; }
    .card { background: var(--card-bg); border: 1px solid var(--border); border-radius: 8px; padding: 12px; margin-bottom: 12px; position: relative; }
    .card-title { color: var(--amber); font-size: 13px; font-weight: bold; text-transform: uppercase; margin-bottom: 10px; display: flex; justify-content: space-between; }
    .status-pill { font-size: 10px; padding: 2px 6px; border-radius: 3px; font-weight: bold; }
    .status-ok { background: rgba(0, 255, 102, 0.2); color: var(--green); border: 1px solid var(--green); }
    .status-warn { background: rgba(255, 170, 0, 0.2); color: var(--amber); border: 1px solid var(--amber); }
    .status-err { background: rgba(255, 51, 68, 0.2); color: var(--red); border: 1px solid var(--red); }
    .progress-track { height: 8px; background: #070a0f; border-radius: 4px; overflow: hidden; margin: 8px 0; border: 1px solid var(--border); }
    .progress-fill { height: 100%; background: var(--cyan); width: 0%; transition: width 0.2s ease; }
    .control-row { margin: 12px 0 6px; }
    .control-label { display: flex; justify-content: space-between; font-size: 12px; margin-bottom: 4px; }
    input[type=range] { width: 100%; -webkit-appearance: none; background: #1a2536; height: 8px; border-radius: 4px; outline: none; }
    input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 22px; height: 22px; border-radius: 50%; background: var(--cyan); border: 2px solid #fff; cursor: pointer; }
    .presets { display: flex; gap: 4px; margin: 8px 0; }
    .btn-preset { flex: 1; padding: 6px 0; font-size: 11px; font-weight: bold; background: #141f2e; border: 1px solid var(--border); color: var(--text); border-radius: 4px; cursor: pointer; }
    .btn-preset:active { background: var(--cyan); color: #000; }
    .btn { display: block; width: 100%; padding: 10px 0; border: none; border-radius: 6px; font-weight: bold; font-size: 13px; text-transform: uppercase; cursor: pointer; margin-top: 8px; text-align: center; }
    .btn-engage { background: var(--cyan); color: #000; box-shadow: 0 0 10px rgba(0, 240, 255, 0.4); }
    .btn-engage:active { filter: brightness(1.2); }
    .btn-stop { background: var(--red); color: #fff; box-shadow: 0 0 10px rgba(255, 51, 68, 0.4); }
    .btn-outline { background: transparent; border: 1px solid var(--border); color: var(--text); }
    .btn-amber { background: var(--amber); color: #000; }
    .modes-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px; margin-bottom: 10px; }
    .btn-mode { padding: 8px 4px; font-size: 11px; font-weight: bold; background: #141f2e; border: 1px solid var(--border); color: var(--text-dim); border-radius: 4px; cursor: pointer; text-align: center; }
    .btn-mode.active { background: rgba(0, 240, 255, 0.2); border-color: var(--cyan); color: var(--cyan); }
    .color-preview { height: 16px; border-radius: 4px; border: 1px solid #fff; margin-top: 6px; }
    .sys-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-top: 8px; }
    .toast { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%); background: rgba(0, 240, 255, 0.9); color: #000; font-weight: bold; padding: 8px 16px; border-radius: 20px; font-size: 12px; display: none; z-index: 99; }
  </style>
</head>
<body>

  <div class="header">
    <div class="title">WING SYSTEM <span class="badge">HUD AP</span></div>
    <div id="connBadge" class="status-pill status-ok">CONNECTED</div>
  </div>

  <div class="telemetry-bar">
    <div class="telem-box">
      <div class="telem-label">2S 電池</div>
      <div class="telem-val" id="valBat">--%</div>
      <div style="font-size: 10px; color: var(--text-dim);" id="valBatV">-- V</div>
    </div>
    <div class="telem-box">
      <div class="telem-label">系統溫度</div>
      <div class="telem-val" id="valTemp">-- °C</div>
      <div style="font-size: 10px; color: var(--text-dim);">DS18B20</div>
    </div>
    <div class="telem-box">
      <div class="telem-label">行程距離</div>
      <div class="telem-val" id="valDist">-- mm</div>
      <div style="font-size: 10px; color: var(--text-dim);">VL53L0X</div>
    </div>
  </div>

  <!-- 馬達控制面板 -->
  <div class="card">
    <div class="card-title">
      <span>馬達伺服定位 (Scotch Yoke)</span>
      <span id="motorStatusBadge" class="status-pill status-ok">IDLE</span>
    </div>
    
    <div class="control-label">
      <span>目前行程進度</span>
      <span id="lblStrokePos">0.0 mm</span>
    </div>
    <div class="progress-track">
      <div class="progress-fill" id="barDist"></div>
    </div>

    <div class="control-row">
      <div class="control-label">
        <span>指定目標位置: <b id="lblTargetPct" style="color:var(--cyan)">50%</b></span>
        <span id="lblTargetMm" style="color:var(--text-dim)">77.4 mm</span>
      </div>
      <input type="range" id="rngTarget" min="0" max="100" value="50" oninput="onTargetSlide(this.value)">
    </div>

    <div class="presets">
      <button class="btn-preset" onclick="setTarget(0)">0% 底點</button>
      <button class="btn-preset" onclick="setTarget(25)">25%</button>
      <button class="btn-preset" onclick="setTarget(50)">50% 中間</button>
      <button class="btn-preset" onclick="setTarget(75)">75%</button>
      <button class="btn-preset" onclick="setTarget(100)">100% 頂點</button>
    </div>

    <button class="btn btn-engage" onclick="engageTarget()">🚀 啟動目標閉迴路定位</button>

    <div class="control-row" style="margin-top: 14px; border-top: 1px dashed var(--border); padding-top: 10px;">
      <div class="control-label">
        <span>手動轉速: <b id="lblSpeedPct" style="color:var(--amber)">0%</b></span>
      </div>
      <input type="range" id="rngSpeed" min="0" max="100" value="0" oninput="onSpeedSlide(this.value)">
      <div style="display:flex; gap:6px; margin-top:6px;">
        <button class="btn btn-amber" style="margin:0;" onclick="sendManualSpeed()">運轉手動速度</button>
        <button class="btn btn-stop" style="margin:0;" onclick="sendStop()">🛑 急停 HALT</button>
      </div>
    </div>
  </div>

  <!-- 燈光控制面板 -->
  <div class="card">
    <div class="card-title">
      <span>WS2812 幻彩燈效</span>
      <span class="badge" id="lblCurStyle">STATIC</span>
    </div>

    <div class="modes-grid">
      <div class="btn-mode active" id="btnMode0" onclick="setLightMode(0)">常態 STATIC</div>
      <div class="btn-mode" id="btnMode1" onclick="setLightMode(1)">彩虹 RAINBOW</div>
      <div class="btn-mode" id="btnMode2" onclick="setLightMode(2)">呼吸 BREATHE</div>
      <div class="btn-mode" id="btnMode3" onclick="setLightMode(3)">雷擊 THUNDER</div>
      <div class="btn-mode" id="btnMode4" onclick="setLightMode(4)">雨滴 RAIN</div>
      <div class="btn-mode" id="btnMode5" onclick="setLightMode(5)">派對 PARTY</div>
    </div>

    <div class="control-row">
      <div class="control-label">
        <span>色相 (Hue)</span>
        <span id="lblHue">180°</span>
      </div>
      <input type="range" id="rngHue" min="0" max="359" value="180" oninput="onLightChange()">
    </div>

    <div class="control-row">
      <div class="control-label">
        <span>飽和度 (Saturation)</span>
        <span id="lblSat">100%</span>
      </div>
      <input type="range" id="rngSat" min="0" max="100" value="100" oninput="onLightChange()">
    </div>

    <div class="control-row">
      <div class="control-label">
        <span>總亮度 (Brightness)</span>
        <span id="lblVal">80%</span>
      </div>
      <input type="range" id="rngVal" min="0" max="100" value="80" oninput="onLightChange()">
    </div>

    <div class="control-row">
      <div class="control-label">
        <span>動態速度 (Speed)</span>
        <span id="lblSpd">50</span>
      </div>
      <input type="range" id="rngSpd" min="-50" max="100" value="50" oninput="onLightChange()">
    </div>

    <div class="color-preview" id="colorSwatch"></div>
  </div>

  <!-- 系統電量與感測器面板 -->
  <div class="card">
    <div class="card-title">系統狀態與電源管理</div>
    <div style="font-size:11px; color:var(--text-dim); line-height:1.6;">
      <div>• 電流感測 1 (20A): <b id="valC1" style="color:#fff">0.00 A</b></div>
      <div>• 電流感測 2 (05B): <b id="valC2" style="color:#fff">0.00 A</b></div>
      <div>• 馬達驅動輸出: <b id="valPWM" style="color:#fff">PWM: 0</b></div>
      <div>• 故障保護狀態: <b id="valGuard" style="color:var(--green)">正常 OK</b></div>
    </div>

    <div class="sys-grid">
      <button class="btn btn-outline" id="btnMute" onclick="toggleMute()">🔇 切換靜音</button>
      <button class="btn btn-outline" onclick="resetGuard()">🔄 防護欄復位</button>
      <button class="btn btn-outline" onclick="systemReset()">⚠️ 系統重置</button>
      <button class="btn btn-stop" onclick="powerOff()">⏻ 關機 (休眠)</button>
    </div>
  </div>

  <div class="toast" id="toastMsg">指令已送出</div>

  <script>
    let curTargetPct = 50;
    let curLightStyle = 0;
    let isUserSliding = false;
    let slideTimer = null;

    function showToast(msg) {
      const t = document.getElementById('toastMsg');
      t.innerText = msg;
      t.style.display = 'block';
      setTimeout(() => { t.style.display = 'none'; }, 1500);
    }

    function onTargetSlide(v) {
      curTargetPct = v;
      document.getElementById('lblTargetPct').innerText = v + '%';
      const mm = 55.7 + (v / 100.0) * (99.2 - 55.7);
      document.getElementById('lblTargetMm').innerText = mm.toFixed(1) + ' mm';
    }

    function setTarget(v) {
      document.getElementById('rngTarget').value = v;
      onTargetSlide(v);
    }

    function engageTarget() {
      fetch('/api/motor?action=engage&target=' + curTargetPct, { method: 'POST' })
        .then(() => showToast('已啟動伺服定位至 ' + curTargetPct + '%'));
    }

    function onSpeedSlide(v) {
      document.getElementById('lblSpeedPct').innerText = v + '%';
    }

    function sendManualSpeed() {
      const v = document.getElementById('rngSpeed').value;
      fetch('/api/motor?action=speed&speed=' + v, { method: 'POST' })
        .then(() => showToast('已設定手動轉速 ' + v + '%'));
    }

    function sendStop() {
      fetch('/api/motor?action=stop', { method: 'POST' })
        .then(() => showToast('緊急急停！馬達已斷電'));
    }

    function setLightMode(m) {
      curLightStyle = m;
      for (let i = 0; i < 6; i++) {
        document.getElementById('btnMode' + i).classList.toggle('active', i === m);
      }
      onLightChange();
    }

    function onLightChange() {
      const h = document.getElementById('rngHue').value;
      const s = document.getElementById('rngSat').value;
      const v = document.getElementById('rngVal').value;
      const spd = document.getElementById('rngSpd').value;

      document.getElementById('lblHue').innerText = h + '°';
      document.getElementById('lblSat').innerText = s + '%';
      document.getElementById('lblVal').innerText = v + '%';
      document.getElementById('lblSpd').innerText = spd;

      document.getElementById('colorSwatch').style.backgroundColor = `hsl(${h}, ${s}%, ${v/2}%)`;

      clearTimeout(slideTimer);
      slideTimer = setTimeout(() => {
        fetch(`/api/light?style=${curLightStyle}&hue=${h}&sat=${s}&val=${v}&speed=${spd}`, { method: 'POST' });
      }, 80);
    }

    function toggleMute() {
      fetch('/api/system?action=mute', { method: 'POST' })
        .then(() => showToast('已切換靜音狀態'));
    }

    function resetGuard() {
      fetch('/api/motor?action=reset', { method: 'POST' })
        .then(() => showToast('防護欄已復位'));
    }

    function systemReset() {
      if (confirm('確定要將系統恢復原廠出廠設定？')) {
        fetch('/api/system?action=reset', { method: 'POST' })
          .then(() => showToast('系統重置中...'));
      }
    }

    function powerOff() {
      if (confirm('確定要關閉電源進入深度休眠？\n(喚醒需長按實體按鍵 2 秒)')) {
        fetch('/api/system?action=sleep', { method: 'POST' })
          .then(() => alert('系統已關閉進入休眠。'));
      }
    }

    function fetchStatus() {
      fetch('/api/status')
        .then(r => r.json())
        .then(d => {
          document.getElementById('valBat').innerText = d.bat_pct + '%';
          document.getElementById('valBatV').innerText = d.bat_volt.toFixed(2) + ' V';
          document.getElementById('valTemp').innerText = d.temp.toFixed(1) + ' °C';
          document.getElementById('valDist').innerText = (d.dist > 0 ? d.dist.toFixed(1) : '--') + ' mm';

          document.getElementById('valC1').innerText = d.curr1.toFixed(2) + ' A';
          document.getElementById('valC2').innerText = d.curr2.toFixed(2) + ' A';
          document.getElementById('valPWM').innerText = 'PWM: ' + d.pwm;

          // 行程進度 Bar
          const strokePct = Math.min(100, Math.max(0, ((d.dist - 53.7) / 48.0) * 100));
          document.getElementById('barDist').style.width = strokePct + '%';
          document.getElementById('lblStrokePos').innerText = (d.dist > 0 ? d.dist.toFixed(1) : '0.0') + ' mm (' + strokePct.toFixed(0) + '%)';

          // 馬達狀態 Badge
          const mb = document.getElementById('motorStatusBadge');
          if (d.guard !== 0) {
            mb.innerText = 'GUARD TRIPPED';
            mb.className = 'status-pill status-err';
            document.getElementById('valGuard').innerText = '警報觸發: ' + d.guard;
            document.getElementById('valGuard').style.color = 'var(--red)';
          } else if (d.motor_active) {
            mb.innerText = 'MOVING.. P:' + d.pwm;
            mb.className = 'status-pill status-warn';
            document.getElementById('valGuard').innerText = '正常 (移動中)';
            document.getElementById('valGuard').style.color = 'var(--amber)';
          } else if (d.motor_arrived) {
            mb.innerText = 'ARRIVED! PWM:0';
            mb.className = 'status-pill status-ok';
            document.getElementById('valGuard').innerText = '正常 (到位保持)';
            document.getElementById('valGuard').style.color = 'var(--green)';
          } else {
            mb.innerText = 'IDLE [PWM:0]';
            mb.className = 'status-pill status-ok';
            document.getElementById('valGuard').innerText = '正常 OK';
            document.getElementById('valGuard').style.color = 'var(--green)';
          }

          document.getElementById('btnMute').innerText = d.muted ? '🔊 解除靜音' : '🔇 切換靜音';
        })
        .catch(() => {
          document.getElementById('connBadge').innerText = 'RECONNECTING';
          document.getElementById('connBadge').className = 'status-pill status-err';
        });
    }

    setInterval(fetchStatus, 500);
    fetchStatus();
  </script>
</body>
</html>
)rawliteral";

// 初始化 Web 控制器與路由
void initWebHud();

// 於 loop() 中非阻塞處理 HTTP 請求
void handleWebHud();
