# ניטור בית עם ESP32 - התראות על ניתוק/הפסקת חשמל

מערכת קטנה שמריצה ESP32 בבית, שולח ממנו "פעימת חיים" (heartbeat) ל-Firebase
כל 15 שניות, ובודקת בענן (GitHub Actions) אם הפעימה נעצרה. אם כן - נשלחת
התראה בטלגרם ובמייל. ברגע שהמכשיר חוזר לדווח, נשלחת גם הודעת "חזר לפעול".

## למה זה עובד גם בהפסקת חשמל?

ה-ESP32 עצמו **לא** מתריע על שום דבר - הוא רק שולח דופק. כל הלוגיקה של
"האם קרה משהו רע" יושבת בענן (ב-GitHub Actions), שבודק כל 5 דקות אם הדופק
האחרון עדיין טרי. אם נפל חשמל בבית, נותק האינטרנט, או שה-ESP32 קרס - בכל
המקרים הוא פשוט מפסיק לשלוח, וה"שומר" בענן שם לב לזה ומתריע. אין צורך
בסוללת גיבוי על ה-ESP32 בשביל זה.

**מגבלה לדעת:** GitHub Actions לא מבטיח דיוק של 5 דקות בדיוק - בעומס גבוה
ההרצה עלולה להתעכב עוד כמה דקות. זה מספיק טוב לניטור ביתי, אבל זו לא
מערכת "מיידית" ברמת שניות.

## מהירות אינטרנט - גרף ודוח חודשי

בנוסף לניטור "חי/מת", ה-ESP32 מריץ כל 5 דקות בדיקת מהירות הורדה מול
`speed.cloudflare.com` (אותו שירות שמניע בדיקות מהירות ציבוריות רבות),
ומדווח את התוצאה (Mbps + זמן תגובה משוער) ל-Firebase לפי חותמת זמן. הדף
מציג גרף של 24 השעות האחרונות, ומתעדכן אוטומטית. כפתור "הורדת דוח חודש
אחרון" מוריד קובץ CSV עם כל המדידות מ-30 הימים האחרונים - נוח לפתוח באקסל.

> **שימו לב לצריכת נתונים:** כברירת מחדל כל בדיקה מורידה 500KB, כלומר
> כ-4GB לחודש. לחיבור בית רגיל זה זניח, אבל אם יש הגבלת נתונים (למשל
> גיבוי סלולרי) - אפשר להקטין את `SPEEDTEST_BYTES` בקוד ה-ESP32, או
> להגדיל את `SPEEDTEST_INTERVAL_MS` כדי לבדוק בתדירות נמוכה יותר.

## מד ספיקה (זרימת מים) - הוספה זמנית

נוסף בלוק קוד זמני שקורא חיישן ספיקה (flow sensor) מבוסס פולסים, מחשב
ספיקה נוכחית (ליטר/דקה) וסך-הכל מים שנמדדו (מצטבר, נשמר בזיכרון הפנימי
כך שלא מתאפס בהפסקת חשמל), ומדווח ל-Firebase כל 15 שניות. מוצג גם בדף
הדשבורד. כל הקוד הזה עטוף ב-`#if ENABLE_FLOW_METER` בקובץ ה-`.ino` -
אפשר להוריד את הדגל ל-0 או למחוק את הבלוקים כשלא רלוונטי יותר.

**לפני שמעלים לחיישן אמיתי, יש לעדכן שני קבועים בקוד:**
- `FLOW_SENSOR_PIN` - הפין שאליו מחובר החיישן בפועל
- `PULSES_PER_LITER` - "מקדם K" של דגם החיישן הספציפי (למשל YF-S201 = 450,
  YF-B1 = 660) - מופיע בדאטה-שיט של החיישן

## מבנה הפרויקט

```
esp32-home-monitor/
├── firmware/esp32_home_monitor/   קוד ה-ESP32 (Arduino)
├── docs/index.html                דף הדשבורד (מתפרסם ע"י GitHub Pages)
├── watchdog/check.js              הסקריפט שבודק תקינות ושולח התראות
├── .github/workflows/watchdog.yml ההרצה המתוזמנת של הבודק
└── database.rules.json            כללי אבטחה ל-Firebase (לעיון/העתקה)
```

## שלב 1: הקמת Firebase

1. כנסי ל-https://console.firebase.google.com ותצרי פרויקט חדש (חינמי, תוכנית Spark).
2. בתפריט הצד: **Build -> Realtime Database -> Create Database**. בחרי מיקום, והתחילי
   ב-"locked mode" (נעדכן כללים בעצמנו).
3. בטאב **Rules** של ה-Realtime Database, הדביקי את התוכן של
   [`database.rules.json`](database.rules.json) (כולל נתיבי `speedtests` ו-`flow`) ופרסמי (Publish).
   > הכללים האלה משאירים כתיבה/קריאה פתוחה רק לנתיב הסטטוס של המכשיר, כדי
   > לשמור על הקוד ב-ESP32 פשוט (בלי אימות/טוקנים). זה תרגיל סביר לפרויקט
   > בית פרטי; ה-`databaseURL` לא ניתן לניחוש בקלות, אבל אל תפרסמי אותו
   > בפומבי. אם בעתיד תרצי הקשחה נוספת - אפשר לעבור ל-Firebase App Check
   > או לאימות אנונימי.
