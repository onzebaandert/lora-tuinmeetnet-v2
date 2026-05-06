/*
  SKETCH : LORA_VV_WATERFLOW_V2
  DEVICE : Seeed XIAO ESP32-C3
  ROLE   : Waterflow — YF-C01 wakeup, YF-201B meting, ESP-NOW naar AGG

  Hardware:
    YF-C01 signaalpin   : GPIO5 (D3) — mechanische flowswitch, gpio wakeup
                          (of testknop: drukknop tussen GPIO5 en GND)
    YF-201B VCC         : T-BAT 5V → MOSFET Drain → YF Rood
    YF-201B GND         : GND
    YF-201B signaalpin  : GPIO4 (D2) — pulstelling via interrupt
    MOSFET gate         : GPIO6 (D4) — HIGH = YF-201B aan
    VBAT                : GPIO3 (A1)

  Werking:
    Deep sleep, wakeup alleen via YF-C01 / testknop (GPIO5 LOW)
    Kraan open → YF-C01 sluit → GPIO wakeup
    MOSFET HIGH → YF-201B aan → tel pulsen
    Geen pulsen meer → sessie klaar → MOSFET LOW
    ESP-NOW sturen elke 2s tot ACK ontvangen (max AGG_TIMEOUT_MIN)
    ACK → deep sleep tot volgende kraan-open

  TEST_MODE:
    Zet #define TEST_MODE 1 voor testen zonder water.
    Drukknop op GPIO5 (tussen GPIO5 en GND) wekt de XIAO.
    Simuleert TEST_ACTIVE_WINDOWS vensters met nep-pulsen (~1L per druk).
    Commentaar TEST_MODE uit voor productie.

  Packet mapping (id=3, FLOW slot in AGG):
    h_x10    = liters deze sessie × 10
    ec_raw   = totaal liters (integer)
    ph_x10   = totaal liters fractie × 1000 (mL)
    case_x10 = 1 (sessie klaar)
    vbat_mv  = accuspanning mV
    flags    = 0xF003
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include "credentials.h"

#define SKETCH_TAG "VV_WATERFLOW_V201"

/* ======= TEST MODE ======================================================= */
#define TEST_MODE 0              // 1 = testknop, 0 = productie (YF-C01)
#define TEST_PULSES_PER_WINDOW  225   // nep-pulsen per venster (~0.5L/venster bij 450 p/L)
#define TEST_ACTIVE_WINDOWS       2   // aantal vensters met pulsen (~1L totaal per druk)
/* ========================================================================= */

/* ======= PINNEN ======= */
#define SWITCH_PIN  5    // GPIO5 = D3 — YF-C01 / testknop (gpio wakeup)
#define FLOW_PIN    4    // GPIO4 = D2 — YF-201B signaal (pulstelling)
#define MOSFET_PIN  6    // GPIO6 = D4 — HIGH = YF-201B aan
#define VBAT_PIN    3    // GPIO3 = A1

/* ======= METING ======= */
#define PULSES_PER_LITER   450.0f
#define MEASURE_WINDOW_MS  500UL   // teltijd per meetronde
#define ZERO_WINDOWS_END   3       // aantal opeenvolgende lege vensters = sessie klaar

/* ======= ESP-NOW ======= */
#define ESPNOW_CHANNEL   6
static const uint8_t AGG_MAC[6] = {0xB0, 0xA6, 0x04, 0x07, 0xA2, 0x80};
#define ACK_WAIT_MS      500
#define RETRY_INTERVAL_MS 2000
#define AGG_TIMEOUT_MIN  70       // max wachttijd op ACK (ruim meer dan 1u AGG cycle)

/* ======= VBAT ======= */
#define VBAT_SAMPLES  12
#define VBAT_FACTOR   3.20f
#define VBAT_CAL      1.000f

/* ======= PACKETS ======= */
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

/* ======= RTC MEMORY ======= */
RTC_DATA_ATTR static bool     rtc_init           = false;
RTC_DATA_ATTR static uint32_t rtc_pulses_session = 0;
RTC_DATA_ATTR static float    rtc_total_L        = 0.0f;
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

/* ======= PULSEN TELLEN ======= */
static volatile uint32_t isr_pulses = 0;
void IRAM_ATTR onPulse() { isr_pulses++; }

static uint32_t count_pulses(uint32_t window_ms) {
#if TEST_MODE
  static uint8_t test_window = 0;
  delay(window_ms);
  if (test_window < TEST_ACTIVE_WINDOWS) {
    test_window++;
    return TEST_PULSES_PER_WINDOW;
  }
  return 0;
#else
  isr_pulses = 0;
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), onPulse, FALLING);
  delay(window_ms);
  detachInterrupt(digitalPinToInterrupt(FLOW_PIN));
  noInterrupts();
  uint32_t cnt = isr_pulses;
  interrupts();
  return cnt;
#endif
}

/* ======= ESP-NOW INIT ======= */
static bool espnow_init() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setChannel(ESPNOW_CHANNEL);
  while (!WiFi.STA.started()) delay(1);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[WF] esp_now_init FAIL");
    return false;
  }
  esp_now_register_recv_cb(on_recv);

  esp_now_peer_info_t p{};
  memcpy(p.peer_addr, AGG_MAC, 6);
  p.channel = ESPNOW_CHANNEL;
  p.encrypt = false;
  esp_now_add_peer(&p);
  return true;
}

