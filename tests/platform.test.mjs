import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,rm,mkdir,link} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {NativeDB} from '../worker/client.mjs';
const suffix=process.platform==='win32'?'.exe':'';
const release=process.env.KESTRO_DB_BINARY||fileURLToPath(new URL('../bin/kestro-worker'+suffix,import.meta.url));
const faults=process.env.KESTRO_FAULT_BINARY||fileURLToPath(new URL('../bin/kestro-worker-faults'+suffix,import.meta.url));
function raw(path,{binary=release,env={}}={}){
 const child=spawn(binary,[path],{stdio:['pipe','pipe','pipe'],env:{...process.env,...env},windowsHide:true});
 let bytes=Buffer.alloc(0),stderr='',failure;const pending=[];
 child.stdout.on('data',b=>{bytes=Buffer.concat([bytes,b]);while(bytes.length>=4&&bytes.length>=bytes.readUInt32LE(0)+4){const n=bytes.readUInt32LE(0),body=bytes.subarray(4,n+4);bytes=bytes.subarray(n+4);const request=pending.shift();if(request)request.resolve(body);}});
 child.stderr.on('data',b=>{stderr=(stderr+b).slice(-4096);});child.stdin.on('error',()=>{});
 const fail=e=>{failure=e;for(const p of pending.splice(0))p.reject(e);};child.on('error',fail);
 const closed=new Promise(resolve=>child.on('close',(code,signal)=>{fail(Error(`worker closed ${code??signal}: ${stderr}`));resolve({code,signal});}));
 return {child,closed,request(cmd,data=Buffer.alloc(0)){if(failure)return Promise.reject(failure);const packet=Buffer.alloc(data.length+5);packet.writeUInt32LE(data.length+1);packet[4]=cmd.charCodeAt(0);data.copy(packet,5);return new Promise((resolve,reject)=>{pending.push({resolve,reject});child.stdin.write(packet);});},async close(){if(!failure)await this.request('Q');child.stdin.end();await closed;}};
}
function batch(key,value){const b=Buffer.alloc(4+17+key.length+value.length);b.writeUInt32LE(1);b[4]=1;b.writeBigUInt64LE(0xffffffffffffffffn,5);b.writeUInt32LE(key.length,13);b.writeUInt32LE(value.length,17);key.copy(b,21);value.copy(b,21+key.length);return b;}
async function seed(path){const db=await NativeDB.open(path,{create:true});try{await db.set('items','ack','safe');}finally{await db.close();}}

test('Unicode, spaces, long paths and invalid Unicode input',async()=>{
 const dir=await mkdtemp(join(tmpdir(),'kdb-paths-'));let db;
 try{const deep=join(dir,...Array.from({length:6},(_,i)=>String(i).repeat(48)));await mkdir(deep,{recursive:true});
  const source=join(deep,'日本語 தமிழ் 🌿 source.kdb'),dest=join(deep,'保存 نسخة 🌿.kdb');
  db=await NativeDB.open(source,{create:true});await db.set('items','unicode',{text:'தமிழ் 日本語 🌿'});await db.compactTo(dest);await db.close();
  db=await NativeDB.open(dest);assert.deepEqual((await db.get('items','unicode')).value,{text:'தமிழ் 日本語 🌿'});
  await assert.rejects(db.backup('\ud800'),/INPUT/);await db.close();
  for(const path of ['', '\ud800','bad\0path'])await assert.rejects(NativeDB.open(path),/INPUT/);
 }finally{await db?.close();await rm(dir,{recursive:true,force:true});}
});

test('binary pipes preserve all 256 byte values, CR, LF and Ctrl-Z',async()=>{
 const dir=await mkdtemp(join(tmpdir(),'kdb-binary-'));let worker;
 try{const p=join(dir,'binary');await seed(p);worker=raw(p);assert.equal((await worker.request('S'))[0],0);
  const key=Buffer.from([0,10,13,26,255]),value=Buffer.from(Array.from({length:65536},(_,i)=>i%256));
  const committed=await worker.request('B',batch(key,value));assert.equal(committed[0],0);
  const got=await worker.request('G',key);assert.equal(got[0],0);assert.deepEqual(got.subarray(9),value);assert.equal((await worker.request('V'))[0],0);
  await worker.close();worker=raw(p);assert.deepEqual((await worker.request('G',key)).subarray(9),value);
 }finally{await worker?.close();await rm(dir,{recursive:true,force:true});}
});

