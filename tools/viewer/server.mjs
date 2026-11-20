#!/usr/bin/env node
// KestroDB viewer: browse tables, schemas and indexes, and run SQL, in a web page.
//
//   node --max-old-space-size=64 tools/viewer/server.mjs --db app.kdb
//   node --max-old-space-size=64 tools/viewer/server.mjs --server tcp://127.0.0.1:7820 --token-file token.txt
//
// Options:
//   --db FILE              open a database file directly (it must not be open elsewhere)
//   --server ADDRESS       or connect to kestro-server (tcp://, tls://, unix://)
//                          With neither, pick a file or a server in the page (Open).
//   --token-file FILE      server token (or KESTRODB_TOKEN); --ca FILE for tls:// with a private CA
//   --port 7821            --host 127.0.0.1 (another host needs --viewer-token-file)
//   --viewer-token-file F  a token the browser must give (required off localhost)
//   --allow-writes         allow INSERT/UPDATE/DELETE/DDL from the SQL console (read-only by default)
//   --cpu-budget 2         percent of this machine's total CPU the viewer may use
//
// Light by design: no dependencies, nothing runs while idle, one query at a time, at most
// 1,000 rows and 2 MB per answer, and a CPU governor: when the viewer has used more than
// --cpu-budget percent of the machine's CPU over the last 10 seconds, queries wait their turn
// with "busy" until it is back under budget.
import http from 'node:http';
import os from 'node:os';
import {readFileSync, existsSync} from 'node:fs';
import {readdir, stat} from 'node:fs/promises';
import {resolve as resolvePath, dirname, join as joinPath, parse as parsePath} from 'node:path';
import {timingSafeEqual, createHash} from 'node:crypto';

const args = process.argv.slice(2);
const opt = (name, fallback) => { const i = args.indexOf('--' + name); return i >= 0 ? args[i + 1] : fallback; };
const flag = name => args.includes('--' + name);
const readSecret = f => (f ? readFileSync(f, 'utf8').trim() : undefined);
const DB_FILE = opt('db'), SERVER = opt('server');
const PORT = Number(opt('port', 7821)), HOST = opt('host', '127.0.0.1');
const ALLOW_WRITES = flag('allow-writes');
const CPU_BUDGET = Number(opt('cpu-budget', 2)) / 100;
const VIEWER_TOKEN = readSecret(opt('viewer-token-file'));
const MAX_ROWS = 1000, MAX_BYTES = 2 << 20, MAX_BODY = 64 << 10, MAX_QUEUE = 4;
if (DB_FILE && SERVER) { console.error('usage: server.mjs [--db FILE | --server ADDRESS [--token-file F]] [--port 7821] [--allow-writes]'); process.exit(2); }
const loopback = HOST === '127.0.0.1' || HOST === 'localhost' || HOST === '::1';
if (!loopback && !VIEWER_TOKEN) { console.error('Listening on ' + HOST + ' needs --viewer-token-file (the viewer can read your data).'); process.exit(2); }

// ---------- Database connection (switchable from the page) ----------
let db = null, label = '';
async function closeDb() { const old = db; db = null; label = ''; if (old) { try { await old.close(); } catch {} } }
async function openFile(file) {
  const path = resolvePath(String(file));
  const info = await stat(path).catch(() => null);
  if (!info || !info.isFile()) throw Object.assign(Error('No such file: ' + path), {status: 404});
  const {Database} = await import('../../index.mjs');
  let next;
  try { next = new Database(path); }  // owns the file while the viewer has it open
  catch (e) {
    const m = String(e.message);
    if (/^LOCKED/.test(m)) throw Object.assign(Error('This database is in use by another program (for example kestro-server). Connect to that server instead (Server tab).'), {status: 409});
    if (/^(FORMAT|TRUNCATED|CORRUPT)/.test(m)) throw Object.assign(Error('This is not a KestroDB database, or it is damaged (' + m + ').'), {status: 400});
    throw e;
  }
  await closeDb(); db = next; label = path;
}
async function openServer(address, token, ca) {
  const {RemoteDB} = await import('../../remote.mjs');
  const next = await RemoteDB.connect(String(address), {token: token || undefined, tls: ca ? {caFile: String(ca)} : {}});
  await closeDb(); db = next; label = String(address);
}
const needDb = () => { if (!db) throw Object.assign(Error('No database is open.'), {status: 409, needOpen: true}); };
const sql = async (text, params = []) => { needDb(); return db.sql(text, params); };
const stats = async () => { needDb(); return db.stats(); };
if (DB_FILE) await openFile(DB_FILE);
else if (SERVER) await openServer(SERVER, readSecret(opt('token-file')) ?? process.env.KESTRODB_TOKEN, opt('ca'));
// Folder listing for the Open window: folders, and files with sizes (KestroDB files marked).
async function browse(dir) {
  if (!dir) {
    if (process.platform === 'win32') {
      const drives = [];
      for (let c = 65; c <= 90; c++) { const d = String.fromCharCode(c) + ':\\'; if (existsSync(d)) drives.push({name: d, path: d, dir: true}); }
      return {dir: '', parent: null, entries: drives, home: os.homedir()};
    }
    dir = '/';
  }
  const full = resolvePath(String(dir));
  const list = await readdir(full, {withFileTypes: true}).catch(e => { throw Object.assign(Error('Cannot open folder: ' + e.message), {status: 400}); });
  const entries = [];
  for (const e of list) {
    if (entries.length >= 2000) break;
    if (e.isDirectory()) entries.push({name: e.name, path: joinPath(full, e.name), dir: true});
    else if (e.isFile()) {
      const info = await stat(joinPath(full, e.name)).catch(() => null);
      entries.push({name: e.name, path: joinPath(full, e.name), dir: false, size: info?.size ?? 0, hdb: /\.kdb$/i.test(e.name)});
    }
  }
  entries.sort((a, b) => (a.dir === b.dir ? a.name.localeCompare(b.name) : a.dir ? -1 : 1));
  const root = parsePath(full).root, parent = full === root ? (process.platform === 'win32' ? '' : null) : dirname(full);
  return {dir: full, parent, entries, home: os.homedir()};
}