/* ======= ESP-NOW SEND MET RETRY TOT ACK ======= */
static void espnow_send_until_ack() {
  if (!espnow_init()) return;

  float    sess_L   = rtc_pulses_session / PULSES_PER_LITER;
  uint16_t vbat_mv  = read_vbat_mv();

  SoilPacket pkt{};
  pkt.magic        = 0xA1;
  pkt.id           = 3;
  pkt.s.session_id = rtc_session_id;
  pkt.s.seq        = ++rtc_seq;
  pkt.s.t_x10      = 0;
  pkt.s.h_x10      = (uint16_t)lroundf(sess_L * 10.0f);
  pkt.s.ec_raw     = (uint16_t)rtc_total_L;
  pkt.s.case_x10   = 1;
  pkt.s.vbat_mv    = vbat_mv;
  pkt.s.ph_x10     = (uint16_t)(fmodf(rtc_total_L, 1.0f) * 1000.0f);
  pkt.s.flags      = 0xF003;
  pkt.s.rsv0       = 0;

  uint32_t deadline = millis() + (uint32_t)AGG_TIMEOUT_MIN * 60000UL;
  uint32_t attempt  = 0;

  while (millis() < deadline) {
    attempt++;
    bool tx_ok  = (esp_now_send(AGG_MAC, (const uint8_t*)&pkt, sizeof(pkt)) == ESP_OK);
    bool ack_ok = tx_ok ? wait_ack(pkt.s.session_id, pkt.s.seq) : false;

    Serial.printf("[WF] TX att=%lu ok=%d | sess=%.2fL tot=%.3fL vbat=%umV\n",
                  (unsigned long)attempt, ack_ok ? 1 : 0,
                  sess_L, rtc_total_L, (unsigned)vbat_mv);

    if (ack_ok) {
      Serial.println("[WF] ACK ontvangen — sessie verzonden");
      rtc_pulses_session = 0;
      rtc_session_id     = esp_random();
      break;
    }

    delay(RETRY_INTERVAL_MS - ACK_WAIT_MS);
  }

  if (rtc_pulses_session > 0) {
    Serial.printf("[WF] Timeout — sessie bewaard in RTC (%.2fL)\n",
                  rtc_pulses_session / PULSES_PER_LITER);
  }

  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
}

/* ======= SLEEP ======= */
static void go_sleep() {
#if TEST_MODE
  Serial.println("[WF] Deep sleep — druk knop op GPIO5 om te wekken");
#else
  Serial.println("[WF] Deep sleep — wacht op YF-C01");
#endif
  Serial.flush();
  digitalWrite(MOSFET_PIN, LOW);
  WiFi.mode(WIFI_OFF);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << SWITCH_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

/* ======= SETUP ======= */
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(MOSFET_PIN, OUTPUT);
  pinMode(FLOW_PIN,   INPUT_PULLUP);
  pinMode(SWITCH_PIN, INPUT_PULLUP);
  digitalWrite(MOSFET_PIN, LOW);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  if (!rtc_init) {
    rtc_session_id = esp_random();
    rtc_init       = true;
    Serial.println("===========================================");
    Serial.println("SKETCH : LORA_VV_WATERFLOW_V2");
    Serial.printf ("TAG    : %s\n", SKETCH_TAG);
#if TEST_MODE
    Serial.println("MODE   : TEST (nep-pulsen, knop op GPIO5)");
#endif
    Serial.printf ("[WF] MAC=%s  sid=0x%08lX\n",
                   WiFi.macAddress().c_str(), (unsigned long)rtc_session_id);
    Serial.println("===========================================");
  }

  // ── EERSTE BOOT ──────────────────────────────────────────────
  if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    Serial.println("[WF] Eerste boot — sleep, wacht op wakeup");

    WiFi.mode(WIFI_STA);
    Serial.printf("[WF] MAC=%s\n", WiFi.macAddress().c_str());
    Serial.println("[WF] >>> Kopieer MAC naar SOIL_MACS[2] in AGG sketch <<<");
    WiFi.mode(WIFI_OFF);
    delay(100);

    go_sleep();
  }

  // ── GPIO WAKEUP ───────────────────────────────────────────────
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
#if TEST_MODE
    Serial.println("[WF] Wakeup: knop ingedrukt — test sessie start");
#else
    Serial.println("[WF] Wakeup: kraan open — meting start");
#endif

    // YF-201B aan (in TEST_MODE irrelevant maar schakel toch)
    digitalWrite(MOSFET_PIN, HIGH);
    delay(50);

    uint8_t zero_count = 0;
    while (zero_count < ZERO_WINDOWS_END) {
      uint32_t pulses = count_pulses(MEASURE_WINDOW_MS);
      if (pulses == 0) {
        zero_count++;
      } else {
        zero_count = 0;
        rtc_pulses_session += pulses;
        rtc_total_L        += pulses / PULSES_PER_LITER;
        Serial.printf("[WF] +%.3fL | sessie=%.2fL | totaal=%.3fL\n",
                      pulses / PULSES_PER_LITER,
                      rtc_pulses_session / PULSES_PER_LITER,
                      rtc_total_L);
      }
    }

    digitalWrite(MOSFET_PIN, LOW);
    Serial.printf("[WF] Sessie klaar: %.2fL | totaal: %.3fL\n",
                  rtc_pulses_session / PULSES_PER_LITER, rtc_total_L);

    espnow_send_until_ack();
    go_sleep();
  }

  // ── Fallback ─────────────────────────────────────────────────
  Serial.println("[WF] Onbekende wakeup — sleep");
  go_sleep();
}

void loop() {
  // Onbereikbaar — setup() eindigt altijd met go_sleep()
}
