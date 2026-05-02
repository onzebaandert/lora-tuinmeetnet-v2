# LoRa Tuinmeetnet v2

Bodemvocht, temperatuur en waterverbruik via LoRa → MQTT → InfluxDB → Grafana.

## Architectuur

```
SOIL1 ──┐
SOIL2 ──┤→ AGG → LoRaNode C3 ──(UART)──→ Heltec LoRa32 V3
FLOW  ──┘                                      │
                                               │ WiFi / MQTT
                                               ▼
                                      Raspberry Pi 4
                                  ┌────────────────────┐
                                  │ Mosquitto :1883     │
                                  │ Node-RED  :1880     │
                                  │ InfluxDB2 :8086     │
                                  │ Grafana   :3000     │
                                  └────────────────────┘
                                               │
                                    Cloudflare Tunnel
                                               │
                                  https://grafana.biobiejo.nl
```

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

### MQTT
- Broker: `192.168.2.13:1883`
- Topic: `tuin/lora/test`
- Authenticatie: gebruiker + wachtwoord vereist (zie `credentials.h`)

**MQTT-gebruiker wijzigen op de Pi:**
```bash
sudo mosquitto_passwd /etc/mosquitto/passwd <gebruikersnaam>
sudo systemctl restart mosquitto
```

## Sketches

| Sketch | Hardware | Functie |
|---|---|---|
| `CLAUDE_LORA_VV_SOIL1_V1` | XIAO ESP32-C3 | Modbus CWT bodems sensor |
| `CLAUDE_LORA_VV_SOIL2_V1` | XIAO ESP32-C3 | Modbus ZTS bodems sensor |
| `CLAUDE_LORA_VV_WATERFLOW_V1` | XIAO ESP32-C3 | Puls-teller waterflow |
| `CLAUDE_LORA_VV_AGG_V1` | XIAO ESP32-C3 | Aggregator, bouwt 97-byte uplink |
| `CLAUDE_LORA_VV_LORANODE_C3_V1` | XIAO ESP32-C3 | Ontvangt binary, stuurt JSON via UART |
| `CLAUDE_LORA_VV_RECEIVER_MQTT_HELTEC_V1` | Heltec LoRa32 V3 | Ontvangt LoRa, publiceert naar MQTT |

## Credentials

Gevoelige gegevens staan **niet** in git maar in een lokaal bestand:

```
sketches/CLAUDE_LORA_VV_RECEIVER_MQTT_HELTEC_V1/credentials.h
```

```cpp
// WiFi
const char* ssid     = "<wifi-naam>";
const char* password = "<wifi-wachtwoord>";

// MQTT
const char* mqtt_user = "<mqtt-gebruiker>";
const char* mqtt_pass = "<mqtt-wachtwoord>";
```

Dit bestand staat in `.gitignore`. Maak het handmatig aan na een verse checkout.

**WiFi of MQTT-wachtwoord wijzigen:** pas `credentials.h` aan en upload de sketch opnieuw.

## JSON-formaat op MQTT

```json
{
  "a":  <agg_seq>,
  "r":  <rssi_lora_intern>,
  "av": <vbat_agg_mV>,
  "ac": <temp_agg_C>,
  "mv": <vbat_mesh_mV>,
  "mc": <temp_mesh_C>,
  "s": [
    [temp, hum, ec, caseTemp, vbat, rssi],      // SOIL1 (6 velden)
    [temp, hum, caseTemp, vbat, rssi],           // SOIL2 (5 velden)
    [flowRate, totalLiters, vbat, rssi]          // FLOW  (4 velden)
  ],
  "wf": <waterflow_pulsen>,
  "lr": <rssi_heltec>,
  "ls": <snr_heltec>
}
```

## Timing (5-minuten cyclus, DS3231 RTC)

| Node | Wake second |
|---|---|
| AGG + LoRaNode C3 | :05 |
| SOIL1 | :08 |
| SOIL2 | :16 |
