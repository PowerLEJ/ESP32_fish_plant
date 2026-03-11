#include <WiFi.h>
#include <WebServer.h>

#include <Wire.h>
#include <RTClib.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------- WiFi ----------------
const char* ap_ssid="ESP32-FARM";
const char* ap_pass="12345678";

WebServer server(80);

// ---------------- 릴레이 ----------------
#define RELAY_ON HIGH
#define RELAY_OFF LOW

// ---------------- 핀 ----------------
#define DHTPIN 13
#define DHTTYPE DHT11
#define ONE_WIRE_BUS 27

#define RELAY_HEATER 14
#define RELAY_FAN 25
#define RELAY_LED 26
#define RELAY_PUMP 33

// ---------------- 객체 ----------------
RTC_DS3231 rtc;
DateTime rtcNow;

DHT dht(DHTPIN,DHTTYPE);

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature waterSensor(&oneWire);

// ---------------- 수온 기준 ----------------
const float HEATER_ON=22.0;
const float HEATER_OFF=22.5;

const float FAN_ON=26.0;
const float FAN_OFF=25.5;

// ---------------- 상태 ----------------
bool heaterState=false;
bool fanState=false;
bool ledState=false;
bool pumpState=false;

// ---------------- 펌프 ----------------
unsigned long pumpTimer=0;

const unsigned long PUMP_ON_TIME=5UL*60UL*1000UL;
const unsigned long PUMP_OFF_TIME=15UL*60UL*1000UL;

// ---------------- 로그 타이머 ----------------
unsigned long lastSensorLog=0;
unsigned long lastStatusLog=0;

// ---------------- 최근 센서값 ----------------
float lastAirTemp=NAN;
float lastHum=NAN;
float lastWaterTemp=DEVICE_DISCONNECTED_C;

// ---------------- 로그 버퍼 ----------------
char logBuffer[12000];
int logIndex=0;

// ---------------- 시간 문자열 ----------------
void getNow(char *buf)
{
 snprintf(buf,32,"%04d-%02d-%02d %02d:%02d:%02d",
 rtcNow.year(),rtcNow.month(),rtcNow.day(),
 rtcNow.hour(),rtcNow.minute(),rtcNow.second());
}

// ---------------- 로그 ----------------
void appendLog(const char* text)
{
 int len=strlen(text);

 if(logIndex+len+1>=12000)
 {
  memmove(logBuffer,logBuffer+2000,10000);
  logIndex=10000;
 }

 memcpy(&logBuffer[logIndex],text,len);
 logIndex+=len;

 logBuffer[logIndex++]='\n';
 logBuffer[logIndex]=0;

 Serial.println(text);
}

// ---------------- 릴레이 로그 ----------------
void logRelay(const char* name,bool state)
{
 char t[32];
 char line[80];

 getNow(t);

 snprintf(line,80,"[%s] [%s] %s",t,name,state?"ON":"OFF");

 appendLog(line);
}

// ---------------- 펌프 ----------------
unsigned long getPumpRemainMs()
{
 unsigned long target=pumpState?PUMP_ON_TIME:PUMP_OFF_TIME;
 unsigned long elapsed=millis()-pumpTimer;

 if(elapsed>=target) return 0;

 return target-elapsed;
}

void handlePump()
{
 unsigned long now=millis();
 unsigned long target;

 if(pumpState)
 {
  target=PUMP_ON_TIME;

  if(now-pumpTimer>=target)
  {
   pumpState=false;
   pumpTimer=now;

   digitalWrite(RELAY_PUMP,RELAY_OFF);
   appendLog("[PUMP] OFF");
  }
 }
 else
 {
  target=PUMP_OFF_TIME;

  if(now-pumpTimer>=target)
  {
   pumpState=true;
   pumpTimer=now;

   digitalWrite(RELAY_PUMP,RELAY_ON);
   appendLog("[PUMP] ON");
  }
 }
}

// ---------------- LED ----------------
void handleLED()
{
 int cur=rtcNow.hour()*60+rtcNow.minute();

 int on=5*60+30;
 int off=22*60+30;

 bool newState=(cur>=on && cur<off);

 if(newState!=ledState)
 {
  ledState=newState;
  digitalWrite(RELAY_LED,ledState?RELAY_ON:RELAY_OFF);
  logRelay("LED",ledState);
 }
}

