#include <WiFi.h>
#include <WebServer.h>

// ---------------- Pins ----------------
#define PANEL_PIN 34    // divider midpoint (10k + 10k)
#define LED_G     25    // high sunlight
#define LED_R     33    // low sunlight
#define RELAY_PIN 18    // relay IN
#define BTN_PIN   27    // optional override button (other leg to GND)

// ---------------- Settings ----------------
const bool  RELAY_ACTIVE_LOW = true;   
const float DIVIDER_FACTOR   = 2.0;    
const float LOAD_WATTS       = 2.5;    
const float TARIFF_RS        = 8.0;    
const float CO2_KG_PER_KWH   = 0.8;    

// ---------------- State ----------------
float smoothV = 0, midV = 3.0, gapV = 0.3;
bool  isHigh = false, relayOn = false;
int   mode = 0;                        // 0 = auto, 1 = force ON, 2 = force OFF
unsigned long loadOnMs = 0, lastHist = 0, lastSample = 0;

const int HIST = 60;
float hist[HIST];
int histIdx = 0;

WebServer server(80);

// ---------------- Helpers ----------------
void setRelay(bool on) {
  relayOn = on;
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? !on : on);
}

float readV() {
  long sum = 0;
  for (int i = 0; i < 200; i++) { sum += analogRead(PANEL_PIN); delayMicroseconds(300); }
  float v = (sum / 200.0) * 3.3 / 4095.0 * DIVIDER_FACTOR;
  smoothV = (smoothV == 0) ? v : 0.7 * smoothV + 0.3 * v;
  return smoothV;
}

float calibrate(unsigned long ms) {
  unsigned long start = millis();
  float sum = 0; int n = 0;
  while (millis() - start < ms) { sum += readV(); n++; }
  return sum / n;
}

String advice() {
  float older = hist[(histIdx + HIST - 7) % HIST];   // about 30 s ago
  if (mode == 1) return "Manual override: load forced ON.";
  if (mode == 2) return "Manual override: load forced OFF.";
  if (isHigh && smoothV < older - gapV) return "Sunlight dropping. Finish heavy loads soon.";
  if (isHigh)  return "Strong sunlight. Load running on solar.";
  if (smoothV > older + gapV) return "Sunlight rising. Get ready to run loads.";
  return "Low sunlight. Load off.";
}

