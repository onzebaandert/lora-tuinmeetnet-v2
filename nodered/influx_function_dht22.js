const d = msg.payload;
const ts = Date.now() * 1000000;

msg.payload = 'mqtt_dht22 mqtt_case_temp=' + (d.temp||0) + ',mqtt_hum=' + (d.hum||0) + ' ' + ts;
msg.headers = {'Authorization': 'Token yP9QgxCGpRP39-ZHPjPdUfRuKcJtDnNfp_PsIW5Btrl9kr75fBfqxWBuj6NXorAs_Z9wbEeTfKxYITsCbZRBWA==', 'Content-Type': 'text/plain'};
msg.url = 'http://localhost:8086/api/v2/write?org=tuinmeetnet&bucket=tuinsensoren_v3&precision=ns';
return msg;
