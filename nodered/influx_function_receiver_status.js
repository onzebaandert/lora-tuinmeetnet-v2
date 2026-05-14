const d = msg.payload;
const ts = Date.now() * 1000000;

// topic: tuin/receiver/status
// payload: {"uptime":123,"rx_total":10,"rx_mqtt_ok":10,"rssi_lora":-85.0,"wifi_rssi":-60}
// uptime: seconden → uren (afgerond op 2 decimalen)
const uptime_h = Math.round((d.uptime||0) / 3600 * 100) / 100;
msg.payload = 'receiver uptime=' + uptime_h + ',rx_total=' + (d.rx_total||0) + 'i,rx_mqtt_ok=' + (d.rx_mqtt_ok||0) + 'i,rssi_lora=' + (d.rssi_lora||0) + ',wifi_rssi=' + (d.wifi_rssi||0) + 'i ' + ts;
msg.headers = {'Authorization': 'Token x84Y0jPGr42X1QCA9rIYFU9GXXuHzMNJBMUdv9v9xPr_baeVKm-WUIB6FIHRW6eRTNfWM-8obb_eJ8J7UT3e2Q==', 'Content-Type': 'text/plain'};
msg.url = 'http://localhost:8086/api/v2/write?org=tuinmeetnet&bucket=tuinsensoren_v4&precision=ns';
return msg;
