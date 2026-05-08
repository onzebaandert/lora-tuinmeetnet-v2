// =====================================================
// CLAUDE_WEMOS_MICROWAKEUPPER_V1
// Wemos D1 mini — powerbank alive houden
//
// Hardware aansluitingen:
//   Wemos D0 (GPIO16) ──── RST          (deep sleep wakeup)
//   microWakeUpper VCC ──── 5V
//   microWakeUpper GND ──── GND
//   microWakeUpper OUT ──── IN
//   microWakeUpper OUT ──── weerstand (47–100Ω) ──── GND
//
// Werking:
//   Wemos wekt zichzelf elke 25 seconden via D0→RST.
//   Opstartspanning trekt genoeg stroom → powerbank blijft aan.
//   microWakeUpper geeft extra puls via weerstand als backup.
// =====================================================

void setup() {
  ESP.deepSleep(25UL * 1000000UL);
}

void loop() {}
