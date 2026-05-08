# LoRa Tuinmeetnet v2

Bodemvocht, temperatuur, licht en waterverbruik meten in de tuin via LoRa → MQTT → InfluxDB → Grafana.

---

## Architectuur

```
SOIL1   ──┐
SOIL2   ──┤                                    LoRa (868 MHz)
BH1750  ──┤→ AGG → LoRaNode C3 ──(UART)──→ Heltec TX ~~~~~~~~~~~→ Heltec RX → WiFi/MQTT
FLOW    ──┘                                                                         │
                                                                                    ▼
Wemos DHT22 ──────────────────────────────────────────────── WiFi/MQTT      Raspberry Pi 4
                                                                          ┌─────────────────┐
                                                                          │ Mosquitto  :1883 │
                                                                          │ Node-RED   :1880 │
                                                                          │ InfluxDB2  :8086 │
                                                                          │ Grafana    :3000 │
                                                                          └─────────────────┘
                                                                                    │
                                                                         Cloudflare Tunnel
                                                                                    │
                                                                       https://grafana.biobiejo.nl
```

Twee gescheiden paden naar MQTT:
- **LoRa-pad**: sensoren → ESP-NOW → AGG → ESP-NOW → C3 → UART → Heltec TX → LoRa → Heltec RX → MQTT
- **WiFi-pad**: Wemos DHT22 publiceert rechtstreeks naar MQTT

---

## Nodes

### Sensor nodes (ESP-NOW → AGG)

| Node | Hardware | Sketch | Interval | Wake second |
|---|---|---|---|---|
| SOIL1 | XIAO ESP32-C3 + CWT Modbus | `CLAUDE_LORA_VV_SOIL1_V1` | 5 min | :08 |
| SOIL2 | XIAO ESP32-C3 + ZTS Modbus | `CLAUDE_LORA_VV_SOIL2_V1` | 5 min | :16 |
| BH1750 | XIAO ESP32-C3 + BH1750 + DS3231 | `CLAUDE_LORA_VV_BH1750_V1` | 5 min | :08 |
| Waterflow | XIAO ESP32-C3 + YF-C01 | `CLAUDE_LORA_VV_WATERFLOW_SOLAR_V1` | event | — |

Alle sensor nodes gebruiken een **DS3231 RTC** voor precisie-timing en een **LilyGo T-BAT** met 18650 batterij voor stroom.

### Netwerk nodes

| Node | Hardware | Sketch | Functie |
|---|---|---|---|
| AGG | XIAO ESP32-C3 | `CLAUDE_LORA_VV_AGG_V1` | Verzamelt ESP-NOW pakketten, bouwt 97-byte uplink |
| LoRaNode C3 | XIAO ESP32-C3 | `CLAUDE_LORA_VV_LORANODE_C3_V1` | Ontvangt uplink van AGG, stuurt JSON via UART naar Heltec TX |
| Heltec TX | Heltec LoRa32 V3 | `CLAUDE_LORA_VV_LORANODE_HELTEC_OLED_V2` | Ontvangt UART, voegt batlev toe, zendt LoRa |
| Heltec RX | Heltec LoRa32 V3 | `CLAUDE_LORA_VV_RECEIVER_MQTT_HELTEC_V1` | Ontvangt LoRa, publiceert naar MQTT |

### Directe WiFi nodes

| Node | Hardware | Sketch | MQTT topic |
|---|---|---|---|
| DHT22 temp/vocht | Wemos D1 mini + DHT22 | `CLAUDE_WEMOS_DHT22_MQTT_V1` | `tuin/mqtt/dht22` |
| Keepalive LoRa node | Wemos D1 mini | `CLAUDE_WEMOS_MICROWAKEUPPER_V1` | — |

---

## Stroomvoorziening

| Node | Voeding | Opmerking |
|---|---|---|
| AGG | 6V solarpanel (6×10cm) + T-BAT | — |
| SOIL1 | 6V solarpanel (6×10cm) + T-BAT | — |
| SOIL2 | Solarpanel aansluiting op behuizing | Nog in uitvoering |
| BH1750 | T-BAT + 18650 | Solar nog te bepalen |
| Waterflow | 6V solarpanel + T-BAT | Buiten bij kraan |
| LoRaNode C3 + Heltec TX | Sounix 30000mAh powerbank | Geen solar |
| Heltec RX (MQTT node) | Sounix 30000mAh powerbank | Geen solar |
| Wemos DHT22 | Sounix powerbank | microWakeUpper, sleep 25s |
| Wemos keepalive | Sounix powerbank | microWakeUpper, sleep 25s |

