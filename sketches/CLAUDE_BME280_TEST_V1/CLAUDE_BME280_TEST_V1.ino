/*
  SKETCH : BME280_TEST_V1
  DEVICE : ESP32-C3 Super Mini
  SENSOR : BME280 op I2C GPIO3 (SDA) + GPIO4 (SCL)

  Probeert adressen 0x76 en 0x77.
*/

#include <Wire.h>
#include <Adafruit_BME280.h>

#define I2C_SDA 3
#define I2C_SCL 4

void setup(){
  Serial.begin(115200);
  delay(500);
  Serial.println("=== BME280 test ===");

  Wire.begin(I2C_SDA, I2C_SCL);

  // I2C scanner
  Serial.println("I2C scan:");
  for(uint8_t a = 1; a < 127; a++){
    Wire.beginTransmission(a);
    if(Wire.endTransmission() == 0)
      Serial.printf("  Gevonden: 0x%02X\n", a);
  }

  // Probeer 0x76
  Adafruit_BME280 bme;
  Serial.println("\nProbeer 0x76...");
  if(bme.begin(0x76)){
    Serial.println("OK op 0x76");
    Serial.printf("  Temp : %.2f C\n",      bme.readTemperature());
    Serial.printf("  Druk : %.2f hPa\n",    bme.readPressure() / 100.0f);
    Serial.printf("  Vocht: %.2f %%\n",     bme.readHumidity());
    return;
  }

  // Probeer 0x77
  Serial.println("Probeer 0x77...");
  if(bme.begin(0x77)){
    Serial.println("OK op 0x77");
    Serial.printf("  Temp : %.2f C\n",      bme.readTemperature());
    Serial.printf("  Druk : %.2f hPa\n",    bme.readPressure() / 100.0f);
    Serial.printf("  Vocht: %.2f %%\n",     bme.readHumidity());
    return;
  }

  Serial.println("BME280 niet gevonden op 0x76 of 0x77.");
  Serial.println("Controleer bedrading en voeding.");
}

void loop(){}
