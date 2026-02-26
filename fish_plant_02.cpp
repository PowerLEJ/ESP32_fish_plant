#include <Wire.h>
#include <RTClib.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------- 핀 ----------------
#define DHTPIN 13
#define DHTTYPE DHT11

#define ONE_WIRE_BUS 27   // DS18B20

#define RELAY_HEATER 14
#define RELAY_FAN    25
#define RELAY_LED    26
#define RELAY_PUMP   33

// ---------------- 객체 ----------------
RTC_DS3231 rtc;
DHT dht(DHTPIN, DHTTYPE);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature waterSensor(&oneWire);

// ---------------- 수온 기준 (히스테리시스) ----------------
const float HEATER_ON  = 22.0;
const float HEATER_OFF = 22.5;

const float FAN_ON  = 26.0;
const float FAN_OFF = 25.5;

// ---------------- 릴레이 상태 ----------------
bool heaterState = false;
bool fanState    = false;
bool ledState    = false;

// ---------------- 펌프 ----------------
bool pumpState = false;   // false=OFF, true=ON
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


// ---------------- 시간 출력 ----------------
void printNow()
{
  DateTime now = rtc.now();

  Serial.print(now.year()); Serial.print("-");
  if(now.month()<10) Serial.print("0");
  Serial.print(now.month()); Serial.print("-");
  if(now.day()<10) Serial.print("0");
  Serial.print(now.day()); Serial.print(" ");

  if(now.hour()<10) Serial.print("0");
  Serial.print(now.hour()); Serial.print(":");
  if(now.minute()<10) Serial.print("0");
  Serial.print(now.minute()); Serial.print(":");
  if(now.second()<10) Serial.print("0");
  Serial.print(now.second());
}

// ---------------- 릴레이 전환 로그 ----------------
void logRelay(const char* name, bool on)
{
  Serial.print("[");
  printNow();
  Serial.print("] [");
  Serial.print(name);
  Serial.print("] ");
  Serial.println(on ? "ON" : "OFF");
}

// ---------------- 펌프 남은시간 계산 ----------------
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

  Serial.print("[");
  printNow();
  Serial.print("] [PUMP] remain ");

  if(min < 10) Serial.print("0");
  Serial.print(min);
  Serial.print(":");
  if(sec < 10) Serial.print("0");
  Serial.println(sec);
}

// ---------------- 펌프 제어 ----------------
void handlePump()
{
  unsigned long nowMs = millis();
  unsigned long targetTime;

  if(pumpState) // ON
  {
    targetTime = PUMP_ON_TIME;

    if(nowMs - pumpTimer >= targetTime)
    {
      pumpState = false;
      pumpTimer = nowMs;

      digitalWrite(RELAY_PUMP, HIGH);

      Serial.print("[");
      printNow();
      Serial.println("] [PUMP] OFF (15 min rest)");

      lastPumpRemainLog = nowMs;
      return;
    }
  }
  else // OFF
  {
    targetTime = PUMP_OFF_TIME;

    if(nowMs - pumpTimer >= targetTime)
    {
      pumpState = true;
      pumpTimer = nowMs;

      digitalWrite(RELAY_PUMP, LOW);

      Serial.print("[");
      printNow();
      Serial.println("] [PUMP] ON (5 min run)");

      lastPumpRemainLog = nowMs;
      return;
    }
  }

  if(nowMs - lastPumpRemainLog >= PUMP_REMAIN_LOG_INTERVAL)
  {
    unsigned long elapsed = nowMs - pumpTimer;

    if(elapsed < targetTime)
    {
      unsigned long remain = targetTime - elapsed;
      logPumpRemain(remain);
    }

    lastPumpRemainLog = nowMs;
  }
}

// ---------------- LED 시간제어 ----------------
void handleLED()
{
  DateTime now = rtc.now();

  int curMin = now.hour() * 60 + now.minute();
  int onMin  = 5 * 60 + 30;   // 05:30
  int offMin = 22 * 60 + 30;  // 22:30

  bool newState = (curMin >= onMin && curMin < offMin);

  if(newState != ledState)
  {
    ledState = newState;
    digitalWrite(RELAY_LED, ledState ? LOW : HIGH);
    logRelay("LED", ledState);
  }
}

