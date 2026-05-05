/*
  SKETCH : LORA_VV_WATERFLOW_SOLAR_V1
  DEVICE : Seeed XIAO ESP32-C3
  ROLE   : Waterflow (YF-201B) — solar 6V, dag/nacht schema 08:00-20:00

  Hardware:
    YF-201B VCC        : T-BAT 5V → MOSFET Drain → YF Rood
    YF-201B GND        : GND
    YF-201B signaalpin : GPIO4 (D2), INPUT_PULLUP  ← gpio wakeup
    MOSFET gate        : GPIO6 (D4) — HIGH = YF aan
    VBAT               : GPIO3 (A1)

  Werking:
    Overdag (08:00–20:00):
      MOSFET HIGH → YF continu gevoed (geen ext1-probleem meer)
      XIAO slaapt met GPIO-wakeup (FLOW_PIN LOW) + timer tot 20:00
      Water stroomt → YF puls → wakeup → tel pulsen → slaap verder
      Bij sessie klaar of ≥10L: WiFi → NTP → ESP-NOW → sleep

    Nacht (20:00–08:00):
      MOSFET LOW → YF stroomloos
      Timer sleep tot 08:00

    Drift: NTP bij elke WiFi-verbinding (morning, evening, send)

  Packet mapping (id=3, FLOW slot in AGG):
    h_x10    = liters deze sessie × 10
    ec_raw   = totaal liters (integer)
    ph_x10   = totaal liters fractie × 1000 (mL)
    case_x10 = 1 als sessie klaar, 0 als tussentijds (≥10L)
    vbat_mv  = accuspanning mV
    flags    = 0xF003
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <time.h>
#include "credentials.h"

#define SKETCH_TAG "VV_WATERFLOW_SOLAR_V100"

/* ======= PINNEN ======= */
#define FLOW_PIN    4    // GPIO4 = D2 — YF-201B signaal (gpio wakeup vereist GPIO0-5)
#define MOSFET_PIN  6    // GPIO6 = D4 — HIGH = YF aan
#define VBAT_PIN    3    // GPIO3 = A1

/* ======= DAG/NACHT SCHEMA ======= */
#define HOUR_START   8   // 08:00
#define HOUR_END    20   // 20:00
#define TZ_STR      "CET-1CEST,M3.5.0,M10.5.0/3"

/* ======= METING ======= */
#define PULSES_PER_LITER    450.0f
#define MEASURE_WINDOW_MS   500UL    // teltijd na wakeup
#define SEND_AFTER_LITERS   0.1f     // TEST: laag voor debug, normaal 10.0

/* ======= ESP-NOW ======= */
#define ESPNOW_CHANNEL  6
static const uint8_t AGG_MAC[6] = {0xB0, 0xA6, 0x04, 0x07, 0xA2, 0x80};
#define ACK_WAIT_MS  500
#define MAX_RETRIES  2

/* ======= VBAT ======= */
#define VBAT_SAMPLES  12
#define VBAT_FACTOR   3.20f
#define VBAT_CAL      1.000f

/* ======= PACKETS (zelfde formaat als SLEEP_V1) ======= */
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
static_assert(sizeof(Soil22) == 22, "Soil22 size");

struct __attribute__((packed)) SoilPacket {
  uint8_t magic;
  uint8_t id;
  Soil22  s;
  uint8_t pad[4];
};
static_assert(sizeof(SoilPacket) == 28, "SoilPacket size");

struct __attribute__((packed)) Ack {
  uint32_t session_id;
  uint16_t seq;
  uint8_t  ok;
  uint8_t  pad;
};
static_assert(sizeof(Ack) == 8, "Ack size");
#pragma pack(pop)

/* ======= RTC MEMORY (bewaard tijdens deep sleep) ======= */
RTC_DATA_ATTR static bool     rtc_init           = false;
RTC_DATA_ATTR static bool     rtc_day_mode       = false;   // true=dag, false=nacht
RTC_DATA_ATTR static uint32_t rtc_pulses_session = 0;       // pulsen deze gietersessie
RTC_DATA_ATTR static float    rtc_total_L        = 0.0f;    // totaal ooit gemeten
RTC_DATA_ATTR static uint32_t rtc_session_id     = 0;
RTC_DATA_ATTR static uint16_t rtc_seq            = 0;

/* ======= ESP-NOW ACK ======= */
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

/* ======= VBAT ======= */
static uint16_t read_vbat_mv() {
  analogReadResolution(12);
  analogSetPinAttenuation(VBAT_PIN, ADC_11db);
  delay(20);
  uint32_t sum = 0;
  for (int i = 0; i < VBAT_SAMPLES; i++) { sum += analogReadMilliVolts(VBAT_PIN); delay(2); }
  float vout = ((float)sum / VBAT_SAMPLES) / 1000.0f;
  return (uint16_t)lroundf(vout * VBAT_FACTOR * VBAT_CAL * 1000.0f);
}

