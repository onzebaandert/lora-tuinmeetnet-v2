const d = msg.payload;
const ts = Date.now() * 1000000;

msg.payload = 'mqtt_dht22 mqtt_case_temp=' + (d.temp||0) + ',mqtt_hum=' + (d.hum||0) + ' ' + ts;
msg.headers = {'Authorization': 'Token x84Y0jPGr42X1QCA9rIYFU9GXXuHzMNJBMUdv9v9xPr_baeVKm-WUIB6FIHRW6eRTNfWM-8obb_eJ8J7UT3e2Q==', 'Content-Type': 'text/plain'};
msg.url = 'http://localhost:8086/api/v2/write?org=tuinmeetnet&bucket=tuinsensoren_v4&precision=ns';
return msg;
