#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <DHT.h>
#include <Adafruit_BMP085.h>

// LCD, RTC, DHT, BMP180 setup
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS1307 rtc;
#define DHTPIN 2
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP085 bmp;

// MQ135 sensor pin
#define MQ135_PIN A0
#define BUZZER_PIN 10
// Button pin
#define BUTTON_PIN 3
#define POWER_SOURCE_PIN 4
#define doorPin 5
#define BATTERY_PIN A6

// Variables
unsigned long lastChange = 0;
int displayMode = 0;  // 0: RTC, 1: DHT, 2: BMP, 3: MQ135
bool manualMode = false;
bool lastButtonState = HIGH;
unsigned long lastButtonTime = 0;
const unsigned long debounceDelay = 200;
const unsigned long autoInterval = 4000; // 4 sec interval for auto change
unsigned long lastSerialPrint = 0;       // For Serial Monitor printing
const unsigned long serialInterval = 15000; // Every 5 sec print to Serial
int lastBuzzedHour = -1; // To avoid multiple buzzes in the same hour

void setup() {
  Wire.begin();
  lcd.init();
  lcd.backlight();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(POWER_SOURCE_PIN, INPUT);
  pinMode(doorPin, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.begin(9600);

  rtc.begin();
  // --- SET RTC TIME (Run once, then remove) ---
  //rtc.adjust(DateTime(2025, 11, 17, 6, 28, 00));  
  // Format: (year, month, day, hour, minute, second)

  dht.begin();
  bmp.begin();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Env Monitor");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");
  delay(2000);
  lcd.clear();

  Serial.println("=== Environmental Monitor Started ===");
}
// ---------------- READ BATTERY VOLTAGE ----------------
float readBatteryVoltage() {
  int raw = analogRead(BATTERY_PIN);
  float voltage = raw * (5.0 / 1023.0);
  voltage = voltage * 2.0;   // divider compensation
  return voltage;
}

void loop() {
  unsigned long currentMillis = millis();
  bool buttonState = digitalRead(BUTTON_PIN);

  // Handle button press (manual mode)
  if (buttonState == LOW && lastButtonState == HIGH && (currentMillis - lastButtonTime) > debounceDelay) {
    displayMode++;
    if (displayMode > 3) displayMode = 0;
    manualMode = true;
    lastButtonTime = currentMillis;
    lcd.clear();
  }
  lastButtonState = buttonState;

  // Auto mode after inactivity
  if ((currentMillis - lastButtonTime) > 15000) { // 15s inactivity → auto mode
    manualMode = false;
  }

  // Change screen automatically if in auto mode
  if (!manualMode && (currentMillis - lastChange > autoInterval)) {
    displayMode++;
    if (displayMode > 3) displayMode = 0;
    lastChange = currentMillis;
    lcd.clear();
  }

  // Display data based on mode
  switch (displayMode) {
    case 0: showTime(); break;
    case 1: showDHT(); break;
    case 2: showBMP(); break;
    case 3: showMQ135(); break;
  }

  // Print all data together to Serial Monitor every few seconds
  if (currentMillis - lastSerialPrint > serialInterval) {
    printAllDataToSerial();
    lastSerialPrint = currentMillis;
  }
  DateTime now = rtc.now();

  // 🔔 Buzz exactly once at every new hour
  if (now.minute() == 0 && now.second() == 0) {
    if (now.hour() != lastBuzzedHour) {
      buzzOneHour();
      lastBuzzedHour = now.hour();
    }
  }
}
// ---------------- BUZZER FUNCTION ----------------
void buzzOneHour() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(300);
  digitalWrite(BUZZER_PIN, LOW);
}
// ---------------- DISPLAY FUNCTIONS ----------------
void showTime() {
  DateTime now = rtc.now();
  lcd.setCursor(0, 0);
  lcd.print("Time: ");
  lcd.print(now.hour());
  lcd.print(":");
  if (now.minute() < 10) lcd.print("0");
  lcd.print(now.minute());

  lcd.setCursor(0, 1);
  lcd.print("Date: ");
  lcd.print(now.day());
  lcd.print("/");
  lcd.print(now.month());
}

void showDHT() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  lcd.setCursor(0, 0);
  lcd.print("Temp: ");
  lcd.print(t, 1);
  lcd.print(" C");
  
  lcd.setCursor(0, 1);
  lcd.print("Hum: ");
  lcd.print(h, 1);
  lcd.print(" %");
}

void showBMP() {
  float p = bmp.readPressure() / 100.0;
  float t = bmp.readTemperature();
  lcd.setCursor(0, 0);
  lcd.print("Pres: ");
  lcd.print(p, 1);
  lcd.print(" hPa");

  lcd.setCursor(0, 1);
  lcd.print("Temp: ");
  lcd.print(t, 1);
  lcd.print(" C");
}

void showMQ135() {
  int value = analogRead(MQ135_PIN);
  lcd.setCursor(0, 0);
  lcd.print("Air Qual: ");
  lcd.print(value);
  lcd.setCursor(0, 1);
  if (value < 200) lcd.print("Fresh Air :)");
  else if (value < 400) lcd.print("Good");
  else if (value < 700) lcd.print("Poor");
  else lcd.print("Bad!");
}
// ---------------- BATTERY PERCENTAGE FUNCTION ----------------
int batteryPercent(float v) {
  if (v >= 4.2) return 100;
  if (v <= 3.0) return 0;
  return (int)((v - 3.0) * 100.0 / (4.2 - 3.0));
}
// ---------------- SERIAL PRINT FUNCTION ----------------
void printAllDataToSerial() {
  DateTime now = rtc.now();
  float h = dht.readHumidity();
  float tDHT = dht.readTemperature();
  float p = bmp.readPressure() / 100.0;
  float tBMP = bmp.readTemperature();
  int mq = analogRead(MQ135_PIN);

  float battV = readBatteryVoltage();
  int battP = batteryPercent(battV);

  Serial.print("<");
  Serial.print(now.hour()); Serial.print(";");
  Serial.print(now.minute()); Serial.print(";");
  Serial.print(now.second()); Serial.print(";");
  Serial.print(now.day()); Serial.print(";");
  Serial.print(now.month()); Serial.print(";");
  Serial.print(now.year()); Serial.print(";");
  Serial.print(tDHT, 1); Serial.print(";");
  Serial.print(h, 1); Serial.print(";");
  Serial.print(tBMP, 1); Serial.print(";");
  Serial.print(p, 1); Serial.print(";");
  Serial.print(mq); Serial.print(";");
  Serial.print(digitalRead(POWER_SOURCE_PIN)); Serial.print(";");
  Serial.print(digitalRead(doorPin)); Serial.print(";");
  Serial.print(battV, 2); Serial.print(";");
  Serial.print(battP);
  Serial.println(">");
}
