# Functioneel Specificatie Document
## LoRa tuinmeetnet — RTC-gestuurde sensor nodes, aggregator, LoRa uplink en MQTT-ontvangst

| | |
|---|---|
| **Versie** | 0.4 |
| **Datum** | 20 mei 2026 |
| **Doel** | Vastleggen van de functionele werking, timing, hardware-architectuur en interfaces van het huidige veldsysteem. |
| **Reikwijdte** | SOIL1, SOIL2, BH1750, Waterflow, Sensorstation, Aggregator, XIAO C3 LoRa node, Heltec LoRa zender, Heltec LoRa ontvanger, MQTT, Node-RED, InfluxDB en Grafana. |

Wijzigingen t.o.v. v0.3: meetcyclus gewijzigd van 5 naar 20 minuten; BH1750, Waterflow en Sensorstation nodes toegevoegd; ontvanger gewijzigd van T-Beam naar Heltec LoRa32 V3; stroomopstelling per node bijgewerkt; JSON-formaat uitgebreid; Waterflow succesvol in tuin getest (20 mei 2026).

---

## 1. Doel en scope

Het project realiseert een energiezuinig tuinmeetnet waarin meerdere XIAO ESP32-C3 sensor nodes via ESP-NOW rapporteren aan een XIAO ESP32-C3 aggregator. De aggregator stuurt een compact uplinkframe door naar een XIAO C3 LoRa node. Deze zet de informatie om naar een JSON-regel voor een Heltec LoRa-zender (lnHel), die de payload via LoRa verstuurt naar een Heltec LoRa-ontvanger (MQTT node), waarna publicatie naar MQTT, parsing in Node-RED en opslag in InfluxDB/Grafana plaatsvindt.

Binnen scope vallen de functionele datastroom, de node-rollen, de timing op basis van DS3231 RTC, de payloaddefinities, de energiehuishouding en de softwarematige verantwoordelijkheden per node.

Buiten scope vallen mechanische constructie, waterdichte behuizing, langdurige milieuvalidatie, formele EMC-certificering en productierijpe beveiliging.

---

## 2. Systeemcontext en hoofdketen

De functionele hoofdketen is lineair. Betrouwbaarheid wordt vergroot door gefaseerde wake-momenten, ACK-mechanismen en herhaalde uplinktransmissies.

| Laag / node | Hardware | Transport | Hoofdfunctie |
|---|---|---|---|
| SOIL1 | XIAO ESP32-C3 + DS3231 + RS485 + CWT sensor | Modbus RTU + ESP-NOW | Leest vocht, temperatuur, EC en pH; stuurt SoilPacket met ACK. |
| SOIL2 | XIAO ESP32-C3 + DS3231 + RS485 + ZTS sensor | Modbus RTU + ESP-NOW | Leest vocht en temperatuur; stuurt SoilPacket met ACK. |
| BH1750 | XIAO ESP32-C3 + DS3231 + BH1750 | I2C + ESP-NOW | Leest lichtintensiteit (lux) en case temperature; stuurt SoilPacket. |
| Waterflow | XIAO ESP32-C3 + DS3231 + YF-C01 + MOSFET | Interrupt + ESP-NOW | Event-driven: wekt op bij waterstroming, telt pulsen, stuurt naar AGG. |
| Sensorstation | ESP32-C3 Super Mini + DS18B20 + BH1750 + T-BAT | I2C/1-Wire + ESP-NOW | Combineert licht, temperatuur en vochtigheid op één node. |
| Aggregator (AGG) | XIAO ESP32-C3 + DS3231 | ESP-NOW | Ontvangt SoilPackets, ACKt zenders, bouwt AggUplink141. |
| LoRa node C3 | XIAO ESP32-C3 + DS3231 | ESP-NOW + UART | Ontvangt AggUplink141, converteert naar JSON, voedt Heltec via MOSFET. |
| lnHel (Heltec TX) | Heltec LoRa32 V3 + SX1262 | LoRa | Zendt de JSON-payload via LoRa uit. |
| MQTT node (Heltec RX) | Heltec LoRa32 V3 + SX1262 | LoRa + WiFi + MQTT | Ontvangt LoRa, voegt RSSI/SNR toe, publiceert naar MQTT. |
| Applicatielaag | Node-RED + InfluxDB + Grafana | MQTT | Parseert payloads, schrijft measurements, visualiseert trends. |

