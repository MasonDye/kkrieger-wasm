#!/usr/bin/env node
// Minimal CDP driver for the wasm build: launches headless chromium (SwiftShader
// WebGL2), runs a list of steps and prints what the page logged.
//
//   node wasm/cdp.js --url http://localhost:8766/kkrieger.html \
//        --steps "wait:3,start,wait:20,shot:/tmp/menu.png,log:frame:3"
//
// steps:
//   wait:<sec>          sleep
//   start               click the shell's start overlay (Module.callMain)
//   click:<x>,<y>       mouse click in page coordinates
//   mdown:<x> <y> / mup:<x> <y>   press / release the left button (hold across frames)
//   key:<name>          key press (Return, Escape, Space, F9, F10, w, a, s, d, ...)
//   down:<name>         key down only        up:<name>   key up only
//   shot:<path>         save a PNG screenshot
//   eval:<js>           evaluate and print the result
//   log:<regex>[:n]     print the last n (default 10) matching window.__kkLog lines
//   px                  sample the canvas pixels (8x6 grid over the game viewport)
//   shots:<dir>         save the post-processing stages captured during an F9 trace
// --window w,h sets the browser window (default 1280,800)
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function arg(name, def) {
  const i = process.argv.indexOf('--' + name);
  return i >= 0 ? process.argv[i + 1] : def;
}
const URL_ = arg('url', 'http://localhost:8766/kkrieger.html');
const STEPS = arg('steps', 'wait:3,start,wait:20,shot:/tmp/kk.png');
const KEEP = process.argv.includes('--keep');
const sleep = ms => new Promise(r => setTimeout(r, ms));

const KEYS = {
  Return:    { key: 'Enter',     code: 'Enter',      vk: 13, text: '\r' },
  Escape:    { key: 'Escape',    code: 'Escape',     vk: 27 },
  Space:     { key: ' ',         code: 'Space',      vk: 32, text: ' ' },
  Tab:       { key: 'Tab',       code: 'Tab',        vk: 9 },
  ArrowUp:   { key: 'ArrowUp',   code: 'ArrowUp',    vk: 38 },
  ArrowDown: { key: 'ArrowDown', code: 'ArrowDown',  vk: 40 },
  ArrowLeft: { key: 'ArrowLeft', code: 'ArrowLeft',  vk: 37 },
  ArrowRight:{ key: 'ArrowRight',code: 'ArrowRight', vk: 39 },
  F9:        { key: 'F9',        code: 'F9',         vk: 120 },
  F1:        { key: 'F1',        code: 'F1',         vk: 112 },
  F2:        { key: 'F2',        code: 'F2',         vk: 113 },
  F3:        { key: 'F3',        code: 'F3',         vk: 114 },
  F4:        { key: 'F4',        code: 'F4',         vk: 115 },
  F5:        { key: 'F5',        code: 'F5',         vk: 116 },
  F6:        { key: 'F6',        code: 'F6',         vk: 117 },
  F7:        { key: 'F7',        code: 'F7',         vk: 118 },
  F8:        { key: 'F8',        code: 'F8',         vk: 119 },
  F10:       { key: 'F10',       code: 'F10',        vk: 121 },
  F11:       { key: 'F11',       code: 'F11',        vk: 122 },
};
function keyDef(name) {
  if (KEYS[name]) return KEYS[name];
  const c = name.length === 1 ? name : name[0];
  return { key: c, code: 'Key' + c.toUpperCase(), vk: c.toUpperCase().charCodeAt(0), text: c };
}

