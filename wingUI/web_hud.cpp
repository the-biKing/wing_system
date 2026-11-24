#include "web_hud.h"

WebServer webServer(80);

void handleRoot() {
  webServer.send_P(200, "text/html", PAGE_INDEX_HTML);
}

void handleStatusApi() {
  char buf[420];
  snprintf(buf, sizeof(buf),
    "{\"dist\":%.1f,\"target_pct\":%d,\"target_mm\":%.1f,\"vel\":%.1f,\"pwm\":%d,\"motor_active\":%s,\"motor_arrived\":%s,\"motor_speed\":%d,\"guard\":%d,\"temp\":%.1f,\"curr1\":%.2f,\"curr2\":%.2f,\"bat_volt\":%.2f,\"bat_pct\":%d,\"light_style\":%d,\"light_hue\":%d,\"light_sat\":%d,\"light_val\":%d,\"light_spd\":%d,\"muted\":%s}",
    currentTofDistMm,
    motorCfg.targetPos,
    motorTargetMm,
    motorFilteredVelocityMmS,
    (int)motorCurrentOutPwm,
    motorServoActive ? "true" : "false",
    motorServoArrived ? "true" : "false",
    motorCfg.speed,
    (int)motorGuardStatus,
    sysTemp,
    sysCurr1,
    sysCurr2,
    sysBatteryVolt,
    sysBatteryPct,
    lightCfg.style,
    lightCfg.hue,
    lightCfg.sat,
    lightCfg.val,
    lightCfg.speed,
    isMuted ? "true" : "false"
  );
  webServer.send(200, "application/json", buf);
}

void handleMotorApi() {
  if (webServer.hasArg("action")) {
    String action = webServer.arg("action");
    if (action.equalsIgnoreCase("engage")) {
      if (webServer.hasArg("target")) {
        float tgt = webServer.arg("target").toFloat();
        motorCfg.targetPos = (uint8_t)constrain(tgt, 0.0f, 100.0f);
      }
      savePreferences(false);
      startMotorServoToPct((float)motorCfg.targetPos);
      if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR) {
        updateMotorSubpage();
      }
      webServer.send(200, "text/plain", "ENGAGED");
      return;
    } else if (action.equalsIgnoreCase("speed")) {
      if (webServer.hasArg("speed")) {
        int spd = webServer.arg("speed").toInt();
        motorCfg.speed = (uint8_t)constrain(spd, 0, 100);
        motorServoActive = false;
        motorServoArrived = false;
        savePreferences(false);
        if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR) {
          updateMotorSubpage();
        }
      }
      webServer.send(200, "text/plain", "SPEED_SET");
      return;
    } else if (action.equalsIgnoreCase("stop")) {
      stopMotorAll();
      if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR) {
        updateMotorSubpage();
      }
      webServer.send(200, "text/plain", "STOPPED");
      return;
    } else if (action.equalsIgnoreCase("reset")) {
      motorGuardStatus = GUARD_OK;
      motorCurrentOutPwm = 0.0f;
      motorIntegralErr = 0.0f;
      if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_MOTOR) {
        updateMotorSubpage();
      }
      webServer.send(200, "text/plain", "GUARD_RESET");
      return;
    }
  }
  webServer.send(400, "text/plain", "BAD_ARGS");
}

void handleLightApi() {
  if (webServer.hasArg("style")) {
    lightCfg.style = (uint8_t)constrain(webServer.arg("style").toInt(), 0, (int)TOTAL_LIGHT_MODES - 1);
  }
  if (webServer.hasArg("hue")) {
    lightCfg.hue = (uint16_t)constrain(webServer.arg("hue").toInt(), 0, 359);
  }
  if (webServer.hasArg("sat")) {
    lightCfg.sat = (uint8_t)constrain(webServer.arg("sat").toInt(), 0, 100);
  }
  if (webServer.hasArg("val")) {
    int reqVal = webServer.arg("val").toInt();
    if (sysCurr1 >= CFG_OVERCURRENT_LIMIT_A && reqVal > lightCfg.val) {
      // 18A 電流安全防護：阻止調高亮度
    } else {
      lightCfg.val = (uint8_t)constrain(reqVal, 0, 100);
    }
  }
  if (webServer.hasArg("speed")) {
    lightCfg.speed = (int8_t)constrain(webServer.arg("speed").toInt(), -50, 100);
  }
  savePreferences(false);
  if (currentState == STATE_SUBPAGE && carouselIndex == PAGE_LIGHT) {
    for (int r = 0; r < 5; r++) updateLightRow(r);
  }
  webServer.send(200, "text/plain", "OK");
}

void handleSystemApi() {
  if (webServer.hasArg("action")) {
    String action = webServer.arg("action");
    if (action.equalsIgnoreCase("mute")) {
      isMuted = !isMuted;
      savePreferences(false);
      drawTopBar();
      webServer.send(200, "text/plain", isMuted ? "MUTED" : "UNMUTED");
      return;
    } else if (action.equalsIgnoreCase("reset")) {
      webServer.send(200, "text/plain", "RESETTING");
      delay(100);
      triggerSystemReset();
      return;
    } else if (action.equalsIgnoreCase("sleep")) {
      webServer.send(200, "text/plain", "SLEEPING");
      delay(100);
      enterDeepSleep(true);
      return;
    }
  }
  webServer.send(400, "text/plain", "BAD_ARGS");
}

void initWebHud() {
  Serial.println(F("\n[WIFI AP] Configuring SoftAP mode..."));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(CFG_WIFI_AP_SSID, CFG_WIFI_AP_PASS, CFG_WIFI_AP_CHANNEL);

  IPAddress apIP = WiFi.softAPIP();
  Serial.print(F("[WIFI AP] SSID: "));
  Serial.println(CFG_WIFI_AP_SSID);
  Serial.print(F("[WIFI AP] IP: "));
  Serial.println(apIP);

  if (MDNS.begin(CFG_WIFI_MDNS_HOST)) {
    Serial.printf("[MDNS] Web HUD: http://%s.local\n", CFG_WIFI_MDNS_HOST);
  }

  // 配置 WebServer 路由
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/api/status", HTTP_GET, handleStatusApi);
  webServer.on("/api/motor", HTTP_POST, handleMotorApi);
  webServer.on("/api/light", HTTP_POST, handleLightApi);
  webServer.on("/api/system", HTTP_POST, handleSystemApi);

  webServer.begin();
  Serial.println(F("[WEB HUD] HTTP Server Started on Port 80."));
}

void handleWebHud() {
  webServer.handleClient();
}