// ---------------- 수온 ----------------
void handleWaterControl(float w)
{
 bool newHeater=heaterState;
 bool newFan=fanState;

 if(!heaterState && w<=HEATER_ON)
 {
  newHeater=true;
  newFan=false;
 }
 else if(heaterState && w>=HEATER_OFF)
 newHeater=false;

 if(!fanState && w>=FAN_ON)
 {
  newFan=true;
  newHeater=false;
 }
 else if(fanState && w<=FAN_OFF)
 newFan=false;

 if(newHeater!=heaterState)
 {
  heaterState=newHeater;
  digitalWrite(RELAY_HEATER,heaterState?RELAY_ON:RELAY_OFF);
  logRelay("HEATER",heaterState);
 }

 if(newFan!=fanState)
 {
  fanState=newFan;
  digitalWrite(RELAY_FAN,fanState?RELAY_ON:RELAY_OFF);
  logRelay("FAN",fanState);
 }
}

// ---------------- 센서 ----------------
void handleSensorLog()
{
 if(millis()-lastSensorLog<5000) return;

 lastSensorLog=millis();

 float h=dht.readHumidity();
 float t=dht.readTemperature();

 waterSensor.requestTemperatures();
 float w=waterSensor.getTempCByIndex(0);

 lastAirTemp=t;
 lastHum=h;
 lastWaterTemp=w;

 char timebuf[32];
 char line[80];

 getNow(timebuf);

 snprintf(line,80,"[%s] SENSOR",timebuf);
 appendLog(line);

 snprintf(line,80,"[DHT11] Temp=%.1fC Hum=%.1f%%",t,h);
 appendLog(line);

 snprintf(line,80,"[DS18B20] Water=%.2fC",w);
 appendLog(line);

 if(w==DEVICE_DISCONNECTED_C || w<-40 || w>80)
 {
  digitalWrite(RELAY_HEATER,RELAY_OFF);
  digitalWrite(RELAY_FAN,RELAY_OFF);
 
  heaterState=false;
  fanState=false;
 
  appendLog("[ERROR] WATER SENSOR FAIL");
 
  return;
 }
 
 handleWaterControl(w);
}

// ---------------- 상태 ----------------
void handleStatusLine()
{
 if(millis()-lastStatusLog<5000) return;

 lastStatusLog=millis();

 unsigned long r=getPumpRemainMs();
 unsigned long sec=r/1000;
 unsigned long min=sec/60;
 sec%=60;

 char t[32];
 char line[200];

 getNow(t);

 snprintf(line,200,
 "[%s] T=%.1fC H=%.1f%% W=%.2fC PUMP_REM=%02lu:%02lu HEATER=%s FAN=%s LED=%s PUMP=%s",
 t,lastAirTemp,lastHum,lastWaterTemp,
 min,sec,
 heaterState?"ON":"OFF",
 fanState?"ON":"OFF",
 ledState?"ON":"OFF",
 pumpState?"ON":"OFF");

 appendLog(line);
}

// ---------------- API ----------------
void handleLogs()
{
 server.send(200,"text/plain",logBuffer);
}

// ----------- RTC TIME API -----------
void handleTime()
{
 char buf[32];
 getNow(buf);
 server.send(200,"text/plain",buf);
}

// ---------------- HTML ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">

<title>ESP32 FARM MONITOR</title>

<style>

body{margin:0;background:#0f172a;color:#e2e8f0;font-family:system-ui;}

header{
background:#020617;
padding:14px 20px;
font-size:20px;
font-weight:600;
}

#clock{
float:right;
font-size:15px;
color:#94a3b8;
}

.container{padding:15px;}

.grid{
display:grid;
grid-template-columns:repeat(auto-fit,minmax(150px,1fr));
gap:12px;
margin-bottom:15px;
}

.card{
background:#020617;
border-radius:12px;
padding:12px;
}

.card h3{
margin:0;
font-size:12px;
color:#94a3b8;
}

.value{
font-size:22px;
font-weight:700;
margin-top:6px;
}

