# LoRa Tuinmeetnet v2 — Technische Referentie

Volledige technische specificatie voor ontwikkelaars en makers die het systeem willen nabouwen, uitbreiden of begrijpen.

Zie [README.md](README.md) voor de algemene beschrijving en architectuuroverzicht.

---

## Inhoudsopgave

1. [Architectuur & dataflow](#architectuur--dataflow)
2. [LoRa parameters](#lora-parameters)
3. [ESP-NOW & UART](#esp-now--uart)
4. [Pakketformaat](#pakketformaat)
5. [Deep sleep & RTC wakeup](#deep-sleep--rtc-wakeup)
6. [Modbus sensoren](#modbus-sensoren)
7. [Waterflow meting](#waterflow-meting)
8. [Node-RED & InfluxDB](#node-red--influxdb)
9. [MQTT JSON-formaat](#mqtt-json-formaat)
10. [InfluxDB schema](#influxdb-schema)
11. [Stroomvoorziening & powerbank workaround](#stroomvoorziening--powerbank-workaround)
12. [Nieuwe node toevoegen](#nieuwe-node-toevoegen)
13. [Arduino omgeving](#arduino-omgeving)

---

## Architectuur & dataflow

```
SOIL1 (id=1) ──┐  ESP-NOW ch6     UART 115200       LoRa 868MHz SF11
SOIL2 (id=2) ──┤  SoilPacket  →   AggUplink141  →   ~97 effectieve bytes
BH1750 (id=4)──┤  28 bytes        141 bytes
FLOW  (id=3) ──┘
                  AGG           LoRaNode C3       Heltec TX ~~~→ Heltec RX
                  XIAO C3       XIAO C3           LoRa32 V3      LoRa32 V3
                                                                      │
                                                                   MQTT
                                                              tuin/lora/test
                                                                      │
                                                               Node-RED
                                                               (parsing + schaling)
                                                                      │
                                                               InfluxDB2
                                                               bucket: tuinsensoren_v4
                                                                      │
                                                               Grafana :3000
                                                               → Cloudflare tunnel
                                                               → grafana.biobiejo.nl
```

Wemos DHT22 → WiFi → MQTT `tuin/mqtt/dht22` (parallel pad, geen LoRa)

---

## LoRa parameters

Identiek geconfigureerd in TX (`CLAUDE_LORA_VV_LORANODE_HELTEC_OLED_V2`) en RX (`CLAUDE_LORA_VV_RECEIVER_MQTT_HELTEC_V3`):

| Parameter | Waarde |
|---|---|
| Frequentie | 868.0 MHz |
| Bandwidth | 125.0 kHz |
| Spreading Factor | 11 |
| Coding Rate | 4/5 (CR=5 in RadioLib) |
| Sync Word | 0x12 |
| TX Power | 14 dBm |
| Radio chip | SX1262 (Heltec LoRa32 V3) |

SF11 op 125 kHz geeft een hoge link budget maar beperkte throughput (~537 bps netto). Eén 97-byte pakket per 5 minuten past ruim binnen de airtime.

---

## ESP-NOW & UART

### ESP-NOW (tuin → AGG)

| Parameter | Waarde |
|---|---|
| WiFi kanaal | 6 |
| Max payload | 250 bytes (ESP-NOW limiet) |
| Gebruikte payload | 28 bytes (SoilPacket) |
| Magic byte | 0xA1 |
| Richting | Sensor → AGG (unicast, AGG MAC hardcoded per node) |

### UART (LoRaNode C3 → Heltec TX)

| Parameter | Waarde |
|---|---|
| Baud rate | 115200 |
| Formaat | SERIAL_8N1 |
| Protocol | JSON string, newline-terminated |
| Payload | Volledige JSON met alle sensordata |

---

## Pakketformaat

### SoilPacket (28 bytes, ESP-NOW payload)

```cpp
#pragma pack(push, 1)
struct SoilPacket {
  uint8_t  magic;       // 0xA1
  uint8_t  id;          // 1=SOIL1, 2=SOIL2, 3=FLOW, 4=BH1750, 5=SENSORSTATION
  Soil22   data;        // 22 bytes (zie hieronder)
  uint8_t  pad[4];      // padding tot 28 bytes
};
#pragma pack(pop)
```

### Soil22 (22 bytes, embedded in SoilPacket en AggUplink141)

```cpp
struct __attribute__((packed)) Soil22 {
  uint32_t session_id;  // unieke wake-sessie ID
  uint16_t seq;         // volgnummer binnen sessie
  int16_t  t_x10;       // bodemtemp × 10 (°C)
  uint16_t h_x10;       // vochtigheid × 10 (%)
  uint16_t ec_raw;      // EC (µS/cm) of lux (BH1750)
  int16_t  case_x10;    // behuizingstemperatuur × 10 (°C)
  uint16_t vbat_mv;     // batterijspanning (mV)
  uint16_t ph_x10;      // pH × 10 (0 = niet beschikbaar)
  uint16_t flags;       // statusvlaggen
  uint16_t rsv0;        // low byte = RSSI als int8_t (dBm)
};
// static_assert(sizeof(Soil22) == 22);
```

### AggUplink141 (141 bytes, volledige uplink van AGG naar LoRaNode C3)

```cpp
struct __attribute__((packed)) AggUplink141 {
  uint32_t agg_sid;     // aggregator session ID
  uint32_t agg_seq;     // aggregator volgnummer
  uint8_t  count;       // aantal gevulde items (max 6)
  Soil22   items[6];    // 6 × 22 = 132 bytes sensordata
};
// Totaal: 4 + 4 + 1 + 132 = 141 bytes
// static_assert(sizeof(AggUplink141) == 141);
```

De LoRaNode C3 pakt deze struct uit en bouwt er een JSON van die via UART naar de Heltec TX gaat. De Heltec TX voegt zijn eigen batterijspanning toe en zendt via LoRa.

---

## Deep sleep & RTC wakeup

### DS3231 alarm-gebaseerde wakeup

De DS3231 (I2C adres 0x68) genereert een actief-laag signaal op INT/SQW bij Alarm 1.

**GPIO-wakeup op XIAO ESP32-C3:**
```cpp
#define WAKE_PIN 2  // D2 = GPIO2, verbonden met DS3231 INT/SQW

pinMode(WAKE_PIN, INPUT_PULLUP);
esp_deep_sleep_enable_gpio_wakeup(1ULL << WAKE_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
esp_deep_sleep_start();
```

**Alarm 1 configuratie (I2C direct):**
```
Register 0x07 (A1M1): alarm seconden
Register 0x08 (A1M2): alarm minuten
Register 0x09 (A1M3): alarm uren
Register 0x0A (A1M4): alarm datum
Register 0x0E (Control): bit 2=INTCN, bit 0=A1IE
Register 0x0F (Status): bit 0=A1F wissen na wakeup
```

Alarm wordt na elke wakeup opnieuw gezet op `now + interval` seconden. Het interval is standaard 300 seconden (5 minuten), instelbaar in de sketch.

**Temperatuur uitlezen van DS3231:**
```cpp
// Register 0x11 = MSB (int8_t), register 0x12 = LSB (bits 7-6 = kwartgraden)
int8_t  msb = wire_read(0x11);
uint8_t lsb = wire_read(0x12);
float temp = msb + ((lsb >> 6) & 0x03) * 0.25;
```

### Timing cyclus (5 minuten, wake second binnen het interval)

| Node | Wake second | Reden |
|---|---|---|
| AGG + LoRaNode C3 | :05 | Eerst wakker, klaar om te ontvangen |
| SOIL1 | :08 | Na AGG, geeft AGG 3s opstarttijd |
| BH1750 | :08 | Zelfde window als SOIL1 |
| SOIL2 | :16 | Na SOIL1, voorkomt ESP-NOW collision |

---

## Modbus sensoren

Communicatie via RS485 op UART1 van de XIAO ESP32-C3, library: **ModbusMaster**.

### SOIL1 (CWT-sensor)

| Parameter | Waarde |
|---|---|
| Slave adres | 1 |
| Baud rate | 4800 |
| Functie code | 0x03 (Read Holding Registers) |
| Start register | 0x0000 |
| Aantal registers | 7 |

Register mapping (0-indexed via `getResponseBuffer()`):

| Register | Inhoud | Schaling |
|---|---|---|
| 0 | Bodemvochtigheid | ÷ 10 → % |
| 1 | Bodemtemperatuur (int16) | ÷ 10 → °C |
| 2 | EC (elektrische geleidbaarheid) | µS/cm, raw |
| 3 | pH | ÷ 10 |
| 4–6 | Overig / niet gebruikt | — |

### SOIL2 (ZTS-sensor)

| Parameter | Waarde |
|---|---|
| Slave adres | 1 |
| Baud rate | 4800 |
| Functie code | 0x04 (Read Input Registers) |
| Start register | 0x0000 |
| Aantal registers | 2 |

| Register | Inhoud | Schaling |
|---|---|---|
| 0 | Bodemvochtigheid | ÷ 10 → % |
| 1 | Bodemtemperatuur (int16) | ÷ 10 → °C |

---

## Waterflow meting

Sensor: **YF-C01** (of YF-201B), Hall-effect pulssensor.

| Parameter | Waarde |
|---|---|
| Pin | GPIO4 (D2) |
| Interrupt | FALLING edge |
| Pulsen per liter | 450 |
| Formule | F(Hz) = 7.5 × Q(L/min) → 450 pulsen/liter |
| Meetvenster | 500 ms per sample |
| Sessie-einde | 3 opeenvolgende lege meetvensters |
| Communicatie | ESP-NOW naar AGG (event-driven, geen RTC slaap) |
| Max wakker | 8 minuten als AGG niet antwoordt |

De waterflow node slaapt in deep sleep tot de flow sensor een puls genereert (ext1 wakeup). Na elke stroomsessie stuurt hij het totaal via ESP-NOW en bevestigt via ACK (NVS opslag van totaal liters).

---

## Node-RED & InfluxDB

### Node-RED flows

Relevante bestanden in `/nodered/`:

| Bestand | Functie |
|---|---|
| `influx_function.js` | Parseert `tuin/lora/test` JSON → InfluxDB line protocol |
| `influx_function_dht22.js` | Parseert `tuin/mqtt/dht22` → InfluxDB |
| `influx_function_receiver_status.js` | Parseert Heltec RX statusberichten → InfluxDB |

### Spanningsschaling in Node-RED

De raw ADC-waarden in mV worden gecorrigeerd met empirisch bepaalde factoren:

| Veld | Factor |
|---|---|
| vbat_agg | × 0.9694 |
| lnHel_vbat | × 1.1818 |
| lnC3_vbat | × 1.0260 |
| mqtt_vbat (TBAT) | × 1.0322 |
| soil2 vbat | × 1.0323 |

### InfluxDB schrijven

```
URL:  http://localhost:8086/api/v2/write
      ?org=tuinmeetnet&bucket=tuinsensoren_v4&precision=ns
Auth: Bearer <token>
Body: InfluxDB line protocol
Timestamp: Date.now() × 1_000_000  (ms → ns)
```

---

## MQTT JSON-formaat

Topic: `tuin/lora/test`

```json
{
  "a":              <agg_seq>,
  "r":              <rssi_lora_intern>,
  "av":             <vbat_agg_mV>,
  "ac":             <temp_agg_C>,
  "mc":             <temp_mesh_C>,
  "lr":             <rssi_heltec_dBm>,
  "ls":             <snr_heltec_dB>,
  "lnHelTuin-vbat": <vbat_heltec_mV>,
  "lnC3-vbat":      <vbat_c3_mV>,
  "lnHelTuin-tbat": <vbat_tbat_mV>,
  "wf":             <waterflow_pulsen>,
  "s": [
    [temp, hum, ec, caseTemp, vbat, rssi, ph],   // SOIL1 — 7 velden
    [temp, hum, caseTemp, vbat, rssi],            // SOIL2 — 5 velden
    [flowRate, totalLiters, vbat, rssi]           // FLOW  — 4 velden
  ],
  "bh": [lux, caseTemp, vbat, rssi],             // BH1750 — 4 velden
  "st": [lux, temp, hum, caseTemp, vbat, rssi]   // SENSORSTATION — 6 velden
}
```

Node-RED detecteert sensortype op basis van array-lengte: 7=SOIL1, 5=SOIL2, 4=FLOW.

---

## InfluxDB schema

Organisatie: `tuinmeetnet` · Bucket: `tuinsensoren_v4`

### measurement `systeem`
| Field | Type | Eenheid |
|---|---|---|
| agg_seq | integer | — |
| rssi_lora | float | dBm |
| vbat_agg | integer | mV |
| temp_agg | float | °C |
| rssi_heltec | float | dBm |
| snr_heltec | float | dB |
| lnHel_vbat | integer | mV |
| lnC3_vbat | integer | mV |
| mqtt_vbat | integer | mV |
| temp_mesh | float | °C |

### measurement `soil1`
| Field | Type | Eenheid |
|---|---|---|
| temperatuur | float | °C |
| vochtigheid | float | % |
| ec | integer | µS/cm |
| ph | float | — |
| temp_case | float | °C |
| vbat | integer | mV |
| rssi | integer | dBm |

### measurement `soil2`
| Field | Type | Eenheid |
|---|---|---|
| temperatuur | float | °C |
| vochtigheid | float | % |
| temp_case | float | °C |
| vbat | integer | mV |
| rssi | integer | dBm |

### measurement `flow`
| Field | Type | Eenheid |
|---|---|---|
| flow_rate | float | L/min |
| total_liters | float | L |
| vbat | integer | mV |
| rssi | integer | dBm |

### measurement `waterflow`
| Field | Type | Eenheid |
|---|---|---|
| pulsen | integer | — |

### measurement `bh1750`
| Field | Type | Eenheid |
|---|---|---|
| lux | integer | lx |
| temp_case | float | °C |
| vbat | integer | mV |
| rssi | integer | dBm |

### measurement `sensorstation`
| Field | Type | Eenheid |
|---|---|---|
| lux | integer | lx |
| temperatuur | float | °C |
| vochtigheid | float | % |
| temp_case | float | °C |
| vbat | integer | mV |
| rssi | integer | dBm |

### measurement `mqtt_dht22`
| Field | Type | Eenheid |
|---|---|---|
| mqtt_case_temp | float | °C |
| mqtt_hum | float | % |

### measurement `receiver`
| Field | Type | Eenheid |
|---|---|---|
| uptime | float | uren |
| rx_total | integer | — |
| rx_mqtt_ok | integer | — |
| rssi_lora | float | dBm |
| snr_lora | float | dB |
| wifi_rssi | integer | dBm |

---

## Stroomvoorziening & powerbank workaround

### Sensor nodes (solar)

Alle buitensensoren: **LilyGo T-BAT** shield met 18650 cel + 6V zonnepaneel (10×7 cm).

- T-BAT heeft ingebouwde MPPT laadregelaar
- VBAT uitgelezen via ADC met voltage divider; schaalfactor per node empirisch bepaald
- Nodes slapen vrijwel altijd (deep sleep ~4.9 van de 5 minuten)

### Vaste nodes (powerbank)

LoRaNode C3, Heltec TX en Heltec RX draaien op een **Sounix 30000 mAh** powerbank.

**Probleem:** Sounix schakelt automatisch uit bij te laag stroomverbruik.

**Oplossing:** Wemos D1 mini + **microWakeUpper** module:
- Wemos wacht in deep sleep (D0→RST circuit)
- microWakeUpper wekt de Wemos elke 25 seconden
- Weerstand 47–100 Ω van microWakeUpper OUT naar GND trekt extra stroom tijdens sleep van de Wemos, zodat de powerbank actief blijft

```
Wemos D0 (GPIO16) ── RST
Wemos 5V          ── microWakeUpper VCC
Wemos GND         ── microWakeUpper GND
microWakeUpper OUT ── weerstand (47–100Ω) ── GND
microWakeUpper IN  ── microWakeUpper OUT
```

---

## Nieuwe node toevoegen

1. **Kies een sensor-ID** (volgende vrije int, momenteel 1–5 in gebruik).
2. **Vul een `SoilPacket`** in de sketch: magic=0xA1, id=<nieuw>, Soil22 data.
3. **Registreer de AGG MAC** in de sensor sketch (`peerInfo.peer_addr`).
4. **Voeg de sensor-ID toe aan AGG** (`CLAUDE_LORA_VV_AGG_V1`): accepteer het pakket, sla `items[]` op.
5. **Voeg parsing toe in LoRaNode C3** (`CLAUDE_LORA_VV_LORANODE_C3_V1`): schrijf de Soil22 velden naar JSON.
6. **Voeg JSON-veld toe in Heltec TX** als nodig.
7. **Voeg parsing toe in Node-RED** (`influx_function.js`): lees het nieuwe veld uit de JSON.
8. **Maak measurement aan in InfluxDB**: automatisch bij eerste write, of handmatig.
9. **Voeg panel toe in Grafana**.

Timing: zet de wake second zodanig dat de node na de AGG wakker wordt (AGG=:05, nieuwe node=:20 of later).

---

## Arduino omgeving

| Component | Versie / detail |
|---|---|
| Arduino IDE | 2.x aanbevolen |
| ESP32 board package | v3.x (espressif/arduino-esp32) |
| Board (XIAO C3) | "XIAO_ESP32C3" |
| Board (Heltec) | "Heltec WiFi LoRa 32(V3)" |

### Libraries (installeer via Library Manager)

| Library | Gebruik |
|---|---|
| RadioLib | LoRa TX/RX op Heltec |
| U8g2 | OLED display Heltec |
| ModbusMaster | SOIL1 / SOIL2 RS485 |
| BH1750 (Christopher Laws) | Lichtsensor |
| DHT sensor library (Adafruit) | Wemos DHT22 |
| Adafruit Unified Sensor | Dependency van DHT |
| PubSubClient | MQTT op Wemos/Heltec RX |

`Wire`, `WiFi` en `esp_now` zijn ingebouwd in de ESP32 core.

### credentials.h (niet in git)

Na checkout aanmaken per sketch die WiFi of MQTT gebruikt:

```cpp
// Heltec RX
const char* ssid      = "...";
const char* password  = "...";
const char* mqtt_user = "...";
const char* mqtt_pass = "...";

// Wemos DHT22
const char* ssid     = "...";
const char* password = "...";
```