// ---------- Guards ----------
const READ_ONLY = /^\s*(SELECT|SHOW|DESCRIBE|DESC|EXPLAIN)\b/i;
function prepare(text) {
  if (typeof text !== 'string' || !text.trim()) throw Object.assign(Error('Enter a SQL statement.'), {status: 400});
  if (text.length > 20000) throw Object.assign(Error('That statement is too long for the viewer.'), {status: 400});
  if (!ALLOW_WRITES && !READ_ONLY.test(text)) throw Object.assign(Error('The viewer is read-only. Start it with --allow-writes to change data.'), {status: 403});
  // Cap result size at the source: a SELECT without LIMIT gets one (one row more, to tell
  // the page there is more).
  if (/^\s*SELECT\b/i.test(text) && !/\bLIMIT\b/i.test(text)) return text.replace(/;\s*$/, '') + ` LIMIT ${MAX_ROWS + 1}`;
  return text;
}
// CPU governor: the share of the whole machine used by this process over the last 10 s.
const cores = os.cpus().length, samples = [];
function cpuShare() {
  const now = performance.now(), u = process.cpuUsage(), used = (u.user + u.system) / 1000;  // ms
  samples.push({now, used});
  while (samples.length > 2 && now - samples[0].now > 10000) samples.shift();
  const first = samples[0], span = Math.max(1000, now - first.now);
  return (used - first.used) / (span * cores);
}
// One query at a time, a short queue.
let running = Promise.resolve(), waiting = 0;
function serial(fn) {
  if (waiting >= MAX_QUEUE) return Promise.reject(Object.assign(Error('Busy: too many queries waiting.'), {status: 429}));
  waiting++;
  const p = running.then(fn, fn);
  running = p.then(() => {}, () => {}).finally(() => { waiting--; });
  return p;
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
let held = 0;  // requests waiting for the CPU budget
const sameSecret = (a, b) => { const x = createHash('sha256').update(String(a)).digest(), y = createHash('sha256').update(String(b)).digest(); return timingSafeEqual(x, y); };

// ---------- HTTP ----------
const headers = {'x-content-type-options': 'nosniff', 'referrer-policy': 'no-referrer', 'x-frame-options': 'DENY',
  'content-security-policy': "default-src 'self'; style-src 'self'; script-src 'self'; img-src 'self' data:; frame-ancestors 'none'", 'cache-control': 'no-store'};
function send(res, status, type, body) { res.writeHead(status, {...headers, 'content-type': type, 'content-length': Buffer.byteLength(body)}); res.end(body); }
const json = (res, status, value) => send(res, status, 'application/json; charset=utf-8', JSON.stringify(value));

async function api(req, res, path) {
  if (VIEWER_TOKEN && !sameSecret(req.headers['x-viewer-token'] ?? '', VIEWER_TOKEN)) return json(res, 401, {error: 'Viewer token required.', needToken: true});
  // Over budget: wait on a timer (no CPU) until back under it. Answering at once would let a
  // fast client turn rejections themselves into load.
  if (cpuShare() > CPU_BUDGET) {
    if (++held > 16) { await sleep(1000); held--; return json(res, 429, {error: 'Busy: too many requests waiting.'}); }
    let waited = 0;
    while (cpuShare() > CPU_BUDGET && waited < 10000) { await sleep(250); waited += 250; }
    held--;
    if (cpuShare() > CPU_BUDGET) return json(res, 429, {error: `Busy: the viewer is over its ${CPU_BUDGET * 100}% CPU budget. Try again in a few seconds.`});
  }
  if (path === '/api/browse' && req.method === 'GET') return json(res, 200, await browse(new URL(req.url, 'http://x').searchParams.get('dir')));
  if (path === '/api/open' && req.method === 'POST') {
    let body = ''; for await (const chunk of req) { body += chunk; if (body.length > MAX_BODY) return json(res, 413, {error: 'Request too large.'}); }
    let input; try { input = JSON.parse(body); } catch { return json(res, 400, {error: 'Bad request.'}); }
    await serial(() => input.server ? openServer(input.server, input.token, input.ca) : openFile(input.file));
    return json(res, 200, {label});
  }
  if (path === '/api/close' && req.method === 'POST') { await serial(closeDb); return json(res, 200, {ok: true}); }
  if (path === '/api/overview' && req.method === 'GET') {
    if (!db) return json(res, 200, {label: '', needOpen: true, readOnly: !ALLOW_WRITES, tables: [], stats: {}});
    const [s, t] = await serial(() => Promise.all([stats(), sql('SHOW TABLES')]));
    return json(res, 200, {label, readOnly: !ALLOW_WRITES, stats: s, tables: t.rows});
  }
  if (path === '/api/sql' && req.method === 'POST') {
    let body = '';
    for await (const chunk of req) { body += chunk; if (body.length > MAX_BODY) return json(res, 413, {error: 'Request too large.'}); }
    let input; try { input = JSON.parse(body); } catch { return json(res, 400, {error: 'Bad request.'}); }
    const text = prepare(input.sql);
    const params = Array.isArray(input.params) ? input.params : [];
    const t0 = performance.now();
    const result = await serial(() => sql(text, params));
    const ms = performance.now() - t0;
    if (result.rows) {
      let rows = result.rows, truncated = rows.length > MAX_ROWS;
      if (truncated) rows = rows.slice(0, MAX_ROWS);
      let out = JSON.stringify({rows, truncated, ms});
      while (out.length > MAX_BYTES && rows.length > 1) { rows = rows.slice(0, rows.length >> 1); truncated = true; out = JSON.stringify({rows, truncated, ms}); }
      return send(res, 200, 'application/json; charset=utf-8', out);
    }
    return json(res, 200, {...result, ms});
  }
  return json(res, 404, {error: 'Not found.'});
}
const server = http.createServer(async (req, res) => {
  const path = new URL(req.url, 'http://x').pathname;
  try {
    if (path === '/' || path === '/index.html') return send(res, 200, 'text/html; charset=utf-8', PAGE);
    if (path === '/app.css') return send(res, 200, 'text/css; charset=utf-8', CSS);
    if (path === '/app.js') return send(res, 200, 'text/javascript; charset=utf-8', JS);
    if (path.startsWith('/api/')) return await api(req, res, path);
    send(res, 404, 'text/plain', 'Not found');
  } catch (e) {
    json(res, e.status ?? 400, {error: String(e.message ?? e), code: e.code, needOpen: e.needOpen});
  }
});
server.maxConnections = 64;
server.keepAliveTimeout = 5000;
server.headersTimeout = 10000;
server.requestTimeout = 30000;
server.listen(PORT, HOST, () => console.log(`KestroDB viewer: http://${HOST === '::1' ? '[::1]' : HOST}:${PORT}  (${label}, ${ALLOW_WRITES ? 'read-write' : 'read-only'})`));
const stop = async () => { server.close(); await closeDb(); process.exit(0); };
process.on('SIGINT', stop); process.on('SIGTERM', stop);

// ---------- Page ----------
const PAGE = `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>KestroDB viewer</title><link rel="stylesheet" href="/app.css"><script src="/app.js" defer></script></head><body>
<header><div class="brand">KestroDB <span>viewer</span></div><button id="open-btn" class="primary">Open…</button><div id="source" class="muted"></div><div id="chips" class="chips"></div></header>
<div class="layout">
<aside><input id="filter" type="search" placeholder="Filter tables" aria-label="Filter tables"><nav id="tables" aria-label="Tables"></nav>
<button id="new-table" class="write">+ New table</button><button id="console-btn" class="ghost">SQL console</button></aside>
<main>
<div id="table-view" hidden>
 <div class="title-row"><h1 id="table-name"></h1><div class="tabs" role="tablist">
  <button role="tab" data-tab="rows" aria-selected="true">Rows</button><button role="tab" data-tab="schema">Schema</button></div></div>
 <section id="tab-rows">
  <div class="toolbar"><input id="where" class="grow" placeholder="WHERE …   e.g.  status = 'paid' AND total > 50" aria-label="Filter rows (SQL WHERE)">
   <button id="apply">Filter</button><button id="clear">Clear</button><span class="sep"></span>
   <button id="new-row" class="write">New row</button><button id="export-csv">CSV</button><button id="export-json">JSON</button></div>
  <div class="toolbar"><label>Order <select id="order"></select></label><label><input type="checkbox" id="desc"> Descending</label>
   <span class="spacer"></span><span id="page-info" class="muted"></span><button id="prev">Previous</button><button id="next">Next</button></div>
  <div class="grid-wrap"><table id="rows" class="grid"></table></div>
 </section>
 <section id="tab-schema" hidden>
  <div class="title-row"><h2>Columns</h2><span class="spacer"></span><button id="add-column" class="write">Add column</button></div>
  <div class="grid-wrap"><table id="columns" class="grid"></table></div>
  <div class="title-row"><h2>Indexes</h2><span class="spacer"></span><button id="add-index" class="write">Add index</button></div>
  <div class="grid-wrap"><table id="indexes" class="grid"></table></div>
  <div class="title-row write"><span class="spacer"></span><button id="drop-table" class="write danger">Drop table…</button></div></section>
</div>
<div id="console-view" hidden>
 <div class="title-row"><h1>SQL console</h1><span id="mode" class="badge"></span></div>
 <textarea id="sql" spellcheck="false" aria-label="SQL statement">SHOW TABLES</textarea>
 <div class="toolbar"><button id="run" class="primary">Run</button><span class="muted">Ctrl+Enter</span>
  <select id="history" aria-label="Query history"><option value="">History</option></select><span class="spacer"></span><span id="sql-info" class="muted"></span>
  <button id="res-csv">CSV</button><button id="res-json">JSON</button></div>
 <div class="grid-wrap"><table id="result" class="grid"></table></div>
</div>
<div id="empty" class="empty">Choose a table on the left, or open the SQL console.</div>
</main>
<dialog id="open"><div class="title-row"><h2>Open a database</h2><div class="tabs" role="tablist">
 <button role="tab" data-open="file" aria-selected="true">File</button><button role="tab" data-open="server">Server</button></div>
 <span class="spacer"></span><button id="open-close">Close</button></div>
 <div id="open-file">
  <div class="toolbar"><input id="path" placeholder="C:\\data\\app.kdb or /var/lib/app/app.kdb" aria-label="Database file path"><button id="path-open" class="primary">Open</button></div>
  <div class="toolbar"><button id="up">Up</button><button id="home">Home</button><span id="cwd" class="muted"></span></div>
  <div class="grid-wrap browse"><table id="entries" class="grid"></table></div>
  <div id="recent"></div>
 </div>
 <div id="open-server" hidden>
  <label>Address<input id="srv-address" placeholder="tls://db.example.com:7820"></label>
  <label>Token<input id="srv-token" type="password" autocomplete="off"></label>
  <label>CA file (tls://, optional)<input id="srv-ca" placeholder="C:\\certs\\ca.pem"></label>
  <div class="toolbar"><button id="srv-open" class="primary">Connect</button></div>
 </div>
 <p id="open-error" class="error"></p>
</dialog>
<dialog id="ddl"><div class="title-row"><h2 id="ddl-title"></h2><span class="spacer"></span><button id="ddl-close">Close</button></div>
 <div id="ddl-form"></div>
 <label class="ddl-sql">SQL to run (you can edit it)<textarea id="ddl-sql" spellcheck="false"></textarea></label>
 <div class="toolbar"><button id="ddl-run" class="primary">Run</button><span id="ddl-error" class="error"></span></div></dialog>
<dialog id="detail"><div class="title-row"><h2 id="detail-title">Row</h2><span class="spacer"></span>
 <button id="detail-save" class="primary write">Save</button><button id="detail-delete" class="write danger">Delete</button><button id="detail-close">Close</button></div>
 <textarea id="detail-json" spellcheck="false" aria-label="Row as JSON"></textarea><p id="detail-error" class="error"></p></dialog>
</div><div id="toast" role="status" aria-live="polite"></div></body></html>`;

const CSS = `:root{--bg:#f7f8fa;--panel:#fff;--ink:#16181d;--muted:#5d6472;--line:#e3e6ec;--accent:#2a63d6;--accent-ink:#fff;--hover:#eef2fb;--bad:#b42318;--code:#f1f3f7}
@media (prefers-color-scheme:dark){:root{--bg:#0f1115;--panel:#161920;--ink:#e7e9ee;--muted:#9aa2b1;--line:#262b35;--accent:#6c9cff;--accent-ink:#0b0d12;--hover:#1c2230;--bad:#ff8a80;--code:#1b1f28}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;gap:16px;align-items:center;flex-wrap:wrap;padding:10px 16px;border-bottom:1px solid var(--line);background:var(--panel)}
.brand{font-weight:700;font-size:16px}.brand span{font-weight:500;color:var(--muted)}.muted{color:var(--muted)}
.chips{display:flex;gap:6px;flex-wrap:wrap;margin-left:auto}.chip{border:1px solid var(--line);border-radius:999px;padding:2px 10px;font-size:12px;color:var(--muted)}
.layout{display:grid;grid-template-columns:240px 1fr;min-height:calc(100vh - 50px)}
aside{border-right:1px solid var(--line);background:var(--panel);padding:12px;display:flex;flex-direction:column;gap:8px}
aside input{width:100%}nav{display:flex;flex-direction:column;gap:2px;overflow:auto;flex:1}
nav button{all:unset;display:flex;justify-content:space-between;gap:8px;padding:6px 8px;border-radius:6px;cursor:pointer}
nav button:hover,nav button:focus-visible{background:var(--hover)}nav button[aria-current=true]{background:var(--hover);font-weight:600}
nav small{color:var(--muted)}main{padding:16px;min-width:0}
.title-row{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:10px}h1{font-size:18px;margin:0}h2{font-size:14px;margin:18px 0 8px}
.tabs{display:flex;gap:4px}.tabs button[aria-selected=true]{background:var(--accent);color:var(--accent-ink);border-color:var(--accent)}
button,select,input,textarea{font:inherit;color:inherit;background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:5px 10px}
button{cursor:pointer}button:hover{background:var(--hover)}button:disabled{opacity:.5;cursor:default}
.primary{background:var(--accent);color:var(--accent-ink);border-color:var(--accent)}.primary:hover{filter:brightness(1.08);background:var(--accent)}
.ghost{margin-top:auto}.toolbar{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:8px 0}.spacer{flex:1}
.grid-wrap{overflow:auto;border:1px solid var(--line);border-radius:8px;background:var(--panel);max-height:calc(100vh - 220px)}
.grid{border-collapse:collapse;width:100%;font-size:13px}.grid th,.grid td{padding:6px 10px;border-bottom:1px solid var(--line);text-align:left;white-space:nowrap;max-width:360px;overflow:hidden;text-overflow:ellipsis}
.grid th{position:sticky;top:0;background:var(--panel);font-weight:600;z-index:1}.grid tbody tr{cursor:pointer}.grid tbody tr:hover{background:var(--hover)}
.null{color:var(--muted);font-style:italic}.num{font-variant-numeric:tabular-nums}.obj{font-family:ui-monospace,Consolas,monospace;color:var(--muted)}
textarea{width:100%;min-height:120px;font-family:ui-monospace,Consolas,monospace;font-size:13px;background:var(--code)}
.badge{font-size:12px;border:1px solid var(--line);border-radius:999px;padding:1px 8px;color:var(--muted)}
.empty{color:var(--muted);padding:40px 0;text-align:center}.error{color:var(--bad)}
dialog{border:1px solid var(--line);border-radius:10px;background:var(--panel);color:var(--ink);width:min(720px,92vw);max-height:80vh}
dialog pre{background:var(--code);padding:12px;border-radius:8px;overflow:auto;max-height:60vh;font-size:12px}
#open{width:min(820px,94vw)}#open label{display:flex;flex-direction:column;gap:4px;margin:8px 0}#open input{width:100%}
#open .toolbar input{flex:1}.browse{max-height:46vh}.browse td{cursor:pointer}.kdb{font-weight:600;color:var(--accent)}
#recent{margin-top:10px;display:flex;gap:6px;flex-wrap:wrap}#recent button{font-size:12px}
#toast{position:fixed;bottom:16px;right:16px;max-width:420px}#toast div{background:var(--panel);border:1px solid var(--line);border-left:3px solid var(--bad);padding:10px 12px;border-radius:8px;margin-top:8px}
.grow{flex:1;min-width:220px}.sep{width:1px;height:22px;background:var(--line)}.danger{color:var(--bad)}
.grid th.sortable{cursor:pointer;user-select:none}.grid th.sortable:hover{color:var(--ink)}.grid th .dir{margin-left:4px;color:var(--accent)}
#detail textarea{min-height:46vh}body.readonly .write{display:none}
#ddl{width:min(760px,94vw)}#ddl-form label{display:inline-flex;flex-direction:column;gap:3px;margin:4px 8px 4px 0;font-size:12px;color:var(--muted)}
#ddl-form .cols{display:flex;flex-direction:column;gap:4px;margin:8px 0}#ddl-form .col{display:flex;gap:6px;align-items:center;flex-wrap:wrap}
#ddl-form .col input[type=text]{width:150px}.ddl-sql{display:flex;flex-direction:column;gap:4px;font-size:12px;color:var(--muted);margin-top:10px}
#ddl-sql{min-height:90px}.actions button{padding:2px 8px;font-size:12px;margin-right:4px}
/* polish */
:root{--accent:#4f46e5;--hover:#f1f2fe}@media (prefers-color-scheme:dark){:root{--accent:#8b8cf8;--hover:#1d1f33}}
header{position:sticky;top:0;z-index:5;box-shadow:0 1px 2px rgb(0 0 0/.04)}.brand::before{content:"";display:inline-block;width:10px;height:10px;border-radius:3px;background:var(--accent);margin-right:8px}
aside{gap:10px}nav button{font-size:13px}nav button[aria-current=true]{color:var(--accent);box-shadow:inset 2px 0 0 var(--accent)}
.grid-wrap{box-shadow:0 1px 2px rgb(0 0 0/.04)}.grid th{font-size:11px;text-transform:uppercase;letter-spacing:.04em;color:var(--muted)}
.grid tbody tr:nth-child(even){background:color-mix(in srgb,var(--bg) 55%,transparent)}.grid td.num{text-align:right}
button,select,input,textarea{transition:background .12s,border-color .12s}input:focus,select:focus,textarea:focus,button:focus-visible{outline:2px solid color-mix(in srgb,var(--accent) 45%,transparent);outline-offset:1px}
@media (max-width:760px){.layout{grid-template-columns:1fr}aside{border-right:0;border-bottom:1px solid var(--line);max-height:40vh}}`;

const JS = `const $=s=>document.querySelector(s);
let token=sessionStorage.getItem('viewer-token')||'';
const PAGE_SIZE=50;let state={table:null,offset:0,columns:[],rows:[]};let readOnly=true;
async function call(path,body){
  const r=await fetch(path,{method:body?'POST':'GET',headers:{'content-type':'application/json','x-viewer-token':token},body:body?JSON.stringify(body):undefined});
  const data=await r.json().catch(()=>({error:'Bad response'}));
  if(data.needToken){token=prompt('Viewer token')||'';sessionStorage.setItem('viewer-token',token);return call(path,body);}
  if(!r.ok)throw new Error(data.error||('HTTP '+r.status));
  return data;
}
function toast(msg){const d=document.createElement('div');d.textContent=msg;$('#toast').append(d);setTimeout(()=>d.remove(),6000);}
const ident=n=>/^[A-Za-z_][A-Za-z0-9_]*$/.test(n)?n:'"'+n.replace(/"/g,'""')+'"';
function cell(v){const td=document.createElement('td');
  if(v===null||v===undefined){td.textContent='NULL';td.className='null';}
  else if(typeof v==='object'){td.textContent=JSON.stringify(v);td.className='obj';}
  else{td.textContent=String(v);if(typeof v==='number')td.className='num';}
  td.title=td.textContent.slice(0,2000);return td;}
function renderGrid(table,rows,cols,opts={}){
  table.replaceChildren();
  if(!cols){const set=new Set(['id']);for(const r of rows)for(const k of Object.keys(r))set.add(k);cols=[...set].filter(c=>rows.some(r=>c in r));}
  const head=table.createTHead().insertRow();
  for(const c of cols){const th=document.createElement('th');th.textContent=c;
    if(opts.onSort){th.className='sortable';th.onclick=()=>opts.onSort(c);if(opts.sorted===c){const d=document.createElement('span');d.className='dir';d.textContent=opts.desc?'▼':'▲';th.append(d);}}
    head.append(th);}
  const body=table.createTBody();
  if(!rows.length){const tr=body.insertRow();const td=tr.insertCell();td.colSpan=Math.max(1,cols.length);td.textContent='No rows';td.className='null';return cols;}
  for(const r of rows){const tr=body.insertRow();for(const c of cols)tr.append(cell(r[c]));tr.onclick=()=>openDetail(r,opts.table);}
  return cols;}
function show(view){for(const v of ['#table-view','#console-view','#empty'])$(v).hidden=v!==view;}
async function overview(){
  const o=await call('/api/overview');
  if(o.needOpen){$('#source').textContent='No database open';$('#chips').replaceChildren();$('#tables').replaceChildren();show('#empty');openDialog();return o;}
  $('#source').textContent=o.label;
  const s=o.stats,chips=[(s.keys??0).toLocaleString()+' keys',((s.bytes??0)/1048576).toFixed(1)+' MB','version '+(s.txid??0)];
  if(s.server)chips.push(s.connections+' connections');if(s.user)chips.push(s.user+' ('+s.role+')');if(s.tls)chips.push('TLS');chips.push(o.readOnly?'read-only':'read-write');
  $('#chips').replaceChildren(...chips.map(t=>{const c=document.createElement('span');c.className='chip';c.textContent=t;return c;}));
  $('#mode').textContent=o.readOnly?'read-only':'read-write';readOnly=o.readOnly;document.body.classList.toggle('readonly',o.readOnly);
  const nav=$('#tables');nav.replaceChildren();
  for(const t of o.tables){const b=document.createElement('button');b.dataset.table=t.table;
    const n=document.createElement('span');n.textContent=t.table;const c=document.createElement('small');c.textContent=t.rows.toLocaleString();
    b.append(n,c);b.onclick=()=>{location.hash='table='+encodeURIComponent(t.table);};nav.append(b);}
  if(!o.tables.length){const p=document.createElement('p');p.className='muted';p.textContent='No SQL tables yet.';nav.append(p);}
  filterTables();}
function filterTables(){const q=$('#filter').value.toLowerCase();for(const b of document.querySelectorAll('#tables button'))b.hidden=!b.dataset.table.toLowerCase().includes(q);}
async function openTable(name){
  state={table:name,offset:0,columns:[],rows:[]};$('#where').value='';$('#desc').checked=false;
  for(const b of document.querySelectorAll('#tables button'))b.setAttribute('aria-current',b.dataset.table===name);
  $('#table-name').textContent=name;show('#table-view');selectTab('rows');
  try{const d=await call('/api/sql',{sql:'DESCRIBE '+ident(name)});state.columns=d.rows.map(r=>r.column);
    const sel=$('#order');sel.replaceChildren(...state.columns.map(c=>{const o=document.createElement('option');o.value=o.textContent=c;return o;}));}
  catch(e){toast(e.message);}
  await loadRows();}
async function loadRows(){
  const t=ident(state.table),order=$('#order').value||'id',dir=$('#desc').checked?' DESC':'';
  const where=$('#where').value.trim(),w=where?' WHERE '+where:'';
  try{const [c,r]=await Promise.all([call('/api/sql',{sql:'SELECT COUNT(*) AS n FROM '+t+w}),
      call('/api/sql',{sql:'SELECT * FROM '+t+w+' ORDER BY '+ident(order)+dir+' LIMIT '+(PAGE_SIZE+1)+' OFFSET '+state.offset})]);
    const total=c.rows[0].n,more=r.rows.length>PAGE_SIZE,rows=r.rows.slice(0,PAGE_SIZE);state.rows=rows;
    renderGrid($('#rows'),rows,null,{table:state.table,sorted:order,desc:!!dir,onSort:col=>{
      if($('#order').value===col)$('#desc').checked=!$('#desc').checked;else{if(![...$('#order').options].some(o=>o.value===col)){const o=document.createElement('option');o.value=o.textContent=col;$('#order').append(o);}$('#order').value=col;$('#desc').checked=false;}
      state.offset=0;loadRows();}});
    $('#page-info').textContent=total?((state.offset+1).toLocaleString()+'–'+(state.offset+rows.length).toLocaleString()+' of '+total.toLocaleString()+(where?' matching':'')+' · '+r.ms.toFixed(1)+' ms'):'No matching rows';
    $('#prev').disabled=state.offset===0;$('#next').disabled=!more;}
  catch(e){toast(e.message);}}
async function loadSchema(){
  try{const [c,i]=await Promise.all([call('/api/sql',{sql:'DESCRIBE '+ident(state.table)}),call('/api/sql',{sql:'SHOW INDEX FROM '+ident(state.table)})]);
    renderGrid($('#columns'),c.rows,['column','type','notNull','key','default']);renderGrid($('#indexes'),i.rows,['name','column','unique','entries']);
    if(readOnly)return;
    addActions($('#columns'),c.rows,r=>r.column==='id'?[]:[['Rename',()=>renameColumn(r.column)],['Drop',()=>dropColumn(r.column)]]);
    addActions($('#indexes'),i.rows,r=>r.name==='PRIMARY'?[]:[['Drop',()=>ddl('Drop index '+r.name,'',()=>'DROP INDEX '+ident(r.name)+' ON '+ident(state.table))]]);}
  catch(e){toast(e.message);}}
function addActions(table,rows,make){
  table.tHead.rows[0].append(document.createElement('th'));
  [...table.tBodies[0].rows].forEach((tr,k)=>{const td=tr.insertCell();td.className='actions';tr.onclick=null;
    if(rows[k])for(const [label,fn] of make(rows[k])){const b=document.createElement('button');b.textContent=label;if(label==='Drop')b.className='danger';b.onclick=e=>{e.stopPropagation();fn();};td.append(b);}});}
// ---------- Schema dialogs: a form that writes SQL, shown before running ----------
const TYPES=['TEXT','INTEGER','DECIMAL','BOOLEAN','DATE','TIMESTAMP','JSON'];
function el(tag,props={},kids=[]){const e=document.createElement(tag);Object.assign(e,props);for(const k of kids)e.append(k);return e;}
function field(label,input){return el('label',{},[label,input]);}
function literal(v,type){if(v==='')return null;if(/^(INTEGER|DECIMAL)$/.test(type)&&!isNaN(Number(v)))return String(Number(v));if(type==='BOOLEAN')return /^(true|1|yes)$/i.test(v)?'TRUE':'FALSE';return "'"+v.replace(/'/g,"''")+"'";}
function columnSql(c){let s=ident(c.name.value.trim()||'?')+' '+c.type.value;if(c.notNull?.checked)s+=' NOT NULL';if(c.unique?.checked)s+=' UNIQUE';const d=literal(c.def.value.trim(),c.type.value);if(d!==null)s+=' DEFAULT '+d;return s;}
function columnRow(onChange,removable=true){
  const c={name:el('input',{type:'text',placeholder:'column name'}),type:el('select',{},TYPES.map(t=>new Option(t,t))),notNull:el('input',{type:'checkbox'}),unique:el('input',{type:'checkbox'}),def:el('input',{type:'text',placeholder:'default'})};
  c.row=el('div',{className:'col'},[c.name,c.type,el('label',{},[c.notNull,'NOT NULL']),el('label',{},[c.unique,'UNIQUE']),c.def]);
  if(removable){const x=el('button',{type:'button',textContent:'Remove'});x.onclick=()=>{c.row.remove();c.removed=true;onChange();};c.row.append(x);}
  for(const k of ['name','type','notNull','unique','def'])c[k].oninput=c[k].onchange=onChange;
  return c;}
let ddlAfter=null;
function ddl(title,formNode,build,after){
  $('#ddl-title').textContent=title;$('#ddl-form').replaceChildren(...(formNode?[formNode]:[]));$('#ddl-error').textContent='';
  const refresh=()=>{$('#ddl-sql').value=build();};refresh();ddl.refresh=refresh;ddlAfter=after||null;$('#ddl').showModal();}
async function runDdl(){
  $('#ddl-error').textContent='';
  try{await call('/api/sql',{sql:$('#ddl-sql').value});$('#ddl').close();await overview();if(ddlAfter)await ddlAfter();else if(state.table){selectTab('schema');}}
  catch(e){$('#ddl-error').textContent=e.message;}}
function newTable(){
  const name=el('input',{type:'text',placeholder:'table name'}),cols=[],list=el('div',{className:'cols'});
  const change=()=>ddl.refresh();
  const add=()=>{const c=columnRow(change);cols.push(c);list.append(c.row);change();};
  const more=el('button',{type:'button',textContent:'+ Column'});more.onclick=add;
  name.oninput=change;
  const form=el('div',{},[field('Table',name),el('p',{className:'muted',textContent:'Every table gets an id INTEGER PRIMARY KEY, numbered automatically.'}),list,more]);
  ddl('New table',form,()=>'CREATE TABLE '+ident(name.value.trim()||'new_table')+' (\\n  id INTEGER PRIMARY KEY'+cols.filter(c=>!c.removed).map(c=>',\\n  '+columnSql(c)).join('')+'\\n)',
    ()=>{const t=name.value.trim();if(t)location.hash='table='+encodeURIComponent(t);});
  add();}
function addColumn(){
  const c=columnRow(()=>ddl.refresh(),false);
  ddl('Add column to '+state.table,c.row,()=>'ALTER TABLE '+ident(state.table)+' ADD COLUMN '+columnSql(c),()=>selectTab('schema'));}
function renameColumn(col){
  const to=el('input',{type:'text',value:col});to.oninput=()=>ddl.refresh();
  ddl('Rename column '+col,field('New name',to),()=>'ALTER TABLE '+ident(state.table)+' RENAME COLUMN '+ident(col)+' TO '+ident(to.value.trim()||col),()=>selectTab('schema'));}
function dropColumn(col){ddl('Drop column '+col,el('p',{className:'error',textContent:'Removes '+col+' from every row of '+state.table+'. This cannot be undone.'}),()=>'ALTER TABLE '+ident(state.table)+' DROP COLUMN '+ident(col),()=>selectTab('schema'));}
function addIndex(){
  const col=el('select',{},state.columns.filter(c=>c!=='id').map(c=>new Option(c,c))),uniq=el('input',{type:'checkbox'});
  col.onchange=uniq.onchange=()=>ddl.refresh();
  ddl('Add index on '+state.table,el('div',{},[field('Column',col),el('label',{},[uniq,'UNIQUE'])]),()=>'CREATE '+(uniq.checked?'UNIQUE ':'')+'INDEX ON '+ident(state.table)+' ('+ident(col.value||'?')+')',()=>selectTab('schema'));}
function dropTable(){
  const t=state.table,confirmBox=el('input',{type:'text',placeholder:'type the table name to confirm'});
  confirmBox.oninput=()=>{$('#ddl-run').disabled=confirmBox.value!==t;};
  ddl('Drop table '+t,el('div',{},[el('p',{className:'error',textContent:'Deletes the table '+t+' and all its rows and indexes. This cannot be undone.'}),confirmBox]),()=>'DROP TABLE '+ident(t),
    ()=>{state.table=null;history.replaceState(null,'','#');show('#empty');});
  $('#ddl-run').disabled=true;}
$('#ddl-run').onclick=runDdl;$('#ddl-close').onclick=()=>$('#ddl').close();$('#ddl').addEventListener('close',()=>{$('#ddl-run').disabled=false;});
$('#new-table').onclick=newTable;$('#add-column').onclick=addColumn;$('#add-index').onclick=addIndex;$('#drop-table').onclick=dropTable;
function selectTab(tab){for(const b of document.querySelectorAll('[role=tab]'))b.setAttribute('aria-selected',b.dataset.tab===tab);
  $('#tab-rows').hidden=tab!=='rows';$('#tab-schema').hidden=tab!=='schema';if(tab==='schema')loadSchema();}
async function runSql(){
  const text=$('#sql').value;$('#run').disabled=true;$('#sql-info').textContent='Running…';$('#sql-info').classList.remove('error');
  try{const r=await call('/api/sql',{sql:text});
    rememberQuery(text);
    if(r.rows){lastResult=r.rows;renderGrid($('#result'),r.rows);$('#sql-info').textContent=r.rows.length+' row'+(r.rows.length===1?'':'s')+(r.truncated?' (first '+r.rows.length+' shown)':'')+' · '+r.ms.toFixed(1)+' ms';}
    else{renderGrid($('#result'),[r],Object.keys(r));$('#sql-info').textContent=r.ms.toFixed(1)+' ms';overview().catch(()=>{});}
    try{localStorage.setItem('viewer-sql',text);}catch{}}
  catch(e){$('#sql-info').textContent=e.message;$('#sql-info').classList.add('error');}
  finally{$('#run').disabled=false;}}
$('#filter').oninput=filterTables;
$('#console-btn').onclick=()=>{if(location.hash!=='#console')history.replaceState(null,'','#console');for(const b of document.querySelectorAll('#tables button'))b.setAttribute('aria-current',false);show('#console-view');$('#sql').focus();};
for(const b of document.querySelectorAll('[role=tab]'))b.onclick=()=>selectTab(b.dataset.tab);
$('#prev').onclick=()=>{state.offset=Math.max(0,state.offset-PAGE_SIZE);loadRows();};
$('#next').onclick=()=>{state.offset+=PAGE_SIZE;loadRows();};
$('#order').onchange=$('#desc').onchange=()=>{state.offset=0;loadRows();};
$('#run').onclick=runSql;
$('#sql').onkeydown=e=>{if(e.key==='Enter'&&(e.ctrlKey||e.metaKey)){e.preventDefault();runSql();}};
try{const saved=localStorage.getItem('viewer-sql');if(saved)$('#sql').value=saved;}catch{}
// ---------- Row detail: view, edit, delete, insert ----------
let detail={row:null,table:null,isNew:false};
function openDetail(row,table,isNew=false){
  detail={row,table:table||null,isNew};
  $('#detail-title').textContent=isNew?'New row in '+table:(table?table+' · id '+JSON.stringify(row.id):'Row');
  $('#detail-json').value=JSON.stringify(row,null,2);$('#detail-json').readOnly=readOnly||!table;$('#detail-error').textContent='';
  $('#detail-save').hidden=$('#detail-delete').hidden=readOnly||!table;if(isNew)$('#detail-delete').hidden=true;
  $('#detail').showModal();}
async function saveDetail(){
  let next;try{next=JSON.parse($('#detail-json').value);}catch(e){$('#detail-error').textContent='Not valid JSON: '+e.message;return;}
  if(!next||typeof next!=='object'||Array.isArray(next)){$('#detail-error').textContent='A row is a JSON object.';return;}
  const t=ident(detail.table);
  try{
    if(detail.isNew){const cols=Object.keys(next).filter(k=>!(k==='id'&&(next.id===null||next.id==='')));
      if(!cols.length)throw new Error('Give the row at least one column.');
      await call('/api/sql',{sql:'INSERT INTO '+t+' ('+cols.map(ident).join(', ')+') VALUES ('+cols.map(()=>'?').join(', ')+')',params:cols.map(k=>next[k])});}
    else{const prev=detail.row,changed=Object.keys(next).filter(k=>k!=='id'&&JSON.stringify(next[k])!==JSON.stringify(prev[k])),removed=Object.keys(prev).filter(k=>k!=='id'&&!(k in next));
      if(next.id!==prev.id)throw new Error('The id cannot be changed.');
      const sets=[...changed,...removed];if(!sets.length){$('#detail').close();return;}
      await call('/api/sql',{sql:'UPDATE '+t+' SET '+sets.map(k=>ident(k)+' = ?').join(', ')+' WHERE id = ?',params:[...changed.map(k=>next[k]),...removed.map(()=>null),prev.id]});}
    $('#detail').close();await overview();if(state.table)loadRows();}
  catch(e){$('#detail-error').textContent=e.message;}}
async function deleteDetail(){
  if(!confirm('Delete row '+JSON.stringify(detail.row.id)+' from '+detail.table+'?'))return;
  try{await call('/api/sql',{sql:'DELETE FROM '+ident(detail.table)+' WHERE id = ?',params:[detail.row.id]});$('#detail').close();await overview();loadRows();}
  catch(e){$('#detail-error').textContent=e.message;}}
$('#detail-save').onclick=saveDetail;$('#detail-delete').onclick=deleteDetail;$('#detail-close').onclick=()=>$('#detail').close();
$('#new-row').onclick=()=>{const tpl={};for(const c of state.columns)tpl[c]=c==='id'?null:'';openDetail(tpl,state.table,true);};
// ---------- Filter and export ----------
$('#apply').onclick=()=>{state.offset=0;loadRows();};$('#where').onkeydown=e=>{if(e.key==='Enter')$('#apply').click();};
$('#clear').onclick=()=>{$('#where').value='';state.offset=0;loadRows();};
function download(name,text,type){const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([text],{type}));a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);}
function toCsv(rows){const cols=[...new Set(rows.flatMap(r=>Object.keys(r)))];const q=v=>{if(v===null||v===undefined)return '';const s=typeof v==='object'?JSON.stringify(v):String(v);return /[",\\n\\r]/.test(s)?'"'+s.replace(/"/g,'""')+'"':s;};
  return [cols.join(','),...rows.map(r=>cols.map(c=>q(r[c])).join(','))].join('\\r\\n');}
async function exportTable(kind){
  const where=$('#where').value.trim();
  try{const r=await call('/api/sql',{sql:'SELECT * FROM '+ident(state.table)+(where?' WHERE '+where:'')+' ORDER BY '+ident($('#order').value||'id')+($('#desc').checked?' DESC':'')+' LIMIT 1000'});
    if(kind==='csv')download(state.table+'.csv',toCsv(r.rows),'text/csv');else download(state.table+'.json',JSON.stringify(r.rows,null,2),'application/json');
    if(r.rows.length>=1000)toast('Exported the first 1,000 rows.');}
  catch(e){toast(e.message);}}
$('#export-csv').onclick=()=>exportTable('csv');$('#export-json').onclick=()=>exportTable('json');
let lastResult=[];
$('#res-csv').onclick=()=>download('result.csv',toCsv(lastResult),'text/csv');$('#res-json').onclick=()=>download('result.json',JSON.stringify(lastResult,null,2),'application/json');
// ---------- History ----------
function queryHistory(){try{return JSON.parse(localStorage.getItem('viewer-history')||'[]');}catch{return [];}}
function renderHistory(){const sel=$('#history');sel.replaceChildren(new Option('History',''));for(const h of queryHistory())sel.append(new Option(h.length>80?h.slice(0,80)+'…':h,h));}
function rememberQuery(text){try{localStorage.setItem('viewer-history',JSON.stringify([text,...queryHistory().filter(h=>h!==text)].slice(0,20)));}catch{}renderHistory();}
$('#history').onchange=()=>{if($('#history').value){$('#sql').value=$('#history').value;$('#sql').focus();}$('#history').value='';};
renderHistory();
// Links: #table=NAME opens a table, #console the SQL console.
function route(){const h=decodeURIComponent(location.hash.slice(1));if(h.startsWith('table='))openTable(h.slice(6));else if(h==='console')$('#console-btn').click();}
window.addEventListener('hashchange',route);
// ---------- Open window ----------
let cwd=null;
const fmtSize=n=>n>=1048576?(n/1048576).toFixed(1)+' MB':n>=1024?(n/1024).toFixed(0)+' KB':n+' B';
function recent(){try{return JSON.parse(localStorage.getItem('viewer-recent')||'[]');}catch{return [];}}
function remember(item){try{const list=[item,...recent().filter(x=>JSON.stringify(x)!==JSON.stringify(item))].slice(0,8);localStorage.setItem('viewer-recent',JSON.stringify(list));}catch{}}
function renderRecent(){const box=$('#recent');box.replaceChildren();const list=recent();if(!list.length)return;
  const l=document.createElement('span');l.className='muted';l.textContent='Recent:';box.append(l);
  for(const r of list){const b=document.createElement('button');b.textContent=r.file||r.server;b.onclick=()=>openDb(r);box.append(b);}}
async function browseTo(dir){
  try{const d=await call('/api/browse'+(dir!=null?'?dir='+encodeURIComponent(dir):''));cwd=d;
    $('#cwd').textContent=d.dir||'Drives';$('#up').disabled=d.parent==null;
    const t=$('#entries');t.replaceChildren();const body=t.createTBody();
    for(const e of d.entries){const tr=body.insertRow();const a=tr.insertCell(),b=tr.insertCell();
      a.textContent=(e.dir?'📁 ':'')+e.name;if(e.kdb)a.className='hdb';b.textContent=e.dir?'':fmtSize(e.size);b.className='num muted';
      tr.onclick=()=>e.dir?browseTo(e.path):($('#path').value=e.path);tr.ondblclick=()=>{if(!e.dir)openDb({file:e.path});};}
    if(!d.entries.length){const tr=body.insertRow();const c=tr.insertCell();c.textContent='Empty folder';c.className='null';}}
  catch(e){$('#open-error').textContent=e.message;}}
async function openDb(target){
  $('#open-error').textContent='';
  try{await call('/api/open',target);remember(target.server?{server:target.server}:{file:target.file});$('#open').close();history.replaceState(null,'','#');await overview();}
  catch(e){$('#open-error').textContent=e.message;}}
function openDialog(){$('#open-error').textContent='';renderRecent();if(!$('#open').open)$('#open').showModal();if(!cwd)browseTo(null);}
$('#open-btn').onclick=openDialog;$('#open-close').onclick=()=>$('#open').close();
$('#up').onclick=()=>{if(cwd&&cwd.parent!=null)browseTo(cwd.parent);};$('#home').onclick=()=>browseTo(cwd?.home??null);
$('#path-open').onclick=()=>openDb({file:$('#path').value.trim()});$('#path').onkeydown=e=>{if(e.key==='Enter')$('#path-open').click();};
$('#srv-open').onclick=()=>openDb({server:$('#srv-address').value.trim(),token:$('#srv-token').value,ca:$('#srv-ca').value.trim()||undefined});
for(const b of document.querySelectorAll('[data-open]'))b.onclick=()=>{for(const x of document.querySelectorAll('[data-open]'))x.setAttribute('aria-selected',x===b);
  $('#open-file').hidden=b.dataset.open!=='file';$('#open-server').hidden=b.dataset.open!=='server';};
overview().then(o=>{if(!o?.needOpen)route();}).catch(e=>toast(e.message));`;
