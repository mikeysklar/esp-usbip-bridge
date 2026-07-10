/*
 * USB/IP Bridge — browser-side script for the status page.
 *
 * This file is embedded verbatim into the served HTML (see
 * EMBED_TXTFILES in main/CMakeLists.txt and stream_html_page() in
 * http_server.c).  Keep it self-contained: it must run in a plain
 * <script> tag with no module loader and no build step.
 *
 * Validate syntax with:  node --check main/web_app.js
 */
'use strict';

/* Refresh all GPIO pin values, wire labels, directions and pull states. */
function rp() {
  var x = new XMLHttpRequest();
  x.open('GET', '/api/pins', true);
  x.onload = function () {
    if (x.status != 200) return;
    var r = JSON.parse(x.responseText);
    r.pins.forEach(function (p) {
      var e = document.getElementById('v' + p.name);
      if (e) {
        e.textContent = p.value ? 'HIGH' : 'LOW';
        e.className = p.value ? 'hi' : 'lo';
      }
      var w = document.getElementById('w' + p.name);
      if (w) {
        w.value = p.wire;
        w.onchange = function () { sw(this); };
      }
      var d = document.getElementById('d' + p.name);
      if (d) { d.value = p.dir; }
    });
    if (r.pulls) {
      r.pulls.forEach(function (p) {
        var s = document.getElementById('pl' + p.header);
        if (s) { s.value = p.state; }
      });
    }
  };
  x.send();
}

/* Set a header-pin pull resistor via its IO-expander line. */
function spul(sel) {
  var exp = sel.getAttribute('data-exp');
  var v = sel.value;
  var x = new XMLHttpRequest();
  x.open('POST', '/api/pins/' + exp, true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () { if (x.status == 200) { rp(); } };
  var b;
  if (v == 'up') { b = '{"direction":"out","value":1}'; }
  else if (v == 'down') { b = '{"direction":"out","value":0}'; }
  else { b = '{"direction":"in"}'; }
  x.send(b);
}

/* Switch between the GPIO-pins and USB-devices tabs. */
function st(n) {
  ['p', 'u'].forEach(function (x) {
    document.getElementById('t' + x).className = '';
    document.getElementById('s' + x).className = 'sec';
  });
  document.getElementById('t' + n).className = 'on';
  document.getElementById('s' + n).className = 'sec on';
}

/* Change a pin's direction (IN / OUT). */
function sd(p) {
  var d = document.getElementById('d' + p).value;
  var x = new XMLHttpRequest();
  x.open('POST', '/api/pins/' + p, true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () { if (x.status == 200) { rp(); } };
  x.send(JSON.stringify({ direction: d }));
}

/* Toggle a pin's output level. */
function tg(p) {
  var x = new XMLHttpRequest();
  x.onload = function () { if (x.status == 200) { rp(); } };
  x.open('POST', '/api/pins/' + p + '/toggle', true);
  x.send();
}

/* Read a single pin and update its cell. */
function rr(p) {
  var x = new XMLHttpRequest();
  x.open('GET', '/api/pins/' + p, true);
  x.onload = function () {
    if (x.status != 200) return;
    var r = JSON.parse(x.responseText);
    var e = document.getElementById('v' + p);
    if (e) {
      e.textContent = r.value ? 'HIGH' : 'LOW';
      e.className = r.value ? 'hi' : 'lo';
    }
  };
  x.send();
}

/* Save a DUT wire name typed into a pin row. */
function sw(elem) {
  var p = elem.id.replace(/^w/, '');
  var x = new XMLHttpRequest();
  x.open('POST', '/api/pins/' + p, true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () { if (x.status == 200) { rp(); } };
  x.send(JSON.stringify({ wire: elem.value }));
}

/* Hostname editing inline UI. */
function he() {
  var h = document.getElementById('hostname');
  var inp = document.getElementById('hn-input');
  inp.value = h.textContent;
  h.style.display = 'none';
  inp.style.display = 'inline';
  document.getElementById('hn-edit').style.display = 'none';
  document.getElementById('hn-save').style.display = 'inline';
  document.getElementById('hn-cancel').style.display = 'inline';
  inp.focus();
  inp.select();
}

function hs() {
  var inp = document.getElementById('hn-input');
  var x = new XMLHttpRequest();
  x.open('POST', '/api/hostname', true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () {
    if (x.status == 200) {
      document.getElementById('hostname').textContent = inp.value;
      hc();
    }
  };
  x.send(JSON.stringify({ hostname: inp.value }));
}

function hc() {
  document.getElementById('hostname').style.display = 'inline';
  document.getElementById('hn-input').style.display = 'none';
  document.getElementById('hn-edit').style.display = 'inline';
  document.getElementById('hn-save').style.display = 'none';
  document.getElementById('hn-cancel').style.display = 'none';
}

/* Config export: GET /api/config and download as <hostname>-config.json. */
function cfgExport() {
  var x = new XMLHttpRequest();
  x.open('GET', '/api/config', true);
  x.onload = function () {
    if (x.status != 200) return;
    var b = new Blob([x.responseText], { type: 'application/json' });
    var a = document.createElement('a');
    a.href = URL.createObjectURL(b);
    var hn = 'usbip';
    try { hn = JSON.parse(x.responseText).hostname || 'usbip'; } catch (e) {}
    a.download = hn + '-config.json';
    document.body.appendChild(a);
    a.click();
    setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 0);
  };
  x.send();
}

/* Config import: open the hidden file picker. */
function cfgImportBtn() {
  document.getElementById('cfgFile').click();
}

/* Config import: read the chosen file and POST it to /api/config. */
function cfgImportFile(inp) {
  if (!inp.files || !inp.files[0]) return;
  var f = inp.files[0];
  var fr = new FileReader();
  fr.onload = function () {
    var x = new XMLHttpRequest();
    x.open('POST', '/api/config', true);
    x.setRequestHeader('Content-Type', 'application/json');
    x.onload = function () {
      if (x.status != 200) {
        alert('Import failed: HTTP ' + x.status);
        return;
      }
      var r;
      try { r = JSON.parse(x.responseText); } catch (e) {}
      if (r && r.error) {
        alert('Import failed: ' + r.error);
        return;
      }
      var msg = 'Config imported.';
      if (r) {
        msg = 'Config imported (' + r.applied + ' wires applied, ' +
              r.skipped + ' skipped).';
      }
      alert(msg + ' Reloading...');
      location.reload();
    };
    x.send(fr.result);
  };
  fr.readAsText(f);
  inp.value = '';
}

/* Refresh pin values, wire names, directions and pull states on initial
   load.  Wire names are also pre-filled server-side in the HTML, but this
   keeps the live state (values/pulls) current too. */
rp();
