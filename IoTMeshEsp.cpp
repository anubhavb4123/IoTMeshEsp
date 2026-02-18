#include <ESP8266WiFi.h>
#include <FirebaseESP8266.h>
#include <SoftwareSerial.h>
#include <WiFiClientSecure.h>
//#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <WiFiServer.h>

// ================= TELNET CONFIG =================
WiFiServer telnetServer(23);
WiFiClient telnetClient;

// ================= OTA CONFIG =================
//#define OTA_HOSTNAME "IOTMesh-Lobby-ESP"
//#define OTA_PASSWORD "iotmesh@4123"   // password for OTA updates

// telegram bot token and host
#define TELEGRAM_BOT_TOKEN "8548360181:AAHRLfV2eVGtTPltHqrxmlKb6NY_0B_J75M"
#define TELEGRAM_API_HOST "api.telegram.org"
WiFiClientSecure telegramClient;

// telnet logging macro
#define LOG(msg) telnetLog(msg)

// telnet authentication
// bool telnetAuthed = false;
// const char* TELNET_PASS = "iotmesh";

// ====== SERIAL PINS ======
#define RX_PIN D6
#define TX_PIN D7
SoftwareSerial Serial2(RX_PIN, TX_PIN);

// ---------------- WIFI ----------------
#define WIFI_SSID "BAJPAI_2.4Ghz"
#define WIFI_PASS "44444422"

