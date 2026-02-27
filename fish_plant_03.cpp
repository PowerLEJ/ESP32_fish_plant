#include <WiFi.h>
#include <WebServer.h>

#include <Wire.h>
#include <RTClib.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------- WiFi AP ----------------
const char* ap_ssid = "ESP32-FARM";
const char* ap_pass = "12345678";

WebServer server(80);

// ---------------- 릴레이 논리 (HIGH = ON 타입) ----------------
#define RELAY_ON  HIGH
#define RELAY_OFF LOW

// ---------------- 핀 ----------------
#define DHTPIN 13
#define DHTTYPE DHT11

#define ONE_WIRE_BUS 27

#define RELAY_HEATER 14
#define RELAY_FAN    25
#define RELAY_LED    26
#define RELAY_PUMP   33

// ---------------- 객체 ----------------
RTC_DS3231 rtc;
DHT dht(DHTPIN, DHTTYPE);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature waterSensor(&oneWire);

// ---------------- 수온 기준 ----------------
const float HEATER_ON  = 22.0;
const float HEATER_OFF = 22.5;

const float FAN_ON  = 26.0;
const float FAN_OFF = 25.5;

// ---------------- 릴레이 상태 ----------------
bool heaterState = false;
bool fanState    = false;
bool ledState    = false;

// ---------------- 펌프 ----------------
bool pumpState = false;
unsigned long pumpTimer = 0;

const unsigned long PUMP_ON_TIME  = 5UL  * 60UL * 1000UL;
const unsigned long PUMP_OFF_TIME = 15UL * 60UL * 1000UL;

unsigned long lastPumpRemainLog = 0;
const unsigned long PUMP_REMAIN_LOG_INTERVAL = 60UL * 1000UL;

// ---------------- 센서 로그 ----------------
unsigned long lastSensorLog = 0;
const unsigned long SENSOR_LOG_INTERVAL = 5000;

// ---------------- 상태 한줄 로그 ----------------
unsigned long lastStatusLog = 0;
const unsigned long STATUS_LOG_INTERVAL = 5000;

// ---------------- 최근 센서값 ----------------
float lastAirTemp   = NAN;
float lastHum       = NAN;
float lastWaterTemp = DEVICE_DISCONNECTED_C;

// ---------------- 웹 로그 버퍼 ----------------
String logBuffer;
const size_t LOG_LIMIT = 12000;

