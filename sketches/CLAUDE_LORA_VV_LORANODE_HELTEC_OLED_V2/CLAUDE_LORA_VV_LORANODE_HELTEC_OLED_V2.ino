#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <Wire.h>

//** LORA_VV_LORANODE_HELTEC_V2
// Toegevoegd: OLED display toont TX status
// Toegevoegd: batlev (hv) injecteren in JSON payload voor LoRa TX

/* ================= HELTEC WIFI LORA 32 V3 / SX1262 PINS ================= */
static const int PIN_NSS  = 8;
static const int PIN_DIO1 = 14;
static const int PIN_NRST = 12;
static const int PIN_BUSY = 13;
static const int PIN_SCK  = 9;
static const int PIN_MISO = 11;
static const int PIN_MOSI = 10;

/* ================= OLED PINS (Heltec LoRa 32 V3) ================= */
static const int OLED_SDA  = 17;
static const int OLED_SCL  = 18;
static const int OLED_RST  = 21;

/* ================= BATTERY ================= */
static const int VBAT_PIN      = 1;     // ADC1_CH0, ingebouwde spanningsdeler
static const int ADC_CTRL_PIN  = 37;    // LOW = ADC ingeschakeld, HIGH = uit
#define VBAT_N_SAMPLES  12
static const float VBAT_RATIO  = 4.9f; // ingebouwde deler: 390k/100k → (390+100)/100
static const float VBAT_CAL    = 1.0f; // kalibratie factor (bijsturen indien nodig)

uint32_t read_vbat_mv() {
  pinMode(ADC_CTRL_PIN, OUTPUT);
  digitalWrite(ADC_CTRL_PIN, LOW);
  delay(5);
  uint32_t sum = 0;
  for (int i = 0; i < VBAT_N_SAMPLES; i++) {
    sum += analogReadMilliVolts(VBAT_PIN);
    delay(2);
  }
  digitalWrite(ADC_CTRL_PIN, HIGH);
  float avg_mv = (float)sum / VBAT_N_SAMPLES;
  return (uint32_t)(avg_mv * VBAT_RATIO * VBAT_CAL + 0.5f);
}

/* ================= UART ================= */
static const int  UART_RX_PIN  = 46;
static const int  UART_TX_PIN  = -1;
static const long UART_BAUD    = 115200;

/* ================= LORA SETTINGS ================= */
static const float   LORA_FREQ = 868.0;
static const float   LORA_BW   = 125.0;
static const uint8_t LORA_SF   = 11;
static const uint8_t LORA_CR   = 5;
static const uint8_t LORA_SYNC = 0x12;
static const int8_t  LORA_PWR  = 14;

/* ================= TX BEHAVIOR ================= */
static const int MAX_LINE     = 240;
static const int UART_WAIT_MS = 25000;
static const int TX_COPIES    = 1;
static const int TX_GAP_MS    = 4000;

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY);
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, OLED_SCL, OLED_SDA, OLED_RST);

char linebuf[MAX_LINE];
static uint32_t tx_count = 0;

/* ================= OLED ================= */
void oled_show(const char* line1, const char* line2 = "", const char* line3 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.drawStr(0, 20, line1);
  u8g2.drawStr(0, 42, line2);
  u8g2.drawStr(0, 62, line3);
  u8g2.sendBuffer();
}

/* ================= LORA ================= */
bool setupLoRa() {
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  delay(50);
  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_PWR);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] begin failed, code=%d\n", state);
    oled_show("LORA FAIL", ("code=" + String(state)).c_str());
    return false;
  }
  Serial.printf("[LORA] %.1f MHz BW=%.0f SF=%u CR=4/%u SYNC=0x%02X PWR=%d\n",
                LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_PWR);
  oled_show("LORA OK", "868MHz SF11");
  return true;
}

/* ================= UART ================= */
bool readLineFromUart(char *out, size_t outSize, uint32_t timeoutMs) {
  size_t idx = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == '\r') continue;
      if (c == '\n') {
        if (idx > 0) {
          out[idx] = '\0';
          return true;
        }
        continue;
      }
      if (idx < outSize - 1) out[idx++] = c;
    }
    delay(2);
  }
  out[0] = '\0';
  return false;
}

// Injecteert "lnHelTuin-vbat":vbat_mv voor de afsluitende } van de JSON
static void inject_batlev(const char* src, char* dst, size_t dstSize, uint32_t vbat_mv) {
  size_t len = strlen(src);
  if (len > 0 && src[len - 1] == '}' && len + 28 < dstSize) {
    snprintf(dst, dstSize, "%.*s,\"lnHelTuin-vbat\":%lu}", (int)(len - 1), src, (unsigned long)vbat_mv);
  } else {
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
  }
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  delay(500);

  // OLED opstarten
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  delay(50);
  digitalWrite(OLED_RST, HIGH);
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  oled_show("HELTEC V2", "opstarten...");

  Serial.println();
  Serial.println("=== LORA_VV_LORANODE_HELTEC_V2 ===");
  Serial.printf("[UART] RX=%d baud=%ld\n", UART_RX_PIN, UART_BAUD);

  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  if (!setupLoRa()) {
    while (true) delay(1000);
  }

  oled_show("Wachten op", "UART data...");
  Serial.println("[UART] waiting for one line...");

  bool ok = readLineFromUart(linebuf, sizeof(linebuf), UART_WAIT_MS);

  if (!ok) {
    Serial.println("[UART] no line received");
    oled_show("UART timeout", "geen data");
    return;
  }

  Serial.printf("[UART] got len=%u\n", (unsigned)strlen(linebuf));
  Serial.println(linebuf);

  uint32_t vbat_mv = read_vbat_mv();
  Serial.printf("[BAT] %u mV (%.2f V)\n", vbat_mv, vbat_mv / 1000.0f);

  char txbuf[MAX_LINE + 32];
  inject_batlev(linebuf, txbuf, sizeof(txbuf), vbat_mv);
  Serial.printf("[OUT] %s\n", txbuf);

  oled_show("UART OK", "verzenden...");

  for (int i = 1; i <= TX_COPIES; i++) {
    int state = radio.transmit(txbuf);
    Serial.printf("[LORA] TX %d/%d state=%d\n", i, TX_COPIES, state);

    if (state == RADIOLIB_ERR_NONE) {
      tx_count++;
      char regel1[24];
      char regel2[24];
      snprintf(regel1, sizeof(regel1), "TX OK  #%lu", (unsigned long)tx_count);
      snprintf(regel2, sizeof(regel2), "%.6s...", txbuf);
      oled_show(regel1, regel2, "868MHz SF11");
      Serial.printf("[LORA] TX OK, totaal=%lu\n", (unsigned long)tx_count);
    } else {
      char fout[24];
      snprintf(fout, sizeof(fout), "TX FAIL code=%d", state);
      oled_show("TX MISLUKT", fout);
    }

    if (i < TX_COPIES) delay(TX_GAP_MS);
  }

  Serial.println("[LORA] done");
}

/* ================= LOOP ================= */
void loop() {
  delay(50);
}
