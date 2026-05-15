#!/usr/bin/env python3
"""Create 4 Grafana dashboards for tuinmeetnet-v2."""

import json
import requests

GRAFANA = "http://192.168.2.13:3000"
API_KEY = "GRAFANA_API_KEY_HIER_INVULLEN"
DS_UID = "afkx36iy17i0wd"
BUCKET = "tuinsensoren_v3"

HDR = {
    "Authorization": f"Bearer {API_KEY}",
    "Content-Type": "application/json",
    "Accept": "application/json",
}


# ── Flux helpers ──────────────────────────────────────────────────────────────

def q(meas, field, label=None):
    label = label or field
    return (
        f'from(bucket: "{BUCKET}")\n'
        f'  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
        f'  |> filter(fn: (r) => r._measurement == "{meas}" and r._field == "{field}")\n'
        f'  |> aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)\n'
        f'  |> set(key: "_field", value: "{label}")\n'
        f'  |> yield(name: "{label}")'
    )


def q_moving_avg(meas, field, label=None, period="10m"):
    label = label or field
    return (
        f'from(bucket: "{BUCKET}")\n'
        f'  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n'
        f'  |> filter(fn: (r) => r._measurement == "{meas}" and r._field == "{field}")\n'
        f'  |> timedMovingAverage(every: v.windowPeriod, period: {period})\n'
        f'  |> set(key: "_field", value: "{label}")\n'
        f'  |> yield(name: "{label}")'
    )


def q_last(meas, field, label=None):
    label = label or field
    return (
        f'from(bucket: "{BUCKET}")\n'
        f'  |> range(start: -24h)\n'
        f'  |> filter(fn: (r) => r._measurement == "{meas}" and r._field == "{field}")\n'
        f'  |> last()\n'
        f'  |> set(key: "_field", value: "{label}")\n'
        f'  |> yield(name: "{label}")'
    )


def q_alert(meas, field, window="30m"):
    return (
        f'from(bucket: "{BUCKET}")\n'
        f'  |> range(start: -{window})\n'
        f'  |> filter(fn: (r) => r._measurement == "{meas}" and r._field == "{field}")\n'
        f'  |> aggregateWindow(every: 5m, fn: mean, createEmpty: false)\n'
        f'  |> last()'
    )


# ── Panel ID counter ──────────────────────────────────────────────────────────

_pid = 0


def pid():
    global _pid
    _pid += 1
    return _pid


def reset_pid():
    global _pid
    _pid = 0


# ── Target builder ────────────────────────────────────────────────────────────

def T(ref_id, flux):
    return {
        "refId": ref_id,
        "datasource": {"type": "influxdb", "uid": DS_UID},
        "query": flux,
        "hide": False,
    }


def targets(flux_list):
    return [T(chr(65 + i), f) for i, f in enumerate(flux_list)]


# ── Color override helper ─────────────────────────────────────────────────────

def color_ov(name, color):
    return {
        "matcher": {"id": "byName", "options": name},
        "properties": [{"id": "color", "value": {"mode": "fixed", "fixedColor": color}}],
    }


# ── Panel builders ────────────────────────────────────────────────────────────

def ts(title, x, y, w, h, flux_list, unit="", decimals=None,
       threshold_steps=None, overrides=None):
    custom = {"lineWidth": 2, "fillOpacity": 10, "spanNulls": True}
    defaults = {"custom": custom, "unit": unit}
    if decimals is not None:
        defaults["decimals"] = decimals
    if threshold_steps:
        defaults["thresholds"] = {"mode": "absolute", "steps": threshold_steps}
        custom["thresholdsStyle"] = {"mode": "line+area"}
    return {
        "type": "timeseries",
        "id": pid(),
        "title": title,
        "gridPos": {"x": x, "y": y, "w": w, "h": h},
        "datasource": {"type": "influxdb", "uid": DS_UID},
        "targets": targets(flux_list),
        "fieldConfig": {"defaults": defaults, "overrides": overrides or []},
        "options": {
            "tooltip": {"mode": "multi", "sort": "none"},
            "legend": {"displayMode": "list", "placement": "bottom", "showLegend": True},
        },
    }