---

## 3. Hardware-architectuur per node

### 3.1 Sensor nodes — gemeenschappelijk patroon

Alle slapende sensor nodes zijn gebaseerd op XIAO ESP32-C3 met DS3231 RTC voor phase-aligned deep-sleep wakes. Externe sensoren worden via een MOSFET alleen tijdens de meetfase van spanning voorzien.

- **Controller:** XIAO ESP32-C3
- **RTC:** DS3231 op I2C; INT-lijn naar GPIO2 voor deep-sleep wakeup
- **Communicatie naar AGG:** ESP-NOW op kanaal 6, unicast naar AGG-MAC
- **Lokale metingen:** VBAT via ADC, case temperature via DS3231 intern

### 3.2 SOIL1

CWT Modbus-sensor. Levert bodemtemperatuur, vocht, EC en pH.

- Modbus: adres 1, 4800 baud, FC03, register 0x0000, lengte 7
- Warm-up: 2500 ms, max 5 pogingen
- SoilPacket.id = 1
- PHASE_SECOND = 8

### 3.3 SOIL2

ZTS Modbus-sensor. Levert bodemtemperatuur en vocht.

- Modbus: adres 1, 4800 baud, FC04, register 0x0000, lengte 2
- Warm-up: 3200 ms, max 5 pogingen
- SoilPacket.id = 2
- PHASE_SECOND = 16

### 3.4 BH1750

Lichtsensor node. Levert lux-waarde en case temperature.

- Sensor: BH1750 op I2C, adres 0x23 (ADDR naar GND)
- SoilPacket.id = 4
- PHASE_SECOND = 8 (zelfde venster als SOIL1, andere MAC)

### 3.5 Waterflow

Event-driven node bij de kraan. Wekt alleen bij daadwerkelijke waterstroming.

- Sensor: YF-C01, 450 pulsen per liter
- Wakeup: ext1 op GPIO3 (waterflow switch, LOW = water actief)
- MOSFET (GPIO6) schakelt 5V naar YF-C01 alleen tijdens meting
- Stuurt elke 25 s naar AGG tot ACK=1, dan NVS opslaan + slapen
- Max 8 minuten wakker als AGG niet antwoordt
- Cumulatief totaal bewaard in NVS (overleeft deep sleep en herstart)
- Reset via GPIO2 → GND bij boot
- SoilPacket.id = 3
- Succesvol getest in tuin: 20 mei 2026

### 3.6 Sensorstation

Gecombineerde node met licht en klimaat op één board.

- Hardware: ESP32-C3 Super Mini + LilyGo T-BAT + DS18B20 (GPIO6) + BH1750
- VBAT_RATIO: 1.46 (gemeten)
- SoilPacket.id = 5

### 3.7 Aggregator (AGG)

Centrale ontvanger in de tuin. Luistert naar alle sensor nodes en bouwt de uplink.

- Luistervenster: LISTEN_MS = 30000 ms
- WAKE_SECOND = 5
- ACK: 8-byte struct met session_id, seq en ok-flag
- Uplink: AggUplink141 (141 bytes), meerdere keren verstuurd naar LoRa node C3

### 3.8 LoRa node C3 en lnHel (Heltec TX)

De XIAO C3 LoRa node ontvangt AggUplink141, bouwt JSON en voedt de Heltec via MOSFET.

- UART naar lnHel: 115200 baud, SERIAL_8N1
- lnHel schakelt niet uit tussen cycli (wacht op UART)
- WAKE_SECOND = 5 (gelijk aan AGG)

### 3.9 MQTT node (Heltec RX)

Staat op het balkon, continu aan, WiFi naar MQTT.

- Hardware: Heltec LoRa32 V3 + SX1262
- Voeding: Redmi powerbank + 25W zonnepaneel (stabiel)
- Publiceert naar topic `tuin/lora/test`
- Voegt lr (RSSI) en ls (SNR) toe aan het bericht

---

## 4. RTC-timing en wake-orkestratie

Alle slapende nodes gebruiken DS3231 Alarm1 in interruptmodus. De INT-lijn trekt GPIO2 laag; de volgende wake wordt direct na afloop van de cyclus opnieuw geprogrammeerd.