void handleData() {
  float kwh = (loadOnMs / 3600000.0) * LOAD_WATTS / 1000.0;
  String j = "{";
  j += "\"v\":" + String(smoothV, 2) + ",";
  j += "\"level\":\"" + String(isHigh ? "HIGH" : "LOW") + "\",";
  j += "\"relay\":" + String(relayOn ? "true" : "false") + ",";
  j += "\"mode\":" + String(mode) + ",";
  j += "\"advice\":\"" + advice() + "\",";
  j += "\"kwh\":" + String(kwh, 4) + ",";
  j += "\"rs\":" + String(kwh * TARIFF_RS, 3) + ",";
  j += "\"co2\":" + String(kwh * CO2_KG_PER_KWH, 4) + ",";
  j += "\"hist\":[";
  for (int i = 0; i < HIST; i++) {
    j += String(hist[(histIdx + i) % HIST], 2);
    if (i < HIST - 1) j += ",";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SolarSync</title>
<style>
body{font-family:sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:16px;text-align:center}
.card{background:#1e293b;border-radius:12px;padding:14px;margin:10px auto;max-width:420px}
#lvl{font-size:2rem;font-weight:bold}
.HIGH{color:#22c55e}.LOW{color:#ef4444}
canvas{width:100%;height:120px;background:#0f172a;border-radius:8px}
</style></head><body>
<h2>SolarSync</h2>
<div class="card"><div id="lvl">--</div><div id="v"></div><div id="adv"></div>
<div>Load: <b id="rl">--</b></div></div>
<div class="card"><canvas id="c" width="400" height="120"></canvas><small>Panel voltage, last 5 min</small></div>
<div class="card">Solar energy used: <b id="kwh">0</b> kWh<br>Saved: Rs <b id="rs">0</b><br>CO2 avoided: <b id="co2">0</b> kg</div>
<script>
async function tick(){
 try{
 const d=await (await fetch('/data')).json();
 const l=document.getElementById('lvl'); l.textContent=d.level; l.className=d.level;
 document.getElementById('v').textContent=d.v+' V';
 document.getElementById('adv').textContent=d.advice;
 document.getElementById('rl').textContent=d.relay?'ON':'OFF';
 document.getElementById('kwh').textContent=d.kwh;
 document.getElementById('rs').textContent=d.rs;
 document.getElementById('co2').textContent=d.co2;
 const c=document.getElementById('c').getContext('2d'); c.clearRect(0,0,400,120);
 const mn=Math.min(...d.hist), mx=Math.max(...d.hist), r=(mx-mn)||1;
 c.strokeStyle='#38bdf8'; c.beginPath();
 d.hist.forEach((y,i)=>{const px=i*400/59, py=110-((y-mn)/r)*100; i?c.lineTo(px,py):c.moveTo(px,py)});
 c.stroke();
 }catch(e){}
}
setInterval(tick,1500); tick();
</script></body></html>
)rawliteral";

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_R, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  analogSetAttenuation(ADC_11db);

  Serial.println("Step 1: BRIGHT light on the panel (5 s)");
  digitalWrite(LED_G, HIGH);
  float bright = calibrate(5000);
  digitalWrite(LED_G, LOW);

  Serial.println("Step 2: COVER the panel (5 s)");
  digitalWrite(LED_R, HIGH);
  float dark = calibrate(5000);
  digitalWrite(LED_R, LOW);

  if (fabs(bright - dark) >= 0.3) {
    midV = (bright + dark) / 2.0;
    gapV = fabs(bright - dark) * 0.15;
  } else {
    Serial.println("Warning: small difference, using defaults");
  }
  Serial.print("Bright: "); Serial.print(bright, 2);
  Serial.print(" V  Dark: "); Serial.print(dark, 2);
  Serial.print(" V  Threshold: "); Serial.println(midV, 2);

  for (int i = 0; i < HIST; i++) hist[i] = smoothV;

  WiFi.softAP("SolarSync", "solar1234");
  Serial.print("Dashboard: http://");
  Serial.println(WiFi.softAPIP());
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/data", handleData);
  server.begin();
}

// ---------------- Loop ----------------
void loop() {
  server.handleClient();
  unsigned long now = millis();

  // button: auto -> force ON -> force OFF -> auto
  static bool lastBtn = HIGH;
  bool b = digitalRead(BTN_PIN);
  if (lastBtn == HIGH && b == LOW) {
    mode = (mode + 1) % 3;
    Serial.print("Mode: ");
    Serial.println(mode == 0 ? "AUTO" : mode == 1 ? "FORCE ON" : "FORCE OFF");
    delay(200);
  }
  lastBtn = b;

  if (now - lastSample >= 200) {
    lastSample = now;
    float v = readV();

    if (!isHigh && v > midV + gapV) isHigh = true;
    if (isHigh  && v < midV - gapV) isHigh = false;

    if (mode == 0)      setRelay(isHigh);
    else if (mode == 1) setRelay(true);
    else                setRelay(false);

    digitalWrite(LED_G, isHigh);
    digitalWrite(LED_R, !isHigh);

    if (relayOn) loadOnMs += 200;

    Serial.print("Panel: "); Serial.print(v, 2);
    Serial.print(" V  "); Serial.print(isHigh ? "HIGH" : "LOW");
    Serial.print("  relay "); Serial.println(relayOn ? "ON" : "OFF");
  }

  if (now - lastHist >= 5000) {
    lastHist = now;
    hist[histIdx] = smoothV;
    histIdx = (histIdx + 1) % HIST;
  }
}