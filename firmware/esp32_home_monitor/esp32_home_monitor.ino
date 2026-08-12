// ניטור בית מבוסס ESP32 - שולח "פעימת חיים" (heartbeat) ל-Firebase כל כמה שניות.
// ההתראות עצמן (טלגרם/מייל) לא נשלחות מכאן - הן נשלחות מ-GitHub Actions
// שבודק בענן אם הפעימה האחרונה "נתקעה". כך גם הפסקת חשמל בבית מזוהה,
// כי אין צורך שה-ESP32 עצמו יתריע - מספיק שהוא ישתוק.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "secrets.h"

// ---- מד ספיקה (חיישן זרימת מים) - הוספה זמנית, אפשר למחוק את כל הבלוקים
// המסומנים ב-ENABLE_FLOW_METER אם לא צריך יותר ----
#define ENABLE_FLOW_METER 1

#if ENABLE_FLOW_METER
#include <Preferences.h>

const int FLOW_SENSOR_PIN = 27; // TODO: התאימי לפין שאליו מחובר חיישן הספיקה בפועל

// פולסים לליטר - תלוי בדגם החיישן! למשל YF-S201 = 450, YF-B1 = 660.
// יש לבדוק בדאטה-שיט של החיישן הספציפי ולעדכן כאן בהתאם.
const float PULSES_PER_LITER = 450.0;

const unsigned long FLOW_REPORT_INTERVAL_MS = 15000; // כל כמה זמן לדווח ספיקה

Preferences flowPrefs;
volatile unsigned long pulseCount = 0;
unsigned long lastFlowReport = 0;
double totalLiters = 0;

void IRAM_ATTR onFlowPulse() {
  pulseCount++;
}

void setupFlowMeter() {
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), onFlowPulse, FALLING);

  // שומרים את הצבירה הכוללת בזיכרון לא-נדיף, כדי שלא תאופס בכל אתחול/הפסקת חשמל
  flowPrefs.begin("flowmeter", false);
  totalLiters = flowPrefs.getDouble("totalLiters", 0.0);
}

bool reportFlow() {
  if (WiFi.status() != WL_CONNECTED) return false;

  noInterrupts();
  unsigned long pulses = pulseCount;
  pulseCount = 0;
  interrupts();

  double liters = pulses / PULSES_PER_LITER;
  totalLiters += liters;
  flowPrefs.putDouble("totalLiters", totalLiters);

  double minutes = FLOW_REPORT_INTERVAL_MS / 60000.0;
  double rateLpm = liters / minutes;

  HTTPClient http;
  String url = String(FIREBASE_HOST) + "/devices/" + DEVICE_ID + "/flow.json";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String payload = "{";
  payload += "\"rateLpm\":" + String(rateLpm, 3) + ",";
  payload += "\"totalLiters\":" + String(totalLiters, 3);
  payload += "}";

  int code = http.PATCH(payload);
  http.end();
  return code == 200;
}
#endif

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
  uint8_t buf[512];
  unsigned long downloadStart = millis();

  while (https.connected() && totalRead < SPEEDTEST_BYTES) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = avail < sizeof(buf) ? avail : sizeof(buf);
      size_t n = stream->readBytes(buf, toRead);
      totalRead += n;
    } else if (!https.connected()) {
      break;
    } else {
      delay(5);
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

void setup() {
  Serial.begin(115200);
  bootMillis = millis();
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) syncTime();

#if ENABLE_FLOW_METER
  setupFlowMeter();
#endif
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

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

#if ENABLE_FLOW_METER
  if (millis() - lastFlowReport >= FLOW_REPORT_INTERVAL_MS) {
    lastFlowReport = millis();
    bool ok = reportFlow();
    Serial.println(ok ? "דיווח ספיקה נשלח" : "דיווח ספיקה נכשל");
  }
#endif

  delay(200);
}