// ---------------- 시간 문자열 ----------------
String nowString()
{
  DateTime now = rtc.now();
  char buf[32];
  snprintf(buf, sizeof(buf),
           "%04d-%02d-%02d %02d:%02d:%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());
  return String(buf);
}

// ---------------- 로그 공통 ----------------
void appendLog(const String &s)
{
  logBuffer += s;
  logBuffer += "\n";

  if (logBuffer.length() > LOG_LIMIT)
    logBuffer.remove(0, logBuffer.length() - LOG_LIMIT);

  Serial.println(s);
}

// ---------------- 릴레이 로그 ----------------
void logRelay(const char* name, bool on)
{
  String s = "[" + nowString() + "] [" + name + "] ";
  s += on ? "ON" : "OFF";
  appendLog(s);
}

// ---------------- 펌프 남은시간 ----------------
unsigned long getPumpRemainMs()
{
  unsigned long target = pumpState ? PUMP_ON_TIME : PUMP_OFF_TIME;
  unsigned long elapsed = millis() - pumpTimer;
  if(elapsed >= target) return 0;
  return target - elapsed;
}

// ---------------- 펌프 남은시간 로그 ----------------
void logPumpRemain(unsigned long remainMs)
{
  unsigned long sec = remainMs / 1000;
  unsigned long min = sec / 60;
  sec %= 60;

  char buf[64];
  snprintf(buf,sizeof(buf),
    "[%s] [PUMP] remain %02lu:%02lu",
    nowString().c_str(), min, sec);

  appendLog(buf);
}

// ---------------- 펌프 제어 ----------------
void handlePump()
{
  unsigned long nowMs = millis();
  unsigned long targetTime;

  if(pumpState)   // ON
  {
    targetTime = PUMP_ON_TIME;

    if(nowMs - pumpTimer >= targetTime)
    {
      pumpState = false;
      pumpTimer = nowMs;

      digitalWrite(RELAY_PUMP, RELAY_OFF);

      appendLog("[" + nowString() + "] [PUMP] OFF (15 min rest)");
      lastPumpRemainLog = nowMs;
      return;
    }
  }
  else            // OFF
  {
    targetTime = PUMP_OFF_TIME;

    if(nowMs - pumpTimer >= targetTime)
    {
      pumpState = true;
      pumpTimer = nowMs;

      digitalWrite(RELAY_PUMP, RELAY_ON);

      appendLog("[" + nowString() + "] [PUMP] ON (5 min run)");
      lastPumpRemainLog = nowMs;
      return;
    }
  }

  if(nowMs - lastPumpRemainLog >= PUMP_REMAIN_LOG_INTERVAL)
  {
    unsigned long elapsed = nowMs - pumpTimer;

    if(elapsed < targetTime)
      logPumpRemain(targetTime - elapsed);

    lastPumpRemainLog = nowMs;
  }
}

// ---------------- LED ----------------
void handleLED()
{
  DateTime now = rtc.now();

  int curMin = now.hour() * 60 + now.minute();
  int onMin  = 5 * 60 + 30;
  int offMin = 22 * 60 + 30;

  bool newState = (curMin >= onMin && curMin < offMin);

  if(newState != ledState)
  {
    ledState = newState;
    digitalWrite(RELAY_LED, ledState ? RELAY_ON : RELAY_OFF);
    logRelay("LED", ledState);
  }
}

// ---------------- 히터/팬 ----------------
void handleWaterControl(float waterTemp)
{
  bool newHeater = heaterState;
  bool newFan    = fanState;

  if(waterTemp == DEVICE_DISCONNECTED_C || waterTemp < 0 || waterTemp > 50)
  {
    if(heaterState || fanState)
    {
      heaterState = false;
      fanState = false;

      digitalWrite(RELAY_HEATER, RELAY_OFF);
      digitalWrite(RELAY_FAN,    RELAY_OFF);

      appendLog("[WATER] abnormal -> HEATER/FAN OFF");
    }
    return;
  }

  if(!heaterState && waterTemp <= HEATER_ON)
  {
    newHeater = true;
    newFan    = false;
  }
  else if(heaterState && waterTemp >= HEATER_OFF)
  {
    newHeater = false;
  }

  if(!fanState && waterTemp >= FAN_ON)
  {
    newFan    = true;
    newHeater = false;
  }
  else if(fanState && waterTemp <= FAN_OFF)
  {
    newFan = false;
  }

  if(newHeater != heaterState)
  {
    heaterState = newHeater;
    digitalWrite(RELAY_HEATER, heaterState ? RELAY_ON : RELAY_OFF);
    logRelay("HEATER", heaterState);
  }

  if(newFan != fanState)
  {
    fanState = newFan;
    digitalWrite(RELAY_FAN, fanState ? RELAY_ON : RELAY_OFF);
    logRelay("FAN", fanState);
  }
}

// ---------------- 센서 로그 ----------------
void handleSensorLog()
{
  unsigned long nowMs = millis();
  if(nowMs - lastSensorLog < SENSOR_LOG_INTERVAL) return;
  lastSensorLog = nowMs;

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  waterSensor.requestTemperatures();
  float waterTemp = waterSensor.getTempCByIndex(0);

  lastAirTemp   = t;
  lastHum       = h;
  lastWaterTemp = waterTemp;

  appendLog("----------------------------------");
  appendLog("[" + nowString() + "]");

  if(isnan(h) || isnan(t))
    appendLog("[DHT11] read fail");
  else
  {
    char buf[64];
    snprintf(buf,sizeof(buf),
      "[DHT11] Temp=%.1fC Hum=%.1f%%", t, h);
    appendLog(buf);
  }

  if(waterTemp == DEVICE_DISCONNECTED_C)
    appendLog("[DS18B20] read fail");
  else
  {
    char buf[64];
    snprintf(buf,sizeof(buf),
      "[DS18B20] Water=%.2fC", waterTemp);
    appendLog(buf);
  }

  handleWaterControl(waterTemp);
}

// ---------------- 상태 한줄 ----------------
void handleStatusLine()
{
  unsigned long nowMs = millis();
  if(nowMs - lastStatusLog < STATUS_LOG_INTERVAL) return;
  lastStatusLog = nowMs;

  unsigned long remain = getPumpRemainMs();
  unsigned long sec = remain / 1000;
  unsigned long min = sec / 60;
  sec %= 60;

  String s = "[" + nowString() + "] ";

  s += "T=";
  s += isnan(lastAirTemp) ? "NA" : String(lastAirTemp,1);
  s += "C ";

  s += "H=";
  s += isnan(lastHum) ? "NA" : String(lastHum,1);
  s += "% ";

  s += "W=";
  s += (lastWaterTemp == DEVICE_DISCONNECTED_C) ? "NA" : String(lastWaterTemp,2);
  s += "C ";

  char buf[16];
  snprintf(buf,sizeof(buf),"%02lu:%02lu",min,sec);

  s += "PUMP_REM=";
  s += buf;

  s += " HEATER="; s += heaterState?"ON":"OFF";
  s += " FAN=";    s += fanState?"ON":"OFF";
  s += " LED=";    s += ledState?"ON":"OFF";
  s += " PUMP=";   s += pumpState?"ON":"OFF";

  appendLog(s);
}

// ---------------- 웹 페이지 ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Farm</title>
<style>
body{margin:0;font-family:system-ui;background:#0f172a;color:#e5e7eb}
header{padding:12px 16px;background:#020617;font-size:18px}
#clock{float:right;font-size:15px;color:#94a3b8}
.grid{padding:12px;display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}
.card{background:#020617;border-radius:10px;padding:12px}
.card h3{margin:0 0 6px 0;font-size:12px;color:#94a3b8}
.v{font-size:22px;font-weight:700}
.on{color:#22c55e}.off{color:#ef4444}
.log{margin:12px;background:#020617;border-radius:10px;padding:10px;height:260px;overflow:auto;
font-family:monospace;font-size:12px;white-space:pre}
</style>
</head>
<body>
<header>
ESP32 Farm Monitor
<span id="clock"></span>
</header>

<div class="grid">
<div class="card"><h3>공기온도</h3><div class="v" id="t">--</div></div>
<div class="card"><h3>습도</h3><div class="v" id="h">--</div></div>
<div class="card"><h3>수온</h3><div class="v" id="w">--</div></div>
<div class="card"><h3>펌프 남은시간</h3><div class="v" id="pr">--</div></div>
<div class="card"><h3>히터</h3><div class="v" id="heater">--</div></div>
<div class="card"><h3>팬</h3><div class="v" id="fan">--</div></div>
<div class="card"><h3>LED</h3><div class="v" id="led">--</div></div>
<div class="card"><h3>펌프</h3><div class="v" id="pump">--</div></div>
</div>

<div class="log" id="log"></div>

<script>
async function load(){
 const s=await fetch('/api/status').then(r=>r.json());
 const l=await fetch('/api/logs').then(r=>r.text());

 clock.textContent = s.now;

 t.textContent = s.airTemp==null?"NA":s.airTemp.toFixed(1)+"C";
 h.textContent = s.hum==null?"NA":s.hum.toFixed(1)+"%";
 w.textContent = s.waterTemp==null?"NA":s.waterTemp.toFixed(2)+"C";
 pr.textContent = s.pumpRemain;

 set(heater,s.heater);
 set(fan,s.fan);
 set(led,s.led);
 set(pump,s.pump);

 log.textContent=l;
 log.scrollTop=log.scrollHeight;
}
function set(e,v){
 e.textContent=v?"ON":"OFF";
 e.className="v "+(v?"on":"off");
}
setInterval(load,2000);
load();
</script>
</body>
</html>
)rawliteral";

// ---------------- API ----------------
void handleStatusApi()
{
  unsigned long r = getPumpRemainMs();
  unsigned long s = r/1000;
  char buf[8];
  snprintf(buf,sizeof(buf),"%02lu:%02lu",s/60,s%60);

  String json = "{";
  json += "\"now\":\"" + nowString() + "\",";
  json += "\"airTemp\":" + (isnan(lastAirTemp)?"null":String(lastAirTemp,1)) + ",";
  json += "\"hum\":"     + (isnan(lastHum)?"null":String(lastHum,1)) + ",";
  json += "\"waterTemp\":" + ((lastWaterTemp==DEVICE_DISCONNECTED_C)?"null":String(lastWaterTemp,2)) + ",";
  json += "\"pumpRemain\":\"" + String(buf) + "\",";
  json += "\"heater\":" + String(heaterState?"true":"false") + ",";
  json += "\"fan\":"    + String(fanState?"true":"false") + ",";
  json += "\"led\":"    + String(ledState?"true":"false") + ",";
  json += "\"pump\":"  + String(pumpState?"true":"false");
  json += "}";

  server.send(200,"application/json",json);
}

// ---------------- SETUP ----------------
void setup()
{
  Serial.begin(115200);

  pinMode(RELAY_HEATER, OUTPUT);
  pinMode(RELAY_FAN,    OUTPUT);
  pinMode(RELAY_LED,    OUTPUT);
  pinMode(RELAY_PUMP,   OUTPUT);

  digitalWrite(RELAY_HEATER, RELAY_OFF);
  digitalWrite(RELAY_FAN,    RELAY_OFF);
  digitalWrite(RELAY_LED,    RELAY_OFF);
  digitalWrite(RELAY_PUMP,   RELAY_OFF);

  Wire.begin(21,22);

  rtc.begin();
  dht.begin();
  waterSensor.begin();

  pumpTimer = millis();
  lastPumpRemainLog = millis();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_pass);

  server.on("/", [](){
    server.send_P(200,"text/html",INDEX_HTML);
  });

  server.on("/api/status", handleStatusApi);

  server.on("/api/logs", [](){
    server.send(200,"text/plain",logBuffer);
  });

  server.begin();

  appendLog("System start");
  appendLog("AP IP : " + WiFi.softAPIP().toString());
}

// ---------------- LOOP ----------------
void loop()
{
  server.handleClient();

  handlePump();
  handleLED();
  handleSensorLog();
  handleStatusLine();
}
