// Where KestroDB's native files are. A development checkout has them in ./bin/ (npm run build);
// the npm package ships prebuilt ones in prebuilds/<platform>-<arch>/ (process.platform and
// process.arch: win32-x64, linux-x64, linux-arm64). ./bin/ wins when both exist.
import {chmodSync, existsSync, readdirSync, statSync} from 'node:fs';
import {fileURLToPath} from 'node:url';

const exe = process.platform === 'win32' ? '.exe' : '';
const FILES = {
  addon: 'kestro.node',
  server: 'kestro-server' + exe,
  worker: 'kestro-worker' + exe,
  library: process.platform === 'win32' ? 'kestrodb.dll' : process.platform === 'darwin' ? 'libkestrodb.dylib' : 'libkestrodb.so',
};
export const platformId = `${process.platform}-${process.arch}`;
const here = p => fileURLToPath(new URL(p, import.meta.url));

/** The platforms this package has prebuilt binaries for (empty in a development checkout). */
export function prebuiltPlatforms() {
  try { return readdirSync(here('./prebuilds/'), {withFileTypes: true}).filter(e => e.isDirectory()).map(e => e.name).sort(); } catch { return []; }
}

/** Path of a native file: 'addon' (kestro.node), 'server', 'worker' or 'library' (the C library). */
export function nativeFile(kind) {
  const name = FILES[kind];
  if (!name) throw new Error(`unknown native file ${kind}`);
  const dev = here('./bin/' + name);
  if (existsSync(dev)) return dev;
  const prebuilt = here(`./prebuilds/${platformId}/${name}`);
  if (existsSync(prebuilt)) {
    // A package unpacked without file modes (some tools drop them) still runs its programs.
    if (!exe && (kind === 'server' || kind === 'worker')) {
      try { const m = statSync(prebuilt).mode; if (!(m & 0o100)) chmodSync(prebuilt, m | 0o755); } catch {}
    }
    return prebuilt;
  }
  const have = prebuiltPlatforms();
  throw new Error(`KestroDB: no ${name} for ${platformId}. ` + (have.length
    ? `This package has prebuilt binaries for ${have.join(', ')}; on other platforms build from source (git clone, then npm run build: CMake and a C++17 compiler).`
    : `Build it first: npm run build (CMake and a C++17 compiler).`));
}

/** Adds the likely cause to an error loading the prebuilt addon on Linux (glibc too old, or musl). */
export function explainLoadError(error) {
  if (process.platform !== 'linux' || !/GLIBC|ld-linux|No such file|cannot open shared object/.test(String(error?.message))) return error;
  let glibc;
  try { glibc = process.report.getReport().header.glibcVersionRuntime; } catch {}
  error.message += glibc
    ? ` (KestroDB's prebuilt Linux binaries need glibc 2.34 or newer; this system has ${glibc}. Build from source: npm run build)`
    : ' (KestroDB\'s prebuilt Linux binaries need glibc 2.34 or newer; this system has none (musl, for example Alpine). Build from source: npm run build)';
  return error;
}
