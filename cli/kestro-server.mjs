#!/usr/bin/env node
// The kestro-server command of the npm package (npx kestro-server --db app.kdb --create --tcp
// 127.0.0.1:7820 --token-file token.txt): runs this platform's server binary with the same
// arguments and exits with its exit code. Under a service manager run node_modules/.bin/kestro-server
// (this launcher) or the binary itself, not npx: npx runs commands through sh -c, which does not
// pass a SIGTERM on, so the server would not get its clean stop.
import {spawn} from 'node:child_process';
import {constants} from 'node:os';
import {nativeFile} from '../native.mjs';

let binary;
try { binary = nativeFile('server'); } catch (e) { console.error(e.message); process.exit(1); }
const child = spawn(binary, process.argv.slice(2), {stdio: 'inherit'});
child.on('error', e => { console.error(`kestro-server: cannot start ${binary}: ${e.message}`); process.exit(1); });
// The server stops cleanly (index snapshot written) on SIGINT/SIGTERM. A terminal's Ctrl+C
// reaches it directly (same process group, or same console on Windows); a SIGTERM sent to this
// launcher alone (systemd, docker, kill) is passed on. Windows has no SIGTERM to pass on, and
// child.kill() there would terminate the server without its clean stop.
for (const signal of ['SIGINT', 'SIGTERM', 'SIGHUP']) {
  process.on(signal, () => { if (process.platform !== 'win32') child.kill(signal); });
}
child.on('exit', (code, signal) => process.exit(code ?? 128 + (constants.signals[signal] || 1)));