4. ב-**Project settings (גלגל השיניים) -> General**, גללי למטה ל-"Your apps" ותצרי
   אפליקציית Web (סמל `</>`). משם תקבלי את `apiKey`, `databaseURL`, `projectId` -
   אלה הפרטים שממלאים ב-`docs/index.html`.

## שלב 2: בוט טלגרם

1. פתחי שיחה עם [@BotFather](https://t.me/BotFather) בטלגרם ושלחי `/newbot`, תני לו שם.
   תקבלי **TELEGRAM_BOT_TOKEN**.
2. שלחי הודעה כלשהי לבוט החדש שלך (חייבים "לפתוח" איתו שיחה כדי שיוכל לכתוב אלייך).
3. גשי לכתובת `https://api.telegram.org/bot<TOKEN>/getUpdates` בדפדפן, ותמצאי שם
   את השדה `chat.id` - זה ה-**TELEGRAM_CHAT_ID**.

## שלב 3: מייל (Gmail)

1. הפעילי אימות דו-שלבי בחשבון ה-Gmail שלך.
2. ביצרי [סיסמת אפליקציה](https://myaccount.google.com/apppasswords) (App Password) -
   זו ה-**GMAIL_APP_PASSWORD** (לא הסיסמה הרגילה שלך!).
3. **GMAIL_USER** הוא כתובת ה-Gmail עצמה. **ALERT_EMAIL_TO** - לאן לשלוח את ההתראות
   (יכול להיות אותה כתובת, או אחרת).

## שלב 4: הריפו ב-GitHub

1. תצרי ריפו חדש ב-GitHub, ותעלי אליו את תוכן התיקייה `esp32-home-monitor`.
2. ב-**Settings -> Secrets and variables -> Actions**, הוסיפי את ה-secrets הבאים:
   - `FIREBASE_HOST` (למשל `https://my-home-monitor-default-rtdb.firebaseio.com`)
   - `TELEGRAM_BOT_TOKEN`
   - `TELEGRAM_CHAT_ID`
   - `GMAIL_USER`
   - `GMAIL_APP_PASSWORD`
   - `ALERT_EMAIL_TO`
3. ב-**Settings -> Pages**, תחת "Build and deployment" בחרי Source: "Deploy from a branch",
   Branch: `main`, תיקייה: `/docs`. אחרי דקה-שתיים האתר יהיה זמין בכתובת
   שתופיע שם (בד"כ `https://<username>.github.io/<repo>/`).
4. לפני שהאתר עולה, מלאי את `firebaseConfig` בקובץ [`docs/index.html`](docs/index.html)
   עם הפרטים משלב 1.
5. אפשר לבדוק את ה-workflow מיד, בלי לחכות ל-5 דקות: טאב **Actions -> ESP32 Watchdog ->
   Run workflow**.

## שלב 5: קוד ה-ESP32 (Arduino IDE)

1. אם עוד אין לך: התקיני את [Arduino IDE](https://www.arduino.cc/en/software), ואז
   ב-**File -> Preferences** הוסיפי ל-"Additional Board Manager URLs":
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   ואז ב-**Tools -> Board -> Boards Manager** חפשי והתקיני "esp32".
2. פתחי את [`firmware/esp32_home_monitor/esp32_home_monitor.ino`](firmware/esp32_home_monitor/esp32_home_monitor.ino).
3. העתיקי את `secrets.h.example` לקובץ בשם `secrets.h` (**באותה תיקייה**), ומלאי
   שם את שם/סיסמת הוויפי ואת `FIREBASE_HOST`. קובץ זה לא עולה ל-git (הוא ב-`.gitignore`).
4. בחרי את הבורד הנכון (**Tools -> Board -> ESP32 -> ...** בהתאם לדגם שלך) והפורט
   ה-USB, ולחצי Upload.
5. פתחי **Tools -> Serial Monitor** (115200 baud) ותוודאי שרואים "מחובר, IP: ..." ואז
   "פעימה נשלחה בהצלחה" כל 15 שניות.

## בדיקה מקצה לקצה

1. עם ה-ESP32 פועל, פתחי את דף הדשבורד (מ-GitHub Pages) - אמור להופיע "מחובר ותקין".
2. נתקי את ה-ESP32 מהחשמל, וחכי כ-5-10 דקות. הדף אמור לעבור ל"לא מגיב" (אחרי
   3 דקות של שקט + עד 5 דקות עד ריצת ה-Actions הבאה), ואמורה להגיע הודעת טלגרם/מייל.
3. חברי בחזרה - תוך כמה דקות אמורה להגיע הודעת "חזר לפעול".

## הרחבות אפשריות בהמשך

- **חיישן נוסף** (למשל DHT22 לטמפרטורה/לחות, או חיישן דלת/מים) - פשוט הוסיפי
  שדה ל-JSON שנשלח מה-ESP32 (`sendHeartbeat()`), הציגי אותו ב-`docs/index.html`,
  והוסיפי תנאי התראה מתאים ב-`watchdog/check.js`.
- **כמה ESP32 בבית** - כל אחד עם `DEVICE_ID` שונה, אותו קוד עובד לכולם; ב-Actions
  צריך להריץ את הבדיקה על כל מזהה (או להרחיב את `check.js` שיעבור על רשימה).
- **התרעה מיידית יותר** - אם 5 דקות זה יותר מדי איטי, אפשר להעביר את הבדיקה
  משרת קטן (למשל Raspberry Pi בבית, או VPS) שרץ כל דקה במקום GitHub Actions.
