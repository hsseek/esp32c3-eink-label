#pragma once
#include <Arduino.h>
#include <pgmspace.h>

// Both pages are fully self-contained: no CDN, no external fonts, no images.
// Everything the phone needs arrives in this one response.

static const char PAGE_MAIN[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>e-ink label</title>
<style>
*{box-sizing:border-box}
:root{--bg:#12141a;--card:#1c1f27;--line:#2c3140;--fg:#e8eaf0;--mut:#8b93a7;--acc:#5b8dee;--err:#ff6b6b;--ok:#4ec9a0}
html,body{margin:0;background:var(--bg);color:var(--fg);
  font:16px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
  -webkit-text-size-adjust:100%}
main{max-width:520px;margin:0 auto;padding:18px 16px 40px}
h1{font-size:19px;margin:4px 0 16px;letter-spacing:.02em}
h1 span{color:var(--mut);font-weight:400}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin-bottom:14px}
#pvwrap{background:#fff;border-radius:8px;padding:6px;line-height:0}
#pv{width:100%;height:auto;image-rendering:pixelated;image-rendering:crisp-edges}
.muted{color:var(--mut);font-size:13px;margin:8px 2px 0}
.seg{display:flex;gap:6px;background:#141720;border:1px solid var(--line);border-radius:10px;padding:4px;margin-bottom:12px}
.seg input{position:absolute;opacity:0;pointer-events:none}
.seg label{flex:1;text-align:center;padding:9px 0;border-radius:7px;font-weight:600;font-size:14px;
  color:var(--mut);cursor:pointer;user-select:none}
.seg input:checked+label{background:var(--acc);color:#fff}
textarea{width:100%;min-height:88px;resize:vertical;background:#141720;color:var(--fg);
  border:1px solid var(--line);border-radius:10px;padding:11px;font:15px/1.4 inherit}
textarea:focus,select:focus{outline:2px solid var(--acc);outline-offset:-1px}
.row{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:10px}
label.fld{font-size:14px;color:var(--mut)}
select{background:#141720;color:var(--fg);border:1px solid var(--line);border-radius:9px;
  padding:9px 10px;font:15px inherit}
button{width:100%;margin-top:12px;padding:13px;border:0;border-radius:11px;background:var(--acc);
  color:#fff;font:600 16px inherit;cursor:pointer}
button:disabled{opacity:.5}
button.ghost{background:transparent;border:1px solid var(--line);color:var(--mut)}
#msg{min-height:20px;font-size:14px;margin:2px 2px 14px}
#msg.err{color:var(--err)} #msg.ok{color:var(--ok)}
a{color:var(--acc)}
.hide{display:none}
</style></head><body><main>

<h1>e-ink label <span id="dim"></span></h1>

<div class="card">
  <div id="pvwrap"><canvas id="pv" width="250" height="122"></canvas></div>
  <p class="muted" id="now">loading&hellip;</p>
</div>

<form class="card" id="f">
  <div class="seg">
    <input type="radio" name="mode" id="m1" value="text" checked><label for="m1">TEXT</label>
    <input type="radio" name="mode" id="m2" value="qr"><label for="m2">QR</label>
  </div>
  <textarea id="txt" placeholder="What should the label say?" maxlength="400"></textarea>
  <div class="row">
    <label class="fld" id="szrow">Font size
      <select id="sz">
        <option value="1">Small</option>
        <option value="2" selected>Medium</option>
        <option value="3">Large</option>
      </select>
    </label>
    <span class="muted" id="cnt">0</span>
  </div>
  <button id="go" type="submit">Update display</button>
  <button id="clr" type="button" class="ghost">Clear panel</button>
</form>

<p id="msg"></p>
<p class="muted"><span id="net">&mdash;</span> &middot; <a href="/wifi">Wi-Fi settings</a></p>

<script>
var $=function(s){return document.querySelector(s)};
function say(t,cls){var m=$("#msg");m.textContent=t;m.className=cls||""}

function paint(b64,w,h){
  var c=$("#pv");c.width=w;c.height=h;
  var ctx=c.getContext("2d"),img=ctx.createImageData(w,h),d=img.data;
  var raw=atob(b64),stride=(w+7)>>3;
  for(var y=0;y<h;y++){
    for(var x=0;x<w;x++){
      var byte=raw.charCodeAt(y*stride+(x>>3));
      var bit=(byte>>(7-(x&7)))&1;      // 1 = black ink
      var o=(y*w+x)*4,v=bit?0:255;
      d[o]=d[o+1]=d[o+2]=v;d[o+3]=255;
    }
  }
  ctx.putImageData(img,0,0);
}

function modeText(){return $("#m1").checked}
function syncMode(){
  $("#szrow").className=modeText()?"fld":"fld hide";
  $("#txt").placeholder=modeText()?"What should the label say?":"URL or text to encode";
}
function syncCount(){$("#cnt").textContent=$("#txt").value.length+" chars"}

function load(){
  fetch("/api/status").then(function(r){return r.json()}).then(function(j){
    paint(j.preview,j.w,j.h);
    $("#dim").textContent=j.w+"×"+j.h;
    $("#now").textContent=j.mode==="none"
      ? "panel is blank"
      : j.mode.toUpperCase()+" · "+j.updates+" update"+(j.updates==1?"":"s")+" since boot";
    if(j.mode!=="none"){
      $(j.mode==="qr"?"#m2":"#m1").checked=true;
      $("#txt").value=j.text;
      $("#sz").value=j.size;
    }
    $("#net").textContent=j.ap?("AP "+j.ssid+" · "+j.ip)
                              :(j.ssid+" · "+j.ip+" · "+j.rssi+" dBm");
    syncMode();syncCount();
  }).catch(function(){say("cannot reach the label","err")});
}

function post(url,body){
  var go=$("#go");go.disabled=true;say("working…");
  return fetch(url,{method:"POST",body:body}).then(function(r){return r.json()})
    .then(function(j){
      go.disabled=false;
      if(j.ok){say("display updated","ok");load()}else{say(j.error,"err")}
    }).catch(function(){go.disabled=false;say("request failed","err")});
}

$("#f").addEventListener("submit",function(e){
  e.preventDefault();
  var b=new URLSearchParams();
  b.set("mode",modeText()?"text":"qr");
  b.set("text",$("#txt").value);
  b.set("size",$("#sz").value);
  post("/api/display",b);
});
$("#clr").addEventListener("click",function(){post("/api/clear",new URLSearchParams())});
$("#m1").addEventListener("change",syncMode);
$("#m2").addEventListener("change",syncMode);
$("#txt").addEventListener("input",syncCount);
load();
</script>
</main></body></html>)HTML";

static const char PAGE_WIFI[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>e-ink label &mdash; Wi-Fi</title>
<style>
*{box-sizing:border-box}
:root{--bg:#12141a;--card:#1c1f27;--line:#2c3140;--fg:#e8eaf0;--mut:#8b93a7;--acc:#5b8dee;--err:#ff6b6b;--ok:#4ec9a0}
html,body{margin:0;background:var(--bg);color:var(--fg);
  font:16px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
main{max-width:520px;margin:0 auto;padding:18px 16px 40px}
h1{font-size:19px;margin:4px 0 6px}
p.lead{color:var(--mut);font-size:14px;margin:0 0 16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px}
label{display:block;font-size:13px;color:var(--mut);margin:12px 2px 5px}
input,select{width:100%;background:#141720;color:var(--fg);border:1px solid var(--line);
  border-radius:10px;padding:11px;font:15px inherit}
button{width:100%;margin-top:16px;padding:13px;border:0;border-radius:11px;background:var(--acc);
  color:#fff;font:600 16px inherit}
button.ghost{background:transparent;border:1px solid var(--line);color:var(--mut);margin-top:8px}
#msg{min-height:20px;font-size:14px;margin:12px 2px}
#msg.err{color:var(--err)} #msg.ok{color:var(--ok)}
a{color:var(--acc);font-size:14px}
</style></head><body><main>
<h1>Wi-Fi setup</h1>
<p class="lead">The label joins your 2.4 GHz network. 5 GHz-only networks are not supported by the ESP32-C3.</p>
<form class="card" id="f">
  <label for="pick">Network</label>
  <select id="pick"><option value="">scanning&hellip;</option></select>
  <label for="ssid">SSID</label>
  <input id="ssid" autocapitalize="none" autocomplete="off" spellcheck="false" required>
  <label for="pass">Password <span id="open"></span></label>
  <input id="pass" type="password" autocapitalize="none" autocorrect="off" spellcheck="false" autocomplete="off">
  <button type="submit">Save &amp; reboot</button>
  <button type="button" class="ghost" id="rescan">Rescan</button>
</form>
<p id="msg"></p>
<p><a href="/">&larr; back to the label</a></p>
<script>
var $=function(s){return document.querySelector(s)};
function say(t,c){var m=$("#msg");m.textContent=t;m.className=c||""}
function scan(){
  $("#pick").innerHTML="<option value=''>scanning…</option>";
  fetch("/api/scan").then(function(r){return r.json()}).then(function(j){
    var o="<option value=''>— pick a network —</option>";
    j.nets.forEach(function(n){
      o+="<option value='"+n.ssid.replace(/'/g,"&#39;")+"'>"+
         n.ssid+"  ("+n.rssi+" dBm"+(n.open?", open":"")+")</option>";
    });
    $("#pick").innerHTML=o;
  }).catch(function(){say("scan failed","err")});
}
$("#pick").addEventListener("change",function(){if(this.value)$("#ssid").value=this.value});
$("#rescan").addEventListener("click",scan);
$("#f").addEventListener("submit",function(e){
  e.preventDefault();
  var b=new URLSearchParams();
  b.set("ssid",$("#ssid").value);b.set("pass",$("#pass").value);
  say("saving…");
  fetch("/api/wifi",{method:"POST",body:b}).then(function(r){return r.json()}).then(function(j){
    if(j.ok)say("saved — rebooting, reconnect your phone to your home Wi-Fi","ok");
    else say(j.error,"err");
  }).catch(function(){say("request failed","err")});
});
scan();
</script>
</main></body></html>)HTML";
