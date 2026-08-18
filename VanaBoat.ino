#include <WiFi.h>
#include <WebServer.h>
#include <TinyGPS++.h>

// ================================================================
// KONFIGURASI WI-FI MODE STATION (STA)
// ================================================================
const char* ssid     = "VANABOT";
const char* password = "00000000";

WebServer server(80);

// --- MODUL GPS (Hardware Serial2) ---
TinyGPSPlus gps;
HardwareSerial serialGPS(2);
#define PIN_GPS_RX 25
#define PIN_GPS_TX 4

// --- VARIABEL LAST SEEN LOCATION ---
double lastLat = 0.0;
double lastLng = 0.0;
bool hasLastLocation = false;
unsigned long lastFixTime = 0;

// Pin Motor 1 (Kiri)
const int M1_RPWM = 21;
const int M1_LPWM = 22;

// Pin Motor 2 (Kanan)
const int M2_RPWM = 32;
const int M2_LPWM = 33;

// Alokasi Channel PWM untuk ESP32 Core v2.x.x
const int CH_M1_R = 0;
const int CH_M1_L = 1;
const int CH_M2_R = 2;
const int CH_M2_L = 3;

// Konfigurasi PWM ESP32
const int FREKUENSI_PWM = 1000;
const int RESOLUSI_PWM  = 8;

const int PWM_STEP = 1;
const int MIN_PWM  = 35;

struct ButtonConfig {
  int speedM1;
  String dirM1;
  int speedM2;
  String dirM2;
};

ButtonConfig cfgUp    = {150, "cw",  150, "cw"};
ButtonConfig cfgDown  = {150, "ccw", 150, "ccw"};
ButtonConfig cfgLeft  = {150, "ccw", 150, "cw"};
ButtonConfig cfgRight = {150, "cw",  150, "ccw"};

int targetM1 = 0;
int targetM2 = 0;
int currentM1 = 0;
int currentM2 = 0;

unsigned long lastClientAction = 0;
const unsigned long ACTION_TIMEOUT = 400;
unsigned long lastSmoothTime = 0;

void driveMotorHardware(int channelR, int channelL, int pwmVal) {
  if (pwmVal > 0) {
    ledcWrite(channelR, pwmVal);
    ledcWrite(channelL, 0);
  } else if (pwmVal < 0) {
    ledcWrite(channelR, 0);
    ledcWrite(channelL, -pwmVal);
  } else {
    ledcWrite(channelR, 0);
    ledcWrite(channelL, 0);
  }
}

void smoothMotor() {
  if (millis() - lastSmoothTime >= 15) {
    lastSmoothTime = millis();

    // Motor 1
    if (targetM1 == 0) {
      currentM1 = 0;
    } else {
      if ((targetM1 > 0 && currentM1 < 0) || (targetM1 < 0 && currentM1 > 0)) currentM1 = 0;
      if (currentM1 == 0) currentM1 = (targetM1 > 0) ? MIN_PWM : -MIN_PWM;

      if (currentM1 < targetM1) {
        currentM1 += PWM_STEP;
        if (currentM1 > targetM1) currentM1 = targetM1;
      } else if (currentM1 > targetM1) {
        currentM1 -= PWM_STEP;
        if (currentM1 < targetM1) currentM1 = targetM1;
      }
    }

    // Motor 2
    if (targetM2 == 0) {
      currentM2 = 0;
    } else {
      if ((targetM2 > 0 && currentM2 < 0) || (targetM2 < 0 && currentM2 > 0)) currentM2 = 0;
      if (currentM2 == 0) currentM2 = (targetM2 > 0) ? MIN_PWM : -MIN_PWM;

      if (currentM2 < targetM2) {
        currentM2 += PWM_STEP;
        if (currentM2 > targetM2) currentM2 = targetM2;
      } else if (currentM2 > targetM2) {
        currentM2 -= PWM_STEP;
        if (currentM2 < targetM2) currentM2 = targetM2;
      }
    }

    driveMotorHardware(CH_M1_R, CH_M1_L, currentM1);
    driveMotorHardware(CH_M2_R, CH_M2_L, currentM2);
  }
}

