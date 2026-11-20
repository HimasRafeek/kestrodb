// KestroDB against other embedded databases on the same machine, with the same JSON
// documents and matched durability (every acknowledged write was flushed to disk).
//
//   node bench/compare.mjs [--keys 20000] [--durable 300] [--runs 3]
//                          [--engines inProcess,worker,sqlite,...] [--worker path] [--addon path]
//                          [--record "label"] [--json out.json]
//
// Engines (bench/engines.mjs): KestroDB in-process and worker, SQLite through node:sqlite,
// plus better-sqlite3, LMDB and LevelDB when installed in bench/competitors
// (cd bench/competitors && npm install). Engines that fail to load are skipped.
//
// --runs N   repeat the suite on fresh databases and report each median
// --record   append the run to results/bench-history.json and regenerate
//            results/bench-history.js, which index.html charts
import {mkdtempSync, mkdirSync, rmSync, writeFileSync, readFileSync, existsSync} from 'node:fs';
import {tmpdir, cpus, platform, arch, totalmem} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {engineFactories} from './engines.mjs';

const arg = (name, fallback) => { const i = process.argv.indexOf('--' + name); return i > 0 ? process.argv[i + 1] : fallback; };
const KEYS = Number(arg('keys', 20000));
const DURABLE = Number(arg('durable', 1000));
const INFLIGHT = DURABLE * 3;  // long enough that a GC pause or disk hiccup cannot dominate
const RUNS = Number(arg('runs', 1));
const REOPENS = 10;
const BATCH = 1000;
const CONCURRENCY = 256;
const only = arg('engines', '') ? arg('engines').split(',') : null;
const label = arg('record', undefined);
const jsonOut = arg('json', undefined);
const root = fileURLToPath(new URL('../', import.meta.url));

const doc = i => ({id: i, name: 'user-' + i, email: `user${i}@example.com`, tags: ['a', 'b', 'c'], score: i * 7 % 1000, bio: 'x'.repeat(120)});
const ctx = {
  docs: Array.from({length: Math.max(KEYS, INFLIGHT)}, (_, i) => doc(i)),
  key: i => 'k' + i,
  ops: (a, b) => Array.from({length: b - a}, (_, j) => ({type: 'set', collection: 'c', key: 'k' + (a + j), value: ctx.docs[a + j]})),
};

// ---------- Engines ----------
const engines = [];
for (const f of engineFactories({workerBinary: arg('worker'), addonPath: arg('addon')})) {
  if (only && !only.includes(f.id)) continue;
  try {
    const lib = await f.load();
    const probe = mkdtempSync(join(tmpdir(), 'kestro-probe-'));
    try { const a = await f.create(lib, probe, ctx); await a.close(); } finally { rmSync(probe, {recursive: true, force: true}); }
    engines.push({...f, lib});
  } catch (e) {
    console.warn(`skipping ${f.name}: ${e.message.split('\n')[0]}`);
  }
}
if (!engines.length) throw Error('no engines available');

// ---------- Phases ----------
function check(ok, what) { if (!ok) throw Error('benchmark check failed: ' + what); }
async function inFlight(n, fn) {
  let next = 0;
  const lane = async () => { while (next < n) await fn(next++); };
  await Promise.all(Array.from({length: Math.min(CONCURRENCY, n)}, lane));
}
// Synchronous engines cannot overlap requests; their in-flight phases run them in turn.
const many = (e, n, fn) => e.overlap ? inFlight(n, fn) : (async () => { for (let i = 0; i < n; i++) await fn(i); })();
const keys100 = s => Array.from({length: 100}, (_, j) => ctx.key((s + j * 37) % KEYS));
const MIXED = Math.floor(KEYS / 2);

const PHASES = [
  {id: 'write-awaited', title: 'Durable writes, one at a time', ops: DURABLE, group: 'write',
    run: async e => { for (let i = 0; i < DURABLE; i++) await e.set(i); }},
  {id: 'write-inflight', title: `Durable writes, ${CONCURRENCY} in flight`, ops: INFLIGHT, group: 'write',
    run: e => many(e, INFLIGHT, e.setInFlight ?? e.set)},
  {id: 'bulk-load', title: `Bulk load, ${BATCH}-write transactions`, ops: KEYS, group: 'write',
    run: async e => { for (let i = 0; i < KEYS; i += BATCH) await e.batch(i, Math.min(i + BATCH, KEYS)); }},
  {id: 'read-awaited', title: 'Point reads, one at a time', ops: KEYS, group: 'read',
    run: async e => { let bad = 0; for (let i = 0; i < KEYS; i++) if ((await e.get(i)).id !== i) bad++; check(!bad, e.id + ' point reads'); }},
  {id: 'read-inflight', title: `Point reads, ${CONCURRENCY} in flight`, ops: KEYS, group: 'read',
    run: async e => { let seen = 0; await many(e, KEYS, async i => { if ((await e.get(i)).id === i) seen++; }); check(seen === KEYS, e.id + ' concurrent reads'); }},
  {id: 'read-many', title: 'Multi-key reads, 100 keys per call', ops: KEYS, group: 'read',
    run: async e => { let n = 0; for (let s = 0; s < KEYS; s += 100) n += (await e.getMany(keys100(s))).filter(Boolean).length; check(n === KEYS, e.id + ' getMany'); }},
  {id: 'mixed', title: `Mixed 95% reads / 5% durable writes, ${CONCURRENCY} in flight`, ops: MIXED, group: 'mixed',
    run: async e => { const w = e.setInFlight ?? e.set; let bad = 0; await many(e, MIXED, async i => { if (i % 20 === 0) await w(i % KEYS); else if ((await e.get((i * 7919) % KEYS)) === null) bad++; }); check(!bad, e.id + ' mixed reads'); }},
  // Ten reopens in a row: a single ~3 ms reopen is dominated by noise (an antivirus
  // scan of the just-written file, scheduler hiccups) for every engine.
  {id: 'open', title: `Open an existing ${KEYS.toLocaleString('en-US')}-key database`, ops: KEYS * REOPENS, unit: 'keys/s', group: 'open',
    run: async e => { for (let i = 0; i < REOPENS; i++) await e.reopen(); }},
];

