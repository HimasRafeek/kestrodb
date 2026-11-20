// Builds the Windows x64 release package from an existing Release build (npm run build):
//   dist/kestrodb-<version>-windows-x64/   and   dist/kestrodb-<version>-windows-x64.zip
// Kept for the old command; scripts/package.mjs makes every package (Linux, npm, Python, PHP).
//   node scripts/package-windows.mjs
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const script = fileURLToPath(new URL('./package.mjs', import.meta.url));
const r = spawnSync(process.execPath, [script, 'windows-x64', ...process.argv.slice(2)], {stdio: 'inherit'});
process.exit(r.status ?? 1);