async function main() {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'kk-chrome-'));
  const chrome = spawn('/usr/bin/chromium', [
    '--headless=new', '--remote-debugging-port=9333', '--user-data-dir=' + profile,
    '--no-sandbox', '--disable-dev-shm-usage', '--window-size=' + arg('window', '1280,800'),
    '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
    '--enable-features=Vulkan', '--disable-features=CalculateNativeWinOcclusion',
    '--autoplay-policy=no-user-gesture-required', '--mute-audio',
    'about:blank',
  ], { stdio: ['ignore', 'ignore', 'pipe'] });
  let chromeErr = '';
  chrome.stderr.on('data', d => { chromeErr += d.toString(); });

  // wait for the devtools endpoint
  let target = null;
  for (let i = 0; i < 100 && !target; i++) {
    await sleep(200);
    try {
      const list = await (await fetch('http://127.0.0.1:9333/json/list')).json();
      target = list.find(t => t.type === 'page');
    } catch (e) { /* not up yet */ }
  }
  if (!target) { console.error('chromium did not start\n' + chromeErr.slice(-2000)); process.exit(1); }

  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  let msgId = 0;
  const pending = new Map();
  const events = [];
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    else if (m.method) events.push(m);
  };
  const send = (method, params = {}) => new Promise((res, rej) => {
    const id = ++msgId;
    pending.set(id, m => m.error ? rej(new Error(method + ': ' + JSON.stringify(m.error))) : res(m.result));
    ws.send(JSON.stringify({ id, method, params }));
  });
  const evaluate = async (expr, awaitPromise = false) => {
    const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise });
    if (r.exceptionDetails) throw new Error('eval: ' + JSON.stringify(r.exceptionDetails.exception?.description || r.exceptionDetails));
    return r.result.value;
  };

  await send('Page.enable');
  await send('Runtime.enable');
  await send('Log.enable');
  await send('Page.navigate', { url: URL_ });
  await sleep(1500);

  for (const raw of STEPS.split(',').map(s => s.trim()).filter(Boolean)) {
    const [cmd, ...restArr] = raw.split(':');
    const rest = restArr.join(':');
    try {
      if (cmd === 'wait') await sleep(parseFloat(rest) * 1000);
      else if (cmd === 'start') {
        const r = await evaluate("(()=>{const b=document.getElementById('start'); if(b){b.click(); return 'clicked';} return 'no start element';})()");
        console.log('[cdp] start -> ' + r);
      } else if (cmd === 'click') {
        const [x, y] = rest.split(/[, ]+/).map(Number);
        for (const type of ['mousePressed', 'mouseReleased'])
          await send('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 });
      } else if (cmd === 'mmove') {
        // mmove:<x> <y> <steps>: move the mouse there in small steps
        const [x, y, n] = rest.split(/[, ]+/).map(Number);
        for (let k = 1; k <= (n || 10); k++) {
          await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 640 + (x - 640) * k / (n || 10), y: 300 + (y - 300) * k / (n || 10) });
          await sleep(30);
        }
      } else if (cmd === 'mdown' || cmd === 'mup') {
        const [x, y] = rest.split(/[, ]+/).map(Number);
        await send('Input.dispatchMouseEvent', { type: cmd === 'mdown' ? 'mousePressed' : 'mouseReleased', x, y, button: 'left', clickCount: 1 });
      } else if (cmd === 'key' || cmd === 'down' || cmd === 'up') {
        const k = keyDef(rest);
        const base = { key: k.key, code: k.code, windowsVirtualKeyCode: k.vk, nativeVirtualKeyCode: k.vk };
        if (cmd !== 'up') await send('Input.dispatchKeyEvent', { type: k.text ? 'keyDown' : 'rawKeyDown', ...base, text: k.text });
        // the game reads one key per frame, so a press has to outlast a frame
        if (cmd === 'key') await sleep(150);
        if (cmd !== 'down') await send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
      } else if (cmd === 'shot') {
        const { data } = await send('Page.captureScreenshot', { format: 'png' });
        fs.writeFileSync(rest, Buffer.from(data, 'base64'));
        console.log('[cdp] screenshot -> ' + rest);
      } else if (cmd === 'focus') {
        await evaluate("(()=>{const c=document.querySelector('canvas'); if(c){ c.setAttribute('tabindex','0'); c.focus(); } window.focus(); return document.activeElement && document.activeElement.tagName;})()");
        console.log('[cdp] focused canvas');
      } else if (cmd === 'evalfile') {
        const js = fs.readFileSync(rest, 'utf8');
        const out = await evaluate(js);
        console.log('[cdp] ' + rest + ':\n' + (typeof out === 'string' ? out : JSON.stringify(out, null, 1)));
      } else if (cmd === 'eval') {
        console.log('[cdp] eval -> ' + JSON.stringify(await evaluate(rest), null, 1));
      } else if (cmd === 'log') {
        const m = rest.match(/^(.*?)(?::(\d+))?$/);
        const re = m[1], n = m[2] ? parseInt(m[2]) : 10;
        const lines = await evaluate(`(()=>{const L=window.__kkLog||[];const r=new RegExp(${JSON.stringify(re)});return L.filter(l=>r.test(l)).slice(-${n});})()`);
        console.log('[cdp] log /' + re + '/:\n' + (lines || []).join('\n'));
      } else if (cmd === 'shots') {
        // save the stages captured by the F9 trace (window.__kkShots)
        const shots = await evaluate('(window.__kkShots || []).splice(0)');
        fs.mkdirSync(rest, { recursive: true });
        for (const [name, url] of shots || [])
          fs.writeFileSync(path.join(rest, /\.\w+$/.test(name) ? name : name + '.png'), Buffer.from(url.split(',')[1], 'base64'));
        console.log('[cdp] saved ' + (shots || []).length + ' stage images to ' + rest);
      } else if (cmd === 'px') {
        const px = await evaluate(`(()=>{const c=document.querySelector('canvas');const t=document.createElement('canvas');t.width=c.width;t.height=c.height;const x=t.getContext('2d');x.drawImage(c,0,0);const rows=[];for(let j=0;j<6;j++){const row=[];for(let i=0;i<8;i++){const px=Math.floor((i+0.5)*c.width/8),py=Math.floor(100+(j+0.5)*400/6);const d=x.getImageData(px,py,1,1).data;row.push(d[0]+','+d[1]+','+d[2]+','+d[3]);}rows.push(row.join(' | '));}return rows.join('\\n');})()`);
        console.log('[cdp] pixels:\n' + px);
      } else console.log('[cdp] unknown step: ' + raw);
    } catch (e) {
      console.log('[cdp] step "' + raw + '" failed: ' + e.message);
    }
  }

  const gl = events.filter(e => e.method === 'Log.entryAdded')
                   .map(e => e.params.entry.text)
                   .filter(t => /WebGL|GL_INVALID|INVALID_/.test(t));
  if (gl.length) {
    const uniq = [...new Set(gl)];
    console.log('[cdp] WebGL messages (' + gl.length + ', ' + uniq.length + ' unique):\n' + uniq.slice(0, 12).join('\n'));
  }

  const errs = events.filter(e => e.method === 'Runtime.exceptionThrown')
                     .map(e => e.params.exceptionDetails.exception?.description || e.params.exceptionDetails.text);
  if (errs.length) console.log('[cdp] page exceptions:\n' + errs.slice(0, 5).join('\n---\n'));

  if (!KEEP) {
    ws.close();
    chrome.kill();
    await sleep(300);
    try { fs.rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); } catch (e) { /* chromium still writing */ }
  }
  process.exit(0);
}
main().catch(e => { console.error(e); process.exit(1); });