void executeConfig(const ButtonConfig& cfg) {
  lastClientAction = millis();

  if (cfg.dirM1 == "cw")        targetM1 = cfg.speedM1;
  else if (cfg.dirM1 == "ccw")  targetM1 = -cfg.speedM1;
  else                          targetM1 = 0;

  if (cfg.dirM2 == "cw")        targetM2 = cfg.speedM2;
  else if (cfg.dirM2 == "ccw")  targetM2 = -cfg.speedM2;
  else                          targetM2 = 0;
}

void stopAllMotors() {
  targetM1 = 0;
  targetM2 = 0;
}

String renderSettingBox(String label, String idPrefix, ButtonConfig cfg) {
  String html = "<div class='card'>";
  html += "<h3>" + label + "</h3>";

  html += "<div class='row'><b>Motor 1:</b> ";
  html += "Spd: <input type='number' id='" + idPrefix + "_s1' value='" + String(cfg.speedM1) + "' min='0' max='255'> ";
  html += "Arah: <select id='" + idPrefix + "_d1'>";
  html += "<option value='cw'"   + String(cfg.dirM1 == "cw" ? " selected" : "") + ">CW</option>";
  html += "<option value='ccw'"  + String(cfg.dirM1 == "ccw" ? " selected" : "") + ">CCW</option>";
  html += "<option value='stop'" + String(cfg.dirM1 == "stop" ? " selected" : "") + ">STOP</option>";
  html += "</select></div>";

  html += "<div class='row'><b>Motor 2:</b> ";
  html += "Spd: <input type='number' id='" + idPrefix + "_s2' value='" + String(cfg.speedM2) + "' min='0' max='255'> ";
  html += "Arah: <select id='" + idPrefix + "_d2'>";
  html += "<option value='cw'"   + String(cfg.dirM2 == "cw" ? " selected" : "") + ">CW</option>";
  html += "<option value='ccw'"  + String(cfg.dirM2 == "ccw" ? " selected" : "") + ">CCW</option>";
  html += "<option value='stop'" + String(cfg.dirM2 == "stop" ? " selected" : "") + ">STOP</option>";
  html += "</select></div>";

  html += "<button class='btn-save' onclick=\"saveConfig('" + idPrefix + "')\">Simpan Config " + label + "</button>";
  html += "</div>";
  return html;
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>ESP32 RC Controller</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; text-align: center; background-color: #1e1e24; color: white; margin: 0; padding: 15px; user-select: none; }";
  html += ".btn-ctrl { width: 75px; height: 75px; margin: 5px; font-size: 22px; font-weight: bold; border-radius: 12px; border: none; background-color: #008CBA; color: white; cursor: pointer; touch-action: manipulation; }";
  html += ".btn-ctrl:active { background-color: #005f7b; }";
  html += ".btn-stop { background-color: #e74c3c; }";
  html += ".card { background: #2b2d42; margin: 10px auto; padding: 12px; max-width: 380px; border-radius: 10px; text-align: left; }";
  html += ".card h3 { margin: 0 0 10px 0; font-size: 16px; color: #4ea8de; }";
  html += ".row { margin-bottom: 8px; display: flex; justify-content: space-between; align-items: center; font-size: 14px; }";
  html += "input[type=number] { width: 55px; padding: 4px; border-radius: 4px; border: none; text-align: center; }";
  html += "select { padding: 4px; border-radius: 4px; border: none; }";
  html += ".btn-save { width: 100%; padding: 8px; margin-top: 5px; background: #2ec4b6; border: none; color: white; font-weight: bold; border-radius: 5px; cursor: pointer; }";
  html += ".btn-map { display: inline-block; width: 100%; padding: 10px; margin-top: 8px; background: #ff9f1c; color: black; font-weight: bold; text-decoration: none; border-radius: 5px; text-align: center; box-sizing: border-box; }";
  html += ".badge-online { color: #2ecc71; font-weight: bold; }";
  html += ".badge-offline { color: #e67e22; font-weight: bold; }";
  html += "</style></head><body>";

  html += "<h2>ESP32 Advanced Controller</h2>";

  html += "<div class='card'>";
  html += "<h3>📍 Status Lokasi GPS</h3>";
  html += "<div id='gps-info'>Mencari Sinyal GPS...</div>";
  html += "<a id='gps-link' class='btn-map' href='#' target='_blank' style='display:none;'>Buka Google Maps</a>";
  html += "</div>";

  html += "<div>";
  html += "  <button class='btn-ctrl' onmousedown=\"startCmd('up')\" ontouchstart=\"startCmd('up')\" onmouseup=\"stopCmd()\" ontouchend=\"stopCmd()\">▲</button><br>";
  html += "  <button class='btn-ctrl' onmousedown=\"startCmd('left')\" ontouchstart=\"startCmd('left')\" onmouseup=\"stopCmd()\" ontouchend=\"stopCmd()\">◀</button>";
  html += "  <button class='btn-ctrl btn-stop' onclick=\"sendCmd('stop')\">■</button>";
  html += "  <button class='btn-ctrl' onmousedown=\"startCmd('right')\" ontouchstart=\"startCmd('right')\" onmouseup=\"stopCmd()\" ontouchend=\"stopCmd()\">▶</button><br>";
  html += "  <button class='btn-ctrl' onmousedown=\"startCmd('down')\" ontouchstart=\"startCmd('down')\" onmouseup=\"stopCmd()\" ontouchend=\"stopCmd()\">▼</button>";
  html += "</div><br>";

  html += "<h2>Pengaturan Tombol</h2>";
  html += renderSettingBox("Tombol ATAS (▲)", "up", cfgUp);
  html += renderSettingBox("Tombol BAWAH (▼)", "down", cfgDown);
  html += renderSettingBox("Tombol KIRI (◀)", "left", cfgLeft);
  html += renderSettingBox("Tombol KANAN (▶)", "right", cfgRight);

  html += "<script>";
  html += "let timer = null;";
  html += "function sendCmd(action) { fetch('/action?dir=' + action); }";
  html += "function startCmd(action) {";
  html += "  if (timer !== null) clearInterval(timer);";
  html += "  sendCmd(action);";
  html += "  timer = setInterval(() => { sendCmd(action); }, 120);";
  html += "}";
  html += "function stopCmd() {";
  html += "  if (timer !== null) { clearInterval(timer); timer = null; }";
  html += "  sendCmd('stop');";
  html += "}";
  html += "function saveConfig(btn) {";
  html += "  let s1 = document.getElementById(btn + '_s1').value;";
  html += "  let d1 = document.getElementById(btn + '_d1').value;";
  html += "  let s2 = document.getElementById(btn + '_s2').value;";
  html += "  let d2 = document.getElementById(btn + '_d2').value;";
  html += "  fetch(`/setconfig?btn=${btn}&s1=${s1}&d1=${d1}&s2=${s2}&d2=${d2}`).then(() => alert('Konfigurasi ' + btn.toUpperCase() + ' Tersimpan!'));";
  html += "}";

  html += "function updateGPS() {";
  html += "  fetch('/gpsdata').then(res => res.json()).then(data => {";
  html += "    let div = document.getElementById('gps-info');";
  html += "    let link = document.getElementById('gps-link');";
  html += "    if (data.has_data) {";
  html += "      let statusTxt = data.live ? \"<span class='badge-online'>🟢 Sinyal GPS Aktif</span>\" : \"<span class='badge-offline'>⚠️ Terakhir Terlihat (\" + data.age_sec + \" dtk lalu)</span>\";";
  html += "      div.innerHTML = `${statusTxt}<br>Lat: ${data.lat}<br>Lng: ${data.lng}<br>Satelit Terkunci: ${data.sat}`;";
  html += "      link.href = `https://maps.google.com/?q=${data.lat},${data.lng}`;";
  html += "      link.style.display = 'block';";
  html += "    } else {";
  html += "      div.innerHTML = 'Mencari sinyal satelit GPS (Belum ada data lokasi)...';";
  html += "      link.style.display = 'none';";
  html += "    }";
  html += "  });";
  html += "}";
  html += "setInterval(updateGPS, 2000);";

  html += "window.oncontextmenu = function(e) { e.preventDefault(); return false; };";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

void handleAction() {
  if (server.hasArg("dir")) {
    String dir = server.arg("dir");
    if (dir == "up") executeConfig(cfgUp);
    else if (dir == "down") executeConfig(cfgDown);
    else if (dir == "left") executeConfig(cfgLeft);
    else if (dir == "right") executeConfig(cfgRight);
    else stopAllMotors();
  }
  server.send(200, "text/plain", "OK");
}

void handleSetConfig() {
  if (server.hasArg("btn")) {
    String btn = server.arg("btn");
    ButtonConfig target;
    target.speedM1 = server.arg("s1").toInt();
    target.dirM1   = server.arg("d1");
    target.speedM2 = server.arg("s2").toInt();
    target.dirM2   = server.arg("d2");

    if (btn == "up") cfgUp = target;
    else if (btn == "down") cfgDown = target;
    else if (btn == "left") cfgLeft = target;
    else if (btn == "right") cfgRight = target;
  }
  server.send(200, "text/plain", "Saved");
}

void handleGPSData() {
  String json = "{";

  if (hasLastLocation) {
    bool isLive = gps.location.isValid() && (millis() - lastFixTime < 4000);
    unsigned long ageSeconds = (millis() - lastFixTime) / 1000;

    json += "\"has_data\":true,";
    json += "\"live\":" + String(isLive ? "true" : "false") + ",";
    json += "\"lat\":" + String(lastLat, 6) + ",";
    json += "\"lng\":" + String(lastLng, 6) + ",";
    json += "\"sat\":" + String(gps.satellites.value()) + ",";
    json += "\"age_sec\":" + String(ageSeconds);
  } else {
    json += "\"has_data\":false";
  }

  json += "}";
  server.send(200, "application/json", json);
}

void processGPS() {
  int count = 0;
  while (serialGPS.available() > 0 && count < 32) {
    gps.encode(serialGPS.read());
    count++;
  }

  if (gps.location.isValid() && gps.location.isUpdated()) {
    lastLat = gps.location.lat();
    lastLng = gps.location.lng();
    hasLastLocation = true;
    lastFixTime = millis();
  }
}

void setup() {
  Serial.begin(115200);

  // Serial GPS
  serialGPS.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  // Setup PWM
  ledcSetup(CH_M1_R, FREKUENSI_PWM, RESOLUSI_PWM);
  ledcSetup(CH_M1_L, FREKUENSI_PWM, RESOLUSI_PWM);
  ledcSetup(CH_M2_R, FREKUENSI_PWM, RESOLUSI_PWM);
  ledcSetup(CH_M2_L, FREKUENSI_PWM, RESOLUSI_PWM);

  ledcAttachPin(M1_RPWM, CH_M1_R);
  ledcAttachPin(M1_LPWM, CH_M1_L);
  ledcAttachPin(M2_RPWM, CH_M2_R);
  ledcAttachPin(M2_LPWM, CH_M2_L);

  stopAllMotors();

  // Mode STA
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  Serial.println("\nMenghubungkan ke Wi-Fi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n==========================================");
  Serial.println("TERHUBUNG KE WI-FI!");
  Serial.print("Alamat IP ESP32 Anda: ");
  Serial.println(WiFi.localIP());
  Serial.println("==========================================");

  server.on("/", handleRoot);
  server.on("/action", handleAction);
  server.on("/setconfig", handleSetConfig);
  server.on("/gpsdata", handleGPSData);

  server.begin();
}

void loop() {
  server.handleClient();
  processGPS();

  if (millis() - lastClientAction > ACTION_TIMEOUT) {
    targetM1 = 0;
    targetM2 = 0;
  }

  smoothMotor();
}
