#include <ESP8266WiFi.h>
#include <FirebaseESP8266.h>
#include <SoftwareSerial.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>

// ================= OTA CONFIG =================
#define OTA_HOSTNAME "IMeh-oby-EP"
#define OTA_PASSWORD "i4123"   // password for OTA updates

// telegram bot token and host
#define TELEGRAM_BOT_TOKEN "8543618:ARLfVeVGTPlHqrxlb6NY_0B_J75M"
#define TELEGRAM_API_HOST "api.telegram.org"
WiFiClientSecure telegramClient;
// ====== SERIAL PINS ======
#define RX_PIN D6
#define TX_PIN D7
SoftwareSerial Serial2(RX_PIN, TX_PIN);

// ---------------- WIFI ----------------
#define WIFI_SSID "BAJPAI_2.4Ghz"
#define WIFI_PASS "44444422"

// ---------------- FIREBASE ----------------
#define FIREBASE_HOST "ioms-12-ealrt.firseo.com"
#define FIREBASE_AUTH "test123"

FirebaseData fb;
int lastPowerState = -1;   // -1 = unknown, 0 = inverter, 1 = grid
// ---------------- SENSOR DATA ----------------
float tDHT, h, pVal, mqVal;
int PowerPin, DoorPin;

// ---------------- RTC TIME (FROM ARDUINO NANO) ----------------
int hour, minute, second, day, month, year;
// ---------------- BATTERY DATA ----------------
float batteryVoltage = 0.0;
int batteryPercent = 0;
// ---------------- HISTORY CONTROL ----------------
unsigned long lastHistoryPush = 0;
const unsigned long HISTORY_INTERVAL = 60000; // 1 minute
// ---------------- ALERT STATES ----------------
bool gasAlertActive   = false;
bool doorAlertActive  = false;
bool powerAlertActive = false;
// ---------------- OUTPUT PINS ----------------
#define lobbyLight D1
#define lobbyFan   D2
#define refrigerator D3
#define lobbyTV D5
#define WIFI_LED D4   // Blue onboard LED

// ---------------- FIREBASE PATHS ----------------
String LIVE_PATH    = "/home/room1/sensor";
String CONTROL_PATH = "/home/room1/controls";
String HISTORY_PATH = "/home/room1/history/h24";

// ---------------- FORWARD DECLARATIONS ----------------
void readAndControl();
void pushHistory();
// telegram alert timers
unsigned long lastGasAlert = 0;
unsigned long lastDoorAlert = 0;
unsigned long lastPowerAlert = 0;
const unsigned long ALERT_COOLDOWN = 60000; // 1 minute

