import test from 'node:test';import assert from 'node:assert/strict';
import {mkdtemp,rm} from 'node:fs/promises';import {tmpdir} from 'node:os';
import {NativeDB} from '../worker/client.mjs';
test('Node adapter: batch, concurrent requests, CAS, compaction, backup and reopen',async()=>{
 const dir=await mkdtemp(tmpdir()+'/kdb-native-node-');let db;
 try{
  db=await NativeDB.open(dir+'/data',{create:true});
  const receipt=await db.batch(Array.from({length:256},(_,i)=>({type:'set',collection:'items',key:String(i),value:{n:i}})));
  const values=await Promise.all(Array.from({length:256},(_,i)=>db.get('items',String(i))));
  values.forEach((r,i)=>assert.equal(r.value.n,i));
  await db.set('items','0',{n:1},{expectedVersion:receipt.version});
  await assert.rejects(db.set('items','0',{}, {expectedVersion:receipt.version}),/CONFLICT/);
  await assert.rejects(db.set('items','bad', {a:undefined}));
  await db.compactTo(dir+'/compact');await db.backup(dir+'/backup');await db.verify();await db.close();
  db=await NativeDB.open(dir+'/compact');assert.equal((await db.get('items','0')).value.n,1);
  assert.equal((await db.stats()).keys,256);await db.close();
 }finally{await db?.close();await rm(dir,{recursive:true,force:true});}
});
