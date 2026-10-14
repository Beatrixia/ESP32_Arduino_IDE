// ESP32 Wi-Fi setup over a web page (AP mode)
// Supports: open network, WPA2-Personal (password), WPA2-Enterprise (PEAP)
// Requires: Arduino-ESP32 core 3.x
//
// Flow:
//    1. On boot, load saved settings from flash (NVS) and try to connect.
//    2. If there are no settings, the connection fails, or SETUP_PIN is held
//       to GND during boot, start AP "ESP32-Setup" and serve a config page.
//    3. Connect a phone/PC to the AP, open http://192.168.4.1, fill the form.
//    4. Settings are saved and the board reboots and connects.
//
// NOTE: 
//    1) Jump GPIO13 to GND at boot to force setup
//    2) do not use GPIO0 as the setup pin. Holding it low during reset
//    puts the chip into download mode.
//    3) websocket is another way to change wi-fi setup.
//    4) use uart to get info about websocket, ip ... when esp32 starting boot (reboot to get info)

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static const int      Baud = 115200;            // Baud-rate for uart communicate
static const char*    AP_SSID = "ESP32-Setup";
static const char*    AP_PASS = "setup1234";    // min 8 chars, change if you like
static const int      SETUP_PIN = 13;          // jumper this pin to GND at boot to force setup
static const uint32_t CONNECT_TIMEOUT_MS = 20000;

enum NetMode { MODE_OPEN = 0, MODE_PSK = 1, MODE_ENTERPRISE = 2 };

struct Config {
  String ssid;
  int    mode = MODE_OPEN;
  String identity;
  String username;
  String password;
};

Preferences prefs;
WebServer   server(80);
Config      cfg;
bool        setupMode = false;

// ---------- storage ----------

bool loadConfig() {
  prefs.begin("wifi", true);
  cfg.ssid     = prefs.getString("ssid", "");
  cfg.mode     = prefs.getInt("mode", MODE_OPEN);
  cfg.identity = prefs.getString("identity", "");
  cfg.username = prefs.getString("username", "");
  cfg.password = prefs.getString("password", "");
  prefs.end();
  return cfg.ssid.length() > 0;
}

void saveConfig() {
  prefs.begin("wifi", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putInt("mode", cfg.mode);
  prefs.putString("identity", cfg.identity);
  prefs.putString("username", cfg.username);
  prefs.putString("password", cfg.password);
  prefs.end();
}

// ---------- web page ----------

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 10);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Wi-Fi Setup</title>
<style>
  body { font-family: sans-serif; max-width: 420px; margin: 20px auto; padding: 0 12px; }
  label { display: block; margin-top: 12px; font-weight: bold; }
  input, select { width: 100%; padding: 8px; box-sizing: border-box; font-size: 16px; margin-top: 4px; }
  button { margin-top: 18px; padding: 10px; width: 100%; font-size: 16px; }
  small { color: #666; }
  .mac-box {
    background: #eef2f5;
    border: 1px solid #cccccc;
    padding: 10px;
    border-radius: 6px;
    margin-bottom: 15px;
  }
  .mac-box input {
    font-family: monospace;
    font-weight: bold;
    text-align: center;
    background: #ffffff;
    color: #333333;
  }
</style>
</head>
<body>
<h2>ESP32 Wi-Fi Setup</h2>

<div class="mac-box">
  <label style="margin-top:0;">Device MAC Address (STA):</label>
  <input type="text" value="%MAC%" readonly onclick="this.select();">
  <small style="display:block; margin-top:4px;">(ใช้สำหรับลงทะเบียน Wi-Fi มหาวิทยาลัย / องค์กร)</small>
</div>

<form method="POST" action="/save">
  <label>Network type</label>
  <select name="mode" id="mode" onchange="upd()">
    <option value="0" %SEL0%>Open (no password)</option>
    <option value="1" %SEL1%>WPA2-Personal (password only)</option>
    <option value="2" %SEL2%>WPA2-Enterprise (username + password)</option>
  </select>

  <label>Select Wi-Fi Network</label>
  <select id="ssid_select" onchange="onSsidSelectChange()">
    %WIFI_OPTIONS%
    <option value="__CUSTOM__">-- Enter Manual SSID --</option>
  </select>

  <input type="hidden" name="ssid" id="ssid_hidden" value="%SSID%">

  <div id="custom_ssid_div" style="display:none;">
    <label>Manual SSID</label>
    <input id="custom_ssid" placeholder="Enter Wi-Fi SSID" oninput="updHiddenSsid()">
  </div>

  <div id="ent">
    <label>Username</label>
    <input name="username" value="%USER%" autocapitalize="none">
    <label>Identity <small>(optional, leave blank to use username)</small></label>
    <input name="identity" value="%IDENT%" autocapitalize="none">
  </div>

  <div id="pw">
    <label>Password <small>(leave blank to keep the saved one)</small></label>
    <input name="password" type="password">
  </div>

  <button type="submit">Save and reboot</button>
</form>
<script>
function upd() {
  var m = document.getElementById('mode').value;
  document.getElementById('ent').style.display = (m == '2') ? 'block' : 'none';
  document.getElementById('pw').style.display  = (m == '0') ? 'none'  : 'block';
}

function onSsidSelectChange() {
  var sel = document.getElementById('ssid_select').value;
  var customDiv = document.getElementById('custom_ssid_div');
  var hiddenSsid = document.getElementById('ssid_hidden');

  if (sel === '__CUSTOM__') {
    customDiv.style.display = 'block';
    hiddenSsid.value = document.getElementById('custom_ssid').value;
  } else {
    customDiv.style.display = 'none';
    hiddenSsid.value = sel;
  }
}

function updHiddenSsid() {
  document.getElementById('ssid_hidden').value = document.getElementById('custom_ssid').value;
}

upd();
onSsidSelectChange();
</script>
</body>
</html>
)rawliteral";