// =====================================================
//        LEAP YEAR CHECK
// =====================================================
bool isLeapYear(int y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}
// encode URL
String urlEncode(String str) {
  String encoded = "";
  char c;
  char bufHex[4];

  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);

    if (isalnum(c)) {
      encoded += c;
    } else {
      sprintf(bufHex, "%%%02X", c);
      encoded += bufHex;
    }
  }
  return encoded;
}
// =====================================================
//        RTC → EPOCH (CORRECT)
// =====================================================
unsigned long rtcToEpochMs() {

  static const int daysInMonth[] =
    {31,28,31,30,31,30,31,31,30,31,30,31};

  unsigned long days = 0;

  // Years
  for (int y = 1970; y < year; y++) {
    days += isLeapYear(y) ? 366 : 365;
  }

  // Months
  for (int m = 1; m < month; m++) {
    days += daysInMonth[m - 1];
    if (m == 2 && isLeapYear(year)) days++;
  }

  // Days
  days += (day - 1);

  unsigned long seconds =
    days * 86400UL +
    hour * 3600UL +
    minute * 60UL +
    second;

  return seconds * 1000UL;
}
// telegram message sender
void sendTelegramMessage(String chatId, String message) {
  telegramClient.setInsecure();

  if (!telegramClient.connect(TELEGRAM_API_HOST, 443)) {
    Serial.println("❌ Telegram connection failed");
    return;
  }

  String encodedMsg = urlEncode(message);

  String url = "/bot" + String(TELEGRAM_BOT_TOKEN) +
               "/sendMessage?chat_id=" + chatId +
               "&text=" + encodedMsg;

  telegramClient.print(
    String("GET ") + url + " HTTP/1.1\r\n" +
    "Host: " + TELEGRAM_API_HOST + "\r\n" +
    "Connection: close\r\n\r\n"
  );

  Serial.println("📤 Telegram SENT to: " + chatId);
}
// Telegram alert to all users
void sendAlertToAll(String alertMessage) {

  Serial.println("🔍 Fetching subscribers...");

  // ADMIN
  if (Firebase.getString(fb, "/telegram/admin/chatId")) {
    sendTelegramMessage(fb.stringData(), alertMessage);
  }

  // Get nextIndex
  if (!Firebase.getInt(fb, "/telegram/subscribers/meta/nextIndex")) {
    Serial.println("❌ No subscribers meta");
    return;
  }

  int total = fb.intData();
  Serial.println("👥 Total subscribers: " + String(total));

  for (int i = 0; i < total; i++) {

    String path = "/telegram/subscribers/list/" + String(i) + "/chatId";

    if (!Firebase.getString(fb, path)) {
      Serial.println("⏭ Skipping index " + String(i));
      continue;
    }

    String chatId = fb.stringData();
    Serial.println("📨 Sending alert to Chat ID: " + chatId);

    sendTelegramMessage(chatId, alertMessage);
    delay(300);
  }

  Serial.println("✅ Alert dispatch completed");
}
// =====================================================
//               PARSE SENSOR PACKET
// =====================================================
void parseSensorData(String data) {

  Serial.println("📩 Packet: " + data);

  String parts[16];
  int idx = 0;

  // 🔥 CORRECT parsing (captures LAST value also)
  while (true) {
    int p = data.indexOf(';');

    if (p == -1) {
      parts[idx++] = data;   // ✅ store last field
      break;
    }

    parts[idx++] = data.substring(0, p);
    data = data.substring(p + 1);

    if (idx >= 16) break;
  }

  // ✅ Arduino sends EXACTLY 15 values
  if (idx != 15) {
    Serial.println("❌ Invalid packet count: " + String(idx));
    return;
  }

  // -------- TIME --------
  hour   = parts[0].toInt();
  minute = parts[1].toInt();
  second = parts[2].toInt();
  day    = parts[3].toInt();
  month  = parts[4].toInt();
  year   = parts[5].toInt();

  // -------- SENSOR VALUES --------
  tDHT     = parts[6].toFloat();
  h        = parts[7].toFloat();
  float tBMP = parts[8].toFloat();   // optional, currently unused
  pVal     = parts[9].toFloat();
  mqVal    = parts[10].toFloat();
  PowerPin = parts[11].toInt();
  DoorPin  = parts[12].toInt();
  batteryVoltage = parts[13].toFloat();
  batteryPercent = parts[14].toInt();

  updateLiveData();
}
// ota setup
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart(onOTAStart);
  ArduinoOTA.onEnd(onOTAEnd);
  ArduinoOTA.onProgress(onOTAProgress);
  ArduinoOTA.onError(onOTAError);

  ArduinoOTA.begin();
  Serial.println("📡 OTA Ready");
}

// ---- OTA CALLBACK FUNCTIONS ----
void onOTAStart() {
  Serial.println("🚀 OTA Start");
}

void onOTAEnd() {
  Serial.println("\n✅ OTA End");
}

void onOTAProgress(unsigned int progress, unsigned int total) {
  Serial.printf("📦 OTA Progress: %u%%\r", (progress * 100) / total);
}

void onOTAError(ota_error_t error) {
  Serial.printf("❌ OTA Error[%u]: ", error);
  if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
  else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
  else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
  else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
  else if (error == OTA_END_ERROR) Serial.println("End Failed");
}
// =====================================================
//         UPDATE LIVE SENSOR VALUES
// =====================================================
void updateLiveData() {

  Firebase.setFloat(fb, LIVE_PATH + "/temperature", tDHT);
  Firebase.setFloat(fb, LIVE_PATH + "/humidity", h);
  Firebase.setFloat(fb, LIVE_PATH + "/pressure", pVal);
  Firebase.setFloat(fb, LIVE_PATH + "/gas", mqVal);
  Firebase.setInt  (fb, LIVE_PATH + "/power", PowerPin);
  Firebase.setInt  (fb, LIVE_PATH + "/door", DoorPin);
  Firebase.setFloat(fb, LIVE_PATH + "/batteryVoltage", batteryVoltage);
  Firebase.setInt  (fb, LIVE_PATH + "/batteryPercent", batteryPercent);
  String lastUpdate =
    String(hour) + ":" + String(minute) + ":" + String(second) + " " +
    String(day) + "-" + String(month) + "-" + String(year);

  Firebase.setString(fb, LIVE_PATH + "/last_update", lastUpdate);

  Serial.println("🔥 Live data updated");
}
// telegram alert checks
void checkAlerts() {

  // ================= GAS ALERT =================
  if (mqVal > 300 && !gasAlertActive) {
    sendAlertToAll(
      "🚨 IOTMesh Alert\nHigh Gas Level!\nPPM: " + String(mqVal)
    );
    gasAlertActive = true;
  }

  if (mqVal <= 280 && gasAlertActive) {  // hysteresis
    gasAlertActive = false;
  }

  // ================= DOOR ALERT =================
  if (DoorPin == 1 && !doorAlertActive) {
    sendAlertToAll("🚪 Door Open Detected");
    doorAlertActive = true;
  }

  if (DoorPin == 0 && doorAlertActive) {
    doorAlertActive = false;
  }

  // ================= POWER ALERT =================
  if (PowerPin != lastPowerState) {

    if (PowerPin == 0) {
      sendAlertToAll("⚡ Power Switched to Inverter");
    }

    if (PowerPin == 1) {
      sendAlertToAll("🔌 Power Switched to Grid");
    }

    lastPowerState = PowerPin;
  }

  // ================= BATTERY ALERT =================
  static bool batteryAlertActive = false;

  if (batteryPercent <= 20 && !batteryAlertActive) {
    sendAlertToAll(
      "🔋 Low Battery Alert\nBattery: " + String(batteryPercent) + "%"
    );
    batteryAlertActive = true;
  }

  if (batteryPercent >= 30 && batteryAlertActive) {
    batteryAlertActive = false;
  }
}
// =====================================================
//          READ FIREBASE & CONTROL OUTPUTS
// =====================================================
void readAndControl() {

  if (Firebase.getBool(fb, CONTROL_PATH + "/lobbyTV"))
    digitalWrite(lobbyTV, fb.boolData() ? HIGH : LOW);

  if (Firebase.getBool(fb, CONTROL_PATH + "/lobbyFan"))
    digitalWrite(lobbyFan, fb.boolData() ? HIGH : LOW);

  if (Firebase.getBool(fb, CONTROL_PATH + "/lobbyLight"))
    digitalWrite(lobbyLight, fb.boolData() ? HIGH : LOW);
    
  if (Firebase.getBool(fb, CONTROL_PATH + "/refrigerator"))
    digitalWrite(refrigerator, fb.boolData() ? HIGH : LOW);
}  