.on{color:#22c55e;}
.off{color:#ef4444;}

.log{
background:#020617;
border-radius:12px;
padding:12px;
height:260px;
overflow:auto;
font-family:monospace;
font-size:12px;
white-space:pre;
}

</style>
</head>

<body>

<header>
ESP32 Farm Monitor
<span id="clock"></span>
</header>

<div class="container">

<div class="grid">

<div class="card"><h3>공기 온도</h3><div class="value" id="airTemp">--</div></div>
<div class="card"><h3>습도</h3><div class="value" id="hum">--</div></div>
<div class="card"><h3>수온</h3><div class="value" id="water">--</div></div>
<div class="card"><h3>펌프 남은시간</h3><div class="value" id="pumpRemain">--</div></div>

<div class="card"><h3>히터</h3><div class="value" id="heater">--</div></div>
<div class="card"><h3>팬</h3><div class="value" id="fan">--</div></div>
<div class="card"><h3>LED</h3><div class="value" id="led">--</div></div>
<div class="card"><h3>펌프 릴레이</h3><div class="value" id="pumpRelay">--</div></div>

</div>

<div class="card">
<h3>실시간 로그</h3>
<div class="log" id="log"></div>
</div>

</div>

<script>

async function load(){

 const txt = await fetch('/api/logs').then(r=>r.text());

 const logEl = document.getElementById("log");

 logEl.textContent = txt;
 logEl.scrollTop = logEl.scrollHeight;

 const lines = txt.trim().split("\n");

 if(lines.length==0) return;

 let statusLine="";

 for(let i=lines.length-1;i>=0;i--)
 {
  if(lines[i].includes("PUMP_REM="))
  {
   statusLine = lines[i];
   break;
  }
 }

 if(statusLine==="") return;

 const t=statusLine.match(/T=([0-9.]+)/);
 const h=statusLine.match(/H=([0-9.]+)/);
 const w=statusLine.match(/W=([0-9.]+)/);
 const p=statusLine.match(/PUMP_REM=([0-9:]+)/);

 const heater=statusLine.match(/HEATER=(ON|OFF)/);
 const fan=statusLine.match(/FAN=(ON|OFF)/);
 const led=statusLine.match(/LED=(ON|OFF)/);
 const pump=statusLine.match(/PUMP=(ON|OFF)/);

 if(t) airTemp.textContent=t[1]+" °C";
 if(h) hum.textContent=h[1]+" %";
 if(w) water.textContent=w[1]+" °C";
 if(p) pumpRemain.textContent=p[1];

 setState("heater",heater);
 setState("fan",fan);
 setState("led",led);
 setState("pumpRelay",pump);
}

function setState(id,m){
 if(!m) return;
 const el=document.getElementById(id);
 el.textContent=m[1];
 el.className="value "+(m[1]=="ON"?"on":"off");
}

async function updateClock(){
 const t=await fetch('/api/time').then(r=>r.text());
 document.getElementById("clock").textContent=t;
}

setInterval(load,2000);
setInterval(updateClock,1000);

load();
updateClock();

</script>

</body>
</html>
)rawliteral";

// ---------------- SETUP ----------------
void setup()
{
 Serial.begin(115200);

 pinMode(RELAY_HEATER,OUTPUT);
 pinMode(RELAY_FAN,OUTPUT);
 pinMode(RELAY_LED,OUTPUT);
 pinMode(RELAY_PUMP,OUTPUT);

 digitalWrite(RELAY_HEATER,RELAY_OFF);
 digitalWrite(RELAY_FAN,RELAY_OFF);
 digitalWrite(RELAY_LED,RELAY_OFF);
 digitalWrite(RELAY_PUMP,RELAY_OFF);

 Wire.begin(21,22);

 rtc.begin();
 dht.begin();
 waterSensor.begin();

 WiFi.mode(WIFI_AP);
 WiFi.softAP(ap_ssid,ap_pass);

 server.on("/",[](){
  server.send_P(200,"text/html",INDEX_HTML);
 });

 server.on("/api/logs",handleLogs);
 server.on("/api/time",handleTime);

 server.begin();

 appendLog("System Start");

 pumpTimer=millis();
}

// ---------------- LOOP ----------------
void loop()
{
 server.handleClient();

 rtcNow=rtc.now();

 handlePump();
 handleLED();
 handleSensorLog();
 handleStatusLine();
}