String getScanResultsHTML() {
  String options = "";
  options.reserve(1024);
  
  int n = WiFi.scanNetworks(false, false);
  if (n <= 0) {
    options += "<option value=\"\">No networks found</option>";
  } else {
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      
      int rssi = WiFi.RSSI(i);
      options += "<option value=\"" + htmlEscape(ssid) + "\"";
      if (ssid == cfg.ssid) {
        options += " selected";
      }
      options += ">" + htmlEscape(ssid) + " (" + String(rssi) + " dBm)</option>";
    }
  }
  
  WiFi.scanDelete();
  return options;
}

void handleRoot() {
  String html;
  html.reserve(sizeof(PAGE) + 1500);
  html = FPSTR(PAGE);

  html.replace("%MAC%", WiFi.macAddress());
  html.replace("%WIFI_OPTIONS%", getScanResultsHTML());
  html.replace("%SSID%", htmlEscape(cfg.ssid));
  html.replace("%USER%", htmlEscape(cfg.username));
  html.replace("%IDENT%", htmlEscape(cfg.identity));
  html.replace("%SEL0%", cfg.mode == MODE_OPEN ? "selected" : "");
  html.replace("%SEL1%", cfg.mode == MODE_PSK ? "selected" : "");
  html.replace("%SEL2%", cfg.mode == MODE_ENTERPRISE ? "selected" : "");
  
  server.send(200, "text/html", html);
}

void handleSave() {
  String newSsid = server.arg("ssid");
  int newMode = server.arg("mode").toInt();
  if (newSsid.length() == 0 || newMode < 0 || newMode > 2) {
    server.send(400, "text/plain", "Invalid input");
    return;
  }

  String newPass = server.arg("password");
  if (newPass.length() == 0 && newSsid == cfg.ssid) {
    newPass = cfg.password;  // keep the old password if left blank
  }

  cfg.ssid     = newSsid;
  cfg.mode     = newMode;
  cfg.username = server.arg("username");
  cfg.identity = server.arg("identity");
  cfg.password = (newMode == MODE_OPEN) ? "" : newPass;
  saveConfig();

  server.send(200, "text/html",
              "<html><body style='font-family:sans-serif'>"
              "<h3>Saved. Rebooting...</h3></body></html>");
  delay(1000);
  ESP.restart();
}