// ---------------- FIREBASE ----------------
#define FIREBASE_HOST "iotmesh-4123-default-rtdb.firebaseio.com"
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
    LOG("❌ Telegram connection failed");
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

  LOG("📤 Telegram SENT to: " + chatId);
}
// Telegram alert to all users
void sendAlertToAll(String alertMessage) {

  LOG("🔍 Fetching subscribers...");

  // ADMIN
  unsigned long start = millis();
  if (Firebase.getString(fb, "/telegram/admin/chatId") && millis() - start < 2000) {
    sendTelegramMessage(fb.stringData(), alertMessage);
  } else if (millis() - start >= 2000) LOG("❌ Timeout: admin chatId");
  yield();

  // Get nextIndex
  start = millis();
  if (!Firebase.getInt(fb, "/telegram/subscribers/meta/nextIndex") || millis() - start >= 2000) {
    if (millis() - start >= 2000) LOG("❌ Timeout: subscribers meta");
    else LOG("❌ No subscribers meta");
    return;
  }
  yield();

  int total = fb.intData();
  LOG("👥 Total subscribers: " + String(total));

  for (int i = 0; i < total; i++) {

    String path = "/telegram/subscribers/list/" + String(i) + "/chatId";

    start = millis();
    if (!Firebase.getString(fb, path) || millis() - start >= 2000) {
      if (millis() - start >= 2000) LOG("❌ Timeout: subscriber " + String(i));
      else LOG("⏭ Skipping index " + String(i));
      yield();
      continue;
    }
    yield();

    String chatId = fb.stringData();
    LOG("📨 Sending alert to Chat ID: " + chatId);

    sendTelegramMessage(chatId, alertMessage);
    delay(300);
  }

  LOG("✅ Alert dispatch completed");
}
// =====================================================
//               PARSE SENSOR PACKET
// =====================================================
void parseSensorData(String data) {

  LOG("📩 Packet: " + data);

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
    LOG("❌ Invalid packet count: " + String(idx));
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
// void setupOTA() {
//   ArduinoOTA.setHostname(OTA_HOSTNAME);
//   ArduinoOTA.setPassword(OTA_PASSWORD);

//   ArduinoOTA.onStart(onOTAStart);
//   ArduinoOTA.onEnd(onOTAEnd);
//   ArduinoOTA.onProgress(onOTAProgress);
//   ArduinoOTA.onError(onOTAError);

//   ArduinoOTA.begin();
//   LOG("📡 OTA Ready");
// }

// ---- OTA CALLBACK FUNCTIONS ----
// void onOTAStart() {
//   LOG("🚀 OTA Start");
// }

// void onOTAEnd() {
//   LOG("\n✅ OTA End");
// }

// void onOTAProgress(unsigned int progress, unsigned int total) {
//   static unsigned long last = 0;
//   if (millis() - last > 500) {
//     last = millis();
//     LOG("📦 OTA Progress: " + String((progress * 100) / total) + "%");
//   }
// }

// void onOTAError(ota_error_t error) {
//   Serial.printf("❌ OTA Error[%u]: ", error);
//   if (error == OTA_AUTH_ERROR) LOG("Auth Failed");
//   else if (error == OTA_BEGIN_ERROR) LOG("Begin Failed");
//   else if (error == OTA_CONNECT_ERROR) LOG("Connect Failed");
//   else if (error == OTA_RECEIVE_ERROR) LOG("Receive Failed");
//   else if (error == OTA_END_ERROR) LOG("End Failed");
// }
// =====================================================
//         UPDATE LIVE SENSOR VALUES
// =====================================================
void updateLiveData() {
  FirebaseJson json;

  json.set("temperature", tDHT);
  json.set("humidity", h);
  json.set("pressure", pVal);
  json.set("gas", mqVal);
  json.set("power", PowerPin);
  json.set("door", DoorPin);
  json.set("batteryVoltage", batteryVoltage);
  json.set("batteryPercent", batteryPercent);

  String lastUpdate =
    String(hour) + ":" + String(minute) + ":" + String(second) + " " +
    String(day) + "-" + String(month) + "-" + String(year);

  json.set("last_update", lastUpdate);

  unsigned long start = millis();
  if (Firebase.setJSON(fb, LIVE_PATH, json) && millis() - start < 2000) {
    LOG("🔥 Live data updated (JSON)");
  } else {
    if (millis() - start >= 2000) LOG("❌ Timeout: updateLiveData");
    else {
      LOG("❌ Live update failed");
      LOG(fb.errorReason());
    }
  }
  yield();
}
// telegram alert checks
void checkAlerts() {

  unsigned long now = millis();

  // ================= GAS ALERT =================
  if (mqVal > 300 && !gasAlertActive && now - lastGasAlert > ALERT_COOLDOWN) {
    sendAlertToAll(
      "🚨 IOTMesh Alert\nHigh Gas Level!\nPPM: " + String(mqVal)
    );
    gasAlertActive = true;
    lastGasAlert = now;
  }

  if (mqVal <= 280 && gasAlertActive) {  // hysteresis
    gasAlertActive = false;
  }

  // ================= DOOR ALERT =================
  if (DoorPin == 1 && !doorAlertActive && now - lastDoorAlert > ALERT_COOLDOWN) {
    sendAlertToAll("🚪 Door Open Detected");
    doorAlertActive = true;
    lastDoorAlert = now;
  }

  if (DoorPin == 0 && doorAlertActive) {
    doorAlertActive = false;
  }

  // ================= POWER ALERT =================
  if (PowerPin != lastPowerState && now - lastPowerAlert > ALERT_COOLDOWN) {

    if (PowerPin == 0) {
      sendAlertToAll("⚡ Power Switched to Inverter");
    } 
    else if (PowerPin == 1) {
      sendAlertToAll("🔌 Power Switched to Grid");
    }

    lastPowerState = PowerPin;
    lastPowerAlert = now;
  }

  // ================= BATTERY ALERT =================
  static bool batteryAlertActive = false;

  if (batteryPercent <= 20 && !batteryAlertActive &&
      now - lastPowerAlert > ALERT_COOLDOWN) {

    sendAlertToAll(
      "🔋 Low Battery Alert\nBattery: " + String(batteryPercent) + "%"
    );
    batteryAlertActive = true;
    lastPowerAlert = now;
  }

  if (batteryPercent >= 30 && batteryAlertActive) {
    batteryAlertActive = false;
  }
}
// =====================================================
//          READ FIREBASE & CONTROL OUTPUTS
// =====================================================
void readAndControl() {

  unsigned long start = millis();
  if (Firebase.getBool(fb, CONTROL_PATH + "/lobbyTV") && millis() - start < 2000)
    digitalWrite(lobbyTV, fb.boolData() ? HIGH : LOW);
  else if (millis() - start >= 2000) LOG("❌ Timeout: lobbyTV");
  yield();

  start = millis();
  if (Firebase.getBool(fb, CONTROL_PATH + "/lobbyFan") && millis() - start < 2000)
    digitalWrite(lobbyFan, fb.boolData() ? HIGH : LOW);
  else if (millis() - start >= 2000) LOG("❌ Timeout: lobbyFan");
  yield();

  start = millis();
  if (Firebase.getBool(fb, CONTROL_PATH + "/lobbyLight") && millis() - start < 2000)
    digitalWrite(lobbyLight, fb.boolData() ? HIGH : LOW);
  else if (millis() - start >= 2000) LOG("❌ Timeout: lobbyLight");
  yield();

  start = millis();
  if (Firebase.getBool(fb, CONTROL_PATH + "/refrigerator") && millis() - start < 2000)
    digitalWrite(refrigerator, fb.boolData() ? HIGH : LOW);
  else if (millis() - start >= 2000) LOG("❌ Timeout: refrigerator");
  yield();
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

  unsigned long start = millis();
  if (Firebase.setJSON(fb, HISTORY_PATH + "/" + String(index), json) && millis() - start < 2000) {
    LOG("📈 History updated @ index: " + String(index));
  } else {
    if (millis() - start >= 2000) LOG("❌ Timeout: pushHistory");
    else {
      LOG("❌ History error:");
      LOG(fb.errorReason());
    }
  }
  yield();
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

  LOG("\n✅ WiFi Connected");
  LOG(WiFi.localIP());
  // ================= TELNET SERVER =================
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  LOG("📡 Telnet ready on port 23");
  // ================= OTA =================
  delay(300);        // short settle delay
  //setupOTA();        // OTA AFTER WiFi only

  // ================= FIREBASE =================
  Firebase.begin(FIREBASE_HOST, FIREBASE_AUTH);
  Firebase.reconnectWiFi(true);
  fb.setBSSLBufferSize(512, 512);

  LOG("🔥 Firebase Connected");

  sendTelegramMessage("1839775992", "✅ IOTMesh ESP Online");
} 
// telegram and serial logging
void telnetLog(const String &msg) {
  Serial.println(msg);
  if (telnetClient && telnetClient.connected()) {
    telnetClient.println(msg);
  }
}

void telnetLog(const IPAddress &ip) {
  telnetLog(ip.toString());
}
// =====================================================
//                      LOOP
// =====================================================
void loop() {

  // 🔥 MUST be first
  // ArduinoOTA.handle();
  // yield();

  // ================= TELNET CLIENT =================
  if (telnetServer.hasClient()) {
    if (!telnetClient || !telnetClient.connected()) {
      telnetClient = telnetServer.available();
      telnetLog("🖥 Telnet client connected");
    } else {
      telnetServer.available().stop(); // only 1 client
    }
  }

  // ================= SERIAL2 (NON-BLOCKING) =================
  static char incoming[128];
  static uint8_t pos = 0;

  while (Serial2.available()) {
    char c = Serial2.read();

    if (c == '\n') {
      incoming[pos] = '\0';   // terminate string

      String packet = String(incoming);
      packet.trim();
      packet.replace("<", "");
      packet.replace(">", "");

      parseSensorData(packet);

      pos = 0; // reset buffer
    } 
    else if (pos < sizeof(incoming) - 1) {
      incoming[pos++] = c;
    }
  }

  // ================= FIREBASE CONTROL (THROTTLED) =================
  static unsigned long lastControlRead = 0;
  if (millis() - lastControlRead > 1500) {   // every 1.5 sec
    lastControlRead = millis();
    readAndControl();
  }
  yield();

  // ================= HISTORY PUSH (THROTTLED) =================
  static unsigned long lastHistoryRun = 0;
  if (millis() - lastHistoryRun > HISTORY_INTERVAL) {
    lastHistoryRun = millis();
    pushHistory();
  }
  yield(); 

  // ================= ALERT CHECK (THROTTLED) =================
  static unsigned long lastAlertCheck = 0;
  if (millis() - lastAlertCheck > 2000) {   // every 2 sec
    lastAlertCheck = millis();
    checkAlerts();
  }
  
}