| Node | Cyclus | Wake second | Functionele betekenis |
|---|---|---|---|
| AGG | 20 min | :05 | Vroeg wakker, luistert 30 s naar sensor nodes |
| LoRa node C3 | 20 min | :05 | Gelijk met AGG, klaar om AggUplink te ontvangen |
| SOIL1 | 20 min | :08 | Na AGG, 3 s marge voor AGG-opstart |
| BH1750 | 20 min | :08 | Zelfde venster als SOIL1 |
| SOIL2 | 20 min | :16 | Na SOIL1, voorkomt ESP-NOW botsing |
| Waterflow | event | — | Wekt op waterstroming (ext1 GPIO3) |

De 20-minutencyclus reduceert het aantal LoRa-transmissies van 12 naar 3 per uur (lnHel) en vergroot de slaaptijd van alle XIAO-nodes met factor 4 ten opzichte van de vorige 5-minutencyclus.

---

## 5. Functionele werking per cyclus

### 5.1 Sensor node cyclus (SOIL1, SOIL2, BH1750)

1. DS3231 Alarm1 trekt GPIO2 laag → node wordt gewekt
2. Node leest VBAT en case temperature
3. Node schakelt sensor in via MOSFET, wacht op warm-up, leest sensor uit
4. Meetdata wordt verpakt in SoilPacket (28 bytes)
5. Node stuurt SoilPacket via ESP-NOW naar AGG, wacht op ACK (max 2 retries)
6. Volgende RTC-alarm wordt geprogrammeerd
7. Node gaat terug naar deep sleep

### 5.2 Waterflow cyclus

1. Waterstroming opent switch → ext1 wakeup op GPIO3
2. MOSFET aan, YF-C01 telt pulsen
3. Elke 25 s: SoilPacket met huidig pulsenaantal naar AGG
4. Bij ACK=1: NVS-totaal opslaan, MOSFET uit, slapen
5. Bij geen ACK na 8 minuten: toch NVS opslaan en slapen

### 5.3 Aggregator cyclus

1. DS3231 wekt AGG op seconde :05
2. AGG activeert WiFi STA + ESP-NOW op kanaal 6
3. AGG luistert 30 s, ontvangt en valideert SoilPackets per node
4. Per geldig packet: data opslaan, ACK terugsturen
5. Na luistervenster: AggUplink141 bouwen en meerdere keren naar LoRa node C3 sturen
6. Volgende RTC-alarm zetten, deep sleep

### 5.4 LoRa uplink cyclus

1. LoRa node C3 waakt op seconde :05, ontvangt AggUplink141 van AGG
2. Bouwt JSON-string, schakelt lnHel in via MOSFET
3. Stuurt JSON via UART (115200) naar lnHel
4. lnHel ontvangt JSON, voegt eigen VBAT toe, zendt via LoRa (SF11, 868 MHz)
5. MQTT node op balkon ontvangt LoRa, voegt lr/ls toe, publiceert naar `tuin/lora/test`
6. Node-RED parseert, schrijft naar InfluxDB, Grafana visualiseert

---

## 6. Datamodellen en interfaces

### 6.1 Binary payloads in de ESP-NOW-laag

| Struct | Grootte | Richting | Beschrijving |
|---|---|---|---|
| Soil22 | 22 bytes | intern | Meetrecord: session_id, seq, t_x10, h_x10, ec_raw, case_x10, vbat_mv, ph_x10, flags, rsv0 |
| SoilPacket | 28 bytes | Sensor → AGG | Envelope: magic (0xA1), node-id, Soil22, 4 bytes padding |
| Ack | 8 bytes | AGG → Sensor | session_id + seq + ok-flag |
| AggUplink141 | 141 bytes | AGG → LoRa C3 | agg_sid, agg_seq, count, 6 × Soil22 slots |

### 6.2 Soil22 veldsemanstiek

