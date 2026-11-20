import test from 'node:test';import assert from 'node:assert/strict';
import {mkdtemp,rm} from 'node:fs/promises';import {tmpdir} from 'node:os';import {NativeDB} from '../worker/client.mjs';
test('getMany preserves order, duplicates, missing records, JSON and committed versions',async()=>{
 const dir=await mkdtemp(tmpdir()+'/kdb-many-');let db;
 try {db=await NativeDB.open(dir+'/data',{create:true});const r=await db.batch([{type:'set',collection:'items',key:'a',value:{n:1}},{type:'set',collection:'items',key:'b',value:null}]);
 assert.deepEqual(await db.getMany('items',['b','missing','a','a']),[{version:r.version,value:null},null,{version:r.version,value:{n:1}},{version:r.version,value:{n:1}}]);
 assert.deepEqual(await db.getMany('items',[]),[]);await assert.rejects(db.getMany('items',new Array(4097).fill('a')),/count/);await assert.rejects(db.getMany('items',['']),/key/i);
 await db.compactTo(dir+'/copy');await db.close();db=await NativeDB.open(dir+'/copy');assert.equal((await db.getMany('items',['a']))[0].value.n,1);
 }finally{await db?.close();await rm(dir,{recursive:true,force:true});}
});
