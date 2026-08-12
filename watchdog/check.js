// רץ ע"י GitHub Actions כל כמה דקות. בודק אם ה-ESP32 שלח פעימת חיים
// לאחרונה. אם לא - שולח התראה בטלגרם ובמייל. כשהוא חוזר לדווח - שולח
// גם הודעת "חזר לפעול", כדי שלא תישאר בספק אם הבעיה נפתרה.

const nodemailer = require("nodemailer");

const {
  FIREBASE_HOST,
  DEVICE_ID = "esp32_home",
  TELEGRAM_BOT_TOKEN,
  TELEGRAM_CHAT_ID,
  GMAIL_USER,
  GMAIL_APP_PASSWORD,
  ALERT_EMAIL_TO,
  STALE_THRESHOLD_SEC = "180",
} = process.env;

const THRESHOLD = parseInt(STALE_THRESHOLD_SEC, 10);

if (!FIREBASE_HOST) {
  console.error("חסר FIREBASE_HOST - יש להגדיר כ-secret ב-GitHub");
  process.exit(1);
}

async function getJson(path) {
  const res = await fetch(`${FIREBASE_HOST}/${path}.json`);
  if (!res.ok) throw new Error(`Firebase GET ${path} נכשל: ${res.status}`);
  return res.json();
}

async function putJson(path, body) {
  const res = await fetch(`${FIREBASE_HOST}/${path}.json`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!res.ok) throw new Error(`Firebase PUT ${path} נכשל: ${res.status}`);
}

async function sendTelegram(text) {
  if (!TELEGRAM_BOT_TOKEN || !TELEGRAM_CHAT_ID) {
    console.log("דילוג על טלגרם - חסרים secrets");
    return;
  }
  const url = `https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage`;
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ chat_id: TELEGRAM_CHAT_ID, text }),
  });
  if (!res.ok) console.error("שליחת טלגרם נכשלה:", await res.text());
}

async function sendEmail(subject, text) {
  if (!GMAIL_USER || !GMAIL_APP_PASSWORD || !ALERT_EMAIL_TO) {
    console.log("דילוג על מייל - חסרים secrets");
    return;
  }
  const transporter = nodemailer.createTransport({
    service: "gmail",
    auth: { user: GMAIL_USER, pass: GMAIL_APP_PASSWORD },
  });
  await transporter.sendMail({ from: GMAIL_USER, to: ALERT_EMAIL_TO, subject, text });
}

async function main() {
  const nowSec = Math.floor(Date.now() / 1000);

  const status = await getJson(`devices/${DEVICE_ID}/status`).catch((e) => {
    console.error("קריאת סטטוס נכשלה:", e.message);
    return null;
  });

  const lastSeen = status && status.lastSeen ? status.lastSeen : 0;
  const ageSec = nowSec - lastSeen;
  const isStale = !lastSeen || ageSec > THRESHOLD;

  const prevAlert = (await getJson(`devices/${DEVICE_ID}/alertState`).catch(() => null)) || {
    isDown: false,
  };

  console.log(
    `[${DEVICE_ID}] פעימה אחרונה לפני ${ageSec} שניות | תקוע=${isStale} | מצב קודם isDown=${prevAlert.isDown}`
  );

  if (isStale && !prevAlert.isDown) {
    const minutes = lastSeen ? Math.round(ageSec / 60) : null;
    const msg = lastSeen
      ? `🔴 ${DEVICE_ID} לא מגיב! לא התקבלה פעימה כבר ${minutes} דקות. יכול להיות שאין חשמל, אין אינטרנט, או שהמכשיר קרס.`
      : `🔴 ${DEVICE_ID} מעולם לא דיווח - בדוק חיווט/קונפיגורציה.`;
    await Promise.all([sendTelegram(msg), sendEmail(`⚠️ ${DEVICE_ID} לא מגיב`, msg)]);
    await putJson(`devices/${DEVICE_ID}/alertState`, { isDown: true, since: nowSec });
  } else if (!isStale && prevAlert.isDown) {
    const msg = `✅ ${DEVICE_ID} חזר לדווח כרגיל.`;
    await Promise.all([sendTelegram(msg), sendEmail(`✅ ${DEVICE_ID} חזר לפעול`, msg)]);
    await putJson(`devices/${DEVICE_ID}/alertState`, { isDown: false, since: nowSec });
  }
}

main().catch((e) => {
  console.error("שגיאה כללית:", e);
  process.exit(1);
});
