#pragma once
const char html_success[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang=en>
<head>
<meta charset=UTF-8>
<title>WiFi Setup - Success</title>
<meta name=viewport content="width=device-width,initial-scale=1">
<style>
*,*::before,*::after{box-sizing:border-box}
body{font-family:sans-serif;margin:0;padding:20px;background:#f2f2f2}
.title-box{background-color:#1b3144;color:#fbf3f3;border-radius:12px;padding:15px;text-align:center;margin-bottom:20px;box-shadow:0 2px 6px rgba(0,0,0,0.15)}
.title-box h2{margin:0;font-size:24px}
.card{background:#fff;padding:20px 15px 25px 15px;border-radius:8px;box-shadow:0 0 10px rgba(0,0,0,.1);max-width:480px;margin:0 auto;text-align:center}
p{font-size:16px;color:#1b3144;margin:10px 0}
.note{font-size:13px;color:#56697b;margin-bottom:20px}
.btn{display:block;width:100%;padding:14px;margin-top:12px;border-radius:6px;border:0;font-size:16px;font-weight:bold;cursor:pointer;text-decoration:none;text-align:center}
.btn-add{background:#1b3144;color:#fff}
.btn-add:hover{background:#dd9904}
.btn-start{background:#2a8a2a;color:#fff}
.btn-start:hover{background:#1e6b1e}
</style>
</head>
<body>
<div class=title-box><h2>&#10003; Network Saved!</h2></div>
<div class=card>
<p>The network was tested and saved successfully.</p>
<p class=note>You can save up to 3 networks. The device will automatically connect to whichever is available.</p>
<a href="/" class="btn btn-add">Add Another Network</a>
<a href="/restart" class="btn btn-start">Start Device</a>
</div>
</body>
</html>
)rawliteral";