const dir = mkdtempSync(join(tmpdir(), 'kestro-bench-'));
const samples = PHASES.map(() => ({}));
const info = {};
try {
  for (let run = 0; run < RUNS; run++) {
    const adapters = [];
    for (const e of engines) {
      const d = join(dir, `run${run}-${e.id}`);
      mkdirSync(d);
      const a = await e.create(e.lib, d, ctx);
      if (a.info) info[e.id] = a.info;
      adapters.push({...a, id: e.id});
    }
    try {
      // Engines alternate inside each phase, so drift in machine load hits them alike, and the
      // order rotates with each phase and run: an engine that runs right after another one's
      // background work (LevelDB compacts on its own threads) is not always the same engine.
      for (const [k, p] of PHASES.entries())
        for (let j = 0; j < adapters.length; j++) {
          const a = adapters[(j + k + run) % adapters.length];
          const t = process.hrtime.bigint();
          await p.run(a);
          (samples[k][a.id] ??= []).push(p.ops / (Number(process.hrtime.bigint() - t) / 1e9));
        }
      for (const a of adapters) check(await a.count() === KEYS, a.id + ' key count');
    } finally {
      for (const a of adapters) await a.close();
    }
    if (RUNS > 1) console.log(`run ${run + 1}/${RUNS} done`);
  }
} finally {
  rmSync(dir, {recursive: true, force: true});
}
const median = xs => { const s = [...xs].sort((a, b) => a - b), m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };
const results = PHASES.map((p, k) => ({id: p.id, phase: p.title, ops: p.ops, unit: p.unit ?? 'ops/s', group: p.group,
  ...Object.fromEntries(Object.entries(samples[k]).map(([id, xs]) => [id, median(xs)]))}));

// ---------- Report ----------
const fmt = n => n === undefined ? '—' : n >= 1e6 ? (n / 1e6).toFixed(2) + 'M' : n >= 1e3 ? (n / 1e3).toFixed(1) + 'K' : n.toFixed(0);
const env = {platform: platform(), arch: arch(), cpu: cpus()[0].model.trim(), cores: cpus().length, memoryGB: Math.round(totalmem() / 2 ** 30), node: process.version, sqlite: process.versions.sqlite};
const kestro = info.inProcess ?? info.worker;
console.log(`\n${env.platform} ${env.arch}, ${env.cpu}, Node ${env.node}; ${KEYS} keys; median of ${RUNS} run(s); higher is better`);
if (kestro) console.log(`KestroDB: ${kestro.readMode} reads, ${kestro.fileSync}\n`);
const ids = engines.map(e => e.id), w = Math.max(...results.map(r => r.phase.length));
console.log('benchmark'.padEnd(w), ...ids.map(id => id.padStart(14)), '  KestroDB rank');
for (const r of results) {
  const ranked = ids.filter(id => r[id] !== undefined).sort((a, b) => r[b] - r[a]);
  const pos = ranked.findIndex(id => engines.find(e => e.id === id).kind === 'kestro') + 1;
  const lead = pos === 1 ? `#1 (${(r[ranked[0]] / r[ranked[1]]).toFixed(2)}x ahead of ${ranked[1]})` : `#${pos} (${ranked[0]} ${(r[ranked[0]] / r[ranked[pos - 1]]).toFixed(2)}x ahead)`;
  console.log(r.phase.padEnd(w), ...ids.map(id => fmt(r[id]).padStart(14)), '  ' + lead);
}

const run = {
  date: new Date().toISOString(), label: label ?? null, ...env, keys: KEYS, durable: DURABLE, concurrency: CONCURRENCY, runs: RUNS,
  kestro, engines: engines.map(e => ({id: e.id, name: e.name, kind: e.kind, version: e.kind === 'kestro' ? JSON.parse(readFileSync(join(root, 'package.json'), 'utf8')).version : e.version?.() ?? null, durability: e.durability})),
  results,
};
if (jsonOut) writeFileSync(jsonOut, JSON.stringify(run, null, 2));
if (label) {
  const historyJson = join(root, 'results', 'bench-history.json');
  const history = existsSync(historyJson) ? JSON.parse(readFileSync(historyJson, 'utf8')) : [];
  history.push(run);
  writeFileSync(historyJson, JSON.stringify(history, null, 2) + '\n');
  // A script file rather than fetched JSON, so index.html also works opened from disk.
  writeFileSync(join(root, 'results', 'bench-history.js'), '// Generated by bench/compare.mjs --record. Edit bench-history.json, not this file.\nwindow.KESTRO_BENCH_HISTORY = ' + JSON.stringify(history) + ';\n');
  console.log(`\nRecorded "${label}" as run ${history.length} in results/bench-history.json`);
}