**Sounix powerbank:** schakelt af bij te weinig stroomverbruik. Oplossing: Wemos D1 mini + microWakeUpper wekt zichzelf elke 25 seconden. De microWakeUpper trekt via een weerstand (47–100Ω, OUT→GND) extra stroom tijdens deep sleep.

---

## Hardware aansluitingen

### BH1750 node (XIAO ESP32-C3)
```
XIAO D4 (GPIO6) ── SDA ── BH1750 + DS3231
XIAO D5 (GPIO7) ── SCL ── BH1750 + DS3231
XIAO A1 (GPIO3) ── T-BAT VBAT (voltage divider)
XIAO D2 (GPIO2) ── DS3231 INT/SQW (wakeup)
BH1750 ADDR     ── GND (adres 0x23)
```

### Wemos D1 mini + microWakeUpper
```
Wemos D0 (GPIO16) ── RST                     (deep sleep wakeup)
Wemos 5V          ── microWakeUpper VCC
Wemos GND         ── microWakeUpper GND
microWakeUpper OUT ── weerstand (47–100Ω) ── GND
microWakeUpper IN  ── microWakeUpper OUT
```

### Wemos D1 mini DHT22
```
Wemos D2 (GPIO4) ── DHT22 DATA
Wemos 3V3        ── DHT22 VCC
Wemos GND        ── DHT22 GND
Wemos D0         ── RST  (deep sleep wakeup)
```

---

## MQTT topics

| Topic | Payload | Bron |
|---|---|---|
| `tuin/lora/test` | JSON (zie hieronder) | Heltec RX |
| `tuin/mqtt/dht22` | `{"temp":21.4,"hum":58.3}` | Wemos DHT22 |

### JSON-formaat op `tuin/lora/test`

```json
{
  "a":  <agg_seq>,
  "r":  <rssi_lora_intern>,
  "av": <vbat_agg_mV>,
  "ac": <temp_agg_C>,
  "mc": <temp_mesh_C>,
  "hv": <vbat_heltec_V>,
  "s": [
    [temp, hum, ec, caseTemp, vbat, rssi],      // SOIL1  (6 velden)
    [temp, hum, caseTemp, vbat, rssi],           // SOIL2  (5 velden)
    [flowRate, totalLiters, vbat, rssi]          // FLOW   (4 velden)
  ],
  "wf": <waterflow_pulsen>
}
```

> `"hv"` is de batterijspanning van de Heltec TX node (JST batterij / Sounix).
> BH1750 node (id=4) zit nog niet in de JSON output — AGG en C3 parsing volgt als hardware klaar is.

---

## Timing (5-minuten cyclus, DS3231 RTC)

Alle nodes waken op een vast tweede binnen het 5-minuten interval. De volgorde zorgt dat data beschikbaar is vóór de volgende schakel stuurt.

| Node | Wake second |
|---|---|
| AGG + LoRaNode C3 | :05 |
| SOIL1 | :08 |
| SOIL2 | :16 |
| BH1750 | :08 |

---

## Raspberry Pi (192.168.2.13)

| Service | Poort | URL |
|---|---|---|
| Mosquitto (MQTT) | 1883 | — |
| Node-RED | 1880 | http://192.168.2.13:1880 |
| InfluxDB2 | 8086 | http://192.168.2.13:8086 |
| Grafana | 3000 | https://grafana.biobiejo.nl |

### InfluxDB
- Organisatie: `tuinmeetnet`
- Bucket: `tuinsensoren`
- Measurements: `systeem`, `soil1`, `soil2`, `flow`, `waterflow`

### MQTT gebruiker wijzigen
```bash
sudo mosquitto_passwd /etc/mosquitto/passwd <gebruikersnaam>
sudo systemctl restart mosquitto
```

---

## Credentials

`credentials.h` staat in `.gitignore` en wordt **niet** meegecommit. Na een verse checkout aanmaken:

**Heltec RX (MQTT receiver):**
```cpp
const char* ssid      = "TP-Link_Extender";
const char* password  = "...";
const char* mqtt_user = "mqtt-user";
const char* mqtt_pass = "...";
```

**Wemos DHT22:**
```cpp
const char* ssid     = "TP-Link_Extender";
const char* password = "...";
```

---

## Libraries per sketch

| Sketch | Libraries |
|---|---|
| SOIL1 / SOIL2 | ModbusMaster |
| BH1750 | BH1750 (Christopher Laws) |
| Heltec TX/RX | RadioLib, U8g2 |
| Wemos DHT22 | DHT sensor library (Adafruit), Adafruit Unified Sensor, PubSubClient |
| Alle ESP32 nodes | Wire, WiFi, esp_now (ingebouwd) |

Installeren via Arduino IDE → **Library Manager**.