| Veld | Type | Schaal | Gebruik |
|---|---|---|---|
| session_id | uint32 | — | Persistente sessie-id over deep sleep |
| seq | uint16 | +1/meting | Monotone teller voor diagnose en ACK |
| t_x10 | int16 | ÷10 | Bodemtemperatuur of sensortemperatuur (°C) |
| h_x10 | uint16 | ÷10 | Vochtigheid of moisture (%) |
| ec_raw | uint16 | raw | EC (SOIL1/CWT) of lux (BH1750) |
| case_x10 | int16 | ÷10 | Case temperature via DS3231 intern (°C) |
| vbat_mv | uint16 | mV | Lokale batterijspanning |
| ph_x10 | uint16 | ÷10 | pH (SOIL1/CWT); 0 = niet beschikbaar |
| flags | uint16 | bitmask | 0x0001 = sensoruitlezing gelukt |
| rsv0 | uint16 | low byte | In AGG: ontvangen ESP-NOW RSSI als int8 |

### 6.3 MQTT JSON-formaat

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

## 7. Energiebeheer en stroomvoorziening

| Node | Voeding | Opmerking |
|---|---|---|
| SOIL1 | 6V solar (10×7 cm) + LilyGo T-BAT + 18650 | Solar aansluiting te voltooien |
| SOIL2 | 6V solar + T-BAT | In uitvoering |
| BH1750 | T-BAT + 18650 | Solar nog te bepalen |
| Waterflow | 6V solar + T-BAT | Buiten bij kraan, getest 20 mei 2026 |
| AGG | 6V solar + T-BAT | Solar aansluiting te voltooien |
| LoRa node C3 | Redmi powerbank | Samen met lnHel in tuin |
| lnHel (Heltec TX) | Redmi powerbank + 10W solar (buck → 5.15V) | In tuin, 3 LoRa TX per uur bij 20 min cyclus |
| MQTT node (Heltec RX) | Redmi powerbank + 25W solar | Balkon, stabiel, continu aan |
| Wemos DHT22 + keepalive | Sounix powerbank + microWakeUpper | Sleep 25 s, weerstand 47–100 Ω trekt keepalive stroom |

**Energiebesparing door 20-minutencyclus:**
- Slapende XIAO-nodes (SOIL1, SOIL2, BH1750, AGG, LoRa C3): 4× langere slaaptijd t.o.v. 5 minuten
- lnHel: 3 LoRa TX/uur i.p.v. 12

**Sounix powerbank keepalive:** schakelt uit bij te laag verbruik. Oplossing: Wemos D1 mini + microWakeUpper wekt zichzelf elke 25 s; weerstand (47–100 Ω) trekt extra stroom tijdens deep sleep.

**XIAO voedingsregel:** XIAO ESP32-C3 nodes uitsluitend voeden via USB 5V van T-BAT, nooit via 3.3V-patch op achterkant (bypast regulator, beschadigt chip bij pieken).

---

## 8. Configureerbare parameters

| Parameter | Huidige waarde | Effect |
|---|---|---|
| INTERVAL_MINUTES | 20 | Cyclustijd van sensor nodes, AGG en LoRa C3 |
| ESPNOW_WIFI_CHANNEL | 6 | Vast kanaal voor alle ESP-NOW communicatie |
| SOIL1 PHASE_SECOND | 8 | Zendmoment SOIL1/BH1750 binnen cyclus |
| SOIL2 PHASE_SECOND | 16 | Zendmoment SOIL2 binnen cyclus |
| AGG WAKE_SECOND | 5 | Luisterstart AGG binnen cyclus |
| AGG LISTEN_MS | 30000 | Luisterduur AGG voor SoilPackets |
| Waterflow PULSES_PER_LITER | 450 | YF-C01 nominaal — kalibratie nog te doen |
| Waterflow max wakker | 8 minuten | Timeout als AGG niet antwoordt |
| LoRa frequentie | 868.0 MHz | EU868 band |
| LoRa SF | 11 | Hoge link budget, ~537 bps netto |
| LoRa BW | 125 kHz | Standaard LoRa bandbreedte |
| LoRa CR | 4/5 | Coding rate |
| LoRa sync word | 0x12 | Privaat netwerk |
| LoRa TX power | 14 dBm | Heltec SX1262 |
| UART baud (C3 → lnHel) | 115200 | SERIAL_8N1 |

---

## 9. Functionele eisen en acceptatiepunten

