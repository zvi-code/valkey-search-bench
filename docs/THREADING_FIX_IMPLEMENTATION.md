# Threading Recall Fix - Implementation Summary

## Date: October 4, 2025

## Changes Implemented

This document summarizes the implementation of the threading fix for vector query recall tracking as specified in `THREADING_RECALL_FIX.md`.

### 1. Header File Changes (`src/valkey-benchmark-vgen.h`)

**Added two new function declarations:**

```c
/**
 * Allocate a query index for recall tracking (thread-safe).
 * 
 * This function should be called when a query request is sent (not when
 * the response arrives) to ensure proper matching of responses to ground truth.
 * 
 * @return The allocated query index
 */
uint64_t vgen_allocate_query_index(void);

/**
 * Compute recall for a search result and update tracking statistics.
 * 
 * This function should be called in the readHandler when search results are received.
 * It extracts the returned keys, compares them with ground truth, and updates
 * recall statistics using the pre-allocated query index.
 * 
 * @param client The client that received the search response
 * @param reply The search response (valkeyReply*)
 * @param query_idx The query index allocated when the request was sent
 */
void vgen_compute_recall_with_index(client c, void *reply, uint64_t query_idx);
```

**Replaced:** Old `vgen_compute_recall(client c, void *reply)` declaration

### 2. Implementation Changes (`src/valkey-benchmark-vgen.c`)

**Added new function `vgen_allocate_query_index()`:**

```c
uint64_t vgen_allocate_query_index(void) {
    pthread_mutex_lock(&recall_tracker.lock);
    uint64_t idx = recall_tracker.current_query_index;
    recall_tracker.current_query_index++;  /* Increment on REQUEST send, not response */
    pthread_mutex_unlock(&recall_tracker.lock);
    return idx;
}
```

**Modified `vgen_compute_recall()` to `vgen_compute_recall_with_index()`:**

- Changed function signature to accept `uint64_t query_idx` parameter
- Removed the buggy code that incremented `current_query_index` on response arrival:
  ```c
  // REMOVED (this was the bug):
  pthread_mutex_lock(&recall_tracker.lock);
  uint64_t query_idx = recall_tracker.current_query_index;
  recall_tracker.current_query_index++;  // BUG: increment on response
  pthread_mutex_unlock(&recall_tracker.lock);
  ```
- Now uses the provided `query_idx` parameter instead
- Added comment explaining the fix

### 3. Client Structure (`src/valkey-benchmark.c`)

**Already implemented** (was done in previous work):
- Added `uint64_t query_index` field to `_client` struct (line 295)

### 4. Request Sending (`src/valkey-benchmark.c` - writeHandler)

**Already implemented** (was done in previous work):
- Allocates query index when request is sent (line 1507-1510):
  ```c
  /* For vector queries, allocate query index for recall tracking */
  if (config.is_vector_generator) {
      c->query_index = vgen_allocate_query_index();
  }
  ```

### 5. Response Processing (`src/valkey-benchmark.c` - readHandler)

**Already implemented** (was done in previous work):
- Uses client's query index when computing recall (line 1359):
  ```c
  if (config.is_vector_generator) {
      vgen_compute_recall_with_index(c, reply, c->query_index);
  }
  ```

## How The Fix Works

### Problem (Before Fix)
With multiple threads, the query index was allocated when the **response arrived**, not when the request was sent. This caused a race condition:

1. Thread 1 sends Query A (stores ground truth at index 0)
2. Thread 2 sends Query C (stores ground truth at index 1)  
3. Thread 1 sends Query B (stores ground truth at index 2)
4. Thread 2 sends Query D (stores ground truth at index 3)
5. **Response C arrives first** → Gets index 0 → Matches with Query A's ground truth ❌ WRONG
6. **Response A arrives** → Gets index 1 → Matches with Query C's ground truth ❌ WRONG

### Solution (After Fix)
The query index is now allocated when the **request is sent** and stored in the client structure:

1. Thread 1 sends Query A → Allocates index 0 → Stores in c->query_index
2. Thread 2 sends Query C → Allocates index 1 → Stores in c->query_index
3. Thread 1 sends Query B → Allocates index 2 → Stores in c->query_index
4. Thread 2 sends Query D → Allocates index 3 → Stores in c->query_index
5. **Response C arrives** → Uses c->query_index (1) → Matches with Query C's ground truth ✅ CORRECT
6. **Response A arrives** → Uses c->query_index (0) → Matches with Query A's ground truth ✅ CORRECT

The client structure acts as a "courier tag" that travels with each request-response pair, ensuring the response is always matched to the correct ground truth.

## Build Status

✅ **Successfully compiled** on October 4, 2025
- Built with: `make -j$(nproc) valkey-benchmark` in `build-debug` directory
- No compilation errors or warnings