/* ======= WIFI / NTP ======= */
static bool wifi_connect_router() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(100);
  bool ok = (WiFi.status() == WL_CONNECTED);
  if (!ok) Serial.println("[WF] WiFi connect FAIL");
  return ok;
}

static bool do_ntp() {
  setenv("TZ", TZ_STR, 1);
  tzset();
  configTime(0, 0, "pool.ntp.org");
  uint32_t t0 = millis();
  while (time(nullptr) < 1000000000UL && millis() - t0 < 8000) delay(100);
  bool ok = (time(nullptr) > 1000000000UL);
  if (!ok) Serial.println("[WF] NTP sync FAIL");
  return ok;
}

static int current_hour() {
  struct tm t;
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  return t.tm_hour;
}

static uint32_t secs_until(int target_hour) {
  struct tm t;
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  int cur = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
  int tgt = target_hour * 3600;
  if (tgt <= cur) tgt += 86400;
  return (uint32_t)(tgt - cur);
}

/* ======= PULSEN TELLEN ======= */
static volatile uint32_t isr_pulses = 0;
void IRAM_ATTR onPulse() { isr_pulses++; }

static uint32_t count_pulses(uint32_t window_ms) {
  isr_pulses = 0;
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), onPulse, FALLING);
  delay(window_ms);
  detachInterrupt(digitalPinToInterrupt(FLOW_PIN));
  noInterrupts();
  uint32_t cnt = isr_pulses;
  interrupts();
  return cnt;
}

/* ======= ESP-NOW SEND ======= */
static void espnow_send(bool session_done) {
  // Router disconnect, dan ESP-NOW channel
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setChannel(ESPNOW_CHANNEL);
  while (!WiFi.STA.started()) delay(1);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[WF] esp_now_init FAIL");
    return;
  }
  esp_now_register_recv_cb(on_recv);
  esp_now_peer_info_t p{};
  memcpy(p.peer_addr, AGG_MAC, 6);
  p.channel = ESPNOW_CHANNEL;
  p.encrypt = false;
  esp_now_add_peer(&p);

  float sess_L     = rtc_pulses_session / PULSES_PER_LITER;
  uint16_t vbat_mv = read_vbat_mv();

  SoilPacket pkt{};
  pkt.magic        = 0xA1;
  pkt.id           = 3;
  pkt.s.session_id = rtc_session_id;
  pkt.s.seq        = ++rtc_seq;
  pkt.s.t_x10      = 0;
  pkt.s.h_x10      = (uint16_t)lroundf(sess_L * 10.0f);
  pkt.s.ec_raw     = (uint16_t)rtc_total_L;
  pkt.s.case_x10   = session_done ? 1 : 0;
  pkt.s.vbat_mv    = vbat_mv;
  pkt.s.ph_x10     = (uint16_t)(fmodf(rtc_total_L, 1.0f) * 1000.0f);
  pkt.s.flags      = 0xF003;
  pkt.s.rsv0       = 0;

  for (int i = 0; i <= MAX_RETRIES; i++) {
    bool tx_ok  = (esp_now_send(AGG_MAC, (const uint8_t*)&pkt, sizeof(pkt)) == ESP_OK);
    bool ack_ok = tx_ok ? wait_ack(pkt.s.session_id, pkt.s.seq) : false;
    Serial.printf("[WF] TX seq=%u att=%d ok=%d | sess=%.2fL tot=%.3fL vbat=%umV\n",
                  (unsigned)pkt.s.seq, i, ack_ok ? 1 : 0,
                  sess_L, rtc_total_L, (unsigned)vbat_mv);
    if (ack_ok) break;
    delay(50);
  }

  esp_now_deinit();
  if (session_done) rtc_pulses_session = 0;
}

/* ======= SLEEP HELPERS ======= */
static void sleep_dag(uint32_t secs_tot_20h) {
  Serial.printf("[WF] Dag sleep: GPIO-wakeup + timer %us (~%u min) tot 20:00\n",
                secs_tot_20h, secs_tot_20h / 60);
  Serial.flush();
  WiFi.mode(WIFI_OFF);
  digitalWrite(MOSFET_PIN, HIGH);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << FLOW_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_sleep_enable_timer_wakeup((uint64_t)secs_tot_20h * 1000000ULL);
  esp_deep_sleep_start();
}

static void sleep_nacht(uint32_t secs_tot_8h) {
  Serial.printf("[WF] Nacht sleep: timer %us (~%u min) tot 08:00\n",
                secs_tot_8h, secs_tot_8h / 60);
  Serial.flush();
  WiFi.mode(WIFI_OFF);
  digitalWrite(MOSFET_PIN, LOW);
  esp_sleep_enable_timer_wakeup((uint64_t)secs_tot_8h * 1000000ULL);
  esp_deep_sleep_start();
}