// ---------------- 수온 기반 히터/팬 (fail-safe + 히스테리시스) ----------------
void handleWaterControl(float waterTemp)
{
  bool newHeater = heaterState;
  bool newFan    = fanState;

  // 센서 이상 처리
  if(waterTemp == DEVICE_DISCONNECTED_C || waterTemp < 0 || waterTemp > 50)
  {
    if(heaterState || fanState)
    {
      heaterState = false;
      fanState = false;

      digitalWrite(RELAY_HEATER, HIGH);
      digitalWrite(RELAY_FAN, HIGH);

      Serial.println("[WATER] abnormal -> HEATER/FAN OFF");
    }
    return;
  }

  // 히터
  if(!heaterState && waterTemp <= HEATER_ON)
  {
    newHeater = true;
    newFan    = false;
  }
  else if(heaterState && waterTemp >= HEATER_OFF)
  {
    newHeater = false;
  }

  // 팬
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
    digitalWrite(RELAY_HEATER, heaterState ? LOW : HIGH);
    logRelay("HEATER", heaterState);
  }

  if(newFan != fanState)
  {
    fanState = newFan;
    digitalWrite(RELAY_FAN, fanState ? LOW : HIGH);
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

  Serial.println("----------------------------------");
  Serial.print("[");
  printNow();
  Serial.println("]");

  if(isnan(h) || isnan(t))
    Serial.println("[DHT11] read fail");
  else
  {
    Serial.print("[DHT11] Temp=");
    Serial.print(t,1);
    Serial.print("C Hum=");
    Serial.print(h,1);
    Serial.println("%");
  }

  if(waterTemp == DEVICE_DISCONNECTED_C)
    Serial.println("[DS18B20] read fail");
  else
  {
    Serial.print("[DS18B20] Water=");
    Serial.print(waterTemp,2);
    Serial.println("C");
  }

  handleWaterControl(waterTemp);
}

// ---------------- 상태 한줄 로그 ----------------
void handleStatusLine()
{
  unsigned long nowMs = millis();
  if(nowMs - lastStatusLog < STATUS_LOG_INTERVAL) return;
  lastStatusLog = nowMs;

  unsigned long remain = getPumpRemainMs();
  unsigned long sec = remain / 1000;
  unsigned long min = sec / 60;
  sec %= 60;

  Serial.print("[");
  printNow();
  Serial.print("] ");

  Serial.print("T=");
  if(isnan(lastAirTemp)) Serial.print("NA");
  else Serial.print(lastAirTemp,1);
  Serial.print("C ");

  Serial.print("H=");
  if(isnan(lastHum)) Serial.print("NA");
  else Serial.print(lastHum,1);
  Serial.print("% ");

  Serial.print("W=");
  if(lastWaterTemp == DEVICE_DISCONNECTED_C) Serial.print("NA");
  else Serial.print(lastWaterTemp,2);
  Serial.print("C ");

  Serial.print("PUMP_REM=");
  if(min < 10) Serial.print("0");
  Serial.print(min);
  Serial.print(":");
  if(sec < 10) Serial.print("0");
  Serial.print(sec);
  Serial.print(" ");

  Serial.print("HEATER=");
  Serial.print(heaterState ? "ON" : "OFF");
  Serial.print(" ");

  Serial.print("FAN=");
  Serial.print(fanState ? "ON" : "OFF");
  Serial.print(" ");

  Serial.print("LED=");
  Serial.print(ledState ? "ON" : "OFF");
  Serial.print(" ");

  Serial.print("PUMP=");
  Serial.println(pumpState ? "ON" : "OFF");
}

// ---------------- SETUP ----------------
void setup()
{
  Serial.begin(115200);

  pinMode(RELAY_HEATER, OUTPUT);
  pinMode(RELAY_FAN,    OUTPUT);
  pinMode(RELAY_LED,    OUTPUT);
  pinMode(RELAY_PUMP,   OUTPUT);

  // active LOW 릴레이 기준
  digitalWrite(RELAY_HEATER, HIGH);
  digitalWrite(RELAY_FAN,    HIGH);
  digitalWrite(RELAY_LED,    HIGH);
  digitalWrite(RELAY_PUMP,   HIGH);

  heaterState = false;
  fanState    = false;
  ledState    = false;
  pumpState   = false;

  Wire.begin(21,22);

  if(!rtc.begin())
  {
    Serial.println("[RTC] begin FAIL");
  }

  dht.begin();
  waterSensor.begin();

  // 최초 1회만 사용
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  pumpTimer = millis();
  lastPumpRemainLog = millis();

  Serial.println("System start");
}

// ---------------- LOOP ----------------
void loop()
{
  handlePump();
  handleLED();
  handleSensorLog();
  handleStatusLine();
}
