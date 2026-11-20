// Validate the supplied binaries without needing a compiler or CMake.
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {readdirSync} from 'node:fs';
import {resolve} from 'node:path';
const root=fileURLToPath(new URL('../',import.meta.url));
function run(cmd,args=[]){const r=spawnSync(cmd,args,{cwd:root,stdio:'inherit',timeout:180000});if(r.error)throw r.error;if(r.status!==0)process.exit(r.status||1);}
run(resolve(root,'bin/kestro-portable-tests'+(process.platform==='win32'?'.exe':'')));
if(process.platform!=='win32')run(resolve(root,'bin/kestro-posix-tests'));
run(process.execPath,['--test',...readdirSync(resolve(root,'tests')).filter(f=>f.endsWith('.test.mjs')).sort().map(f=>'tests/'+f)]);
