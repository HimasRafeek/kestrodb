// Release packages, made from Release builds. Nothing is published: everything lands in dist/.
//   node scripts/package.mjs [target...] [--bin-<platform> DIR]... [--docs DIR]
// Targets (default: the archive for this machine's platform):
//   windows-x64, linux-x64, linux-arm64   release archive: dist/kestrodb-<version>-<platform>.zip
//                                         (Windows) or .tar.gz (Linux), with SHA256SUMS.txt inside
//                                         and <archive>.sha256 beside it
//   npm                                   dist/kestrodb-<version>.tgz (npm pack): the Node package with
//                                         prebuilds/<process.platform>-<process.arch>/ for every platform
//   wheel-<platform>                      dist/kestrodb-<version>-py3-none-<tag>.whl: the Python client with
//                                         the C library inside (python -m build, setuptools from PyPI)
//   composer                              dist/kestrodb-php-<version>.zip: the PHP client with the C library
//                                         of every platform (a Composer "artifact" repository holds it)
//   all                                   every target whose binaries are there
//   sums                                  only rewrite dist/SHA256SUMS (every run does it at the end)
// Binaries: --bin-<platform> DIR; by default this machine's platform uses ./bin (npm run build) and
// the others dist/build/<platform>/bin (scripts/build-linux.sh). Linux archives and the npm
// package carry executable file modes, so make them on Linux (WSL on Windows).
// The layout of an archive matches the repository, so every client finds the native files where it
// looks for them by default (Node: ./bin/kestro.node; PHP and Python: ../../bin/<library>).
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {chmodSync, cpSync, existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync} from 'node:fs';
import {dirname, join, relative, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const dist = join(root, 'dist');
const pkg = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'));
const version = pkg.version;
const fail = m => { console.error('package: ' + m); process.exit(1); };

// Python wheels say which glibc they need (PEP 600); scripts/build-linux.sh checks the binaries
// need no more than this.
const GLIBC = '2_34';
const PLATFORMS = {
  'windows-x64': {node: 'win32-x64', exe: '.exe', lib: 'kestrodb.dll', wheel: 'win_amd64', machine: 'pe-x64'},
  'linux-x64': {node: 'linux-x64', exe: '', lib: 'libkestrodb.so', wheel: `manylinux_${GLIBC}_x86_64`, machine: 'elf-x64'},
  'linux-arm64': {node: 'linux-arm64', exe: '', lib: 'libkestrodb.so', wheel: `manylinux_${GLIBC}_aarch64`, machine: 'elf-arm64'},
};
const hostPlatform = {'win32-x64': 'windows-x64', 'linux-x64': 'linux-x64', 'linux-arm64': 'linux-arm64'}[`${process.platform}-${process.arch}`];
const DOCS = ['README.md', 'docs/GUIDE.md', 'SQL.md', 'CLIENTS.md', 'FORMAT.md', 'CHANGELOG.md'];
// The Node API: the npm package and every archive carry these.
const NODE_FILES = ['index.mjs', 'native.mjs', 'remote.mjs', 'protocol.mjs', 'sqlapi.mjs', 'bulk.mjs', 'validation.mjs',
  'worker/client.mjs', 'cli/kestro-server.mjs', 'tools/viewer/server.mjs'];
// Every package (archives, npm, wheels, Composer) carries the license and NOTICE.
const LEGAL = ['LICENSE', 'NOTICE', 'THIRD-PARTY.md'];

// Arguments.
const args = process.argv.slice(2), targets = [], binDirs = {};
let docsDir = process.env.KESTRO_DOCS_DIR || root;
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a.startsWith('--bin-')) binDirs[a.slice(6)] = resolve(args[++i] ?? fail(`${a} needs a directory`));
  else if (a === '--docs') docsDir = resolve(args[++i] ?? fail('--docs needs a directory'));
  else if (a.startsWith('--')) fail(`unknown option ${a}`);
  else targets.push(a);
}
for (const p of Object.keys(binDirs)) if (!PLATFORMS[p]) fail(`unknown platform in --bin-${p}`);
const binaries = p => { const {exe, lib} = PLATFORMS[p]; return ['kestro.node', 'kestro-server' + exe, 'kestro-worker' + exe, lib]; };
const complete = (p, dir) => binaries(p).every(f => existsSync(join(dir, f)));
const binDir = p => binDirs[p] ?? (p === hostPlatform && complete(p, join(root, 'bin')) ? join(root, 'bin') : join(dist, 'build', p, 'bin'));
const haveBinaries = p => complete(p, binDir(p));
if (!targets.length) targets.push(hostPlatform ?? fail(`no default target on ${process.platform}-${process.arch}`));
if (targets.includes('all')) {
  targets.splice(targets.indexOf('all'), 1);
  for (const p of Object.keys(PLATFORMS)) if (haveBinaries(p)) targets.push(p, 'wheel-' + p);
  targets.push('npm', 'composer');
}