def stat(title, x, y, w, h, flux, unit="", threshold_steps=None):
    steps = threshold_steps or [{"color": "green", "value": None}]
    return {
        "type": "stat",
        "id": pid(),
        "title": title,
        "gridPos": {"x": x, "y": y, "w": w, "h": h},
        "datasource": {"type": "influxdb", "uid": DS_UID},
        "targets": [T("A", flux)],
        "fieldConfig": {
            "defaults": {
                "unit": unit,
                "thresholds": {"mode": "absolute", "steps": steps},
            },
            "overrides": [],
        },
        "options": {
            "reduceOptions": {"calcs": ["lastNotNull"]},
            "colorMode": "background",
            "graphMode": "area",
            "justifyMode": "center",
            "textMode": "auto",
        },
    }


def row(title, y):
    return {
        "type": "row",
        "id": pid(),
        "title": title,
        "gridPos": {"x": 0, "y": y, "w": 24, "h": 1},
        "collapsed": False,
        "panels": [],
    }


def make_dashboard(uid, title, panels):
    return {
        "dashboard": {
            "id": None,
            "uid": uid,
            "title": title,
            "tags": ["tuin"],
            "timezone": "browser",
            "refresh": "1m",
            "time": {"from": "now-1h", "to": "now"},
            "timepicker": {},
            "panels": panels,
            "schemaVersion": 39,
            "version": 0,
            "editable": True,
        },
        "folderId": 0,
        "overwrite": True,
        "message": "Created by Claude Code",
    }


# ── DASHBOARD 1: Moestuin ─────────────────────────────────────────────────────

def build_moestuin():
    reset_pid()
    p, y = [], 0

    p.append(row("🫐 Blauwe bessen", y)); y += 1
    p.append(ts("Bodemtemperatuur — soil1", 0, y, 12, 8,
                [q("soil1", "temperatuur", "soil1-soiltemp")], unit="celsius"))
    p.append(ts("Bodemvochtigheid — soil1", 12, y, 12, 8,
                [q("soil1", "vochtigheid", "soil1-soilhum")], unit="humidity"))
    y += 8

    p.append(ts("EC — soil1", 0, y, 12, 8,
                [q("soil1", "ec", "soil1-soilec")], unit="µS/cm"))
    p.append(ts("pH — soil1 (moving avg 10 min)", 12, y, 12, 8,
                [
                    q("soil1", "ph", "pH raw"),
                    q_moving_avg("soil1", "ph", "pH moving avg 10m", "10m"),
                ],
                overrides=[
                    color_ov("pH raw", "light-blue"),
                    color_ov("pH moving avg 10m", "blue"),
                ]))
    y += 8

    p.append(row("🥝 Kiwi bessen", y)); y += 1
    p.append(ts("Bodemtemperatuur — soil2", 0, y, 12, 8,
                [q("soil2", "temperatuur", "soil2-soiltemp")], unit="celsius"))
    p.append(ts("Bodemvochtigheid — soil2", 12, y, 12, 8,
                [q("soil2", "vochtigheid", "soil2-soilhum")], unit="humidity"))
    y += 8

    p.append(row("☀️ Zon", y)); y += 1
    p.append(ts("Lichtsterkte", 0, y, 24, 8,
                [
                    q("bh1750",       "lux", "bh1750-lux"),
                    q("sensorstation","lux", "sensorstation-lux"),
                ],
                unit="lux",
                overrides=[
                    color_ov("bh1750-lux",       "yellow"),
                    color_ov("sensorstation-lux", "orange"),
                ]))
    y += 8

    p.append(row("🌡️ Sensorstation", y)); y += 1
    p.append(ts("Temperatuur — sensorstation (SHT3x)", 0, y, 12, 8,
                [q("sensorstation", "temperatuur", "stnst-temp")], unit="celsius"))
    p.append(ts("Luchtvochtigheid — sensorstation (SHT3x)", 12, y, 12, 8,
                [q("sensorstation", "vochtigheid", "stnst-hum")], unit="humidity"))
    y += 8

    p.append(row("💧 Water", y)); y += 1
    p.append(ts("Totaal liters — wf1", 0, y, 24, 8,
                [q("flow", "total_liters", "wf1-totalliters")], unit="litre"))

    return make_dashboard("moestuin", "Moestuin", p)


