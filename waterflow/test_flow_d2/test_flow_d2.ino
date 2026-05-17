// Simpele flowtest — YF-201B op D2 (GPIO4)
// Seriële monitor: 115200 baud
// Elke 5 seconden: pulsen, flow L/min, totaal L

#define FLOW_PIN         4
#define PULSES_PER_LITER 450.0f
#define INTERVAL_MS      5000

volatile uint32_t pulses = 0;

void IRAM_ATTR onPulse() { pulses++; }

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== flowtest D2 (GPIO4) ===");
  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), onPulse, FALLING);
}

void loop() {
  delay(INTERVAL_MS);

  noInterrupts();
  uint32_t p = pulses;
  pulses = 0;
  interrupts();

  static float total = 0;
  float liters   = p / PULSES_PER_LITER;
  float flowRate = liters / (INTERVAL_MS / 60000.0f);
  total += liters;

  Serial.printf("pulsen=%u  flow=%.2f L/min  totaal=%.3f L\n", p, flowRate, total);
}
