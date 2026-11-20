// Run only when regenerating compatibility fixtures with authentic older workers.
import {NativeDB} from '../worker/client.mjs';
import {resolve} from 'node:path';
import {writeFile} from 'node:fs/promises';
const sources=process.argv.slice(2);if(sources.length!==2)throw Error('Pass 0.4 and 0.6 worker paths');
const metadata=[];
for(let i=0;i<2;i++){
 const version=i?'0.6':'0.4',binary=resolve(sources[i]);
 const path=resolve(`fixtures/native-${version}.kdb`),db=await NativeDB.open(path,{create:true,binary});
 await db.set('items','a',{n:1,text:'日本語 தமிழ் 🌿'});await db.set('items','deleted',{ok:true});await db.delete('items','deleted');await db.set('items','empty',null);await db.verify();
 if(i)await db.compactTo(resolve('fixtures/native-0.6-compacted.hdb'));
 metadata.push({version,stats:await db.stats()});await db.close();
}
await writeFile('fixtures/provenance.json',JSON.stringify(metadata,null,2)+'\n');
