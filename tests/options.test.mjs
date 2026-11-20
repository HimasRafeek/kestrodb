import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp,rm,copyFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {NativeDB} from '../worker/client.mjs';

test('0.4 and 0.6 files and compacted checkpoints retain data and CAS versions',async()=>{
 const dir=await mkdtemp(join(tmpdir(),'kdb-compatible-'));let db;
 try{for(const fixture of ['native-0.4.hdb','native-0.6.hdb','native-0.6-compacted.hdb']){
  const path=join(dir,fixture);await copyFile(new URL('../fixtures/'+fixture,import.meta.url),path);
  db=await NativeDB.open(path);assert.deepEqual(await db.get('items','a'),{version:1,value:{n:1,text:'日本語 தமிழ் 🌿'}});
  assert.equal(await db.get('items','deleted'),null);assert.deepEqual(await db.get('items','empty'),{version:4,value:null});
  assert.equal((await db.set('items','a',{n:2},{expectedVersion:1})).version,5);await db.verify();await db.close();
  db=await NativeDB.open(path);assert.deepEqual(await db.get('items','a'),{version:5,value:{n:2}});await db.close();
 }}finally{await db?.close();await rm(dir,{recursive:true,force:true});}
});

test('auto and explicit read modes with both synchronization policies',async()=>{
 const dir=await mkdtemp(join(tmpdir(),'kdb-options-'));let db;let create=true;
 try{for(const readMode of ['auto','mapped','pread'])for(const orderedSync of [false,true]){
   db=await NativeDB.open(join(dir,'data'),{create,readMode,orderedSync});create=false;
   const stats=await db.stats();assert.equal(stats.readMode,readMode==='auto'?'mapped':readMode);
   assert.equal(stats.commitPolicy,orderedSync?'ordered':'single-barrier');
   await db.set('items','a',{readMode,orderedSync});assert.deepEqual((await db.get('items','a')).value,{readMode,orderedSync});await db.verify();await db.close();
  }
  await assert.rejects(NativeDB.open(join(dir,'data'),{readMode:'invalid'}),/INPUT/);
  await assert.rejects(NativeDB.open(join(dir,'data'),{orderedSync:'false'}),/INPUT/);
 }finally{await db?.close();await rm(dir,{recursive:true,force:true});}
});
