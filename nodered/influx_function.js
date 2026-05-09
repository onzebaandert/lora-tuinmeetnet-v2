const d = msg.payload;
const ts = Date.now() * 1000000;
const lines = [];

lines.push('systeem agg_seq=' + (d.a||0) + 'i,rssi_lora=' + (d.r||0) + ',vbat_agg=' + (d.av||0) + 'i,temp_agg=' + (d.ac||0) + ',vbat_mesh=' + (d.mv||0) + 'i,temp_mesh=' + (d.mc||0) + ',rssi_heltec=' + (d.lr||0) + ',snr_heltec=' + (d.ls||0) + ' ' + ts);

if (d.wf !== undefined) {
  lines.push('waterflow pulsen=' + d.wf + 'i ' + ts);
}

if (Array.isArray(d.s)) {
  d.s.forEach(function(a) {
    if (a.length === 7) {
      lines.push('soil1 temperatuur=' + a[0] + ',vochtigheid=' + a[1] + ',ec=' + Math.round(a[2]) + 'i,temp_case=' + a[3] + ',vbat=' + a[4] + 'i,rssi=' + a[5] + ',ph=' + a[6] + ' ' + ts);
    } else if (a.length === 5) {
      lines.push('soil2 temperatuur=' + a[0] + ',vochtigheid=' + a[1] + ',temp_case=' + a[2] + ',vbat=' + a[3] + 'i,rssi=' + a[4] + ' ' + ts);
    } else if (a.length === 4) {
      lines.push('flow flow_rate=' + a[0] + ',total_liters=' + a[1] + ',vbat=' + a[2] + 'i,rssi=' + a[3] + ' ' + ts);
    }
  });
}

if (d.bh !== undefined) {
  lines.push('bh1750 lux=' + d.bh[0] + 'i,temp_case=' + d.bh[1] + ',vbat=' + d.bh[2] + 'i,rssi=' + d.bh[3] + ' ' + ts);
}

msg.payload = lines.join(String.fromCharCode(10));
msg.headers = {'Authorization': 'Token B9WHlDAmAKTof1ER9arNe9a0kr-g9OSrODn7_vtRH0n99ziOqs1u3BFDElMwfet_wQZkNi5aQJfjVy72LqYDYw==', 'Content-Type': 'text/plain'};
msg.url = 'http://localhost:8086/api/v2/write?org=tuinmeetnet&bucket=tuinsensoren_v2&precision=ns';
return msg;