# ── DASHBOARD 2: Systeem ──────────────────────────────────────────────────────

CASE_COLORS = {
    "soil1": "blue", "soil2": "green", "agg": "orange",
    "lnC3": "red", "wf1": "purple", "stnst": "yellow",
}

VBAT_COLORS = {
    "soil1": "blue", "soil2": "green", "wf": "light-blue",
    "agg": "orange", "lnC3": "purple",
    "lnHelTuin": "yellow", "lnHelTuin-tbat": "super-light-blue",
    "stnst": "orange",
}


def build_systeem():
    reset_pid()
    p, y = [], 0

    # Behuizing temperaturen
    p.append(row("🌡️ Behuizing temperaturen", y)); y += 1
    p.append({
        "type": "timeseries",
        "id": pid(),
        "title": "Behuizingstemperatuur alle nodes",
        "gridPos": {"x": 0, "y": y, "w": 24, "h": 9},
        "datasource": {"type": "influxdb", "uid": DS_UID},
        "targets": targets([
            q("soil1",        "temp_case", "soil1"),
            q("soil2",        "temp_case", "soil2"),
            q("systeem",      "temp_agg",  "agg"),
            q("systeem",      "temp_mesh", "lnC3"),
            q("bh1750",       "temp_case", "wf1"),
            q("sensorstation","temp_case", "stnst"),
        ]),
        "fieldConfig": {
            "defaults": {
                "unit": "celsius",
                "custom": {
                    "lineWidth": 2, "fillOpacity": 10, "spanNulls": True,
                    "thresholdsStyle": {"mode": "line"},
                },
                "thresholds": {
                    "mode": "absolute",
                    "steps": [
                        {"color": "green",  "value": None},
                        {"color": "orange", "value": 45},
                        {"color": "red",    "value": 55},
                    ],
                },
            },
            "overrides": [color_ov(n, c) for n, c in CASE_COLORS.items()],
        },
        "options": {
            "tooltip": {"mode": "multi", "sort": "none"},
            "legend": {"displayMode": "list", "placement": "bottom", "showLegend": True},
        },
    })
    y += 9

    # Batterij niveaus
    p.append(row("🔋 Batterij niveaus", y)); y += 1
    p.append({
        "type": "timeseries",
        "id": pid(),
        "title": "Batterijniveaus alle nodes",
        "gridPos": {"x": 0, "y": y, "w": 24, "h": 9},
        "datasource": {"type": "influxdb", "uid": DS_UID},
        "targets": targets([
            q("soil1",        "vbat",          "soil1"),
            q("soil2",        "vbat",          "soil2"),
            q("flow",         "vbat",          "wf"),
            q("systeem",      "vbat_agg",      "agg"),
            q("systeem",      "lnC3_vbat",     "lnC3"),
            q("systeem",      "lnHel_vbat",    "lnHelTuin"),
            q("systeem",      "lnHelTuin_tbat","lnHelTuin-tbat"),
            q("sensorstation","vbat",          "stnst"),
        ]),
        "fieldConfig": {
            "defaults": {
                "unit": "mV",
                "custom": {"lineWidth": 2, "fillOpacity": 10, "spanNulls": True},
                "thresholds": {
                    "mode": "absolute",
                    "steps": [
                        {"color": "red",    "value": None},
                        {"color": "orange", "value": 3400},
                        {"color": "yellow", "value": 3500},
                        {"color": "green",  "value": 3700},
                    ],
                },
            },
            "overrides": [color_ov(n, c) for n, c in VBAT_COLORS.items()],
        },
        "options": {
            "tooltip": {"mode": "multi", "sort": "none"},
            "legend": {"displayMode": "list", "placement": "bottom", "showLegend": True},
        },
    })
    y += 9

    # Agg cycle count
    p.append(row("🔢 Agg cycle count", y)); y += 1
    p.append(ts("Agg cyclenummer", 0, y, 8, 5,
                [q("systeem", "agg_seq", "agg-cyclenr")], decimals=0))
    y += 5

    # MQTT node online
    p.append(row("📡 MQTT node online", y)); y += 1
    p.append(stat("lnHelTuin — laatste vbat waarde", 0, y, 6, 4,
                  q_last("systeem", "lnHel_vbat", "lnHelTuin-vbat"),
                  unit="mV",
                  threshold_steps=[
                      {"color": "red",    "value": None},
                      {"color": "orange", "value": 3400},
                      {"color": "yellow", "value": 3500},
                      {"color": "green",  "value": 3700},
                  ]))
    y += 4

    # lnHelTuin online via LoRa RSSI
    p.append(row("📶 lnHelTuin online", y)); y += 1
    p.append(stat("LoRa RSSI — lnHelTuin (laatste)", 0, y, 6, 4,
                  q_last("receiver", "rssi_lora", "LoRa RSSI"),
                  unit="dBm",
                  threshold_steps=[
                      {"color": "red",    "value": None},
                      {"color": "orange", "value": -110},
                      {"color": "yellow", "value": -90},
                      {"color": "green",  "value": -70},
                  ]))

    return make_dashboard("systeem", "Systeem", p)


