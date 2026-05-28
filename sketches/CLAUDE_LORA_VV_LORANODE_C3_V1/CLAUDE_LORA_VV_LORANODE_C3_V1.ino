#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include "esp_sleep.h"

// **LORA_VV_LORANODE_C3_V1

/* ================= CONFIG ================= */
#define ESPNOW_WIFI_CHANNEL 6

#define I2C_SDA   6
#define I2C_SCL   7
#define WAKE_PIN  2

#define INTERVAL_MINUTES 10
#define MINUTE_PHASE     0
#define WAKE_SECOND      5
#define LISTEN_MAX_MS    40000UL

#define UART_TX_PIN      9
#define UART_BAUD        115200

#define MOSFET_PIN       5
#define MOSFET_ACTIVE_LOW false

#define HELTEC_BOOT_MS   15000
#define HELTEC_POST_MS   20000

#define FILTER_AGG_MAC   1
static const uint8_t AGG_MAC[6] = {0xB0,0xA6,0x04,0x07,0xA2,0x80};

/* ================= VBAT ================= */
#define VBAT_PIN_C3    3
#define VBAT_N_SAMPLES 12
static const float VBAT_RATIO = 1.478f; // gemeten: 2700mV op ADC bij 3990mV batterij
static const float VBAT_CAL   = 1.0f;   // kalibratie factor

static uint32_t read_vbat_mv() {
  uint32_t sum = 0;
  for (int i = 0; i < VBAT_N_SAMPLES; i++) {
    sum += analogReadMilliVolts(VBAT_PIN_C3);
  }
  float avg_mv = (float)sum / VBAT_N_SAMPLES;
  return (uint32_t)(avg_mv * VBAT_RATIO * VBAT_CAL + 0.5f);
}

/* ========================================== */

static void mosfet_init_off() {
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, MOSFET_ACTIVE_LOW ? HIGH : LOW);
}
static void mosfet_on()  { digitalWrite(MOSFET_PIN, MOSFET_ACTIVE_LOW ? LOW : HIGH); }
static void mosfet_off() { digitalWrite(MOSFET_PIN, MOSFET_ACTIVE_LOW ? HIGH : LOW); }

/* ===== RTC / DS3231 ===== */
static const uint8_t DS3231_ADDR = 0x68;

static uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static void ds_wreg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t ds_rreg(uint8_t reg) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(DS3231_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

static bool ds_read_hms(uint8_t &hh, uint8_t &mm, uint8_t &ss) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(DS3231_ADDR, (uint8_t)3);
  if (Wire.available() < 3) return false;
  ss = bcd2dec(Wire.read() & 0x7F);
  mm = bcd2dec(Wire.read() & 0x7F);
  hh = bcd2dec(Wire.read() & 0x3F);
  return true;
}

static float ds_read_temp_c() {
  uint8_t msb = ds_rreg(0x11);
  uint8_t lsb = ds_rreg(0x12);
  int8_t t = (int8_t)msb;
  float frac = ((lsb >> 6) & 0x03) * 0.25f;
  return (float)t + frac;
}

static void ds_clear_flags() {
  uint8_t st = ds_rreg(0x0F);
  st &= ~(uint8_t)0x03;
  ds_wreg(0x0F, st);
}

static void ds_config_int_mode() {
  uint8_t ctrl = ds_rreg(0x0E);
  ctrl |=  (1 << 2);
  ctrl &= ~(1 << 1);
  ctrl &= ~(1 << 0);
  ds_wreg(0x0E, ctrl);
}

static void ds_enable_a1_int() {
  uint8_t ctrl = ds_rreg(0x0E);
  ctrl |= (1 << 2);
  ctrl |= (1 << 0);
  ds_wreg(0x0E, ctrl);
}

static void ds_set_alarm1_hms_ignore_date(uint8_t hh, uint8_t mm, uint8_t ss) {
  ds_wreg(0x07, (dec2bcd(ss) & 0x7F));
  ds_wreg(0x08, (dec2bcd(mm) & 0x7F));
  ds_wreg(0x09, (dec2bcd(hh) & 0x3F));
  ds_wreg(0x0A, 0x80);
  ds_clear_flags();
  ds_enable_a1_int();
  delay(5);
  if (digitalRead(WAKE_PIN) == LOW) ds_clear_flags();
}

