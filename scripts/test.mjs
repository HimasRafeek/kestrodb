import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {readdirSync} from 'node:fs';
import {resolve} from 'node:path';
const root=fileURLToPath(new URL('../',import.meta.url));
function run(cmd,args){const r=spawnSync(cmd,args,{cwd:root,stdio:'inherit'});if(r.error)throw r.error;if(r.status!==0)process.exit(r.status||1);}
run(process.env.CTEST||'ctest',['--test-dir','build','-C','Release','--output-on-failure']);
const files=readdirSync(resolve(root,'tests')).filter(f=>f.endsWith('.test.mjs')).sort().map(f=>'tests/'+f);
run(process.execPath,['--test',...files]);
