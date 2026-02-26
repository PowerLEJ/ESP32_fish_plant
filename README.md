# ESP32_fish_plant  

## 연결 정보  

### DS3231 RTC 모듈(3.3V)  
RTC SDA: GPIO21  
RTC SCL: GPIO22  

### 온습도 센서(3.3V)  
DHT11: GPIO13  

### 수온 센서(3.3V)  
DS18B20: GPIO27  
DATA – 3.3V 사이에 4.7kΩ 저항 1개  

### 릴레이(5V)  
히터 릴레이: GPIO14  
팬 릴레이: GPIO25  
LED 릴레이: GPIO26  
펌프 릴레이: GPIO33  

## 논리  

### DS3231 RTC 모듈  
시간 정보 기억  

### 온습도 센서  
단순 조회  

### 수온 센서  
히터, 팬 온도 제어  

### 릴레이(5V)  
히터 릴레이: 22도 이하 시 ON  
팬 릴레이: 26도 이상 시 ON  
LED 릴레이: 05:30 ~ 22:30 ON  
펌프 릴레이: 5분 ON & 15분 OFF  
