/*
  SKETCH : LORA_VV_WATERFLOW_SLEEP_V1
  DEVICE : Seeed XIAO ESP32-C3
  ROLE   : Waterflow sender (YF-201B) — timer polling + ESP-NOW

  Hardware:
    YF-201B signaalpin : GPIO7 (D5), INPUT_PULLUP
    MOSFET sensorpower : GPIO6 (D4), OUTPUT — HIGH = sensor aan
    VBAT meting        : GPIO3 (A1)

  Bedrading:
    T-BAT 5V ──────────── YF Rood (VCC)
                           YF Zwart (GND) ── MOSFET Drain
    XIAO GPIO6 (D4) ────── MOSFET Gate
    XIAO GND ──────────── MOSFET Source
                           YF Geel (Signaal) ── XIAO GPIO7 (D5)

  Werking:
    1. Deep sleep → timer wakeup elke POLL_INTERVAL_S seconden
    2. MOSFET aan → wacht SENSOR_WARMUP_MS → tel pulsen COUNT_WINDOW_MS
    3. MOSFET uit → sla pulsen op in RTC memory
    4. Na SEND_EVERY_N_POLLS polls: stuur uurdata via ESP-NOW naar AGG, reset teller
    5. Terug naar deep sleep

  Puls berekening:
    YF-201B formule: F(Hz) = 7.5 × Q(L/min) → 450 pulsen per liter

  Packet mapping (Soil22, id=3, FLOW slot in AGG):
    h_x10    = liters_dit_uur × 10  (0.1 L resolutie, max 6553.5 L/uur)
    ec_raw   = totaal_liters integer (0–65535 L)
    ph_x10   = totaal_liters fractie × 1000 (milliliter deel)
    case_x10 = 0
    vbat_mv  = accuspanning mV
    flags    = 0xF003

  EERSTE KEER FLASHEN:
    Lees MAC uit seriële output: [WF] MAC=XX:XX:XX:XX:XX:XX
    Vul dit in als SOIL_MACS[2] (FLOW slot) in de AGG sketch.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>

#define SKETCH_TAG "VV_WATERFLOW_SLEEP_V100"

/* ============== CONFIG ============== */
#define ESPNOW_WIFI_CHANNEL   6
static const uint8_t AGG_MAC[6] = {0xB0, 0xA6, 0x04, 0x07, 0xA2, 0x80};

#define FLOW_PIN              7          // GPIO7 = D5 — YF-201B signaalpin
#define MOSFET_PIN            6          // GPIO6 = D4 — N-MOSFET gate (HIGH = sensor aan)

#define PULSES_PER_LITER      450.0f    // YF-201B: F(Hz) = 7.5 × Q(L/min)

#define POLL_INTERVAL_S       60ULL     // wakeup interval in seconden
#define SEND_EVERY_N_POLLS    60        // stuur elk uur (60 × 60s = 3600s)
#define SENSOR_WARMUP_MS      20UL      // stabilisatietijd na MOSFET aan
#define COUNT_WINDOW_MS       500UL     // puls-telvenster

#define VBAT_PIN              3
#define VBAT_SAMPLES          12
#define VBAT_FACTOR           3.20f
#define VBAT_CAL              1.000f

#define ACK_WAIT_MS           500
#define MAX_RETRIES           2
/* ==================================== */

/* ===== RTC memory — bewaard tijdens deep sleep ===== */
RTC_DATA_ATTR static uint32_t rtc_pulses_hour = 0;
RTC_DATA_ATTR static float    rtc_total_L     = 0.0f;
RTC_DATA_ATTR static uint8_t  rtc_poll_count  = 0;
RTC_DATA_ATTR static uint32_t rtc_session_id  = 0;
RTC_DATA_ATTR static uint16_t rtc_seq         = 0;
RTC_DATA_ATTR static bool     rtc_init        = false;

/* ===== ESP-NOW structs ===== */
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
  uint32_t sum = 0;
  for (int i = 0; i < VBAT_SAMPLES; i++) {
    sum += analogReadMilliVolts(VBAT_PIN);
    delay(2);
  }
  float vout = ((float)sum / VBAT_SAMPLES) / 1000.0f;
  return (uint16_t)lroundf(vout * VBAT_FACTOR * VBAT_CAL * 1000.0f);
}

/* ===== Puls tellen ===== */
static volatile uint32_t isr_pulses = 0;

void IRAM_ATTR onPulse() { isr_pulses++; }

static uint32_t count_pulses_window(uint32_t window_ms) {
  isr_pulses = 0;
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), onPulse, FALLING);
  delay(window_ms);
  detachInterrupt(digitalPinToInterrupt(FLOW_PIN));
  noInterrupts();
  uint32_t cnt = isr_pulses;
  interrupts();
  return cnt;
}

