// Server lifecycle at the edges: a stop with clients that keep their connection open, and
// BUSY at the connection limit reaching a client that has already sent its login.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import net from 'node:net';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const TOKEN = 'lifecycle-token-0123456789abcdef';

async function start(dir, args = []) {
  await writeFile(join(dir, 'token'), TOKEN);
  const child = spawn(serverBinary, ['--db', join(dir, 'l.kdb'), '--create', '--tcp', '127.0.0.1:0', '--token-file', join(dir, 'token'), '--stop-on-stdin-eof', ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  const exited = new Promise(r => child.on('exit', r));
  const address = await new Promise((resolve, reject) => {
    let out = ''; child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve(m[1]); });
    child.on('exit', code => reject(Error('server exited ' + code)));
  });
  return {child, exited, address};
}
const frame = (cmd, payload) => { const p = Buffer.from(payload); const b = Buffer.alloc(5 + p.length); b.writeUInt32LE(1 + p.length); b[4] = cmd.charCodeAt(0); p.copy(b, 5); return b; };

test('a stop does not wait for clients that keep their connection open', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-life-'));
  const s = await start(dir);
  try {
    const [host, port] = s.address.split(':');
    // Logged in and then silent, ignoring the server's close (as some clients do).
    const quiet = [];
    for (let i = 0; i < 3; i++) {
      const sock = net.connect({host, port: Number(port), allowHalfOpen: true}); sock.on('error', () => {});
      await new Promise(r => sock.on('connect', r)); sock.write(frame('A', TOKEN));
      await new Promise(r => sock.once('data', r)); quiet.push(sock);
    }
    const t0 = Date.now(); s.child.stdin.end(); await s.exited;
    assert.ok(Date.now() - t0 < 10000, `stopped in ${Date.now() - t0} ms`);
    for (const q of quiet) q.destroy();
  } finally { s.child.kill(); await rm(dir, {recursive: true, force: true}); }
});

test('BUSY at the connection limit reaches a client that already sent its login', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-life-'));
  const s = await start(dir, ['--max-connections', '1']);
  try {
    const holder = await RemoteDB.connect('tcp://' + s.address, {token: TOKEN, reconnect: false});
    const [host, port] = s.address.split(':');
    for (let i = 0; i < 30; i++) {
      const got = await new Promise(resolve => {
        const sock = net.connect({host, port: Number(port)}); let data = Buffer.alloc(0);
        sock.on('connect', () => sock.write(frame('A', TOKEN)));  // the login, sent at once
        sock.on('data', d => { data = Buffer.concat([data, d]); });
        sock.on('error', e => resolve('error ' + e.code));
        sock.on('close', () => resolve(data.length > 5 ? data.toString('latin1', 5) : 'closed with nothing'));
      });
      assert.match(got, /^BUSY too many connections/, `attempt ${i + 1}`);
    }
    await holder.close();
  } finally { s.child.stdin.end(); await s.exited; await rm(dir, {recursive: true, force: true}); }
});
