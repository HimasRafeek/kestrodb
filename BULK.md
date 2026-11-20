# Streaming write batches

`bulk.mjs` streams a large import as a series of batches. It works with `Database`
(and with the advanced worker client).

```js
import {Database} from './index.mjs';
import {importBatches} from './bulk.mjs';
const db = new Database('./app.kdb');
try {
  async function* operations() {
    for (let i=0; i<10000; i++)
      yield {type:'set', collection:'items', key:String(i), value:{i}};
  }
  for await (const receipt of importBatches(db, operations(), {batchSize:256}))
    console.log(receipt.committedOperations, receipt.version);
} finally { db.close(); }
```

Each batch is one atomic transaction and receives one version. The whole stream
is not atomic. Earlier yielded receipts remain committed if a later batch fails.
The failed batch can have an unknown outcome after an I/O error; the helper does
not automatically retry. Reconcile before resuming. Duplicate keys within one
batch are rejected by the existing API, and its byte/value limits still apply.

The helper consumes at most one batch of operation references before awaiting its
commit, then yields a receipt before requesting more input. Breaking the loop stops
further submissions. The batch size bounds operation count, not input byte size.
A producer's own buffering is outside this helper's control. Normal individual
set/delete semantics and the underlying storage engine are unchanged.

Run `node --test tests/bulk.test.mjs` to verify backpressure and partial-completion behavior. Batching amortizes commit and IPC overhead; it does not establish superiority over another database.
