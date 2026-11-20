/** Bounded streaming ingestion. Each yielded receipt is a committed batch.
 * The whole stream is NOT atomic. Stop consuming to stop submitting batches.
 * Errors retain the worker's unknown-outcome semantics; never retry blindly.
 */
export async function* importBatches(db, operations, {batchSize=256}={}) {
  if (!Number.isInteger(batchSize) || batchSize<1 || batchSize>65536)
    throw new RangeError('batchSize must be an integer from 1 to 65536');
  let batch=[];
  let committedOperations=0;
  for await (const operation of operations) {
    batch.push(operation);
    if (batch.length===batchSize) {
      const receipt=await db.batch(batch);
      committedOperations+=batch.length;
      batch=[];
      yield {...receipt,committedOperations};
    }
  }
  if (batch.length) {
    const receipt=await db.batch(batch);
    committedOperations+=batch.length;
    yield {...receipt,committedOperations};
  }
}