/* ===== ESP-NOW versturen ===== */
static void do_send() {
  Serial.println("[WF] ESP-NOW send...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  while (!WiFi.STA.started()) delay(1);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[WF] esp_now_init FAILED");
    WiFi.mode(WIFI_OFF);
    return;
  }
  esp_now_register_recv_cb(on_recv);
  esp_now_peer_info_t p{};
  memcpy(p.peer_addr, AGG_MAC, 6);
  p.channel = ESPNOW_WIFI_CHANNEL;
  p.encrypt = false;
  esp_now_add_peer(&p);

  float liters_this_hour = rtc_pulses_hour / PULSES_PER_LITER;
  uint16_t vbat_mv       = read_vbat_mv();
  uint16_t h_x10         = (uint16_t)lroundf(liters_this_hour * 10.0f);
  uint16_t liters_i      = (uint16_t)rtc_total_L;
  uint16_t liters_ml     = (uint16_t)(fmodf(rtc_total_L, 1.0f) * 1000.0f);

  SoilPacket pkt{};
  pkt.magic        = 0xA1;
  pkt.id           = 3;
  pkt.s.session_id = rtc_session_id;
  pkt.s.seq        = ++rtc_seq;
  pkt.s.t_x10      = 0;
  pkt.s.h_x10      = h_x10;
  pkt.s.ec_raw     = liters_i;
  pkt.s.case_x10   = 0;
  pkt.s.vbat_mv    = vbat_mv;
  pkt.s.ph_x10     = liters_ml;
  pkt.s.flags      = 0xF003;
  pkt.s.rsv0       = 0;

  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    esp_err_t txe = esp_now_send(AGG_MAC, (const uint8_t*)&pkt, sizeof(pkt));
    bool tx_ok    = (txe == ESP_OK);
    bool ack_ok   = tx_ok ? wait_ack(pkt.s.session_id, pkt.s.seq) : false;

    Serial.printf("[WF] TX seq=%u att=%d tx=%d ack=%d | uurL=%.2f totL=%.3f vbat=%umV\n",
                  (unsigned)pkt.s.seq, attempt,
                  tx_ok ? 1 : 0, ack_ok ? 1 : 0,
                  liters_this_hour, rtc_total_L, (unsigned)vbat_mv);

    if (ack_ok) break;
    delay(50);
  }

  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
}

/* ===== Deep sleep ===== */
static void go_sleep() {
  Serial.printf("[WF] Sleep %llus | poll %u/%d | totaal %.3f L\n",
                POLL_INTERVAL_S, (unsigned)rtc_poll_count,
                SEND_EVERY_N_POLLS, rtc_total_L);
  Serial.flush();
  esp_sleep_enable_timer_wakeup(POLL_INTERVAL_S * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, LOW);
  pinMode(FLOW_PIN, INPUT_PULLUP);

  if (!rtc_init) {
    rtc_session_id = esp_random();
    rtc_init       = true;
    Serial.println("===========================================");
    Serial.println("SKETCH : LORA_VV_WATERFLOW_SLEEP_V1");
    Serial.printf ("TAG    : %s\n", SKETCH_TAG);
    Serial.println("===========================================");
    Serial.printf("[WF] MAC=%s  sid=0x%08lX\n",
                  WiFi.macAddress().c_str(), (unsigned long)rtc_session_id);
    Serial.println("[WF] >>> Kopieer MAC naar SOIL_MACS[2] in AGG sketch <<<");
  }

  // MOSFET aan → warmup → tel pulsen → MOSFET uit
  digitalWrite(MOSFET_PIN, HIGH);
  delay(SENSOR_WARMUP_MS);
  uint32_t new_pulses = count_pulses_window(COUNT_WINDOW_MS);
  digitalWrite(MOSFET_PIN, LOW);

  rtc_pulses_hour += new_pulses;
  rtc_total_L     += new_pulses / PULSES_PER_LITER;
  rtc_poll_count++;

  if (new_pulses > 0) {
    Serial.printf("[WF] +%lu p (%.4f L) | uur: %lu p (%.2f L) | totaal: %.3f L\n",
                  (unsigned long)new_pulses,
                  new_pulses / PULSES_PER_LITER,
                  (unsigned long)rtc_pulses_hour,
                  rtc_pulses_hour / PULSES_PER_LITER,
                  rtc_total_L);
  }

  if (rtc_poll_count >= SEND_EVERY_N_POLLS) {
    do_send();
    rtc_pulses_hour = 0;
    rtc_poll_count  = 0;
  }

  go_sleep();
}

void loop() {
  // Onbereikbaar — setup() eindigt altijd met esp_deep_sleep_start()
}