// Checks a platform's binaries: all there, this version, and built for that machine.
const checked = new Set();
function checkBinaries(p) {
  if (checked.has(p)) return;
  const dir = binDir(p), want = PLATFORMS[p].machine;
  for (const f of binaries(p)) {
    const file = join(dir, f);
    if (!existsSync(file)) fail(`missing ${file} (${p === hostPlatform ? 'npm run build' : p.startsWith('linux') ? 'scripts/build-linux.sh' : 'a Release build on that platform'}, or --bin-${p} DIR)`);
    const b = readFileSync(file);
    let machine = 'unknown';
    if (b[0] === 0x7f && b.toString('latin1', 1, 4) === 'ELF') machine = {0x3e: 'elf-x64', 0xb7: 'elf-arm64'}[b.readUInt16LE(18)] ?? machine;
    else if (b.toString('latin1', 0, 2) === 'MZ') { const pe = b.readUInt32LE(0x3c); machine = {0x8664: 'pe-x64', 0xaa64: 'pe-arm64'}[b.readUInt16LE(pe + 4)] ?? machine; }
    if (machine !== want) fail(`${file} is ${machine}, not ${want}`);
  }
  const lib = readFileSync(join(dir, PLATFORMS[p].lib));
  if (!lib.includes(Buffer.from(version + '\0'))) fail(`${join(dir, PLATFORMS[p].lib)} is not a ${version} build`);
  checked.add(p);
}

function copy(from, to) { mkdirSync(dirname(to), {recursive: true}); cpSync(from, to, {recursive: true}); }
function walk(d, out = []) { for (const e of readdirSync(d, {withFileTypes: true})) { const x = join(d, e.name); e.isDirectory() ? walk(x, out) : out.push(x); } return out; }
const sha256 = file => createHash('sha256').update(readFileSync(file)).digest('hex');
// Files copied from a Windows drive into WSL look executable: 0644, and 0755 for programs.
function fixModes(dir, executable) {
  if (process.platform === 'win32') return;
  for (const f of walk(dir)) chmodSync(f, executable(relative(dir, f).replaceAll('\\', '/')) ? 0o755 : 0o644);
}
function docs(out) {
  const missing = [];
  for (const d of DOCS) existsSync(join(docsDir, d)) ? copy(join(docsDir, d), join(out, d)) : missing.push(d);
  if (missing.length) console.warn(`package: warning: no ${missing.join(', ')} in ${docsDir} (--docs DIR or KESTRO_DOCS_DIR)`);
}
// package.json for a package: no build or test scripts.
function packageJson(out) {
  const p = structuredClone(pkg);
  p.scripts = {viewer: pkg.scripts.viewer};
  writeFileSync(join(out, 'package.json'), JSON.stringify(p, null, 2) + '\n');
}
function run(cmd, cmdArgs, options = {}) {
  const r = spawnSync(cmd, cmdArgs, {stdio: 'inherit', ...options});
  if (r.error || r.status !== 0) fail(`${cmd} ${cmdArgs.join(' ')} failed${r.error ? ': ' + r.error.message : ''}`);
}
function needLinuxModes(what) {
  if (process.platform === 'win32') fail(`make ${what} on Linux (WSL): it carries Linux file modes, which Windows tools drop`);
}
function done(file) {
  const sum = sha256(file);
  writeFileSync(file + '.sha256', `${sum}  ${relative(dirname(file), file)}\n`);
  console.log(`${relative(root, file).replaceAll('\\', '/')}: ${(statSync(file).size / 1048576).toFixed(1)} MB, sha256 ${sum}`);
}

