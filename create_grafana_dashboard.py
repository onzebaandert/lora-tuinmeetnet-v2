#!/usr/bin/env python3
"""
Maakt Tuinmeetsysteem dashboard aan in Grafana via de API.
Vul GRAFANA_API_KEY in en voer dan uit: python3 create_grafana_dashboard.py
"""

import sys
import requests

GRAFANA_URL = "http://192.168.2.13:3000"
GRAFANA_API_KEY = "VULL_IN"  # <-- jouw Grafana API key hier
BUCKET = "tuinsensoren"

HEADERS = {
    "Authorization": f"Bearer {GRAFANA_API_KEY}",
    "Content-Type": "application/json",
}

COLORS = ["blue", "green", "orange", "red", "purple", "yellow", "light-blue"]


def get_influx_uid():
    r = requests.get(f"{GRAFANA_URL}/api/datasources", headers=HEADERS)
    r.raise_for_status()
    for ds in r.json():
        if ds["type"] == "influxdb":
            print(f"  Datasource gevonden: '{ds['name']}' (uid: {ds['uid']})")
            return ds["uid"]
    print("FOUT: Geen InfluxDB datasource gevonden in Grafana!", file=sys.stderr)
    sys.exit(1)


def flux_query(measurement, moving_avg=False, n=10):
    q = (
        f'from(bucket: "{BUCKET}")\n'
        f"  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)\n"
        f'  |> filter(fn: (r) => r["_measurement"] == "{measurement}")\n'
        f"  |> aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)"
    )
    if moving_avg:
        q += f"\n  |> movingAverage(n: {n})"
    q += '\n  |> yield(name: "mean")'
    return q


def make_target(ref_id, measurement, ds_uid, moving_avg=False):
    return {
        "refId": ref_id,
        "query": flux_query(measurement, moving_avg),
        "datasource": {"type": "influxdb", "uid": ds_uid},
    }


def color_override(field_name, color):
    return {
        "matcher": {"id": "byName", "options": field_name},
        "properties": [
            {"id": "color", "value": {"mode": "fixed", "fixedColor": color}}
        ],
    }


def row_panel(panel_id, title, y):
    return {
        "id": panel_id,
        "type": "row",
        "title": title,
        "gridPos": {"x": 0, "y": y, "w": 24, "h": 1},
        "collapsed": False,
    }


def ts_panel(panel_id, title, x, y, w, h, targets, ds_uid, overrides=None):
    return {
        "id": panel_id,
        "type": "timeseries",
        "title": title,
        "gridPos": {"x": x, "y": y, "w": w, "h": h},
        "datasource": {"type": "influxdb", "uid": ds_uid},
        "targets": targets,
        "fieldConfig": {
            "defaults": {
                "custom": {"lineWidth": 2, "fillOpacity": 5},
            },
            "overrides": overrides or [],
        },
        "options": {
            "tooltip": {"mode": "multi", "sort": "none"},
            "legend": {"displayMode": "list", "placement": "bottom"},
        },
    }