- Het systeem werkt zonder handmatige interventie cyclisch op basis van RTC-gestuurde wakes.
- SOIL1, SOIL2 en Waterflow schakelen sensorvoeding uit na de meetfase.
- AGG ACKt geldige SoilPackets en neemt de meest recente data per node op in de uplink.
- De uplinkketen levert minimaal de measurements `systeem`, `soil1`, `soil2`, `flow` en `waterflow` in InfluxDB.
- Sequence-tellers maken diagnose van gemiste cycli mogelijk.
- Waterflow node slaat cumulatief totaal op in NVS (overleeft herstart en deep sleep).
- Grafana dashboard is publiekelijk bereikbaar via Cloudflare tunnel (grafana.biobiejo.nl) zonder inloggen.

---

## 10. Bekende aandachtspunten

- DS3231 is een functioneel kernonderdeel; verkeerde fase-instellingen of RTC-sync verlies kunnen direct resulteren in gemiste vensters.
- Waterflow PULSES_PER_LITER staat op nominale waarde (450) — kalibratie met bekende hoeveelheid water verdient aanbeveling.
- BH1750 node zit nog niet in de JSON output van de LoRa keten — AGG en C3 parsing volgt als hardware stabiel is.
- RF-gedrag blijft gevoelig voor antennekwaliteit, plaatsing en polarisatie. Yagi-antenne geeft stabiele verbinding.
- MQTT node moet reconnect-veilig zijn bij lange tussenpozen tussen LoRa-pakketten.

---

## 11. Aanbevolen vervolgstappen

- Waterflow PULSES_PER_LITER kalibreren met gemeten hoeveelheid water.
- BH1750 JSON-parsing toevoegen in LoRa C3 sketch en Node-RED.
- Solar aansluitingen voltooien voor SOIL1, SOIL2 en AGG.
- Langdurige veldrun (meerdere dagen) documenteren als validatie van de 20-minutencyclus.
- Sensorstation (id=5) volledig integreren in AGG → C3 → JSON keten.

---

## 12. Architectuurdiagram

```
SOIL1 (id=1) ──┐
SOIL2 (id=2) ──┤  ESP-NOW ch6    UART 115200      LoRa 868MHz SF11
BH1750(id=4) ──┤  SoilPacket ──► AggUplink141 ──► JSON ──► LoRa ──► MQTT
FLOW  (id=3) ──┘  28 bytes       141 bytes

                  AGG             LoRa node C3    lnHel TX ~~~► Heltec RX
                  XIAO C3         XIAO C3         LoRa32 V3     LoRa32 V3
                  6V solar        Redmi PB        Redmi PB      25W solar
                  wake :05        wake :05        altijd aan    altijd aan
                                                                    │
                                                               MQTT broker
                                                               Raspberry Pi 4
                                                               192.168.2.13:1883
                                                                    │
                                                               Node-RED :1880
                                                               InfluxDB :8086
                                                               Grafana  :3000
                                                                    │
                                                          Cloudflare Tunnel
                                                          grafana.biobiejo.nl
```

Wemos DHT22 → WiFi → MQTT `tuin/mqtt/dht22` (parallel pad, geen LoRa)

---

## 13. Pinout per node

### 13.1 SOIL1 — XIAO ESP32-C3 + CWT RS485

| Signaal | GPIO | Richting | Functie |
|---|---|---|---|
| DS3231 SDA | GPIO6 | I2C | SDA naar DS3231 |
| DS3231 SCL | GPIO7 | I2C | SCL naar DS3231 |
| DS3231 INT | GPIO2 | Input | Deep sleep wakeup |
| RS485 RX | GPIO21 | UART RX | Modbus ontvangst |
| RS485 TX | GPIO20 | UART TX | Modbus zenden |
| RS485 DE/RE | GPIO4 | Output | Richting RS485 transceiver |
| Sensor MOSFET | GPIO5 | Output | Voeding CWT sensor (alleen tijdens meting) |
| VBAT | A1 (GPIO3) | Analog | Batterijspanning |

### 13.2 SOIL2 — XIAO ESP32-C3 + ZTS RS485

