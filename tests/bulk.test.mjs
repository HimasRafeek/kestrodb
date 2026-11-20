import test from 'node:test';
import assert from 'node:assert/strict';
import {importBatches} from '../bulk.mjs';
test('stream applies backpressure and stopping leaves unread input untouched',async()=>{
 let produced=0,calls=0;
 async function* source(){for(let i=0;i<100;i++){produced++;yield {key:i};}}
 const db={async batch(rows){calls++;return {version:calls,operations:rows.length};}};
 for await(const receipt of importBatches(db,source(),{batchSize:4})){
  assert.equal(produced,4);assert.equal(receipt.committedOperations,4);break;
 }
 assert.equal(calls,1);assert.equal(produced,4);
});
test('partial completion and failing batch are not silently retried',async()=>{
 let calls=0;const receipts=[];const error=Error('UNKNOWN_COMMIT');
 const db={async batch(rows){if(++calls===2)throw error;return {version:1,operations:rows.length};}};
 await assert.rejects(async()=>{for await(const r of importBatches(db,[1,2,3,4,5],{batchSize:2}))receipts.push(r);},e=>e===error);
 assert.equal(calls,2);assert.equal(receipts.length,1);assert.equal(receipts[0].committedOperations,2);
});
test('flushes final partial batch and rejects invalid batch size',async()=>{
 const sizes=[];const db={async batch(rows){sizes.push(rows.length);return {version:sizes.length,operations:rows.length};}};
 for await(const r of importBatches(db,[1,2,3,4,5],{batchSize:2})){}
 assert.deepEqual(sizes,[2,2,1]);
 await assert.rejects(async()=>{for await(const r of importBatches(db,[],{batchSize:0})){}},RangeError);
});