def build_dashboard(ds_uid):
    panels = []
    pid = 1
    y = 0

    # ── ROW 1: Soil1 ──────────────────────────────────────────────────────────
    panels.append(row_panel(pid, "Soil1", y))
    pid += 1
    y += 1

    soil1_items = [
        ("Soil1 Temperatuur", "soil1-soiltemp", False),
        ("Soil1 Vochtigheid", "soil1-soilhum", False),
        ("Soil1 EC", "soil1-soilec", False),
        ("Soil1 pH (moving avg)", "soil1-ph", True),
    ]
    for i, (title, meas, mavg) in enumerate(soil1_items):
        panels.append(
            ts_panel(pid, title, i * 6, y, 6, 8,
                     [make_target("A", meas, ds_uid, mavg)], ds_uid)
        )
        pid += 1
    y += 8

    # ── ROW 2: Soil2 ──────────────────────────────────────────────────────────
    panels.append(row_panel(pid, "Soil2", y))
    pid += 1
    y += 1

    soil2_items = [
        ("Soil2 Temperatuur", "soil2-soiltemp"),
        ("Soil2 Vochtigheid", "soil2-soilhum"),
    ]
    for i, (title, meas) in enumerate(soil2_items):
        panels.append(
            ts_panel(pid, title, i * 12, y, 12, 8,
                     [make_target("A", meas, ds_uid)], ds_uid)
        )
        pid += 1
    y += 8

    # ── ROW 3: Waterflow ──────────────────────────────────────────────────────
    panels.append(row_panel(pid, "Waterflow", y))
    pid += 1
    y += 1

    panels.append(
        ts_panel(pid, "Waterverbruik totaal (L)", 0, y, 12, 6,
                 [make_target("A", "wf1-totalliters", ds_uid)], ds_uid)
    )
    pid += 1
    y += 6

    # ── ROW 4: BH1750 ─────────────────────────────────────────────────────────
    panels.append(row_panel(pid, "BH1750 Lichtmeter", y))
    pid += 1
    y += 1

    panels.append(
        ts_panel(pid, "BH1750 Lux", 0, y, 24, 8,
                 [make_target("A", "bh1750-lux", ds_uid)], ds_uid)
    )
    pid += 1
    y += 8

    # ── ROW 4: ESP-NOW RSSI ───────────────────────────────────────────────────
    panels.append(row_panel(pid, "ESP-NOW RSSI", y))
    pid += 1
    y += 1

    rssi_meases = ["soil1-espnowrssi", "soil2-espnowrssi", "agg-espnowrssi"]
    rssi_targets = [make_target(chr(65 + i), m, ds_uid) for i, m in enumerate(rssi_meases)]
    rssi_overrides = [color_override(m, COLORS[i]) for i, m in enumerate(rssi_meases)]
    panels.append(
        ts_panel(pid, "ESP-NOW RSSI", 0, y, 24, 8,
                 rssi_targets, ds_uid, rssi_overrides)
    )
    pid += 1
    y += 8

    # ── ROW 5: Behuizing Temperatuur ──────────────────────────────────────────
    panels.append(row_panel(pid, "Behuizing Temperatuur", y))
    pid += 1
    y += 1

    case_meases = [
        "soil1-casetemp", "soil2-casetemp", "agg-casetemp",
        "lnC3-casetemp", "wf1-casetemp", "bh1750-casetemp",
    ]
    case_targets = [make_target(chr(65 + i), m, ds_uid) for i, m in enumerate(case_meases)]
    case_overrides = [color_override(m, COLORS[i]) for i, m in enumerate(case_meases)]
    panels.append(
        ts_panel(pid, "Behuizing Temperaturen", 0, y, 24, 8,
                 case_targets, ds_uid, case_overrides)
    )
    pid += 1
    y += 8

    # ── ROW 6: Batterij Levels ────────────────────────────────────────────────
    panels.append(row_panel(pid, "Batterij Levels", y))
    pid += 1
    y += 1

    bat_meases = ["soil1-vbat", "soil2-vbat", "wf-vbat", "agg-vbat", "bh1750-vbat"]
    bat_targets = [make_target(chr(65 + i), m, ds_uid) for i, m in enumerate(bat_meases)]
    bat_overrides = [color_override(m, COLORS[i]) for i, m in enumerate(bat_meases)]
    panels.append(
        ts_panel(pid, "Batterij Voltages", 0, y, 24, 8,
                 bat_targets, ds_uid, bat_overrides)
    )

    return {
        "dashboard": {
            "title": "Tuinmeetsysteem",
            "tags": ["tuin", "lora"],
            "timezone": "browser",
            "time": {"from": "now-1h", "to": "now"},
            "timepicker": {},
            "refresh": "30s",
            "schemaVersion": 38,
            "version": 0,
            "panels": panels,
        },
        "folderUid": "",
        "message": "Aangemaakt via create_grafana_dashboard.py",
        "overwrite": True,
    }


def main():
    if GRAFANA_API_KEY == "VULL_IN":
        print("FOUT: Vul je Grafana API key in bij GRAFANA_API_KEY bovenaan het script.")
        sys.exit(1)

    print("Ophalen InfluxDB datasource UID...")
    ds_uid = get_influx_uid()

    print("Dashboard aanmaken...")
    payload = build_dashboard(ds_uid)
    r = requests.post(
        f"{GRAFANA_URL}/api/dashboards/db",
        headers=HEADERS,
        json=payload,
    )
    r.raise_for_status()
    result = r.json()
    url = result.get("url", "")
    print(f"Klaar! Dashboard: {GRAFANA_URL}{url}")


if __name__ == "__main__":
    main()
