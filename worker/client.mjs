import {spawn} from 'node:child_process';
import {nativeFile} from '../native.mjs';
import {name,key,JSON_MODES} from '../validation.mjs';
import {MAX,packet,encodeBatch,encodeKeys,decodeValues} from '../protocol.mjs';
/** Async Node adapter to an isolated native process. Explicit batch transactions. */
export class NativeDB {
 #child;#pending=[];#bytes=0;#error;#closing=false;#exit;#json;
 static async open(path,{create=false,readMode='auto',orderedSync=false,jsonValidation='strict',verifyOnOpen=false,binary=process.env.KESTRO_DB_BINARY || nativeFile('worker')}={}){
  if(typeof path!=='string'||!path.length||!path.isWellFormed()||path.includes('\0'))throw Error('INPUT path');
  if(!['auto','mapped','pread'].includes(readMode)||typeof orderedSync!=='boolean')throw Error('INPUT readMode/orderedSync');
  if(!Object.hasOwn(JSON_MODES,jsonValidation))throw Error('INPUT jsonValidation');
  const db=new NativeDB();db.#json=JSON_MODES[jsonValidation];db.#child=spawn(binary,[path,...(create?['--create']:[]),...(readMode==='pread'?['--pread']:readMode==='mapped'?['--mapped']:[]),...(orderedSync?['--ordered-sync']:[]),...(verifyOnOpen===true?['--verify-on-open']:[])],{stdio:['pipe','pipe','pipe'],windowsHide:true});
  let stderr='';db.#child.stderr.on('data',b=>{stderr=(stderr+b).slice(-4096);});
  let header=Buffer.alloc(4),headPos=0,body=null,bodyPos=0;
  const deliver=frame=>{const item=db.#pending.shift();if(!item)throw Error('Unexpected response');db.#bytes-=item.bytes;if(frame[0]===255)item.reject(Error(frame.toString('utf8',1)));else item.resolve(frame);};
  db.#child.stdout.on('data',chunk=>{
   try{let pos=0;while(pos<chunk.length){
    // Fast path: a whole response inside this chunk is used in place, without a copy.
    if(!body&&headPos===0&&chunk.length-pos>=4){const len=chunk.readUInt32LE(pos);if(!len||len>MAX)throw Error('Invalid worker response length');if(chunk.length-pos-4>=len){deliver(chunk.subarray(pos+4,pos+4+len));pos+=4+len;continue;}}
    if(!body){const n=Math.min(4-headPos,chunk.length-pos);chunk.copy(header,headPos,pos,pos+n);headPos+=n;pos+=n;if(headPos<4)continue;const len=header.readUInt32LE();if(!len||len>MAX)throw Error('Invalid worker response length');body=Buffer.allocUnsafe(len);bodyPos=0;}
    const n=Math.min(body.length-bodyPos,chunk.length-pos);chunk.copy(body,bodyPos,pos,pos+n);bodyPos+=n;pos+=n;
    if(bodyPos===body.length){const frame=body;body=null;headPos=0;deliver(frame);}
   }}catch(e){db.#fail(e);db.#child.kill();}
  });
  db.#child.stdin.on('error',e=>db.#fail(e));db.#child.on('error',e=>db.#fail(e));
  db.#exit=new Promise(resolve=>db.#child.on('close',(code,signal)=>{db.#fail(Error(`Worker closed (${code??signal}): ${stderr}. A write without a response has an unknown outcome.`));resolve();}));
  try{await db.stats();return db;}catch(e){db.#child.kill();throw e;}
 }
 #fail(e){this.#error=e;for(const p of this.#pending)p.reject(e);this.#pending=[];this.#bytes=0;}
 #corked=false;
 #call(cmd,data=Buffer.alloc(0)){const p=packet(cmd,data.length);data.copy(p,5);return this.#send(cmd,p);}
 #send(cmd,packet){
  if(this.#error)return Promise.reject(this.#error);
  if(this.#closing&&cmd!=='Q')return Promise.reject(Error('CLOSED'));
  const size=packet.length;
  if(size>MAX||this.#bytes+size>MAX||this.#pending.length>=4096)return Promise.reject(Error('BUSY request queue limit'));
  // Requests issued in the same tick reach the worker as one pipe write, so it can
  // answer them together and group-commit consecutive writes under one flush.
  const stdin=this.#child.stdin;
  if(!this.#corked){this.#corked=true;stdin.cork();process.nextTick(()=>{this.#corked=false;stdin.uncork();});}
  return new Promise((resolve,reject)=>{this.#pending.push({resolve,reject,bytes:size});this.#bytes+=size;stdin.write(packet);});
 }
 async batch(operations){
  const b=encodeBatch(operations,this.#json);
  const result=await this.#send('B',b);const v=result.readBigUInt64LE(1);if(v>BigInt(Number.MAX_SAFE_INTEGER))throw Error('Version exceeds Node safe integer range');return {version:Number(v),operations:operations.length};
 }
 set(collection,key,value,options={}){return this.batch([{type:'set',collection,key,value,...options}]);}
 delete(collection,key,options={}){return this.batch([{type:'delete',collection,key,...options}]);}
 async get(collection,id){name(collection);key(id);const k=collection+'\0'+id,p=packet('G',Buffer.byteLength(k));p.write(k,5,'utf8');const r=await this.#send('G',p);if(r[0]===1)return null;const version=r.readBigUInt64LE(1);if(version>BigInt(Number.MAX_SAFE_INTEGER))throw Error('Version exceeds Node safe integer range');return {version:Number(version),value:JSON.parse(r.toString('utf8',9))};}
 async stats(){return JSON.parse((await this.#call('S')).toString('utf8',1));}
 async getMany(collection,ids){
  const r=await this.#send('M',encodeKeys(collection,ids));return decodeValues(r,ids.length);
 }
 async verify(){await this.#call('V');return {ok:true};}
 async backup(path){await this.#path('P',path);return {path};}
 async compactTo(path){await this.#path('C',path);return {path};}
 #path(cmd,path){if(typeof path!=='string'||!path.length||!path.isWellFormed()||path.includes('\0'))return Promise.reject(Error('INPUT path'));return this.#call(cmd,Buffer.from(path));}
 async close(){if(this.#closing){await this.#exit;return;}this.#closing=true;try{if(!this.#error)await this.#call('Q');}finally{this.#child.stdin.end();await this.#exit;}}
}
