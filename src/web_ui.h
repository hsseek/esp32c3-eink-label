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
#pvwrap{background:#fff;border-radius:8px;padding:6px;line-height:0;box-shadow:0 0 0 0 transparent;transition:box-shadow .15s}
#pvwrap.pending{box-shadow:0 0 0 2px var(--acc)}
#pv{width:100%;height:auto;image-rendering:pixelated;image-rendering:crisp-edges}
.muted{color:var(--mut);font-size:13px;margin:8px 2px 0}
.seg{display:flex;gap:6px;background:#141720;border:1px solid var(--line);border-radius:10px;padding:4px;margin-bottom:12px}
.seg input{position:absolute;opacity:0;pointer-events:none}
.seg label{flex:1;min-width:0;text-align:center;padding:9px 0;border-radius:7px;font-weight:600;
  font-size:13px;letter-spacing:.01em;white-space:nowrap;color:var(--mut);cursor:pointer;user-select:none}
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
input[type=file]{width:100%;background:#141720;color:var(--mut);border:1px solid var(--line);
  border-radius:10px;padding:10px;font:13px inherit;margin-top:5px}
#imgrow{margin-top:12px}
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
    <input type="radio" name="mode" id="m3" value="rich"><label for="m3">UNICODE</label>
    <input type="radio" name="mode" id="m4" value="image"><label for="m4">IMAGE</label>
  </div>
  <div id="txtrow">
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
  </div>
  <div class="hide" id="imgrow">
    <label class="fld" for="file">Picture &mdash; scaled to fit and dithered to 1 bit</label>
    <input type="file" id="file" accept="image/*">
  </div>
  <button id="btnpv" type="button">Preview</button>
  <button id="go" type="submit" class="ghost">Print now</button>
  <button id="clr" type="button" class="ghost">Clear panel</button>
</form>

<p id="msg"></p>
<p class="muted"><span id="net">&mdash;</span> &middot; <a href="/wifi">Wi-Fi settings</a></p>

<script>
var $=function(s){return document.querySelector(s)};
function say(t,cls){var m=$("#msg");m.textContent=t;m.className=cls||""}

var panel=null;        // last image known to be on the glass
var previewed=false;   // canvas is showing an unprinted preview

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

function curMode(){
  return $("#m1").checked?"text":$("#m2").checked?"qr":$("#m3").checked?"rich":"image";
}
// Each tab has exactly one source, so there is never a stale input to clear.
function syncMode(){
  var m=curMode();
  $("#txtrow").className=(m==="image")?"hide":"";
  $("#szrow").className=(m==="qr")?"fld hide":"fld";
  $("#imgrow").className=(m==="image")?"":"hide";
  $("#txt").placeholder = m==="qr" ? "URL or text to encode"
    : m==="rich" ? "Emoji, Hangul, any script your phone can draw"
    : "What should the label say?";
}
function isLocal(){var m=curMode();return m==="rich"||m==="image"}

// ---- IMAGE mode: the phone rasterises, the panel just blits ----------------
// The device has no glyphs beyond 5x7 ASCII, but this browser already has every
// font and emoji on the system. So render here, threshold to 1 bit, and send
// the finished frame. Same packing the panel uses: MSB first, 1 = black.
var W=250,H=122,pic=null,pendingBits=null;

function sizePx(){return {"1":13,"2":19,"3":27}[$("#sz").value]||19}

function drawSource(ctx){
  ctx.fillStyle="#fff";ctx.fillRect(0,0,W,H);
  if(curMode()==="image"){
    if(!pic) throw new Error("choose a picture first");
    var s=Math.min(W/pic.width,H/pic.height);
    var dw=Math.max(1,Math.round(pic.width*s)),dh=Math.max(1,Math.round(pic.height*s));
    ctx.drawImage(pic,(W-dw)>>1,(H-dh)>>1,dw,dh);
    return;
  }
  var px=sizePx(),lh=Math.round(px*1.25);
  ctx.fillStyle="#000";ctx.textBaseline="top";
  ctx.font=px+'px system-ui,-apple-system,"Segoe UI",Roboto,"Noto Color Emoji","Apple Color Emoji",sans-serif';
  var lines=[];
  $("#txt").value.split("\n").forEach(function(para){
    var cur="";
    para.split(" ").forEach(function(word){
      var t2=cur?cur+" "+word:word;
      if(!cur||ctx.measureText(t2).width<=W-6) cur=t2; else {lines.push(cur);cur=word}
    });
    lines.push(cur);
  });
  var max=Math.max(1,Math.floor(H/lh));
  if(lines.length>max){lines=lines.slice(0,max);lines[max-1]+="\u2026"}
  var y0=Math.max(0,Math.round((H-lines.length*lh)/2));
  lines.forEach(function(l,i){
    ctx.fillText(l,Math.max(0,(W-ctx.measureText(l).width)/2),y0+i*lh);
  });
}

// Floyd-Steinberg. Plain black text has no error to diffuse so it stays crisp,
// while a bright emoji or a photo becomes a dot pattern instead of vanishing
// under a hard threshold.
function packDithered(ctx){
  var d=ctx.getImageData(0,0,W,H).data,g=new Float32Array(W*H);
  for(var i=0,p=0;i<d.length;i+=4,p++){
    var a=d[i+3]/255;
    var r=d[i]*a+255*(1-a),gg=d[i+1]*a+255*(1-a),b=d[i+2]*a+255*(1-a);
    g[p]=0.299*r+0.587*gg+0.114*b;
  }
  var stride=(W+7)>>3,out=new Uint8Array(stride*H);
  for(var y=0;y<H;y++)for(var x=0;x<W;x++){
    var k=y*W+x,old=g[k],nv=old<128?0:255,err=old-nv;
    g[k]=nv;
    if(x+1<W) g[k+1]+=err*7/16;
    if(y+1<H){
      if(x>0)   g[k+W-1]+=err*3/16;
      g[k+W]+=err*5/16;
      if(x+1<W) g[k+W+1]+=err*1/16;
    }
    if(nv===0) out[y*stride+(x>>3)]|=0x80>>(x&7);
  }
  return out;
}

function localBits(){
  var c=document.createElement("canvas");c.width=W;c.height=H;
  var ctx=c.getContext("2d",{willReadFrequently:true});
  drawSource(ctx);
  var u=packDithered(ctx),s="";
  for(var i=0;i<u.length;i++) s+=String.fromCharCode(u[i]);
  return btoa(s);
}
function syncCount(){$("#cnt").textContent=$("#txt").value.length+" chars"}

function params(){
  var b=new URLSearchParams();
  b.set("mode",curMode());
  b.set("text",$("#txt").value);
  b.set("size",$("#sz").value);
  return b;
}

function clearPreview(){
  previewed=false;pendingBits=null;
  $("#btnpv").textContent="Preview";
  $("#pvwrap").className="";
}

// An edit invalidates the preview on screen, but throwing the image away would
// be more jarring than leaving it with an honest label.
function stale(){
  if(!previewed) return;
  previewed=false;pendingBits=null;
  $("#btnpv").textContent="Preview";
  $("#now").textContent="preview is out of date \u2014 press Preview";
}

function load(){
  fetch("/api/status").then(function(r){return r.json()}).then(function(j){
    var cap = j.mode==="none"
      ? "panel is blank"
      : j.mode.toUpperCase()+" \u00b7 "+j.updates+" update"+(j.updates==1?"":"s")+" since boot";
    panel={b64:j.preview,w:j.w,h:j.h,caption:cap};
    $("#dim").textContent=j.w+"\u00d7"+j.h;
    if(!previewed){ paint(j.preview,j.w,j.h); $("#now").textContent=cap; }
    if(j.mode!=="none"){
      // The device stores both bitmap tabs as one mode; the source string tells
      // them apart, since IMAGE never sends one.
      $(j.mode==="qr"?"#m2":j.mode==="image"?(j.text?"#m3":"#m4"):"#m1").checked=true;
      $("#txt").value=j.text;
      $("#sz").value=j.size;
    }
    $("#net").textContent=j.ap?("AP "+j.ssid+" \u00b7 "+j.ip)
                              :(j.ssid+" \u00b7 "+j.ip+" \u00b7 "+j.rssi+" dBm");
    syncMode();syncCount();
  }).catch(function(){say("cannot reach the label","err")});
}

function busy(b){$("#go").disabled=b;$("#btnpv").disabled=b;$("#clr").disabled=b}

function post(url,body,okmsg){
  busy(true);say("working\u2026");
  return fetch(url,{method:"POST",body:body}).then(function(r){return r.json()})
    .then(function(j){
      busy(false);
      if(!j.ok){say(j.error,"err");return}
      say(j.changed===false
            ? "no change \u2014 the panel already shows this"
            : (okmsg||"display updated"),"ok");
      clearPreview();
      load();
    }).catch(function(){busy(false);say("request failed","err")});
}

function doPrint(){
  if(!isLocal()){ post("/api/display",params(),"printed to the panel"); return; }
  var b=new URLSearchParams();
  try{ b.set("bits",pendingBits||localBits()); }
  catch(e){ say(e.message,"err"); return; }
  // Kept only so the page can repopulate its field, and so a reload knows
  // which of the two bitmap tabs produced this frame.
  b.set("text",curMode()==="rich"?$("#txt").value:"");
  post("/api/image",b,"printed to the panel");
}

function markPreviewed(){
  previewed=true;
  $("#btnpv").textContent="Print to panel";
  $("#pvwrap").className="pending";
  $("#now").textContent="preview \u2014 the panel has not been touched";
  say("this is how it will look","ok");
}

$("#btnpv").addEventListener("click",function(){
  if(previewed){ doPrint(); return; }
  if(isLocal()){
    try{ pendingBits=localBits(); paint(pendingBits,W,H); markPreviewed(); }
    catch(e){ say(e.message,"err"); }
    return;
  }
  busy(true);say("rendering\u2026");
  fetch("/api/preview",{method:"POST",body:params()}).then(function(r){return r.json()})
    .then(function(j){
      busy(false);
      if(!j.ok){say(j.error,"err");return}
      paint(j.preview,j.w,j.h);
      markPreviewed();
    }).catch(function(){busy(false);say("request failed","err")});
});

$("#f").addEventListener("submit",function(e){e.preventDefault();doPrint()});
$("#clr").addEventListener("click",function(){post("/api/clear",new URLSearchParams(),"panel cleared")});
$("#m1").addEventListener("change",function(){syncMode();stale()});
$("#m2").addEventListener("change",function(){syncMode();stale()});
$("#sz").addEventListener("change",stale);
$("#txt").addEventListener("input",function(){syncCount();stale()});
$("#m3").addEventListener("change",function(){syncMode();stale()});
$("#m4").addEventListener("change",function(){syncMode();stale()});
$("#file").addEventListener("change",function(){
  var f=this.files&&this.files[0];
  if(!f){pic=null;stale();return}
  var fr=new FileReader();
  fr.onload=function(){
    var im=new Image();
    im.onload=function(){pic=im;stale();say("picture loaded \u2014 press Preview","ok")};
    im.onerror=function(){say("could not read that image","err")};
    im.src=fr.result;
  };
  fr.readAsDataURL(f);
});

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