static void compute_next_phase_sec(uint8_t hh, uint8_t mm, uint8_t ss,
                                   uint8_t &oh, uint8_t &om, uint8_t &os) {
  const int nowMinAbs = (int)hh * 60 + (int)mm;
  for (int d = 0; d <= (24 * 60); d++) {
    const int candMinAbs = nowMinAbs + d;
    const int ch = (candMinAbs / 60) % 24;
    const int cm = candMinAbs % 60;
    int diff = cm - MINUTE_PHASE;
    if (diff < 0) diff += 60;
    if ((diff % INTERVAL_MINUTES) != 0) continue;
    if (d == 0 && WAKE_SECOND <= ss) continue;
    oh = (uint8_t)ch;
    om = (uint8_t)cm;
    os = (uint8_t)WAKE_SECOND;
    return;
  }
  oh = hh;
  om = (uint8_t)((mm + INTERVAL_MINUTES) % 60);
  os = (uint8_t)WAKE_SECOND;
}

/* ===== helpers ===== */
static uint32_t rd_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static int16_t rd_i16_le(const uint8_t *p) {
  return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t rd_u16_le(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool mac_eq6(const uint8_t a[6], const uint8_t b[6]) {
  return memcmp(a, b, 6) == 0;
}

/* ===== uplink state ===== */
static volatile bool got_uplink = false;
static uint8_t uplink_buf[141];
static int uplink_rssi = 0;

/*
  Soil22 layout:
  0  session_id (4)
  4  seq        (2)
  6  t_x10      (2)
  8  h_x10      (2)
  10 ec_raw     (2)
  12 case_x10   (2)
  14 vbat_mv    (2)
  16 ph_x10     (2)
  18 flags      (2)
  20 rsv0       (2)
*/
static void parse_item22(const uint8_t *it,
                         uint32_t &sid, uint16_t &seq,
                         float &t, float &h, uint16_t &ec,
                         float &caseT, uint16_t &vbat_mv,
                         uint16_t &ph_x10, uint16_t &flags, uint16_t &rsv0) {
  sid     = rd_u32_le(it + 0);
  seq     = rd_u16_le(it + 4);
  t       = rd_i16_le(it + 6) / 10.0f;
  h       = rd_u16_le(it + 8) / 10.0f;
  ec      = rd_u16_le(it + 10);
  caseT   = rd_i16_le(it + 12) / 10.0f;
  vbat_mv = rd_u16_le(it + 14);
  ph_x10  = rd_u16_le(it + 16);
  flags   = rd_u16_le(it + 18);
  rsv0    = rd_u16_le(it + 20);
}

/* ===== ESP-NOW callback ===== */
void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !data || len <= 0) return;

#if FILTER_AGG_MAC
  if (!mac_eq6(info->src_addr, AGG_MAC)) return;
#endif

  int rssi = 0;
  if (info->rx_ctrl) rssi = info->rx_ctrl->rssi;

  if (len == 141 && !got_uplink) {
    memcpy(uplink_buf, data, 141);
    uplink_rssi = rssi;
    got_uplink = true;
  }
}

static bool start_espnow_rx() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  delay(20);

  if (esp_now_init() != ESP_OK) {
    return false;
  }

  esp_now_register_recv_cb(on_recv);
  return true;
}

static void stop_espnow_rx() {
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
}

static void uart_send_line(const char *line) {
  Serial1.print(line);
  Serial1.print("\r\n");
  Serial1.flush();
  Serial.printf("[UART] sent len=%u\n", (unsigned)strlen(line));
}

