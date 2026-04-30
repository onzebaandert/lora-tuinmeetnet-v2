#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#define FLOW_PIN           7        // D5
#define INTERVAL_MS        5000     // meetinterval flow
#define GRAPH_INTERVAL_MS  300000   // 5 minuten per grafiekpunt
#define GRAPH_POINTS       24       // 24 punten = 2 uur
// YF-201B: F(Hz) = 7.5 * Q(L/min)  →  450 pulsen per liter
#define PULSES_PER_LITER   450.0f

const char* ssid     = "WaterMeter";
const char* password = "water123";

volatile uint32_t pulseCount = 0;
float totalLiters   = 0.0f;
float flowRate      = 0.0f;
uint32_t lastTime   = 0;
uint32_t lastGraphTime = 0;

float   graphData[GRAPH_POINTS] = {0};
uint8_t graphHead  = 0;   // volgende schrijfpositie
uint8_t graphCount = 0;   // aantal geldige punten (max GRAPH_POINTS)

Preferences prefs;
WebServer server(80);

void IRAM_ATTR onPulse() {
  pulseCount++;
}

void saveTotal() {
  prefs.begin("waterflow", false);
  prefs.putFloat("total", totalLiters);
  prefs.end();
}

void handleRoot() {
  // Bouw JS-array met grafiekdata (oudste → nieuwste)
  String jsData = "[";
  uint8_t count = min((int)graphCount, (int)GRAPH_POINTS);
  uint8_t start = (graphCount >= GRAPH_POINTS) ? graphHead : 0;
  for (uint8_t i = 0; i < count; i++) {
    if (i > 0) jsData += ",";
    jsData += String(graphData[(start + i) % GRAPH_POINTS], 3);
  }
  jsData += "]";

  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta http-equiv='refresh' content='5'>"
    "<title>Waterflow</title>"
    "<style>"
    "body{font-family:sans-serif;text-align:center;padding:1em;background:#f0f4f8}"
    "h1{color:#2c3e50}"
    ".card{display:inline-block;margin:.5em;padding:1.2em 2em;background:#fff;"
    "border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,.15)}"
    ".val{font-size:2.2em;font-weight:bold;color:#2980b9}"
    ".lbl{color:#7f8c8d;margin-top:.3em}"
    ".chart-wrap{background:#fff;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,.15);"
    "display:inline-block;padding:1em;margin:.5em}"
    "</style></head><body>"
    "<h1>YF-201B Waterflow</h1>"
    "<div class='card'><div class='val'>" + String(flowRate, 2) + " L/min</div>"
    "<div class='lbl'>Flow</div></div>"
    "<div class='card'><div class='val'>" + String(totalLiters, 2) + " L</div>"
    "<div class='lbl'>Totaal</div></div>"
    "<br><div class='chart-wrap'>"
    "<canvas id='c' width='400' height='200'></canvas></div>"
    "<p style='color:#aaa;font-size:.8em'>Grafiek: cumulatief totaal per 5 min (laatste 2 uur) &bull; pagina ververst elke 5s</p>"
    "<script>"
    "var d=" + jsData + ";"
    "var cv=document.getElementById('c'),ctx=cv.getContext('2d');"
    "var w=cv.width,h=cv.height,pad=34;"
    "var max=Math.max.apply(null,d.concat([0.01]));"
    // achtergrond
    "ctx.fillStyle='#f8fafc';ctx.fillRect(0,0,w,h);"
    // rasterlijnen + y-labels
    "for(var i=0;i<=4;i++){"
    "var y=pad+(h-2*pad)*i/4;"
    "ctx.strokeStyle='#e0e0e0';ctx.lineWidth=1;"
    "ctx.beginPath();ctx.moveTo(pad,y);ctx.lineTo(w-pad,y);ctx.stroke();"
    "ctx.fillStyle='#aaa';ctx.font='10px sans-serif';ctx.textAlign='right';"
    "ctx.fillText((max*(1-i/4)).toFixed(2),pad-4,y+4);}"
    // lijn + punten
    "if(d.length>1){"
    "ctx.strokeStyle='#2980b9';ctx.lineWidth=2;ctx.beginPath();"
    "for(var i=0;i<d.length;i++){"
    "var x=pad+(w-2*pad)*i/(d.length-1);"
    "var y=pad+(h-2*pad)*(1-d[i]/max);"
    "i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);}"
    "ctx.stroke();"
    "ctx.fillStyle='#2980b9';"
    "for(var i=0;i<d.length;i++){"
    "var x=pad+(w-2*pad)*i/(d.length-1);"
    "var y=pad+(h-2*pad)*(1-d[i]/max);"
    "ctx.beginPath();ctx.arc(x,y,3,0,2*Math.PI);ctx.fill();}}"
    // assen
    "ctx.strokeStyle='#888';ctx.lineWidth=1;ctx.beginPath();"
    "ctx.moveTo(pad,pad);ctx.lineTo(pad,h-pad);ctx.lineTo(w-pad,h-pad);ctx.stroke();"
    "ctx.fillStyle='#555';ctx.font='11px sans-serif';ctx.textAlign='center';"
    "ctx.fillText('L totaal',w/2,h-4);"
    "</script>"
    "</body></html>";

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), onPulse, FALLING);

  // Herstel totaal uit NVS na stroomuitval
  prefs.begin("waterflow", true);
  totalLiters = prefs.getFloat("total", 0.0f);
  prefs.end();
  Serial.printf("Totaal hersteld uit NVS: %.2f L\n", totalLiters);

  lastTime      = millis();
  lastGraphTime = millis();

  WiFi.softAP(ssid, password);
  Serial.print("Access Point gestart: ");
  Serial.println(ssid);
  Serial.print("IP-adres: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.begin();
  Serial.println("Webserver gestart op poort 80");
}

void loop() {
  server.handleClient();

  uint32_t now = millis();

  // Flow meting elke 5 seconden
  if (now - lastTime >= INTERVAL_MS) {
    noInterrupts();
    uint32_t count = pulseCount;
    pulseCount = 0;
    interrupts();

    float intervalMin = INTERVAL_MS / 60000.0f;
    flowRate    = (count / PULSES_PER_LITER) / intervalMin;
    totalLiters += count / PULSES_PER_LITER;

    Serial.printf("Flow: %.2f L/min | Totaal: %.2f L\n", flowRate, totalLiters);
    lastTime = now;
  }

  // Grafiekpunt opslaan + NVS bijwerken elke 5 minuten
  if (now - lastGraphTime >= GRAPH_INTERVAL_MS) {
    graphData[graphHead] = totalLiters;
    graphHead = (graphHead + 1) % GRAPH_POINTS;
    if (graphCount < GRAPH_POINTS) graphCount++;
    lastGraphTime = now;
    saveTotal();
    Serial.printf("Grafiekpunt: %.2f L totaal | NVS opgeslagen\n", totalLiters);
  }
}
