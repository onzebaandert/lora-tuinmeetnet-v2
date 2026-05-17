/*
  SKETCH : LORA_VV_WATERFLOW_V1
  DEVICE : XIAO ESP32-C3
  ROLE   : Waterflow sender (YF-201B)
  NOTES  :
    - Waterflow switch op D3 (GPIO5) wekt XIAO uit deep sleep
    - MOSFET op D4 (GPIO6) schakelt 5V naar YF-201B
    - YF-201B op D2 (GPIO4) telt pulsen tijdens meting
    - Na meting: data naar AGG via ESP-NOW, dan terug naar deep sleep
    - Soil22 veld-mapping voor waterflow:
        t_x10    = 0 (ongebruikt)
        h_x10    = flowRate * 10  (L/min, 0.1 resolutie)
        ec_raw   = (uint16)totalLiters  (hele liters, 0-65535 L)
        case_x10 = 0 (geen DS3231)
        vbat_mv  = accuspanning in mV
        ph_x10   = (uint16)(frac(totalLiters) * 1000)  (milli-liter deel)
        flags    = 0xF001 (waterflow OK) / 0xF000 (fout)
    - totalLiters cumulatief opgeslagen in NVS (overleeft herstart)
    - Jaarlijkse reset (1 mei): RESET_BTN_PIN ingedrukt houden bij inschakelen

  TESTEN ZONDER WATER:
    D3 even naar GND = simuleert waterflow switch → XIAO waakt, meet, stuurt, slaapt

  RESET PROCEDURE (bijv. elke 1 mei):
    1. Houd de resetknop (D0 → GND) ingedrukt
    2. Zet de stroom aan
    3. Wacht op "[NVS] RESET" in seriële output, laat knop los

  EERSTE KEER FLASHEN:
    Lees MAC-adres uit seriële output: [WF] MAC=XX:XX:XX:XX:XX:XX
    Vul dit in als SOIL_MACS[2] (FLOW slot) in de AGG sketch.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include <esp_sleep.h>

#define SKETCH_TAG "VV_WATERFLOW_V103"

/* ============== CONFIG ============== */
#define ESPNOW_WIFI_CHANNEL   6
static const uint8_t AGG_MAC[6] = {0xB0, 0xA6, 0x04, 0x07, 0xA2, 0x80};

#define FLOW_PIN              4        // GPIO4 = D2, YF-201B signaalpin
#define MOSFET_PIN            6        // GPIO6 = D4, 5V voeding YF-201B
#define FLOW_SWITCH_PIN       5        // GPIO5 = D3, waterflow switch (LOW = water)
#define RESET_BTN_PIN         2        // GPIO2 = D0, ingedrukt bij boot = NVS reset
#define PULSES_PER_LITER      450.0f   // YF-201B: F(Hz) = 7.5 * Q(L/min) → 450 p/L

#define MEASURE_MS            10000UL  // meetvenster na wakeup (10 sec)
#define SEND_INTERVAL_MS      60000UL  // minimale tijd tussen versturen

#define VBAT_PIN              3
#define VBAT_SAMPLES          12
#define VBAT_FACTOR           3.20f
#define VBAT_CAL              1.000f

#define ACK_WAIT_MS           400
#define MAX_RETRIES           2
/* ==================================== */

#pragma pack(push, 1)
struct __attribute__((packed)) Soil22 {
  uint32_t session_id;
  uint16_t seq;
  int16_t  t_x10;
  uint16_t h_x10;
  uint16_t ec_raw;
  int16_t  case_x10;
  uint16_t vbat_mv;
  uint16_t ph_x10;
  uint16_t flags;
  uint16_t rsv0;
};
#pragma pack(pop)
static_assert(sizeof(Soil22) == 22, "Soil22 size");

#pragma pack(push, 1)
struct __attribute__((packed)) SoilPacket {
  uint8_t magic;
  uint8_t id;
  Soil22  s;
  uint8_t pad[4];
};
#pragma pack(pop)
static_assert(sizeof(SoilPacket) == 28, "SoilPacket size");

#pragma pack(push, 1)
struct __attribute__((packed)) Ack {
  uint32_t session_id;
  uint16_t seq;
  uint8_t  ok;
  uint8_t  pad;
};
#pragma pack(pop)
static_assert(sizeof(Ack) == 8, "Ack size");

/* ===== puls interrupt ===== */
static volatile uint32_t pulse_isr = 0;
void IRAM_ATTR onPulse() { pulse_isr++; }

/* ===== ESP-NOW ACK ===== */
static volatile bool     ack_got    = false;
static volatile uint32_t ack_sid_rx = 0;
static volatile uint16_t ack_seq_rx = 0;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !data || len < (int)sizeof(Ack)) return;
  Ack a;
  memcpy(&a, data, sizeof(a));
  if (a.ok != 1) return;
  ack_sid_rx = a.session_id;
  ack_seq_rx = a.seq;
  ack_got    = true;
}

static bool wait_ack(uint32_t sid, uint16_t seqv) {
  ack_got = false;
  uint32_t t0 = millis();
  while (millis() - t0 < ACK_WAIT_MS) {
    if (ack_got && ack_sid_rx == sid && ack_seq_rx == seqv) return true;
    delay(1);
  }
  return false;
}