# ── DASHBOARD 3: Ontvangst ────────────────────────────────────────────────────

ESPNOW_COLORS = {
    "soil1": "blue", "soil2": "green", "bh1750": "orange", "stnst": "yellow",
}


def build_ontvangst():
    reset_pid()
    p, y = [], 0

    p.append(row("📡 ESP-NOW RSSI", y)); y += 1
    p.append({
        "type": "timeseries",
        "id": pid(),
        "title": "ESP-NOW RSSI — soil1, soil2, bh1750, stnst",
        "gridPos": {"x": 0, "y": y, "w": 24, "h": 8},
        "datasource": {"type": "influxdb", "uid": DS_UID},
        "targets": targets([
            q("soil1",        "rssi", "soil1"),
            q("soil2",        "rssi", "soil2"),
            q("bh1750",       "rssi", "bh1750"),
            q("sensorstation","rssi", "stnst"),
        ]),
        "fieldConfig": {
            "defaults": {
                "unit": "dBm",
                "custom": {"lineWidth": 2, "fillOpacity": 10, "spanNulls": True},
            },
            "overrides": [color_ov(n, c) for n, c in ESPNOW_COLORS.items()],
        },
        "options": {
            "tooltip": {"mode": "multi", "sort": "none"},
            "legend": {"displayMode": "list", "placement": "bottom", "showLegend": True},
        },
    })
    y += 8

    p.append(row("📻 LoRa RSSI & SNR", y)); y += 1
    p.append(ts("LoRa RSSI — lnHelTuin", 0, y, 12, 8,
                [
                    q("receiver", "rssi_lora",    "LoRa RSSI (receiver)"),
                    q("systeem",  "rssi_heltec",  "RSSI Heltec"),
                ],
                unit="dBm",
                overrides=[
                    color_ov("LoRa RSSI (receiver)", "blue"),
                    color_ov("RSSI Heltec", "purple"),
                ]))
    p.append(ts("LoRa SNR — lnHelTuin", 12, y, 12, 8,
                [q("systeem", "snr_heltec", "LoRa SNR")], unit="dB"))
    y += 8

    p.append(row("📶 WiFi RSSI MQTT node", y)); y += 1
    p.append(ts("WiFi RSSI — receiver (lnHelTuin)", 0, y, 12, 8,
                [q("receiver", "wifi_rssi", "WiFi RSSI")], unit="dBm"))

    return make_dashboard("ontvangst", "Ontvangst", p)


# ── DASHBOARD 4: Waarschuwingen ───────────────────────────────────────────────