test('process ownership blocks a hardlink alias and releases after forced exit',async()=>{
 const dir=await mkdtemp(join(tmpdir(),'kdb-owner-'));let worker,db;
 try{const p=join(dir,'data'),alias=join(dir,'alias');await seed(p);await link(p,alias);worker=raw(p);assert.equal((await worker.request('S'))[0],0);
  await assert.rejects(NativeDB.open(alias),/LOCKED/);worker.child.kill('SIGKILL');await worker.closed;
  db=await NativeDB.open(alias);assert.equal((await db.get('items','ack')).value,'safe');await db.verify();
 }finally{await worker?.close();await db?.close();await rm(dir,{recursive:true,force:true});}
});

for(const [stage,exit,hasPending] of [['after_data',71,true],['before_sync',75,true],['after_meta',73,true],['torn_meta',72,true]])test('process fault '+stage,async()=>{
 const dir=await mkdtemp(join(tmpdir(),'kdb-fault-'));let worker,db;
 try{const p=join(dir,'data');await seed(p);worker=raw(p,{binary:faults,env:{KESTRO_FAULT:stage}});assert.equal((await worker.request('S'))[0],0);
  await assert.rejects(worker.request('B',batch(Buffer.from('items\0pending'),Buffer.from('"maybe"'))));assert.equal((await worker.closed).code,exit);
  if(hasPending===null)await assert.rejects(NativeDB.open(p),/CORRUPT/);
  else{db=await NativeDB.open(p);assert.equal((await db.get('items','ack')).value,'safe');assert.equal(!!await db.get('items','pending'),hasPending);await db.verify();}
 }finally{await worker?.close();await db?.close();await rm(dir,{recursive:true,force:true});}
});
for(const stage of ['sync_error','final_sync_error'])test('synchronization failure poisons connection: '+stage,async()=>{
 const dir=await mkdtemp(join(tmpdir(),'kdb-sync-'));let worker,db;
 try{const p=join(dir,'data');await seed(p);worker=raw(p,{binary:faults,env:{KESTRO_FAULT:stage}});assert.equal((await worker.request('S'))[0],0);
  assert.equal((await worker.request('B',batch(Buffer.from('items\0pending'),Buffer.from('"maybe"'))))[0],255);
  const response=await worker.request('G',Buffer.from('items\0ack'));assert.equal(response[0],255);assert.match(response.toString('utf8',1),/UNKNOWN_COMMIT/);await worker.close();
  db=await NativeDB.open(p);assert.equal((await db.get('items','ack')).value,'safe');assert.equal(!!await db.get('items','pending'),true);  // its group reached the file: committed, though unacknowledgedawait db.verify();
 }finally{await worker?.close();await db?.close();await rm(dir,{recursive:true,force:true});}
});
test('release binary ignores fault-injection environment',async()=>{
 const dir=await mkdtemp(join(tmpdir(),'kdb-release-'));let worker;
 try{const p=join(dir,'data');await seed(p);worker=raw(p,{env:{KESTRO_FAULT:'after_data'}});assert.equal((await worker.request('S'))[0],0);assert.equal((await worker.request('B',batch(Buffer.from('key'),Buffer.from('value'))))[0],0);assert.equal((await worker.request('V'))[0],0);
 }finally{await worker?.close();await rm(dir,{recursive:true,force:true});}
});
test('EOF closes worker with success',async()=>{
 const dir=await mkdtemp(join(tmpdir(),'kdb-eof-'));
 try{const p=join(dir,'data');await seed(p);const worker=raw(p);assert.equal((await worker.request('S'))[0],0);worker.child.stdin.end();assert.equal((await worker.closed).code,0);}finally{await rm(dir,{recursive:true,force:true});}
});
