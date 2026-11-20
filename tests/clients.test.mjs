// Client libraries against a real kestro-server: the PHP and Python SQL APIs (prepared
// statements, named parameters, transactions, error classes, embedded mode) and
// reconnecting after a server restart.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import fs from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const TOKEN = 'client-token-0123456789';
const library = join(root, 'bin', process.platform === 'win32' ? 'kestrodb.dll' : process.platform === 'darwin' ? 'libkestrodb.dylib' : 'libkestrodb.so');
const haveLibrary = fs.existsSync(library);

function findTool(envName, candidates) {
  for (const c of [process.env[envName], ...candidates].filter(Boolean)) {
    const r = spawnSync(c, ['--version'], {encoding: 'utf8'});
    if (r.status === 0 && !/not found|Microsoft Store/i.test(r.stdout + r.stderr)) return c;
  }
  return null;
}
const php = findTool('KESTRO_PHP', ['php']);
const phpFfi = php && spawnSync(php, ['-d', 'extension=ffi', '-r', 'exit(extension_loaded("ffi") ? 0 : 1);']).status === 0;
const python = findTool('KESTRO_PYTHON', process.platform === 'win32' ? ['py', 'python'] : ['python3', 'python']);

async function startServer(dir, {port = 0, create = true} = {}) {
  const tokenFile = join(dir, 'token');
  await writeFile(tokenFile, TOKEN + '\n');
  const child = spawn(serverBinary, ['--db', join(dir, 'shared.kdb'), ...(create ? ['--create'] : []), '--tcp', `127.0.0.1:${port}`, '--stop-on-stdin-eof', '--token-file', tokenFile], {stdio: ['pipe', 'pipe', 'pipe']});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  const address = await new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${err}`)));
  });
  const exited = new Promise(resolve => child.on('exit', resolve));
  return {address, port: Number(address.split(':').pop()), stop: async () => { child.stdin.end(); await exited; }};
}
function run(cmd, args, {input} = {}) {
  return new Promise(resolve => {
    const child = spawn(cmd, args, {stdio: ['pipe', 'pipe', 'pipe'], env: {...process.env, KESTRODB_LIBRARY: library}});
    let out = '', err = '';
    child.stdout.on('data', d => { out += d; });
    child.stderr.on('data', d => { err += d; });
    child.on('exit', code => resolve({code, out, err}));
    if (input !== undefined) child.stdin.end(input);
  });
}
const phpArgs = phpFfi ? ['-d', 'extension=ffi'] : [];

test('PHP: SQL API on the server and embedded (named/numbered parameters, prepared statements, transactions, errors, persistent connections)', {skip: php ? false : 'PHP not found (set KESTRO_PHP)'}, async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-php-'));
  const srv = await startServer(dir);
  try {
    const args = [...phpArgs, join(root, 'clients/php/test_api.php'), 'api', srv.address, TOKEN];
    if (phpFfi && haveLibrary) args.push(join(dir, 'embedded.kdb'));
    const r = await run(php, args);
    assert.equal(r.code, 0, r.out + r.err);
    assert.match(r.out, /ok php api/);
  } finally { await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('PHP: an idle client reconnects after the server restarts', {skip: php ? false : 'PHP not found (set KESTRO_PHP)'}, async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-php-rc-'));
  let srv = await startServer(dir);
  try {
    const child = spawn(php, [join(root, 'clients/php/test_api.php'), 'reconnect', srv.address, TOKEN], {stdio: ['pipe', 'pipe', 'pipe']});
    let out = '', err = '';
    child.stderr.on('data', d => { err += d; });
    await new Promise(resolve => child.stdout.on('data', d => { out += d; if (out.includes('READY')) resolve(); }));
    const port = srv.port;
    await srv.stop();
    srv = await startServer(dir, {port, create: false});
    child.stdin.end('go\n');
    const code = await new Promise(resolve => child.on('exit', resolve));
    assert.equal(code, 0, out + err);
    assert.match(out, /ok php reconnect/);
  } finally { await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('Python: DB-API on the server and embedded (cursors, parameters, transactions, exceptions)', {skip: python ? false : 'Python not found (set KESTRO_PYTHON)'}, async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-py-'));
  const srv = await startServer(dir);
  try {
    const args = [join(root, 'clients/python/test_dbapi.py'), srv.address, TOKEN];
    if (haveLibrary) args.push(join(dir, 'embedded.kdb'));
    const r = await run(python, args);
    assert.equal(r.code, 0, r.out + r.err);
    assert.match(r.out, /ok python dbapi/);
  } finally { await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});