def build_waarschuwingen():
    reset_pid()
    p, y = [], 0

    p.append(row("🔋 Batterij waarschuwing (< 3500 mV, 3× achter elkaar)", y)); y += 1
    p.append({
        "type": "timeseries",
        "id": pid(),
        "title": "Batterijniveaus — alert bij < 3500 mV gedurende 15 min",
        "gridPos": {"x": 0, "y": y, "w": 24, "h": 10},
        "datasource": {"type": "influxdb", "uid": DS_UID},
        "targets": targets([
            q("soil1",  "vbat",      "soil1"),
            q("soil2",  "vbat",      "soil2"),
            q("flow",   "vbat",      "wf"),
            q("systeem","vbat_agg",  "agg"),
            q("systeem","lnC3_vbat", "lnC3"),
        ]),
        "fieldConfig": {
            "defaults": {
                "unit": "mV",
                "custom": {
                    "lineWidth": 2, "fillOpacity": 10, "spanNulls": True,
                    "thresholdsStyle": {"mode": "line+area"},
                },
                "thresholds": {
                    "mode": "absolute",
                    "steps": [
                        {"color": "red",    "value": None},
                        {"color": "orange", "value": 3400},
                        {"color": "yellow", "value": 3500},
                        {"color": "green",  "value": 3700},
                    ],
                },
            },
            "overrides": [color_ov(n, c) for n, c in {
                "soil1": "blue", "soil2": "green", "wf": "light-blue",
                "agg": "orange", "lnC3": "purple",
            }.items()],
        },
        "options": {
            "tooltip": {"mode": "multi"},
            "legend": {"displayMode": "list", "placement": "bottom", "showLegend": True},
        },
    })
    y += 10

    p.append(row("🌡️ Behuizing temperatuur waarschuwing (> 55 °C)", y)); y += 1
    p.append({
        "type": "timeseries",
        "id": pid(),
        "title": "Behuizingstemperaturen — directe alert bij > 55 °C",
        "gridPos": {"x": 0, "y": y, "w": 24, "h": 10},
        "datasource": {"type": "influxdb", "uid": DS_UID},
        "targets": targets([
            q("soil1",  "temp_case", "soil1"),
            q("soil2",  "temp_case", "soil2"),
            q("systeem","temp_agg",  "agg"),
            q("systeem","temp_mesh", "lnC3"),
        ]),
        "fieldConfig": {
            "defaults": {
                "unit": "celsius",
                "custom": {
                    "lineWidth": 2, "fillOpacity": 10, "spanNulls": True,
                    "thresholdsStyle": {"mode": "line+area"},
                },
                "thresholds": {
                    "mode": "absolute",
                    "steps": [
                        {"color": "green",  "value": None},
                        {"color": "orange", "value": 40},
                        {"color": "red",    "value": 55},
                    ],
                },
            },
            "overrides": [color_ov(n, c) for n, c in {
                "soil1": "blue", "soil2": "green",
                "agg": "orange", "lnC3": "red",
            }.items()],
        },
        "options": {
            "tooltip": {"mode": "multi"},
            "legend": {"displayMode": "list", "placement": "bottom", "showLegend": True},
        },
    })

    return make_dashboard("waarschuwingen", "Waarschuwingen", p)


# ── Alert rule builders ───────────────────────────────────────────────────────

def alert_data_query(meas, field):
    flux = q_alert(meas, field, "30m")
    return {
        "refId": "A",
        "queryType": "",
        "relativeTimeRange": {"from": 1800, "to": 0},
        "datasourceUid": DS_UID,
        "model": {
            "datasource": {"type": "influxdb", "uid": DS_UID},
            "query": flux,
            "refId": "A",
            "hide": False,
        },
    }


def alert_reduce():
    return {
        "refId": "B",
        "queryType": "",
        "relativeTimeRange": {"from": 0, "to": 0},
        "datasourceUid": "__expr__",
        "model": {
            "type": "reduce",
            "datasource": {"type": "__expr__", "uid": "__expr__"},
            "expression": "A",
            "reducer": "last",
            "settings": {"mode": "dropNN"},
            "refId": "B",
        },
    }


def alert_threshold(operator, value):
    return {
        "refId": "C",
        "queryType": "",
        "relativeTimeRange": {"from": 0, "to": 0},
        "datasourceUid": "__expr__",
        "model": {
            "type": "threshold",
            "datasource": {"type": "__expr__", "uid": "__expr__"},
            "expression": "B",
            "conditions": [{
                "evaluator": {"params": [value], "type": operator},
                "operator": {"type": "and"},
                "query": {"params": ["C"]},
                "reducer": {"params": [], "type": "last"},
                "type": "query",
            }],
            "refId": "C",
        },
    }