// =====================================================
//         PUSH HISTORY (24H RING BUFFER)
// =====================================================
void pushHistory() {

  if (millis() - lastHistoryPush < HISTORY_INTERVAL) return;
  lastHistoryPush = millis();

  // Minute index of the day (0–1439)
  int index = hour * 60 + minute;

  FirebaseJson json;

  // 🔥 Use SERVER timestamp (correct & frontend-safe)
  json.set("timestamp/.sv", "timestamp");

  json.set("temperature", tDHT);
  json.set("humidity", h);
  json.set("gas", mqVal);
  json.set("pressure", pVal);
  json.set("waterLevel", 0);

  if (Firebase.setJSON(fb, HISTORY_PATH + "/" + String(index), json)) {
    Serial.println("📈 History updated @ index: " + String(index));
  } else {
    Serial.println("❌ History error:");
    Serial.println(fb.errorReason());
  }
}

// =====================================================
//                      SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600);

  pinMode(lobbyLight, OUTPUT);
  pinMode(lobbyFan, OUTPUT);
  pinMode(refrigerator, OUTPUT);
  pinMode(lobbyTV, OUTPUT);
  pinMode(WIFI_LED, OUTPUT);

  // Relays OFF (active LOW)
  digitalWrite(lobbyLight, HIGH);
  digitalWrite(lobbyFan, HIGH);
  digitalWrite(refrigerator, HIGH);
  digitalWrite(lobbyTV, HIGH);

  digitalWrite(WIFI_LED, HIGH); // LED OFF (active LOW)

  // ================= WIFI CONNECT =================
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("📶 Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(WIFI_LED, LOW);   // LED ON
    delay(150);
    digitalWrite(WIFI_LED, HIGH);  // LED OFF
    delay(150);
    Serial.print(".");
    yield();  // 🔥 VERY IMPORTANT for OTA/WDT
  }

  // WiFi connected
  digitalWrite(WIFI_LED, LOW); // LED ON (connected)

  Serial.println("\n✅ WiFi Connected");
  Serial.println(WiFi.localIP());

  // ================= OTA =================
  delay(300);        // short settle delay
  setupOTA();        // OTA AFTER WiFi only

  // ================= FIREBASE =================
  Firebase.begin(FIREBASE_HOST, FIREBASE_AUTH);
  Firebase.reconnectWiFi(true);
  fb.setBSSLBufferSize(1024, 1024);

  Serial.println("🔥 Firebase Connected");

  sendTelegramMessage("1839775992", "✅ IOTMesh ESP Online");
}

// =====================================================
//                      LOOP
// =====================================================
void loop() {

  ArduinoOTA.handle();   // 🔥 MUST BE FIRST LINE

  if (Serial2.available()) {
    String incoming = Serial2.readStringUntil('\n');
    incoming.trim();
    incoming.replace("<", "");
    incoming.replace(">", "");
    parseSensorData(incoming);
  }

  readAndControl();
  pushHistory();
  checkAlerts();

  yield();
}
