const d = msg.payload;
const ts = Date.now() * 1000000;
const lines = [];

lines.push('systeem agg_seq=' + (d.a||0) + 'i,rssi_lora=' + (d.r||0) + ',vbat_agg=' + Math.round((d.av||0) * 0.9552) + 'i,temp_agg=' + (d.ac||0) + ',rssi_heltec=' + (d.lr||0) + ',snr_heltec=' + (d.ls||0) + ',lnHel_vbat=' + Math.round((d['lnHelTuin-vbat']||0) * 1.2020) + 'i,lnC3_vbat=' + Math.round((d['lnC3-vbat']||0) * 0.9763) + 'i,mqtt_vbat=' + Math.round((d['lnHelTuin-tbat']||0) * 1.0322) + 'i,temp_mesh=' + (d.mc||0) + ' ' + ts);

if (d.wf !== undefined) {
  lines.push('waterflow pulsen=' + d.wf + 'i ' + ts);
}

if (Array.isArray(d.s)) {
  d.s.forEach(function(a) {
    if (a.length === 7) {
      lines.push('soil1 temperatuur=' + a[0] + ',vochtigheid=' + a[1] + ',ec=' + Math.round(a[2]) + 'i,temp_case=' + a[3] + ',vbat=' + a[4] + 'i,rssi=' + a[5] + ',ph=' + (a[6] + 2.0) + ' ' + ts);
    } else if (a.length === 5) {
      lines.push('soil2 temperatuur=' + a[0] + ',vochtigheid=' + a[1] + ',temp_case=' + a[2] + ',vbat=' + Math.round(a[3] * 0.9901) + 'i,rssi=' + a[4] + ' ' + ts);
    } else if (a.length === 4) {
      lines.push('flow flow_rate=' + a[0] + ',total_liters=' + a[1] + ',vbat=' + a[2] + 'i,rssi=' + a[3] + ' ' + ts);
    }
  });
}

if (d.bh !== undefined) {
  lines.push('bh1750 lux=' + d.bh[0] + 'i,temp_case=' + d.bh[1] + ',vbat=' + d.bh[2] + 'i,rssi=' + d.bh[3] + ' ' + ts);
}

if (d.st !== undefined) {
  lines.push('sensorstation lux=' + d.st[0] + 'i,temperatuur=' + d.st[1] + ',vochtigheid=' + d.st[2] + ',temp_case=' + d.st[3] + ',vbat=' + d.st[4] + 'i,rssi=' + d.st[5] + ' ' + ts);
}

msg.payload = lines.join(String.fromCharCode(10));
msg.headers = {'Authorization': 'Token x84Y0jPGr42X1QCA9rIYFU9GXXuHzMNJBMUdv9v9xPr_baeVKm-WUIB6FIHRW6eRTNfWM-8obb_eJ8J7UT3e2Q==', 'Content-Type': 'text/plain'};
msg.url = 'http://localhost:8086/api/v2/write?org=tuinmeetnet&bucket=tuinsensoren_v4&precision=ns';
return msg;
