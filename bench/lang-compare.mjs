// Many application processes on one machine, in PHP or Python: KestroDB vs SQLite vs MySQL.
//
//   node bench/lang-compare.mjs --lang php|python [--workers 1,4,16] [--writes 300]
//        [--reads 20000] [--keys 20000] [--mysql "mysql:host=127.0.0.1;port=3306;dbname=bench"]
//        [--php path] [--python path] [--json out.json]
//
// N processes run at once (PHP-FPM workers, gunicorn workers...). Each does the same work
// against every engine: durable single writes, then random point reads of JSON documents.
//   KestroDB          writes through kestro-server (group commit across processes); reads
//                    in-process from the database file (readFrom / read_from, C library)
//   KestroDB-socket   reads through kestro-server too
//   SQLite           WAL + synchronous=FULL, in-process, busy_timeout for lock waits
//   MySQL            InnoDB, autocommit (PHP only, with --mysql); run it with
//                    innodb_flush_log_at_trx_commit=1 for matched durability
//   KestroDB-embedded one process owns the file (workers = 1 only, Python)
// Throughput = all operations / (last process's end - first process's start), so
// interpreter start-up is not counted. Every engine stores JSON text and decodes it on read.
import {spawn} from 'node:child_process';
import {mkdtempSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir, cpus} from 'node:os';
import {join, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? process.argv[i + 1] : d; };
const LANG = arg('lang', 'php');
const WORKERS = arg('workers', '1,4,16').split(',').map(Number);
const WRITES = Number(arg('writes', 300)), READS = Number(arg('reads', 20000)), KEYS = Number(arg('keys', 20000));
const MYSQL = LANG === 'php' ? arg('mysql', process.env.KESTRO_MYSQL_DSN) : null;
const root = fileURLToPath(new URL('../', import.meta.url));
const server = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
let cmd, pre, worker;
if (LANG === 'php') {
  cmd = arg('php', process.env.KESTRO_PHP || 'php');
  worker = join(root, 'bench', 'php-worker.php');
  // The PDO drivers and FFI may be built but not enabled in php.ini.
  // Windows builds (e.g. Local) ship the drivers without enabling them; Linux packages enable their own.
  pre = process.platform === 'win32' ? ['-d', 'extension_dir=' + join(dirname(cmd), 'ext'), '-d', 'extension=pdo_sqlite', '-d', 'extension=ffi', ...(MYSQL ? ['-d', 'extension=pdo_mysql'] : [])] : [];
} else if (LANG === 'python') {
  cmd = arg('python', process.env.KESTRO_PYTHON || (process.platform === 'win32' ? 'py' : 'python3'));
  worker = join(root, 'bench', 'py-worker.py');
  pre = [];
} else throw Error('--lang php|python');

function run(args) {
  return new Promise((resolve, reject) => {
    const c = spawn(cmd, args, {stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true});
    let out = '', err = '';
    c.stdout.on('data', d => { out += d; }); c.stderr.on('data', d => { err += d; });
    c.on('close', code => code === 0 ? resolve(JSON.parse(out.trim().split('\n').pop())) : reject(Error(`${cmd} exited ${code}: ${err}${out}`)));
  });
}
async function phase(engine, target, mode, n, count) {
  const results = await Promise.all(Array.from({length: n}, (_, w) => run([...pre, worker, engine, target, '-', mode, String(w), String(count), String(KEYS)])));
  const start = Math.min(...results.map(r => r.start)), end = Math.max(...results.map(r => r.end));
  return results.reduce((s, r) => s + r.ops, 0) / (end - start);
}
async function startServer(dir) {
  const child = spawn(server, ['--db', join(dir, 'h.kdb'), '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof'], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '';
  const address = await new Promise((resolve, reject) => { child.stdout.on('data', d => { out += d; const m = /READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); }); child.on('exit', c => reject(Error('server exited ' + c))); });
  return {address, stop: () => new Promise(r => { child.on('exit', r); child.stdin.end(); })};
}

const fmt = n => n >= 1e3 ? (n / 1e3).toFixed(1) + 'K' : n.toFixed(0);
const rows = [];
for (const n of WORKERS) {
  const dir = mkdtempSync(join(tmpdir(), 'kestro-lang-bench-'));
  try {
    const srv = await startServer(dir);
    const engines = [['KestroDB', 'kestrodb', srv.address + '|' + join(dir, 'h.kdb')], ['KestroDB-socket', 'kestrodb', srv.address],
      ['SQLite', 'sqlite', join(dir, 's.sqlite')], ...(MYSQL ? [['MySQL', 'mysql', MYSQL]] : []),
      ...(LANG === 'python' && n === 1 ? [['KestroDB-embedded', 'embedded', join(dir, 'e.kdb')]] : [])];
    const row = {workers: n};
    for (const [name, engine, target] of engines) {
      await phase(engine, target, 'seed', 1, 0);
      row[name] = {writes: await phase(engine, target, 'write', n, WRITES), reads: await phase(engine, target, 'read', n, READS)};
    }
    await srv.stop();
    rows.push(row);
    const line = k => engines.map(([name]) => `${name} ${fmt(row[name][k])}/s`).join('  ');
    console.log(`${String(n).padStart(3)} ${LANG} processes | durable writes: ${line('writes')}`);
    console.log(`${' '.repeat(14 + LANG.length)}| point reads:    ${line('reads')}`);
  } finally { rmSync(dir, {recursive: true, force: true}); }
}
const outFile = arg('json');
if (outFile) writeFileSync(outFile, JSON.stringify({date: new Date().toISOString(), lang: LANG, platform: process.platform, cpu: cpus()[0].model.trim(), cores: cpus().length, writesPerProcess: WRITES, readsPerProcess: READS, keys: KEYS, mysql: !!MYSQL, rows}, null, 2) + '\n');