/* ===== VBAT ===== */
static uint16_t read_vbat_mv() {
  analogReadResolution(12);
  analogSetPinAttenuation(VBAT_PIN, ADC_11db);
  delay(20);
  uint32_t sum_mv = 0;
  for (int i = 0; i < VBAT_SAMPLES; i++) {
    sum_mv += analogReadMilliVolts(VBAT_PIN);
    delay(2);
  }
  float vout = ((float)sum_mv / VBAT_SAMPLES) / 1000.0f;
  return (uint16_t)lroundf(vout * VBAT_FACTOR * VBAT_CAL * 1000.0f);
}

/* ===== toestand ===== */
static float    totalLiters = 0.0f;
static float    flowRate    = 0.0f;
static uint32_t session_id  = 0;
static uint16_t seq         = 0;

/* ===== NVS ===== */
static Preferences prefs;

static void nvs_save() {
  prefs.begin("waterflow", false);
  prefs.putFloat("total", totalLiters);
  prefs.end();
}

static void go_to_sleep() {
  // Wacht tot switch loslaat (HIGH) voor we slapen
  Serial.println("[WF] wacht op switch loslaten...");
  while (digitalRead(FLOW_SWITCH_PIN) == LOW) delay(50);
  delay(100);  // debouncen

  digitalWrite(MOSFET_PIN, LOW);
  Serial.println("[WF] MOSFET uit, ga slapen. D3→GND om wakker te worden.");
  Serial.flush();

  esp_deep_sleep_enable_gpio_wakeup(1ULL << FLOW_SWITCH_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(120);
  Serial.println();
  Serial.println("===========================================");
  Serial.println("SKETCH : LORA_VV_WATERFLOW_V1");
  Serial.printf ("TAG    : %s\n", SKETCH_TAG);
  Serial.println("===========================================");

  // MOSFET aan: 5V naar YF-201B
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, HIGH);

  // Waterflow switch
  pinMode(FLOW_SWITCH_PIN, INPUT_PULLUP);

  // Reset knop
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  delay(50);

  // Flow interrupt
  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), onPulse, FALLING);

  // NVS laden of resetten
  if (digitalRead(RESET_BTN_PIN) == LOW) {
    prefs.begin("waterflow", false);
    prefs.putFloat("total", 0.0f);
    prefs.end();
    totalLiters = 0.0f;
    Serial.println("[NVS] RESET: teller op 0.000 L");
  } else {
    prefs.begin("waterflow", true);
    totalLiters = prefs.getFloat("total", 0.0f);
    prefs.end();
    Serial.printf("[NVS] hersteld: %.3f L\n", totalLiters);
  }

  // WiFi + ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  while (!WiFi.STA.started()) delay(1);

  session_id = esp_random();
  Serial.printf("[WF] MAC=%s sid=0x%08lX\n",
                WiFi.macAddress().c_str(), (unsigned long)session_id);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[WF] esp_now_init FAILED");
    go_to_sleep();
  }
  esp_now_register_recv_cb(on_recv);
  esp_now_peer_info_t p{};
  memcpy(p.peer_addr, AGG_MAC, 6);
  p.channel = ESPNOW_WIFI_CHANNEL;
  p.encrypt = false;
  esp_now_add_peer(&p);
  Serial.println("[WF] ESP-NOW gereed");

  // Meetvenster
  Serial.printf("[WF] meten voor %lu ms...\n", MEASURE_MS);
  delay(MEASURE_MS);

  noInterrupts();
  uint32_t pulses = pulse_isr;
  pulse_isr = 0;
  interrupts();

  float intervalMin = MEASURE_MS / 60000.0f;
  flowRate     = (pulses / PULSES_PER_LITER) / intervalMin;
  totalLiters += pulses / PULSES_PER_LITER;

  Serial.printf("[WF] pulsen=%u  flow=%.2f L/min  totaal=%.3f L\n",
                pulses, flowRate, totalLiters);

  nvs_save();

  // Versturen naar AGG
  uint16_t vbat_mv   = read_vbat_mv();
  uint16_t flow_x10  = (uint16_t)lroundf(flowRate * 10.0f);
  uint16_t liters_i  = (uint16_t)totalLiters;
  uint16_t liters_ml = (uint16_t)(fmodf(totalLiters, 1.0f) * 1000.0f);

  SoilPacket pkt{};
  pkt.magic        = 0xA1;
  pkt.id           = 3;
  pkt.s.session_id = session_id;
  pkt.s.seq        = ++seq;
  pkt.s.t_x10      = 0;
  pkt.s.h_x10      = flow_x10;
  pkt.s.ec_raw     = liters_i;
  pkt.s.case_x10   = 0;
  pkt.s.vbat_mv    = vbat_mv;
  pkt.s.ph_x10     = liters_ml;
  pkt.s.flags      = 0xF001;
  pkt.s.rsv0       = 0;

  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    esp_err_t txe = esp_now_send(AGG_MAC, (const uint8_t*)&pkt, sizeof(pkt));
    bool tx_ok    = (txe == ESP_OK);
    bool ack_ok   = tx_ok ? wait_ack(pkt.s.session_id, pkt.s.seq) : false;
    Serial.printf("[WF] TX seq=%u attempt=%d tx=%d ack=%d vbat=%umV\n",
                  (unsigned)pkt.s.seq, attempt, tx_ok, ack_ok, (unsigned)vbat_mv);
    if (ack_ok) break;
    delay(50);
  }

  go_to_sleep();
}

void loop() {}
