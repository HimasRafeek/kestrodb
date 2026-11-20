import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';
const root=fileURLToPath(new URL('../',import.meta.url));
const cmake=process.env.CMAKE||'cmake';
function run(args){const r=spawnSync(cmake,args,{cwd:root,stdio:'inherit'});if(r.error)throw r.error;if(r.status!==0)process.exit(r.status||1);}
run(['-S','.', '-B','build','-DCMAKE_BUILD_TYPE=Release',`-DKESTRO_OUTPUT_DIR=${resolve(root,'bin')}`,...process.argv.slice(2)]);
run(['--build','build','--config','Release','--parallel',process.env.KESTRO_BUILD_JOBS||'2']);