void startSetupMode() {
  setupMode = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  server.begin();
  Serial.println("Setup mode started");
  Serial.printf("Join Wi-Fi '%s' (password '%s'), then open http://%s\n",
                AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
}

// ---------- API Services ----------

const char* API_BASE_URL = "http://apikkuvolt.raphaelvigilance.site/api/v1";

unsigned long lastApiCheck = 0;
const unsigned long API_INTERVAL = 5000;

float gridVoltage = 0.0;
float powerstationVoltage = 0.0;
int batterySOH = 0;
String batteryStatus = "Unknown";
float batteryVoltage = 0.0;
float batteryCurrent = 0.0;

void fetchVoltage() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(API_BASE_URL) + "/Voltage";
  
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      gridVoltage = doc["Voltage"]["Grid"];
      powerstationVoltage = doc["Voltage"]["Powerstation"];
      Serial.printf("[API] Grid: %.1f V | Powerstation: %.1f V\n", gridVoltage, powerstationVoltage);
    }
  } else {
    Serial.printf("[API Error] /Voltage HTTP Code: %d\n", httpCode);
  }
  
  http.end();
}

void fetchBatteryStatus() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(API_BASE_URL) + "/battery";
  
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      batterySOH = doc["Device"]["SOH"];
      batteryStatus = doc["Device"]["Status"].as<String>();
      batteryVoltage = doc["Device"]["Voltage"];
      batteryCurrent = doc["Device"]["Current"];

      Serial.printf("[API] Battery Status: %s | SOH: %d%% | V: %.1fV | I: %.1fA\n", 
                    batteryStatus.c_str(), batterySOH, batteryVoltage, batteryCurrent);
    }
  } else {
    Serial.printf("[API Error] /battery HTTP Code: %d\n", httpCode);
  }

  http.end();
}

void handleApiTasks() {
  if (WiFi.status() == WL_CONNECTED && !setupMode) {
    if (millis() - lastApiCheck >= API_INTERVAL) {
      lastApiCheck = millis();

      fetchVoltage();
      fetchBatteryStatus();

      if (batteryStatus == "Abnormal") {
        Serial.println("[WARNING] Battery Status is Abnormal!");
      }
    }
  }
}

// ---------- station connection ----------

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  Serial.printf("Connecting to '%s' (mode %d)\n", cfg.ssid.c_str(), cfg.mode);

  if (cfg.mode == MODE_ENTERPRISE) {
    String ident = cfg.identity.length() ? cfg.identity : cfg.username;
    WiFi.begin(cfg.ssid.c_str(), WPA2_AUTH_PEAP,
               ident.c_str(), cfg.username.c_str(), cfg.password.c_str());
  } else if (cfg.mode == MODE_PSK) {
    WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
  } else {
    WiFi.begin(cfg.ssid.c_str());
  }

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void checkInternet() {
  HTTPClient http;
  http.begin("http://connectivitycheck.gstatic.com/generate_204");
  int code = http.GET();
  Serial.printf("Internet check HTTP code: %d\n", code);
  if (code == 204) Serial.println("Internet OK");
  else if (code > 0) Serial.println("Connected to Wi-Fi but no direct internet (captive portal or blocked?)");
  else Serial.println("Request failed (no internet or DNS problem)");
  http.end();
}

// ---------- main ----------

void setup() {
  Serial.begin(Baud);
  delay(500);
  pinMode(SETUP_PIN, INPUT_PULLUP);

  bool force = (digitalRead(SETUP_PIN) == LOW);
  bool haveConfig = loadConfig();

  if (!force && haveConfig && connectWiFi()) {
    Serial.print("Connected, IP: ");
    Serial.println(WiFi.localIP());

    // แสดง MAC Address ที่ถูกต้องหลังจากต่อ Wi-Fi สำเร็จแล้ว
    Serial.print("Wi-Fi MAC: ");
    Serial.println(WiFi.macAddress());

    WiFi.setAutoReconnect(true);
    checkInternet();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleRoot);
    server.begin();
    Serial.printf("Web server running at http://%s\n", WiFi.localIP().toString().c_str());
  } else {
    if (force) Serial.println("Setup pin held: entering setup mode");
    else if (!haveConfig) Serial.println("No saved config: entering setup mode");
    else Serial.println("Connection failed: entering setup mode");
    startSetupMode();
  }
}

void loop() {
  server.handleClient();
  handleApiTasks();
}
