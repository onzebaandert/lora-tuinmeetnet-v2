/*
  SKETCH : CLAUDE_DS3231_SETTIME_V1
  DEVICE : XIAO ESP32-C3
  ROLE   : DS3231 tijd instellen via Serial Monitor

  Gebruik:
    1. Open Serial Monitor op 115200 baud
    2. Typ: HH:MM:SS  (bijv. 14:35:00)  en druk Enter
    3. Klok is gezet — huidig tijd verschijnt elke seconde
*/

#include <Arduino.h>
#include <Wire.h>

#define I2C_SDA  6
#define I2C_SCL  7

static const uint8_t DS_ADDR = 0x68;

static uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static void ds_wreg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(DS_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static bool ds_read(uint8_t &hh, uint8_t &mm, uint8_t &ss) {
  Wire.beginTransmission(DS_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(DS_ADDR, (uint8_t)3);
  if (Wire.available() < 3) return false;
  ss = bcd2dec(Wire.read() & 0x7F);
  mm = bcd2dec(Wire.read() & 0x7F);
  hh = bcd2dec(Wire.read() & 0x3F);
  return true;
}

static bool ds_set(uint8_t hh, uint8_t mm, uint8_t ss) {
  if (hh > 23 || mm > 59 || ss > 59) return false;
  ds_wreg(0x00, dec2bcd(ss));
  ds_wreg(0x01, dec2bcd(mm));
  ds_wreg(0x02, dec2bcd(hh));
  return true;
}

static uint32_t last_print = 0;

void setup() {
  Serial.begin(115200);
  delay(300);
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(100);

  Serial.println();
  Serial.println("=== DS3231 Tijd Instellen ===");

  uint8_t hh, mm, ss;
  if (ds_read(hh, mm, ss)) {
    Serial.printf("Huidige tijd: %02u:%02u:%02u\n", hh, mm, ss);
  } else {
    Serial.println("FOUT: DS3231 niet gevonden op I2C!");
  }

  Serial.println("Typ HH:MM:SS en druk Enter om tijd in te stellen.");
  Serial.println();
}

void loop() {
  // Elke seconde huidige tijd tonen
  if (millis() - last_print >= 1000) {
    last_print = millis();
    uint8_t hh, mm, ss;
    if (ds_read(hh, mm, ss)) {
      Serial.printf(">> %02u:%02u:%02u\n", hh, mm, ss);
    }
  }

  // Serieel invoer verwerken
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();

    // Verwacht formaat: HH:MM:SS
    if (line.length() == 8 && line[2] == ':' && line[5] == ':') {
      uint8_t hh = (uint8_t)line.substring(0, 2).toInt();
      uint8_t mm = (uint8_t)line.substring(3, 5).toInt();
      uint8_t ss = (uint8_t)line.substring(6, 8).toInt();

      if (ds_set(hh, mm, ss)) {
        Serial.printf("Tijd gezet op %02u:%02u:%02u\n", hh, mm, ss);
      } else {
        Serial.println("FOUT: ongeldige waarden (HH 0-23, MM/SS 0-59)");
      }
    } else if (line.length() > 0) {
      Serial.println("Formaat: HH:MM:SS  (bijv. 14:35:00)");
    }
  }
}