static void go_sleep_c3_gpio() {
  pinMode(WAKE_PIN, INPUT_PULLUP);
  uint64_t mask = 1ULL << WAKE_PIN;
  esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
  WiFi.mode(WIFI_OFF);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  

Serial.begin(115200);
delay(2000);

Serial.println();
Serial.println("=== LORA_VV_LORANODE_C3_V1 ===");

esp_reset_reason_t reason = esp_reset_reason();

Serial.print("Reset reason: ");
Serial.println((int)reason);

switch(reason) {
  case ESP_RST_POWERON:   Serial.println("POWERON"); break;
  case ESP_RST_EXT:       Serial.println("EXTERNAL RESET"); break;
  case ESP_RST_SW:        Serial.println("SW RESET"); break;
  case ESP_RST_PANIC:     Serial.println("PANIC (CRASH)"); break;
  case ESP_RST_INT_WDT:   Serial.println("INT WDT"); break;
  case ESP_RST_TASK_WDT:  Serial.println("TASK WDT"); break;
  case ESP_RST_WDT:       Serial.println("OTHER WDT"); break;
  case ESP_RST_BROWNOUT:  Serial.println("BROWNOUT ⚡"); break;
  case ESP_RST_DEEPSLEEP: Serial.println("WAKE FROM DEEPSLEEP"); break;
  default:                Serial.println("UNKNOWN"); break;
}

  

  Serial.println();
  Serial.println("=== LORA_VV_LORANODE_C3_V1 ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  pinMode(WAKE_PIN, INPUT_PULLUP);
  ds_config_int_mode();
  ds_clear_flags();

  mosfet_init_off();
  mosfet_off();

  got_uplink = false;

  if (!start_espnow_rx()) {
    Serial.println("[ESPNOW] init FAILED");
  } else {
    Serial.printf("[ESPNOW] listening ch=%d\n", ESPNOW_WIFI_CHANNEL);
  }

  Serial.printf("[C3] MAC=%s wake=%d\n",
                WiFi.macAddress().c_str(),
                (int)esp_sleep_get_wakeup_cause());

  uint32_t t0 = millis();
  while (!got_uplink && (millis() - t0 < LISTEN_MAX_MS)) {
    delay(2);
  }

  if (!got_uplink) {
    Serial.println("[C3] no uplink received");
    stop_espnow_rx();
  } else {
    Serial.println("[C3] uplink received");
    stop_espnow_rx();

    const uint32_t agg_seq = rd_u32_le(uplink_buf + 4);
    const uint8_t  count   = uplink_buf[8];
    const uint8_t *items   = uplink_buf + 9;

    uint32_t sid0;
    uint16_t seq0, aggPh10;
    float aggTemp, aggHum, aggCaseT;
    uint16_t aggEc, aggVbat, aggFlags, aggRsv0;

    parse_item22(items + 0 * 22, sid0, seq0, aggTemp, aggHum, aggEc,
                 aggCaseT, aggVbat, aggPh10, aggFlags, aggRsv0);

    float meshCaseT = ds_read_temp_c();
    uint32_t vbat_mv = read_vbat_mv();
    Serial.printf("[VBAT] %u mV\n", (unsigned)vbat_mv);

    char out[512];
    int n = 0;

    n += snprintf(out + n, sizeof(out) - n,
                  "{\"a\":%lu,\"r\":%d,\"av\":%u,\"ac\":%.1f,\"mc\":%.1f,\"s\":[",
                  (unsigned long)agg_seq,
                  uplink_rssi,
                  (unsigned)aggVbat,
                  aggCaseT,
                  meshCaseT);

    bool first = true;
    uint16_t flow_ec = 0;
    bool have_flow = false;
    uint16_t bh_lux = 0;
    float bh_caseT = 0;
    uint16_t bh_vbat = 0;
    int8_t bh_rssi = 0;
    bool have_bh = false;
    uint16_t st_lux = 0;
    float st_temp = 0, st_hum = 0, st_caseT = 0;
    uint16_t st_vbat = 0;
    int8_t st_rssi = 0;
    bool have_st = false;

    for (uint8_t si = 1; si < count && si < 6; si++) {
      uint32_t sid;
      uint16_t seq, ph10;
      float t, h, caseT;
      uint16_t ec, vbat, flags, rsv0;

      parse_item22(items + si * 22, sid, seq, t, h, ec, caseT, vbat, ph10, flags, rsv0);
      int8_t  soil_rssi   = (int8_t)(rsv0 & 0xFF);
      uint8_t sensor_slot = (rsv0 >> 8) & 0xFF;  // 1=SOIL1, 2=SOIL2, 3=FLOW, 4=BH1750

      if (sensor_slot == 1) {
        // SOIL1: [t, h, ec/10, caseT, vbat, rssi, ph]
        n += snprintf(out + n, sizeof(out) - n,
                      "%s[%.1f,%.1f,%.1f,%.1f,%u,%d,%.1f]",
                      first ? "" : ",",
                      t, h, ec / 10.0f, caseT, (unsigned)vbat, (int)soil_rssi, ph10 / 10.0f);
        first = false;
      } else if (sensor_slot == 3) {
        // FLOW: [flowRate, totalLiters, vbat, rssi]
        float flowRate    = h / 10.0f;
        float totalLiters = ec + ph10 / 1000.0f;
        flow_ec = ec;
        have_flow = true;
        n += snprintf(out + n, sizeof(out) - n,
                      "%s[%.1f,%.3f,%u,%d]",
                      first ? "" : ",",
                      flowRate, totalLiters, (unsigned)vbat, (int)soil_rssi);
        first = false;
      } else if (sensor_slot == 4) {
        // BH1750: verzamel data, niet in "s" array
        bh_lux   = ec;
        bh_caseT = caseT;
        bh_vbat  = vbat;
        bh_rssi  = soil_rssi;
        have_bh  = true;
        continue;
      } else if (sensor_slot == 5) {
        // STNST: sensorstation (BH1750+SHT3x+SI7021), niet in "s" array
        st_lux   = ec;
        st_temp  = t;
        st_hum   = h;
        st_caseT = caseT;
        st_vbat  = vbat;
        st_rssi  = soil_rssi;
        have_st  = true;
        continue;
      } else {
        // SOIL2: [t, h, caseT, vbat, rssi]
        n += snprintf(out + n, sizeof(out) - n,
                      "%s[%.1f,%.1f,%.1f,%u,%d]",
                      first ? "" : ",",
                      t, h, caseT, (unsigned)vbat, (int)soil_rssi);
        first = false;
      }

      if (n > (int)sizeof(out) - 60) break;
    }

    if (have_flow)
      n += snprintf(out + n, sizeof(out) - n, "],\"wf\":%u", (unsigned)flow_ec);
    else
      n += snprintf(out + n, sizeof(out) - n, "]");

    if (have_bh)
      n += snprintf(out + n, sizeof(out) - n, ",\"bh\":[%u,%.1f,%u,%d]",
                    (unsigned)bh_lux, bh_caseT, (unsigned)bh_vbat, (int)bh_rssi);

    if (have_st)
      n += snprintf(out + n, sizeof(out) - n, ",\"st\":[%u,%.1f,%.1f,%.1f,%u,%d]",
                    (unsigned)st_lux, st_temp, st_hum, st_caseT, (unsigned)st_vbat, (int)st_rssi);

    n += snprintf(out + n, sizeof(out) - n, ",\"lnC3-vbat\":%u}", (unsigned)vbat_mv);
    out[sizeof(out) - 1] = '\0';

    Serial.printf("[OUT] %s\n", out);
    Serial.printf("[OUT] len=%u\n", (unsigned)strlen(out));

    mosfet_on();
    Serial.printf("[PWR] HELTEC ON, boot wait=%dms\n", HELTEC_BOOT_MS);

    Serial1.begin(UART_BAUD, SERIAL_8N1, -1, UART_TX_PIN);
    delay(HELTEC_BOOT_MS);

    uart_send_line(out);

    delay(HELTEC_POST_MS);
    pinMode(UART_TX_PIN, INPUT);

    mosfet_off();
    Serial.println("[PWR] HELTEC OFF");
  }

  uint8_t hh, mm, ss;
  if (ds_read_hms(hh, mm, ss)) {
    uint8_t th, tm, ts;
    compute_next_phase_sec(hh, mm, ss, th, tm, ts);
    Serial.printf("[RTC] Now %02u:%02u:%02u -> Alarm %02u:%02u:%02u\n",
                  hh, mm, ss, th, tm, ts);
    ds_set_alarm1_hms_ignore_date(th, tm, ts);
  } else {
    Serial.println("[RTC] DS3231 read failed, timer fallback");
    esp_sleep_enable_timer_wakeup((uint64_t)INTERVAL_MINUTES * 60ULL * 1000000ULL);
  }

  go_sleep_c3_gpio();
}

void loop() {}