def make_battery_alert(node_label, meas, field, folder_uid):
    return {
        "title": f"Batterij laag — {node_label}",
        "ruleGroup": "battery_alerts",
        "folderUID": folder_uid,
        "for": "15m",
        "orgId": 1,
        "condition": "C",
        "data": [
            alert_data_query(meas, field),
            alert_reduce(),
            alert_threshold("lt", 3500),
        ],
        "noDataState": "NoData",
        "execErrState": "Error",
        "annotations": {"summary": f"Batterij van {node_label} onder 3500 mV"},
        "labels": {"type": "battery", "node": node_label},
    }


def make_temp_alert(node_label, meas, field, folder_uid):
    return {
        "title": f"Temperatuur te hoog — {node_label}",
        "ruleGroup": "temperature_alerts",
        "folderUID": folder_uid,
        "for": "0s",
        "orgId": 1,
        "condition": "C",
        "data": [
            alert_data_query(meas, field),
            alert_reduce(),
            alert_threshold("gt", 55),
        ],
        "noDataState": "NoData",
        "execErrState": "Error",
        "annotations": {"summary": f"Behuizingstemperatuur van {node_label} boven 55 °C"},
        "labels": {"type": "temperature", "node": node_label},
    }


# ── API calls ─────────────────────────────────────────────────────────────────

def api_get(path):
    r = requests.get(f"{GRAFANA}{path}", headers=HDR)
    r.raise_for_status()
    return r.json()


def api_post(path, payload, ok_codes=(200, 201)):
    r = requests.post(f"{GRAFANA}{path}", headers=HDR, json=payload)
    if r.status_code not in ok_codes:
        raise RuntimeError(f"POST {path} → {r.status_code}: {r.text[:300]}")
    return r.json()


def api_delete(path):
    r = requests.delete(f"{GRAFANA}{path}", headers=HDR)
    return r.status_code


def get_or_create_folder(title, uid):
    r = requests.get(f"{GRAFANA}/api/folders/{uid}", headers=HDR)
    if r.status_code == 200:
        return r.json()["uid"]
    r = requests.post(f"{GRAFANA}/api/folders", headers=HDR,
                      json={"title": title, "uid": uid})
    if r.status_code in (200, 201):
        return r.json()["uid"]
    # uid conflict — create without uid
    r2 = requests.post(f"{GRAFANA}/api/folders", headers=HDR, json={"title": title})
    r2.raise_for_status()
    return r2.json()["uid"]


def make_public(dash_uid):
    r = requests.get(f"{GRAFANA}/api/dashboards/uid/{dash_uid}/public-dashboards",
                     headers=HDR)
    if r.status_code == 200:
        data = r.json()
        pub_uid = data.get("uid")
        if pub_uid and data.get("isEnabled"):
            return data  # already public
        if pub_uid:
            r2 = requests.patch(
                f"{GRAFANA}/api/dashboards/uid/{dash_uid}/public-dashboards/{pub_uid}",
                headers=HDR,
                json={"isEnabled": True, "annotationsEnabled": False,
                      "timeSelectionEnabled": False},
            )
            r2.raise_for_status()
            return r2.json()
    # Create new public dashboard
    r = requests.post(
        f"{GRAFANA}/api/dashboards/uid/{dash_uid}/public-dashboards",
        headers=HDR,
        json={"isEnabled": True, "annotationsEnabled": False,
              "timeSelectionEnabled": False},
    )
    r.raise_for_status()
    return r.json()