/* ======= SETUP ======= */
void setup() {
  Serial.begin(115200);
  delay(100);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  pinMode(MOSFET_PIN, OUTPUT);
  pinMode(FLOW_PIN, INPUT_PULLUP);
  // Bij GPIO wakeup (flow) YF aan houden zodat count_pulses() echt pulsen ziet.
  // In alle andere gevallen pas later aanzetten als nodig.
  digitalWrite(MOSFET_PIN, (cause == ESP_SLEEP_WAKEUP_GPIO) ? HIGH : LOW);

  if (!rtc_init) {
    rtc_session_id = esp_random();
    rtc_init       = true;
    Serial.println("===========================================");
    Serial.println("SKETCH : LORA_VV_WATERFLOW_SOLAR_V1");
    Serial.printf ("TAG    : %s\n", SKETCH_TAG);
    Serial.println("===========================================");
  }

  // ── EERSTE BOOT ──────────────────────────────────────────────
  if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    Serial.println("[WF] Eerste boot — NTP sync, bepaal dag/nacht");
    bool ok = wifi_connect_router();
    if (ok) {
      do_ntp();
      Serial.printf("[WF] MAC=%s  sid=0x%08lX\n",
                    WiFi.macAddress().c_str(), (unsigned long)rtc_session_id);
      Serial.println("[WF] >>> Kopieer MAC naar SOIL_MACS[2] in AGG sketch <<<");
    }
    int h = current_hour();
    if (h >= HOUR_START && h < HOUR_END) {
      rtc_day_mode = true;
      uint32_t s = ok ? secs_until(HOUR_END) : (uint32_t)(HOUR_END - HOUR_START) * 3600;
      sleep_dag(s);
    } else {
      rtc_day_mode = false;
      uint32_t s = ok ? secs_until(HOUR_START) : (uint32_t)43200;
      sleep_nacht(s);
    }
  }

  // ── TIMER WAKEUP ─────────────────────────────────────────────
  if (cause == ESP_SLEEP_WAKEUP_TIMER) {

    if (!rtc_day_mode) {
      // ── 08:00: nacht→dag ──
      Serial.println("[WF] 08:00 wakeup — dag start");
      rtc_day_mode = true;
      bool ok = wifi_connect_router();
      if (ok) do_ntp();
      // Keepalive sturen (ook als 0 pulsen — bewijst dat device leeft)
      if (ok) espnow_send(true);
      uint32_t s = ok ? secs_until(HOUR_END) : (uint32_t)(HOUR_END - HOUR_START) * 3600;
      sleep_dag(s);

    } else {
      // ── 20:00: dag→nacht ──
      Serial.println("[WF] 20:00 wakeup — nacht start");
      rtc_day_mode = false;
      bool ok = wifi_connect_router();
      if (ok) do_ntp();
      if (ok && rtc_pulses_session > 0) espnow_send(true);  // resterende sessiedata
      uint32_t s = ok ? secs_until(HOUR_START) : (uint32_t)(24 - HOUR_END + HOUR_START) * 3600;
      sleep_nacht(s);
    }
  }

  // ── GPIO WAKEUP (flow gedetecteerd) ──────────────────────────
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    uint32_t pulses = count_pulses(MEASURE_WINDOW_MS);
    rtc_pulses_session += pulses;
    rtc_total_L        += pulses / PULSES_PER_LITER;

    float sess_L = rtc_pulses_session / PULSES_PER_LITER;
    Serial.printf("[WF] Flow: +%.3fL | sessie=%.2fL | totaal=%.3fL\n",
                  pulses / PULSES_PER_LITER, sess_L, rtc_total_L);

    bool session_done = (pulses == 0);           // geen flow meer in meetvenster
    bool force_send   = (sess_L >= SEND_AFTER_LITERS);
    bool moet_sturen  = session_done || force_send;

    bool ok = false;
    if (moet_sturen) {
      ok = wifi_connect_router();
      if (ok) do_ntp();
      if (ok) espnow_send(session_done);
    } else {
      // Geen send: wel NTP voor juiste timer tot 20:00
      ok = wifi_connect_router();
      if (ok) do_ntp();
    }

    uint32_t s = ok ? secs_until(HOUR_END) : (uint32_t)(HOUR_END - HOUR_START) * 3600;
    sleep_dag(s);
  }

  // ── Fallback (zou nooit moeten bereikt worden) ────────────────
  Serial.println("[WF] Onbekende wakeup — nacht fallback");
  sleep_nacht(43200);
}

void loop() {
  // Onbereikbaar — setup() eindigt altijd met esp_deep_sleep_start()
}
