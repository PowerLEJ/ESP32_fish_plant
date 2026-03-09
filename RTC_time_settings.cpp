#include <Wire.h>
#include <RTClib.h>

RTC_DS3231 rtc;

void setup() {
  Serial.begin(115200);
  Wire.begin(21,22);

  rtc.begin();

  rtc.adjust(DateTime(2026,3,8,23,40,0)); // 시간 수동 설정

  Serial.println("RTC SET");
}

void loop() {
  DateTime now = rtc.now();

  Serial.print(now.year());
  Serial.print("-");
  Serial.print(now.month());
  Serial.print("-");
  Serial.print(now.day());
  Serial.print(" ");

  Serial.print(now.hour());
  Serial.print(":");
  Serial.print(now.minute());
  Serial.print(":");
  Serial.println(now.second());

  delay(1000);
}