## Testing Plan

### Test 1: Single Thread Baseline
```bash
cd /home/ubuntu/valkey/build-debug

# Run with single thread to establish baseline recall
./bin/valkey-benchmark \
  --search --search-name vindex2_128dim --search-prefix "v2-128:" \
  --vector-dim 128 --vector-field embed2 \
  -t vec-query -n 10000 -r 1000000 -c 1 --threads 1 \
  --rfr 'yes' --ef-search 32 \
  --cluster -h ec-search-zvi-ec-1shard-no-tls.ajfdds.clustercfg.euw1devo.cache.amazonaws.com
```

### Test 2: Multi-Thread (Should Match Baseline)
```bash
cd /home/ubuntu/valkey/build-debug

# Run with 25 threads - recall should match single-thread test
./bin/valkey-benchmark \
  --search --search-name vindex2_128dim --search-prefix "v2-128:" \
  --vector-dim 128 --vector-field embed2 \
  -t vec-query -n 10000 -r 1000000 -c 25 --threads 25 \
  --rfr 'yes' --ef-search 32 \
  --cluster -h ec-search-zvi-ec-1shard-no-tls.ajfdds.clustercfg.euw1devo.cache.amazonaws.com
```

### Test 3: Stress Test (High Concurrency)
```bash
cd /home/ubuntu/valkey/build-debug

# Run with 50 threads and 100 connections to stress test
./bin/valkey-benchmark \
  --search --search-name vindex2_128dim --search-prefix "v2-128:" \
  --vector-dim 128 --vector-field embed2 \
  -t vec-query -n 100000 -r 1000000 -c 100 --threads 50 \
  --rfr 'yes' --ef-search 32 \
  --cluster -h ec-search-zvi-ec-1shard-no-tls.ajfdds.clustercfg.euw1devo.cache.amazonaws.com
```

### Expected Results

**Before Fix:**
- Single thread: Recall @ 32 = ~X%
- Multi-thread: Recall @ 32 = Random/incorrect (could be 0%-100%)

**After Fix:**
- Single thread: Recall @ 32 = ~X%
- Multi-thread: Recall @ 32 = ~X% (same as single thread ±1% due to statistical variation)

The recall percentage should be **deterministic and consistent** across different thread counts.

## Known Limitations (Not Fixed in This PR)

### 1. vec-ground-truth Threading Issue
**Location**: `src/valkey-benchmark-vgen.c:287`

The ground truth key index counter is not thread-safe:
```c
static uint64_t ground_truth_key_index = 0;  // NOT THREAD-SAFE
vector_key_t key = (ground_truth_key_index++) % 1000000;  // RACE CONDITION
```

**Future Fix**: Make it atomic:
```c
static _Atomic uint64_t ground_truth_key_index = 0;
vector_key_t key = (atomic_fetch_add(&ground_truth_key_index, 1)) % 1000000;
```

### 2. Hardcoded thread_id = 0
**Location**: Multiple places in `src/valkey-benchmark-vgen.c`

All vector generator functions use `int thread_id = 0;` which means all threads share the same iterator pool. This is **benign for queries** (just occasional duplicate vectors) but should eventually be fixed to use actual thread IDs.

## Files Modified

1. ✅ `src/valkey-benchmark-vgen.h` - Added function declarations
2. ✅ `src/valkey-benchmark-vgen.c` - Implemented new functions
3. ✅ `src/valkey-benchmark.c` - Already had client struct and call sites (from previous work)

## Verification

```bash
# Check that all references use the new function name
grep -r "vgen_compute_recall[^_]" src/
# Should return: NO MATCHES (old function removed)

grep -r "vgen_compute_recall_with_index" src/
# Should return: 
#   - Declaration in valkey-benchmark-vgen.h
#   - Implementation in valkey-benchmark-vgen.c
#   - Call site in valkey-benchmark.c readHandler

grep -r "vgen_allocate_query_index" src/
# Should return:
#   - Declaration in valkey-benchmark-vgen.h
#   - Implementation in valkey-benchmark-vgen.c
#   - Call site in valkey-benchmark.c writeHandler
```

## Status

- **Issue Identified**: ✅ Complete
- **Solution Designed**: ✅ Complete  
- **Implementation**: ✅ Complete
- **Build**: ✅ Success
- **Testing**: ⏳ Ready for testing
- **Documentation**: ✅ Complete

---

**Implemented By**: GitHub Copilot  
**Date**: October 4, 2025  
**Severity**: HIGH (incorrect recall measurements with threads)  
**Complexity**: LOW (minimal code changes required)  
**Risk**: LOW (isolated to recall tracking, doesn't affect actual queries)  
**Build Status**: ✅ Compiled successfully with no errors