function archive(p) {
  checkBinaries(p);
  const linux = p.startsWith('linux');
  if (linux) needLinuxModes(`the ${p} archive`);
  const name = `kestrodb-${version}-${p}`, out = join(dist, name);
  rmSync(out, {recursive: true, force: true});
  for (const f of binaries(p)) copy(join(binDir(p), f), join(out, 'bin', f));
  for (const f of [...NODE_FILES, ...LEGAL, 'clients/php/KestroDB.php', 'clients/php/composer.json', 'clients/python/kestrodb.py']) copy(join(root, f), join(out, f));
  copy(join(root, 'third-party-notices'), join(out, 'third-party-notices'));
  copy(join(root, 'src/kestrodb.h'), join(out, 'include/kestrodb.h'));
  if (linux) for (const f of ['packaging/install.sh', 'packaging/systemd/kestrodb.service']) copy(join(root, f), join(out, f));
  else copy(join(root, 'packaging/windows/README.md'), join(out, 'packaging/windows/README.md'));
  docs(out);
  packageJson(out);
  fixModes(out, f => f.startsWith('bin/kestro-') || f.endsWith('.sh'));
  const files = walk(out);
  const sums = files.map(f => `${sha256(f)}  ${relative(out, f).replaceAll('\\', '/')}`).sort((a, b) => a.slice(66).localeCompare(b.slice(66)));
  writeFileSync(join(out, 'SHA256SUMS.txt'), sums.join('\n') + '\n');
  if (!linux) {
    const zip = out + '.zip';
    rmSync(zip, {force: true});
    // bsdtar from Windows writes the zip (Git's GNU tar on PATH cannot); elsewhere Python's zipfile.
    if (process.platform === 'win32') run(join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tar.exe'), ['-a', '-c', '-f', zip, '-C', dist, name]);
    else run('python3', ['-m', 'zipfile', '-c', zip, out]);
    done(zip);
  } else {
    const tgz = out + '.tar.gz';
    rmSync(tgz, {force: true});
    run('tar', ['-C', dist, '--owner=0', '--group=0', '--numeric-owner', '--sort=name', '-czf', tgz, name]);
    done(tgz);
  }
}

function npmPackage() {
  const platforms = Object.keys(PLATFORMS).filter(haveBinaries);
  if (!platforms.length) fail('npm: no binaries for any platform');
  if (platforms.some(p => p.startsWith('linux'))) needLinuxModes('the npm package');
  const out = join(dist, 'npm', 'kestrodb');
  rmSync(out, {recursive: true, force: true});
  for (const f of [...NODE_FILES, ...LEGAL]) copy(join(root, f), join(out, f));
  copy(join(root, 'third-party-notices'), join(out, 'third-party-notices'));
  for (const p of platforms) {
    checkBinaries(p);
    for (const f of binaries(p)) copy(join(binDir(p), f), join(out, 'prebuilds', PLATFORMS[p].node, f));
  }
  docs(out);
  packageJson(out);
  fixModes(out, f => f.startsWith('prebuilds/') && /\/kestro-(server|worker)$/.test(f) || f === 'cli/kestro-server.mjs');
  const tgz = join(dist, `kestrodb-${version}.tgz`);
  rmSync(tgz, {force: true});
  run(process.platform === 'win32' ? 'npm.cmd' : 'npm', ['pack', '--pack-destination', dist], {cwd: out, shell: process.platform === 'win32'});
  console.log(`npm: prebuilds for ${platforms.map(p => PLATFORMS[p].node).join(', ')}`);
  done(tgz);
}

function wheel(p) {
  checkBinaries(p);
  const out = join(dist, 'python', p);
  rmSync(out, {recursive: true, force: true});
  for (const f of ['kestrodb.py', 'pyproject.toml']) copy(join(root, 'clients/python', f), join(out, f));
  for (const f of ['LICENSE', 'NOTICE']) copy(join(root, f), join(out, f));  // pyproject.toml license-files
  copy(join(binDir(p), PLATFORMS[p].lib), join(out, 'kestrodb_native', PLATFORMS[p].lib));
  writeFileSync(join(out, 'kestrodb_native', '__init__.py'), '"""The KestroDB C library this wheel bundles (kestrodb.py loads it)."""\n');
  copy(join(root, 'THIRD-PARTY.md'), join(out, 'kestrodb_native', 'THIRD-PARTY.md'));
  copy(join(root, 'third-party-notices'), join(out, 'kestrodb_native', 'third-party-notices'));
  // A pure-Python wheel tagged for the platform of the library it carries.
  writeFileSync(join(out, 'setup.cfg'), `[bdist_wheel]\npython_tag = py3\nplat_name = ${PLATFORMS[p].wheel}\n`);
  const pyVersion = readFileSync(join(out, 'pyproject.toml'), 'utf8').match(/^version = "(.*)"/m)?.[1];
  if (pyVersion !== version) fail(`clients/python/pyproject.toml has version ${pyVersion}, package.json ${version}`);
  const python = process.env.PYTHON || (process.platform === 'win32' ? 'python' : 'python3');
  const file = join(dist, `kestrodb-${version}-py3-none-${PLATFORMS[p].wheel}.whl`);
  rmSync(file, {force: true});
  run(python, ['-m', 'build', '--wheel', '--outdir', dist, out]);
  if (!existsSync(file)) fail(`expected ${file}`);
  done(file);
}

function composer() {
  const platforms = Object.keys(PLATFORMS).filter(haveBinaries);
  if (!platforms.length) fail('composer: no binaries for any platform');
  const name = `kestrodb-php-${version}`, out = join(dist, 'php', name);
  rmSync(out, {recursive: true, force: true});
  copy(join(root, 'clients/php/KestroDB.php'), join(out, 'KestroDB.php'));
  for (const f of LEGAL) copy(join(root, f), join(out, f));
  copy(join(root, 'third-party-notices'), join(out, 'third-party-notices'));
  for (const p of platforms) { checkBinaries(p); copy(join(binDir(p), PLATFORMS[p].lib), join(out, 'native', p, PLATFORMS[p].lib)); }
  // An artifact repository reads the version from composer.json (a VCS repository from tags).
  const c = JSON.parse(readFileSync(join(root, 'clients/php/composer.json'), 'utf8'));
  writeFileSync(join(out, 'composer.json'), JSON.stringify({name: c.name, version, ...c}, null, 4) + '\n');
  fixModes(out, () => false);
  const zip = join(dist, name + '.zip');
  rmSync(zip, {force: true});
  if (process.platform === 'win32') run(join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tar.exe'), ['-a', '-c', '-f', zip, '-C', out, '.']);
  else run('python3', ['-c', 'import os,sys,zipfile\nz=zipfile.ZipFile(sys.argv[1],"w",zipfile.ZIP_DEFLATED)\nfor d,_,fs in os.walk(sys.argv[2]):\n for f in fs: p=os.path.join(d,f); z.write(p,os.path.relpath(p,sys.argv[2]))', zip, out]);
  console.log(`composer: native libraries for ${platforms.join(', ')}`);
  done(zip);
}

mkdirSync(dist, {recursive: true});
for (const t of targets) {
  if (PLATFORMS[t]) archive(t);
  else if (t === 'npm') npmPackage();
  else if (t === 'composer') composer();
  else if (t === 'sums') continue;
  else if (t.startsWith('wheel-') && PLATFORMS[t.slice(6)]) wheel(t.slice(6));
  else fail(`unknown target ${t} (${Object.keys(PLATFORMS).join(', ')}, npm, wheel-<platform>, composer, all)`);
}
// dist/SHA256SUMS: every package in dist/, whichever run made it.
const sums = readdirSync(dist).filter(f => f.endsWith('.sha256')).map(f => readFileSync(join(dist, f), 'utf8').trim()).sort((a, b) => a.slice(66).localeCompare(b.slice(66)));
writeFileSync(join(dist, 'SHA256SUMS'), sums.join('\n') + '\n');