def create_alert(rule):
    r = requests.post(f"{GRAFANA}/api/v1/provisioning/alert-rules",
                      headers=HDR, json=rule)
    if r.status_code not in (200, 201):
        print(f"     ✗ {rule['title']}: HTTP {r.status_code} — {r.text[:200]}")
        return None
    return r.json()


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    print("═══════════════════════════════════════════")
    print("  Tuinmeetnet — Dashboard creator")
    print("═══════════════════════════════════════════\n")

    # 1. Delete existing dashboards
    print("1. Bestaande dashboards verwijderen...")
    existing = api_get("/api/search?type=dash-db")
    for db in existing:
        code = api_delete(f"/api/dashboards/uid/{db['uid']}")
        print(f"   [{code}] {db['title']}  ({db['uid']})")

    # 2. Alert folder
    print("\n2. Alert folder aanmaken/ophalen...")
    folder_uid = get_or_create_folder("Tuinmeetnet Alerts", "tuin-alerts")
    print(f"   Folder UID: {folder_uid}")

    # 3. Create dashboards
    print("\n3. Dashboards aanmaken...")
    specs = [
        ("Moestuin",        "moestuin",        build_moestuin()),
        ("Systeem",         "systeem",          build_systeem()),
        ("Ontvangst",       "ontvangst",        build_ontvangst()),
        ("Waarschuwingen",  "waarschuwingen",   build_waarschuwingen()),
    ]
    for name, uid, db_json in specs:
        result = api_post("/api/dashboards/db", db_json)
        print(f"   ✓ {name:16s}  uid={uid}  url={result.get('url')}")

    # 4. Public dashboards
    print("\n4. Dashboards publiek maken...")
    public_urls = {}
    for name, uid, _ in specs:
        try:
            result = make_public(uid)
            token = result.get("accessToken", "???")
            url = f"https://grafana.biobiejo.nl/public-dashboards/{token}?kiosk"
            public_urls[name] = url
            print(f"   ✓ {name:16s}  {url}")
        except Exception as e:
            print(f"   ✗ {name}: {e}")

    # 5. Alert rules
    print("\n5. Alert regels aanmaken...")

    battery_nodes = [
        ("soil1",  "soil1",        "vbat"),
        ("soil2",  "soil2",        "vbat"),
        ("wf",     "flow",         "vbat"),
        ("agg",    "systeem",      "vbat_agg"),
        ("lnC3",   "systeem",      "lnC3_vbat"),
        ("stnst",  "sensorstation","vbat"),
    ]
    print("   Batterij-alerts (< 3500 mV, FOR 15 min):")
    for label, meas, field in battery_nodes:
        rule = make_battery_alert(label, meas, field, folder_uid)
        result = create_alert(rule)
        if result:
            print(f"     ✓ {label}")

    temp_nodes = [
        ("soil1", "soil1",   "temp_case"),
        ("soil2", "soil2",   "temp_case"),
        ("agg",   "systeem", "temp_agg"),
        ("lnC3",  "systeem", "temp_mesh"),
    ]
    print("   Temperatuur-alerts (> 55 °C, direct):")
    for label, meas, field in temp_nodes:
        rule = make_temp_alert(label, meas, field, folder_uid)
        result = create_alert(rule)
        if result:
            print(f"     ✓ {label}")

    # 6. Summary
    print("\n═══════════════════════════════════════════")
    print("  Klaar!\n")
    print("  Lokale links:")
    for name, uid, _ in specs:
        print(f"    {name:16s}  http://192.168.2.13:3000/d/{uid}")
    print()
    print("  Publieke kiosk-links:")
    for name, url in public_urls.items():
        print(f"    {name:16s}  {url}")
    print()
    print("  Veldmapping (afwijkend van gebruikersnamen):")
    print("    soil1-soiltemp    → soil1.temperatuur")
    print("    soil1-soilhum     → soil1.vochtigheid")
    print("    soil1-espnowrssi  → soil1.rssi")
    print("    agg-casetemp      → systeem.temp_agg")
    print("    lnC3-casetemp     → systeem.temp_mesh  (best guess)")
    print("    wf1-casetemp      → bh1750.temp_case   (bh1750 co-located?)")
    print("    wf-vbat           → flow.vbat")
    print("    lnHelTuin-vbat    → systeem.lnHel_vbat")
    print("    lnHelTuin-lorarssi→ receiver.rssi_lora + systeem.rssi_heltec")
    print("    agg-espnowrssi    → niet direct beschikbaar (bh1750.rssi gebruikt)")


if __name__ == "__main__":
    main()
