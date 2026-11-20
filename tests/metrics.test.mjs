// kestro-server --metrics: the Prometheus endpoint (format checked by a small parser here),
// health checks (live, ready: 503 while degraded, without a cluster leader, a follower not
// connected), bearer tokens, the address rules, and counters that follow real activity.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdir, mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const TOKEN = 'metrics-token-0123456789', SCRAPE = 'scrape-token-0123456789abcdef';
const sleep = ms => new Promise(r => setTimeout(r, ms));

// Starts kestro-server with --metrics on a free port: {address, http, stop, log}.
async function start(dir, args = [], {db = 'm.kdb', token = true, metrics = ['--metrics', '127.0.0.1:0'], tcp = '127.0.0.1:0'} = {}) {
  if (token) await writeFile(join(dir, 'token'), TOKEN + '\n');
  const child = spawn(serverBinary, ['--db', join(dir, db), '--tcp', tcp, '--stop-on-stdin-eof', ...(token ? ['--token-file', join(dir, 'token')] : []), ...metrics, ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  const exited = new Promise(r => child.on('exit', r));
  const [address, http] = await new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const a = /KESTRODB READY tcp (\S+)/.exec(out), h = /KESTRODB METRICS (\S+)/.exec(out); if (a && (h || !metrics.length)) resolve(['tcp://' + a[1], h?.[1]]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${out}${err}`)));
  });
  return {address, http, log: () => out + err, stop: async () => { child.stdin.end(); return exited; }};
}
async function withServer(fn, args, options) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-metrics-'));
  const server = await start(dir, ['--create', ...(args || [])], options);
  try { await fn(server, dir); } finally { await server.stop(); await rm(dir, {recursive: true, force: true}); }
}
const get = (url, headers = {}) => fetch(url, {headers});

// A Prometheus text format 0.0.4 parser that rejects what the format does not allow:
// every sample belongs to a family announced by HELP and TYPE, names and label names are
// valid, label values are escaped, counters end in _total, a histogram's buckets are
// cumulative, end with le="+Inf" equal to _count, and _sum and _count are present.
function parseProm(text) {
  assert.ok(text.endsWith('\n'), 'ends with a newline');
  const families = new Map(); let current = null;
  const NAME = /^[a-zA-Z_:][a-zA-Z0-9_:]*$/, LABEL = /^[a-zA-Z_][a-zA-Z0-9_]*$/;
  for (const line of text.slice(0, -1).split('\n')) {
    assert.ok(line.length, 'no empty lines');
    let m;
    if ((m = /^# HELP (\S+) (.*)$/.exec(line))) {
      assert.match(m[1], NAME); assert.ok(!families.has(m[1]), `${m[1]} announced once`);
      current = {name: m[1], help: m[2], type: null, samples: []}; families.set(m[1], current); continue;
    }
    if ((m = /^# TYPE (\S+) (counter|gauge|histogram|summary|untyped)$/.exec(line))) {
      assert.equal(m[1], current?.name, 'TYPE follows its HELP'); current.type = m[2]; continue;
    }
    assert.ok(!line.startsWith('#'), `unknown comment line: ${line}`);
    m = /^([a-zA-Z_:][a-zA-Z0-9_:]*)(\{(.*)\})? (\S+)$/.exec(line);
    assert.ok(m, `sample line: ${line}`);
    const [, name, , body = '', value] = m;
    const labels = {};
    for (let i = 0; i < body.length;) {
      const eq = body.indexOf('=', i); const key = body.slice(i, eq); assert.match(key, LABEL);
      assert.equal(body[eq + 1], '"', 'label values are quoted');
      let j = eq + 2, v = '';
      for (; body[j] !== '"'; j++) {
        assert.ok(j < body.length, 'label value terminated');
        if (body[j] === '\\') { const e = body[++j]; assert.ok('\\"n'.includes(e), `escape \\${e}`); v += e === 'n' ? '\n' : e; } else { assert.notEqual(body[j], '\n'); v += body[j]; }
      }
      labels[key] = v; i = j + 1; if (body[i] === ',') i++;
    }
    assert.ok(current && current.type, `${name} after its HELP and TYPE`);
    const suffix = name.slice(current.name.length);
    assert.ok(name.startsWith(current.name) && (suffix === '' || (current.type === 'histogram' && ['_bucket', '_sum', '_count'].includes(suffix))), `${name} belongs to ${current.name}`);
    const num = value === '+Inf' ? Infinity : value === '-Inf' ? -Infinity : Number(value);
    assert.ok(!Number.isNaN(num), `numeric value: ${line}`);
    current.samples.push({name, labels, value: num});
  }
  for (const f of families.values()) {
    assert.ok(f.name.startsWith('kestrodb_'), `${f.name} is prefixed`);
    assert.ok(f.help.length > 5, `${f.name} has help`);
    if (f.type === 'counter') { assert.match(f.name, /_total$/, `counter ${f.name} ends in _total`); for (const s of f.samples) assert.ok(s.value >= 0); }
    if (f.type === 'histogram') {
      assert.match(f.name, /_seconds$/, 'durations are in seconds');
      const buckets = f.samples.filter(s => s.name.endsWith('_bucket'));
      assert.ok(buckets.length >= 2);
      let prev = -1, prevLe = -Infinity;
      for (const b of buckets) { const le = b.labels.le === '+Inf' ? Infinity : Number(b.labels.le); assert.ok(le > prevLe, 'le increases'); assert.ok(b.value >= prev, 'buckets are cumulative'); prev = b.value; prevLe = le; }
      assert.equal(buckets.at(-1).labels.le, '+Inf', 'the last bucket is +Inf');
      const count = f.samples.find(s => s.name === f.name + '_count'), sum = f.samples.find(s => s.name === f.name + '_sum');
      assert.ok(count && sum, `${f.name} has _sum and _count`); assert.equal(count.value, buckets.at(-1).value, '+Inf bucket equals _count');
    }
  }
  return families;
}
// The value of one sample: name and (a subset of) its labels.
function value(families, name, labels = {}) {
  const fam = families.get(name) ?? families.get(name.replace(/_(bucket|sum|count)$/, ''));
  const s = fam?.samples.find(x => x.name === name && Object.entries(labels).every(([k, v]) => x.labels[k] === v));
  return s ? s.value : undefined;
}
async function scrape(http, headers) {
  const r = await get(http + '/metrics', headers);
  assert.equal(r.status, 200); assert.match(r.headers.get('content-type'), /^text\/plain; version=0\.0\.4/);
  return parseProm(await r.text());
}

test('metrics: the exposition format is valid, and counters follow SQL and key/value activity', () => withServer(async ({address, http}) => {
  const before = await scrape(http);
  for (const name of ['kestrodb_build_info', 'kestrodb_uptime_seconds', 'kestrodb_connections', 'kestrodb_connections_total', 'kestrodb_connections_refused_total', 'kestrodb_auth_failures_total',
    'kestrodb_requests_total', 'kestrodb_sql_statements_total', 'kestrodb_sql_statement_duration_seconds', 'kestrodb_commits_total', 'kestrodb_commit_groups_total', 'kestrodb_file_flushes_total',
    'kestrodb_commit_duration_seconds', 'kestrodb_network_receive_bytes_total', 'kestrodb_network_transmit_bytes_total', 'kestrodb_database_bytes', 'kestrodb_keys', 'kestrodb_index_memory_bytes',
    'kestrodb_index_file_bytes', 'kestrodb_ordered_memory_bytes', 'kestrodb_snapshot_undo_memory_bytes', 'kestrodb_snapshot_yields_total', 'kestrodb_compactions_total', 'kestrodb_backups_total',
    'kestrodb_backup_failures_total', 'kestrodb_last_backup_success_timestamp_seconds', 'kestrodb_role', 'kestrodb_replication_lag_transactions', 'kestrodb_replication_lag_bytes',
    'kestrodb_replication_lag_seconds', 'kestrodb_replication_followers', 'kestrodb_listeners', 'kestrodb_notifications_sent_total', 'kestrodb_listeners_dropped_total', 'kestrodb_ready',
    'kestrodb_slow_queries_total', 'kestrodb_slow_query_log_dropped_total'])
    assert.ok(before.has(name), `${name} is exported`);
  const info = before.get('kestrodb_build_info').samples[0];
  assert.equal(info.value, 1); assert.match(info.labels.version, /^\d+\.\d+\.\d+/); assert.ok(['windows', 'linux', 'macos'].includes(info.labels.platform));
  assert.equal(value(before, 'kestrodb_role', {role: 'leader'}), 1); assert.equal(value(before, 'kestrodb_ready'), 1);

  const db = await RemoteDB.connect(address, {token: TOKEN});
  try {
    await db.execute('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT, n INTEGER)');
    for (let i = 0; i < 20; i++) await db.execute('INSERT INTO t (v, n) VALUES (?, ?)', ['v' + i, i]);
    await db.execute('UPDATE t SET n = n + 1 WHERE id < 5');
    await db.execute('DELETE FROM t WHERE id = 20');
    for (let i = 0; i < 7; i++) await db.query('SELECT * FROM t WHERE n > ?', [i]);
    await assert.rejects(db.execute('INSERT INTO t (id, v) VALUES (1, ?)', ['dup']), /CONFLICT/);
    await assert.rejects(db.query('SELECT nope FROM missing'), /INPUT/);
    await Promise.all(Array.from({length: 30}, (_, i) => db.set('kv', 'k' + i, {i})));
    for (let i = 0; i < 10; i++) await db.get('kv', 'k' + i);
    await db.getMany('kv', ['k1', 'k2']);
    await db.stats();
    const after = await scrape(http);
    const d = (name, labels) => value(after, name, labels) - (value(before, name, labels) ?? 0);
    assert.equal(d('kestrodb_sql_statements_total', {kind: 'ddl'}), 1);
    assert.equal(d('kestrodb_sql_statements_total', {kind: 'insert'}), 21);
    assert.equal(d('kestrodb_sql_statements_total', {kind: 'update'}), 1);
    assert.equal(d('kestrodb_sql_statements_total', {kind: 'delete'}), 1);
    assert.equal(d('kestrodb_sql_statements_total', {kind: 'select'}), 8);
    assert.equal(value(after, 'kestrodb_sql_errors_total', {code: 'CONFLICT'}), 1);
    assert.equal(value(after, 'kestrodb_sql_errors_total', {code: 'INPUT'}), 1);
    assert.ok(value(after, 'kestrodb_request_errors_total', {code: 'CONFLICT'}) >= 1);
    // The live connection's counts are in the totals already (it is still open).
    assert.equal(d('kestrodb_requests_total', {type: 'batch'}), 30);
    assert.equal(d('kestrodb_requests_total', {type: 'get'}), 10);
    assert.equal(d('kestrodb_requests_total', {type: 'get_many'}), 1);
    assert.equal(d('kestrodb_requests_total', {type: 'sql'}), 32);
    assert.equal(d('kestrodb_requests_total', {type: 'stats'}), 1);
    assert.equal(d('kestrodb_sql_statement_duration_seconds_count'), 32, 'every statement is timed');
    assert.ok(d('kestrodb_sql_statement_duration_seconds_sum') > 0);
    assert.ok(d('kestrodb_commits_total') >= 52, 'DDL, 20 inserts, update, delete, 30 sets');
    assert.ok(d('kestrodb_commit_groups_total') >= 1 && d('kestrodb_commit_groups_total') <= d('kestrodb_commits_total'));
    assert.equal(d('kestrodb_commit_duration_seconds_count'), d('kestrodb_commit_groups_total'));
    assert.ok(d('kestrodb_network_receive_bytes_total') > 1000 && d('kestrodb_network_transmit_bytes_total') > 1000);
    assert.equal(value(after, 'kestrodb_keys'), (await db.stats()).keys);
    assert.equal(value(after, 'kestrodb_transaction_id'), (await db.stats()).txid);
    assert.ok(value(after, 'kestrodb_database_bytes') > value(before, 'kestrodb_database_bytes'));
    assert.ok(value(after, 'kestrodb_database_file_size_bytes') >= value(after, 'kestrodb_database_bytes'));
    assert.equal(value(after, 'kestrodb_connections'), 1);
    assert.ok(value(after, 'kestrodb_connections_total') >= 1);
  } finally { await db.close(); }
  // A closed connection's counts stay in the totals.
  const closed = await scrape(http);
  assert.equal(value(closed, 'kestrodb_connections'), 0);
  assert.ok(value(closed, 'kestrodb_requests_total', {type: 'sql'}) >= 32);
  assert.ok(value(closed, 'kestrodb_requests_total', {type: 'quit'}) >= 1);

  // HEAD, unknown paths and methods; the reply always closes the connection.
  let r = await fetch(http + '/metrics', {method: 'HEAD'}); assert.equal(r.status, 200); assert.equal(await r.text(), '');
  r = await get(http + '/nope'); assert.equal(r.status, 404); await r.text();
  r = await fetch(http + '/metrics', {method: 'POST', body: 'x'}); assert.equal(r.status, 405); await r.text();
  r = await get(http + '/health/live'); assert.equal(r.status, 200); assert.deepEqual(await r.json(), {status: 'live'});
  r = await get(http + '/health/ready'); assert.equal(r.status, 200); assert.deepEqual(await r.json(), {status: 'ready', role: 'leader'});
  assert.equal(r.headers.get('connection'), 'close');
  // An oversized request is refused, not buffered.
  r = await get(http + '/metrics', {'x-padding': 'a'.repeat(9000)}); assert.equal(r.status, 431); await r.text();
}));

test('metrics: auth failures, refused connections, compaction, backups, LISTEN/NOTIFY', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-metrics-'));
  await mkdir(join(dir, 'backups'));
  const server = await start(dir, ['--create', '--backup-dir', join(dir, 'backups'), '--max-connections', '2']);
  try {
    const {address, http} = server;
    await assert.rejects(RemoteDB.connect(address, {token: 'wrong-token-0123456789'}), /AUTH/);
    const a = await RemoteDB.connect(address, {token: TOKEN}), b = await RemoteDB.connect(address, {token: TOKEN});
    await assert.rejects(RemoteDB.connect(address, {token: TOKEN, reconnect: false}), /BUSY/);
    let m = await scrape(http);
    assert.equal(value(m, 'kestrodb_auth_failures_total', {reason: 'invalid_token'}), 1);
    assert.equal(value(m, 'kestrodb_connections_refused_total', {reason: 'max_connections'}), 1);
    assert.equal(value(m, 'kestrodb_connections'), 2);
    // LISTEN: listeners now, notifications sent.
    const got = [];
    await a.listen('jobs', n => got.push(n.payload));
    for (let i = 0; i < 5; i++) await b.notify('jobs', 'p' + i);
    for (const until = Date.now() + 5000; got.length < 5 && Date.now() < until;) await sleep(10);
    assert.equal(got.length, 5);
    for (const until = Date.now() + 5000; Date.now() < until; await sleep(10)) { m = await scrape(http); if (value(m, 'kestrodb_notifications_sent_total') === 5) break; }  // counted just after the send
    assert.equal(value(m, 'kestrodb_listeners'), 1);
    assert.equal(value(m, 'kestrodb_notifications_sent_total'), 5);
    // Compaction and backups.
    for (let i = 0; i < 50; i++) await b.set('kv', 'k', {i});
    await b.compact();
    await b.backup('one.kdb');
    await assert.rejects(b.backup('../escape.kdb'), /INPUT/);
    m = await scrape(http);
    assert.equal(value(m, 'kestrodb_compactions_total'), 1);
    assert.ok(Math.abs(value(m, 'kestrodb_last_compaction_timestamp_seconds') - Date.now() / 1000) < 60);
    assert.equal(value(m, 'kestrodb_backups_total'), 1);
    assert.ok(Math.abs(value(m, 'kestrodb_last_backup_success_timestamp_seconds') - Date.now() / 1000) < 60);
    assert.equal(value(m, 'kestrodb_requests_total', {type: 'compact'}), 1);
    await a.close(); await b.close();
  } finally { await server.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('metrics: a bearer token, when configured, is required on every path', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-metrics-'));
  await writeFile(join(dir, 'scrape'), SCRAPE + '\n');
  const server = await start(dir, ['--create', '--metrics-token-file', join(dir, 'scrape')]);
  try {
    for (const path of ['/metrics', '/health/live', '/health/ready']) {
      let r = await get(server.http + path); assert.equal(r.status, 401); assert.match(r.headers.get('www-authenticate'), /^Bearer/); await r.text();
      r = await get(server.http + path, {authorization: 'Bearer ' + SCRAPE.slice(0, -1) + 'x'}); assert.equal(r.status, 401); await r.text();
      r = await get(server.http + path, {authorization: 'Basic ' + SCRAPE}); assert.equal(r.status, 401); await r.text();
      r = await get(server.http + path, {authorization: 'Bearer ' + SCRAPE}); assert.equal(r.status, 200); await r.text();
    }
    parseProm(await (await get(server.http + '/metrics', {authorization: 'Bearer ' + SCRAPE})).text());
  } finally { await server.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('metrics: the address must be explicit; a non-loopback one needs a token or --metrics-public', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-metrics-'));
  await writeFile(join(dir, 'scrape'), 'short\n');
  try {
    const run = args => new Promise(resolve => {
      const child = spawn(serverBinary, ['--db', join(dir, 'x.kdb'), '--create', '--tcp', '127.0.0.1:0', ...args], {stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true});
      let out = ''; child.stdout.on('data', d => { out += d; }); child.stderr.on('data', d => { out += d; });
      child.on('exit', code => resolve({code, out}));
    });
    let r = await run(['--metrics', '0.0.0.0:0']);
    assert.equal(r.code, 1); assert.match(r.out, /refusing to serve metrics on 0\.0\.0\.0:0 without a token/);
    r = await run(['--metrics', '[::]:0']); assert.equal(r.code, 1); assert.match(r.out, /without a token/);
    r = await run(['--metrics', ':9187']); assert.equal(r.code, 1); assert.match(r.out, /host given/);
    r = await run(['--metrics', '9187']); assert.equal(r.code, 1); assert.match(r.out, /HOST:PORT/);
    r = await run(['--metrics-token-file', join(dir, 'scrape')]); assert.equal(r.code, 1); assert.match(r.out, /goes with --metrics/);
    r = await run(['--metrics', '127.0.0.1:0', '--metrics-token-file', join(dir, 'scrape')]); assert.equal(r.code, 1); assert.match(r.out, /at least 16/);
    r = await run(['--slow-query-ms', '-1']); assert.equal(r.code, 1); assert.match(r.out, /--slow-query-ms/);
  } finally { await rm(dir, {recursive: true, force: true}); }
});

test('metrics: ready is 503 while synchronous replication is degraded', () => withServer(async ({address, http}) => {
  const db = await RemoteDB.connect(address, {token: TOKEN});
  try {
    let r = await get(http + '/health/ready'); assert.equal(r.status, 200); await r.text();
    await db.set('kv', 'a', 1);  // no follower confirms within --sync-timeout: degraded
    r = await get(http + '/health/ready'); assert.equal(r.status, 503);
    const body = await r.json(); assert.equal(body.status, 'unavailable'); assert.match(body.reason, /degraded/);
    r = await get(http + '/health/live'); assert.equal(r.status, 200); await r.text();
    const m = await scrape(http);
    assert.equal(value(m, 'kestrodb_sync_replication_degraded'), 1);
    assert.equal(value(m, 'kestrodb_sync_replication_timeouts_total'), 1);
    assert.equal(value(m, 'kestrodb_ready'), 0);
  } finally { await db.close(); }
}, ['--sync-replicas', '1', '--sync-timeout', '200ms']));

test('metrics: a follower is ready while it streams; not when its leader is gone; lag and followers', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-metrics-'));
  const leader = await start(dir, ['--create'], {db: 'leader.kdb'});
  let follower;
  try {
    const L = await RemoteDB.connect(leader.address, {token: TOKEN});
    for (let i = 0; i < 20; i++) await L.set('kv', 'k' + i, {i});
    follower = await start(dir, ['--replica-of', leader.address, '--replica-token-file', join(dir, 'token'), '--ready-max-lag', '5s'], {db: 'follower.kdb'});
    const want = (await L.stats()).txid;
    let m;
    for (const until = Date.now() + 10000; Date.now() < until; await sleep(50)) {
      m = await scrape(follower.http);
      if (value(m, 'kestrodb_transaction_id') >= want && value(m, 'kestrodb_replication_connected') === 1) break;
    }
    assert.equal(value(m, 'kestrodb_transaction_id'), want);
    assert.equal(value(m, 'kestrodb_role', {role: 'follower'}), 1);
    assert.equal(value(m, 'kestrodb_read_only'), 1);
    assert.equal(value(m, 'kestrodb_replication_lag_transactions'), 0);
    assert.equal(value(m, 'kestrodb_replication_lag_bytes'), 0);
    assert.ok(value(m, 'kestrodb_replication_lag_seconds') < 5);
    let r = await get(follower.http + '/health/ready'); assert.equal(r.status, 200); assert.deepEqual(await r.json(), {status: 'ready', role: 'follower'});
    const lm = await scrape(leader.http);
    assert.equal(value(lm, 'kestrodb_replication_followers'), 1);
    assert.ok(value(lm, 'kestrodb_requests_total', {type: 'replicate'}) >= 1);
    await L.close();
    await leader.stop();
    // The leader is gone: not ready (it is no longer connected).
    for (const until = Date.now() + 10000; Date.now() < until; await sleep(50)) { r = await get(follower.http + '/health/ready'); if (r.status === 503) break; await r.text(); }
    assert.equal(r.status, 503); assert.match((await r.json()).reason, /not connected to the leader/);
    m = await scrape(follower.http);
    assert.equal(value(m, 'kestrodb_replication_connected'), 0); assert.equal(value(m, 'kestrodb_ready'), 0);
  } finally { if (follower) await follower.stop(); await leader.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('metrics: a cluster member without a leader is live but not ready', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-metrics-'));
  // One member of three, the others never started: no majority, so no leader.
  const self = 'tcp://127.0.0.1:1', members = [self, 'tcp://127.0.0.1:2', 'tcp://127.0.0.1:3'].join(',');
  const server = await start(dir, ['--create', '--cluster', members, '--cluster-self', self, '--cluster-token-file', join(dir, 'token'), '--election-timeout', '300ms']);
  try {
    await sleep(800);
    let r = await get(server.http + '/health/live'); assert.equal(r.status, 200); await r.text();
    r = await get(server.http + '/health/ready'); assert.equal(r.status, 503); assert.match((await r.json()).reason, /no leader/);
    const m = await scrape(server.http);
    assert.equal(value(m, 'kestrodb_cluster_has_leader'), 0);
    assert.equal(value(m, 'kestrodb_cluster_is_leader'), 0);
    assert.equal(value(m, 'kestrodb_cluster_members'), 3);
    assert.equal(value(m, 'kestrodb_ready'), 0);
    assert.ok(value(m, 'kestrodb_cluster_term') >= 0);
  } finally { await server.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('metrics: without --metrics no endpoint and nothing timed; stats report version and uptime', () => withServer(async ({address, log}) => {
  assert.doesNotMatch(log(), /KESTRODB METRICS/);
  const db = await RemoteDB.connect(address, {token: TOKEN});
  try {
    const st = await db.stats();
    assert.match(st.version, /^\d+\.\d+\.\d+/); assert.equal(st.serverRole, 'leader'); assert.ok(st.uptimeMs >= 0); assert.ok(st.connectionId >= 1); assert.equal(st.slowQueryMs, 0);
  } finally { await db.close(); }
}, [], {metrics: []}));
