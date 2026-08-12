// ניטור בית מבוסס ESP32 - שולח "פעימת חיים" (heartbeat) ל-Firebase כל כמה שניות.
// ההתראות עצמן (טלגרם/מייל) לא נשלחות מכאן - הן נשלחות מ-GitHub Actions
// שבודק בענן אם הפעימה האחרונה "נתקעה". כך גם הפסקת חשמל בבית מזוהה,
// כי אין צורך שה-ESP32 עצמו יתריע - מספיק שהוא ישתוק.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoOTA.h>
#include "secrets.h"

const unsigned long HEARTBEAT_INTERVAL_MS = 15000;           // כל כמה זמן לשלוח פעימה
const unsigned long SPEEDTEST_INTERVAL_MS = 5UL * 60 * 1000; // כל 5 דקות - בדיקת מהירות אינטרנט
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

// גודל הקובץ שמורידים לבדיקת מהירות. 500KB כל 5 דקות = כ-4GB בחודש -
// סביר לחיבור בית רגיל, אבל אם יש הגבלת נתונים אפשר להקטין את המספר.
const size_t SPEEDTEST_BYTES = 500000;

unsigned long lastHeartbeat = 0;
unsigned long lastSpeedtest = 0;
unsigned long bootMillis = 0;

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("מתחבר לוויפי");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("מחובר, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("החיבור נכשל, ננסה שוב ב-loop");
  }
}

void syncTime() {
  // אזור זמן ישראל: UTC+2 קבוע + שעון קיץ אוטומטי (DST) של שעה נוספת מרץ-אוקטובר
  configTime(2 * 3600, 3600, "pool.ntp.org", "time.google.com");
  struct tm timeinfo;
  int tries = 0;
  while (!getLocalTime(&timeinfo) && tries < 20) {
    delay(500);
    tries++;
  }
}

bool sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return false;

  time_t now;
  time(&now);
  // אם השעון עוד לא סונכרן (near epoch), ננסה לסנכרן שוב
  if (now < 1700000000) {
    syncTime();
    time(&now);
  }

  HTTPClient http;
  String url = String(FIREBASE_HOST) + "/devices/" + DEVICE_ID + "/status.json";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String payload = "{";
  payload += "\"lastSeen\":" + String((unsigned long)now) + ",";
  payload += "\"uptimeSec\":" + String((millis() - bootMillis) / 1000) + ",";
  payload += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  payload += "}";

  int code = http.PATCH(payload);
  http.end();
  return code == 200;
}

// מודדת מהירות הורדה ע"י שליפת קובץ בגודל קבוע משרת הבדיקה של Cloudflare
// (speed.cloudflare.com - אותו שירות שמניע בדיקות מהירות ציבוריות רבות),
// ומדווחת את התוצאה ל-Firebase תחת מפתח = חותמת הזמן, כדי שיהיה נוח
// לשלוף טווח תאריכים בהמשך (לגרף ולדוח).
bool measureAndReportSpeed() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure(); // מדלגים על אימות תעודה - מקובל כאן כי לא מועבר מידע רגיש, רק מודדים מהירות

  HTTPClient https;
  String url = "https://speed.cloudflare.com/__down?bytes=" + String(SPEEDTEST_BYTES);
  if (!https.begin(client, url)) return false;

  unsigned long requestStart = millis();
  int code = https.GET();
  unsigned long timeToFirstByte = millis() - requestStart; // קירוב ל-latency (ping)

  if (code != HTTP_CODE_OK) {
    https.end();
    return false;
  }

  WiFiClient *stream = https.getStreamPtr();
  size_t totalRead = 0;
  // באפר גדול יותר (היה 512 בייט) + בלי השהיה מלאכותית בין קריאות -
  // באפר קטן מדי ו-delay() מיותר האטו את המדידה בהרבה מתחת למהירות האמיתית.
  static uint8_t buf[4096];
  unsigned long downloadStart = millis();

  while (https.connected() && totalRead < SPEEDTEST_BYTES) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = avail < sizeof(buf) ? avail : sizeof(buf);
      size_t n = stream->readBytes(buf, toRead);
      totalRead += n;
    } else if (!https.connected()) {
      break;
    }
  }
  unsigned long elapsedMs = millis() - downloadStart;
  https.end();

  if (totalRead == 0 || elapsedMs == 0) return false;

  double mbps = (totalRead * 8.0) / (elapsedMs / 1000.0) / 1000000.0;

  time_t now;
  time(&now);

  HTTPClient http;
  String putUrl = String(FIREBASE_HOST) + "/devices/" + DEVICE_ID + "/speedtests/" + String((unsigned long)now) + ".json";
  http.begin(putUrl);
  http.addHeader("Content-Type", "application/json");

  String payload = "{";
  payload += "\"mbps\":" + String(mbps, 2) + ",";
  payload += "\"pingMs\":" + String(timeToFirstByte) + ",";
  payload += "\"bytes\":" + String(totalRead);
  payload += "}";

  int putCode = http.PUT(payload);
  http.end();
  return putCode == 200;
}

// מאפשר להעלות קוד חדש דרך הוויפי (Sketch -> Upload Using: Network Port ב-
// Arduino IDE), בלי USB. עובד רק כשהמחשב שממנו מעלים נמצא באותה רשת כמו הבקר.
void setupOTA() {
  ArduinoOTA.setHostname(DEVICE_ID);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA
    .onStart([]() { Serial.println("OTA: מתחיל עדכון..."); })
    .onEnd([]() { Serial.println("OTA: הסתיים בהצלחה"); })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("OTA: %u%%\n", (progress * 100) / total);
    })
    .onError([](ota_error_t error) {
      Serial.printf("OTA: שגיאה [%u]\n", error);
    });

  ArduinoOTA.begin();
  Serial.println("OTA מוכן - אפשר להעלות דרך הוויפי");
}

void setup() {
  Serial.begin(115200);
  bootMillis = millis();
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    syncTime();
    setupOTA();
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  ArduinoOTA.handle();

  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    bool ok = sendHeartbeat();
    Serial.println(ok ? "פעימה נשלחה בהצלחה" : "שליחת פעימה נכשלה");
  }

  if (millis() - lastSpeedtest >= SPEEDTEST_INTERVAL_MS) {
    lastSpeedtest = millis();
    Serial.println("מריץ בדיקת מהירות...");
    bool ok = measureAndReportSpeed();
    Serial.println(ok ? "בדיקת מהירות דווחה בהצלחה" : "בדיקת מהירות נכשלה");
  }

  delay(200);
}