| Signaal | GPIO | Richting | Functie |
|---|---|---|---|
| DS3231 SDA | GPIO7 | I2C | SDA (omgewisseld t.o.v. SOIL1) |
| DS3231 SCL | GPIO6 | I2C | SCL (omgewisseld t.o.v. SOIL1) |
| DS3231 INT | GPIO2 | Input | Deep sleep wakeup |
| RS485 RX | GPIO21 | UART RX | Modbus ontvangst |
| RS485 TX | GPIO20 | UART TX | Modbus zenden |
| RS485 DE/RE | GPIO4 | Output | Richting RS485 transceiver |
| Sensor MOSFET | GPIO5 | Output | Voeding ZTS sensor (alleen tijdens meting) |
| VBAT | GPIO3 | Analog | Batterijspanning |

### 13.3 BH1750 — XIAO ESP32-C3

| Signaal | GPIO | Richting | Functie |
|---|---|---|---|
| DS3231 + BH1750 SDA | GPIO6 | I2C | SDA gedeeld |
| DS3231 + BH1750 SCL | GPIO7 | I2C | SCL gedeeld |
| DS3231 INT | GPIO2 | Input | Deep sleep wakeup |
| BH1750 ADDR | GND | — | I2C adres 0x23 |
| VBAT | GPIO3 (A1) | Analog | Batterijspanning |

### 13.4 Waterflow — XIAO ESP32-C3

| Signaal | GPIO | Richting | Functie |
|---|---|---|---|
| YF-C01 signaal | GPIO4 (D2) | Input | Pulssignaal flowsensor |
| Waterflow switch | GPIO3 (D3) | Input | LOW = water actief, ext1 wakeup |
| Sensor MOSFET | GPIO6 (D4) | Output | 5V naar YF-C01 (alleen actief) |
| VBAT | GPIO1 (D1) | Analog | Batterijspanning |
| NVS reset | GPIO2 (D0) | Input | GND bij boot = teller reset |

### 13.5 AGG — XIAO ESP32-C3

| Signaal | GPIO | Richting | Functie |
|---|---|---|---|
| DS3231 SDA | GPIO6 | I2C | SDA naar DS3231 |
| DS3231 SCL | GPIO7 | I2C | SCL naar DS3231 |
| DS3231 INT | GPIO2 | Input | Deep sleep wakeup |
| VBAT | A1 | Analog | Batterijspanning |

### 13.6 LoRa node C3 — XIAO ESP32-C3

| Signaal | GPIO | Richting | Functie |
|---|---|---|---|
| DS3231 SDA | GPIO6 | I2C | SDA naar DS3231 |
| DS3231 SCL | GPIO7 | I2C | SCL naar DS3231 |
| DS3231 INT | GPIO2 | Input | Deep sleep wakeup |
| UART TX naar lnHel | GPIO9 | UART TX | JSON naar Heltec TX |
| Heltec MOSFET | GPIO5 | Output | Voeding lnHel (alleen tijdens uplink) |

### 13.7 lnHel — Heltec LoRa32 V3 (TX)

| Signaal | GPIO | Richting | Functie |
|---|---|---|---|
| UART RX van C3 | GPIO46 | UART RX | JSON ontvangst van LoRa node C3 |
| SX1262 NSS | GPIO8 | SPI CS | Chip select LoRa radio |
| SX1262 DIO1 | GPIO14 | Input | IRQ/status |
| SX1262 NRST | GPIO12 | Output | Reset LoRa radio |
| SX1262 BUSY | GPIO13 | Input | Busy status |
| SPI SCK | GPIO9 | SPI | Klok |
| SPI MISO | GPIO11 | SPI | MISO |
| SPI MOSI | GPIO10 | SPI | MOSI |

### 13.8 MQTT node — Heltec LoRa32 V3 (RX)

Zelfde SX1262 pinout als lnHel. WiFi client publiceert naar MQTT `tuin/lora/test`.

---

## 14. Software en ontwikkelomgeving

| Component | Versie / detail |
|---|---|
| Arduino IDE | 2.x |
| ESP32 board package | v3.x (espressif/arduino-esp32) |
| LoRa library | RadioLib |
| OLED library | U8g2 |
| Modbus library | ModbusMaster |
| Lichtsensor library | BH1750 (Christopher Laws) |
| MQTT library | PubSubClient |
| Ingebouwd | Wire, WiFi, esp_now |

Sketches worden beheerd in de GitHub repository. Aanpassingen worden gedaan en gedocumenteerd via Claude Code.

`credentials.h` staat in `.gitignore` en wordt niet meegecommit. Na checkout aanmaken per node met WiFi/MQTT credentials.
