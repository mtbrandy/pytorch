#include <c10/cuda/CUDACachingAllocator.h>

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAException.h>
#include <c10/cuda/CUDAFunctions.h>
#include <c10/util/UniqueVoidPtr.h>

#include <cuda_runtime_api.h>
#include <algorithm>
#include <bitset>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace c10 {

C10_DEFINE_REGISTRY(FreeCudaMemoryCallbacksRegistry, FreeMemoryCallback);

namespace cuda {
namespace CUDACachingAllocator {

//
// Yet another caching allocator for CUDA device allocations.
//
// - Allocations are associated with a stream. Once freed, blocks can be
//   re-allocated on the same stream, but not on any other stream.
// - The allocator attempts to find the smallest cached block that will fit the
//   requested size. If the block is larger than the requested size, it may be
//   split. If no block is found, the allocator will delegate to cudaMalloc.
// - If the cudaMalloc fails, the allocator will free all cached blocks that
//   are not split and retry the allocation.
// - Large (>1MB) and small allocations are stored in separate pools.
//   Small requests are packed into 2MB buffers. Large requests will use the
//   smallest available free block or allocate a new block using cudaMalloc.
//   To reduce fragmentation, requests between 1MB and 10MB will allocate and
//   split a 20MB block, if no free block of sufficient size is available.
//
// With this allocator, allocations and frees should logically be considered
// "usages" of the memory segment associated with streams, just like kernel
// launches. The programmer must insert the proper synchronization if memory
// segments are used from multiple streams.
//
// The library provides a recordStream() function to help insert the correct
// synchronization when allocations are used on multiple streams. This will
// ensure that the block is not reused before each recorded stream completes
// work.
//


namespace {

using stream_set = std::unordered_set<cuda::CUDAStream>;

constexpr size_t kMinBlockSize = 512;       // all sizes are rounded to at least 512 bytes
constexpr size_t kSmallSize = 1048576;      // largest "small" allocation is 1 MiB
constexpr size_t kSmallBuffer = 2097152;    // "small" allocations are packed in 2 MiB blocks
constexpr size_t kLargeBuffer = 20971520;   // "large" allocations may be packed in 20 MiB blocks
constexpr size_t kMinLargeAlloc = 10485760; // allocations between 1 and 10 MiB may use kLargeBuffer
constexpr size_t kRoundLarge = 2097152;     // round up large allocs to 2 MiB

typedef std::bitset<static_cast<size_t>(StatType::NUM_TYPES)> StatTypes;

void update_stat(Stat& stat, int64_t amount) {
  stat.current += amount;

  TORCH_INTERNAL_ASSERT(stat.current >= 0, "Negative tracked stat in CUDA allocator (likely logic error).");

  stat.peak = std::max(stat.current, stat.peak);
  if (amount > 0) {
    stat.allocated += amount;
  }
  if (amount < 0) {
    stat.freed += -amount;
  }
}

void reset_accumulated_stat(Stat& stat) {
  stat.allocated = 0;
  stat.freed = 0;
}

void reset_peak_stat(Stat& stat) {
  stat.peak = stat.current;
}

void update_stat_array(StatArray& stat_array, int64_t amount, const StatTypes& stat_types) {
  for (size_t stat_type = 0; stat_type < stat_types.size(); ++stat_type) {
    if (stat_types[stat_type]) {
      update_stat(stat_array[stat_type], amount);
    }
  }
}

struct Block;
typedef bool (*Comparison)(const Block*, const Block*);
typedef std::set<Block*, Comparison> BlockPool;

struct Block {
  int           device;      // gpu
  cudaStream_t  stream;      // allocation stream
  stream_set    stream_uses; // streams on which the block was used
  size_t        size;        // block size in bytes
  BlockPool*    pool;        // owning memory pool
  void*         ptr;         // memory address
  bool          allocated;   // in-use flag
  Block*        prev;        // prev block if split from a larger allocation
  Block*        next;        // next block if split from a larger allocation
  int           event_count; // number of outstanding CUDA events

  Block(int device, cudaStream_t stream, size_t size, BlockPool* pool, void* ptr) :
    device(device), stream(stream), stream_uses(), size(size), pool(pool),
    ptr(ptr), allocated(0), prev(nullptr), next(nullptr), event_count(0) { }

  // constructor for search key
  Block(int device, cudaStream_t stream, size_t size) :
    device(device), stream(stream), stream_uses(), size(size), pool(nullptr),
    ptr(nullptr), allocated(0), prev(nullptr), next(nullptr), event_count(0) { }

  bool is_split() const {
    return (prev != nullptr) || (next != nullptr);
  }
};

static bool BlockComparator(const Block* a, const Block* b)
{
  if (a->stream != b->stream) {
    return (uintptr_t)a->stream < (uintptr_t)b->stream;
  }
  if (a->size != b->size) {
    return a->size < b->size;
  }
  return (uintptr_t)a->ptr < (uintptr_t)b->ptr;
}

static std::string format_size(uint64_t size) {
  std::ostringstream os;
  os.precision(2);
  os << std::fixed;
  if (size <= 1024) {
    os << size << " bytes";
  } else if (size <= 1048576) {
    os << (size / 1024.0);
    os << " KiB";
  } else if (size <= 1073741824ULL) {
    os << size / 1048576.0;
    os << " MiB";
  } else {
    os << size / 1073741824.0;
    os << " GiB";
  }
  return os.str();
}

struct LMSSettings {
  LMSSettings() :
    enabled_(false), limit_(0), host_allocator_(nullptr) {}

  bool enabled()                 { return enabled_; }
  void set_enabled(bool enabled) { enabled_ = enabled; }
  size_t limit()                 { return limit_; }
  void set_limit(size_t limit)   { limit_ = limit; }
  at::Allocator* host_allocator()                        { return host_allocator_; }
  void set_host_allocator(at::Allocator* host_allocator) { host_allocator_ = host_allocator; }
  bool limit_alloc(DeviceStats& stats, size_t alloc_size) {
    auto reserved = stats.reserved_bytes[static_cast<size_t>(StatType::AGGREGATE)].current;
    return (reserved + alloc_size) > limit_;
  }

private:
  bool enabled_;
  size_t limit_;
  at::Allocator* host_allocator_;
};

struct AllocParams {
  AllocParams(int device, size_t size, cudaStream_t stream, BlockPool* pool, size_t alloc_size,
              LMSSettings* lms, DeviceStats& stats) :
    search_key(device, stream, size),
    pool(pool),
    alloc_size(alloc_size),
    lms_enabled(lms->enabled()),
    limit_alloc(lms_enabled && lms->limit_alloc(stats, alloc_size)),
    block(nullptr),
    err(cudaSuccess) {}

  int device() { return search_key.device; }
  cudaStream_t stream() { return search_key.stream; }
  size_t size() { return search_key.size; }

  Block search_key;
  BlockPool* pool;
  size_t alloc_size;
  bool lms_enabled;
  bool limit_alloc;
  Block* block;
  StatTypes stat_types;
  AllocSource source;
  cudaError_t err;
};

#define LMS_INVALID_STREAM ((cudaStream_t)-1)

struct CudaLmsStorageImpl : public at::LmsStorageImpl {
  CudaLmsStorageImpl(at::StorageImpl* storage);
  ~CudaLmsStorageImpl() {}

  Block* block() const {
    return block_;
  }

 protected:
  void reclaim_list_add();
  bool reclaim_list_remove();

  cudaStream_t stream() const {
    AT_ASSERT(stream_ != LMS_INVALID_STREAM);
    return stream_;
  }

  void do_pagein(void* dst, void* src, size_t size) {
    C10_CUDA_CHECK(cudaMemcpyAsync(dst, src, size, cudaMemcpyHostToDevice, stream()));
  }

  void do_pagein_sync() {
    C10_CUDA_CHECK(cudaStreamSynchronize(stream()));
  }

  void do_pageout(void* dst, void* src, size_t size, at::LMSSyncEvent_t sync_event) {
    if (stream_ == LMS_INVALID_STREAM) {
      stream_ = cuda::getLMSCUDAStream().stream();
    }
    C10_CUDA_CHECK(cudaStreamWaitEvent(stream(), (cudaEvent_t)sync_event, 0));
    C10_CUDA_CHECK(cudaMemcpyAsync(dst, src, size, cudaMemcpyDeviceToHost, stream()));
  }

  void do_pageout_sync() {
    C10_CUDA_CHECK(cudaStreamSynchronize(stream()));
  }

  Block* block_; // cache block pointer for use while on reclaim list
  cudaStream_t stream_;
};

} // namespace

class DeviceCachingAllocator {

 private:

  // lock around all operations
  mutable std::recursive_mutex mutex;

  // device statistics
  DeviceStats stats;

  // unallocated cached blocks larger than 1 MB
  BlockPool large_blocks;

  // unallocated cached blocks 1 MB or smaller
  BlockPool small_blocks;

  // allocated or in use by a stream
  std::unordered_set<Block*> active_blocks;

  // outstanding cuda events
  std::deque<std::pair<cudaEvent_t, Block*>> cuda_events;

  // set of reclaim candidates for LMS
  at::LmsStorageList reclaim_list;

 public:

  DeviceCachingAllocator() :
      large_blocks(BlockComparator),
      small_blocks(BlockComparator) {}

  // All public methods (except the above) acquire the allocator mutex.
  // Thus, do not call a public method from another public method.

  Block* malloc(int device, size_t size, cudaStream_t stream, LMSSettings* lms)
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // process outstanding cudaEvents
    process_events();

    size = round_size(size);
    auto& pool = get_pool(size);
    const size_t alloc_size = get_allocation_size(size);
    AllocParams params(device, size, stream, &pool, alloc_size, lms, stats);
    params.stat_types[static_cast<size_t>(StatType::AGGREGATE)] = true;
    params.stat_types[static_cast<size_t>(get_stat_type_for_pool(pool))] = true;

    bool block_found =
      // 1. Search pool
      get_free_block(params, AllocSource::FREELIST)
      // 2. Trigger callbacks and retry search
      || (trigger_free_memory_callbacks(params) && get_free_block(params, AllocSource::FREELIST))
      // 3. Attempt allocate (if not limited by lms settings)
      || (!params.limit_alloc && alloc_block(params, false, AllocSource::CUDAMALLOC))
      // 4. If LMS enabled, try to reclaim inactive allocations
      || (params.lms_enabled && try_lms_reclaim(params))
      // 5. Free all non-split cached blocks and retry alloc.
      || (free_cached_blocks() && alloc_block(params, true, AllocSource::CUDAMALLOC_RETRY));

    AT_ASSERT((!block_found && params.err != cudaSuccess) || params.block);
    if (!block_found) {
      if (params.err == cudaErrorMemoryAllocation) {
        cudaGetLastError();  // clear CUDA error

        size_t device_free;
        size_t device_total;
        C10_CUDA_CHECK(cudaMemGetInfo(&device_free, &device_total));

        stats.num_ooms += 1;

        // "total capacity": total global memory on GPU
        // "already allocated": memory allocated by the program using the
        //                      caching allocator
        // "free": free memory as reported by the CUDA API
        // "cached": memory held by the allocator but not used by the program
        //
        // The "allocated" amount  does not include memory allocated outside
        // of the caching allocator, such as memory allocated by other programs
        // or memory held by the driver.
        //
        // The sum of "allocated" + "free" + "cached" may be less than the
        // total capacity due to memory held by the driver and usage by other
        // programs.
        //
        // Note that at this point free_cached_blocks has already returned all
        // possible "cached" memory to the driver. The only remaining "cached"
        // memory is split from a larger block that is partially in-use.
        AT_ERROR(
          "CUDA out of memory. Tried to allocate ", format_size(alloc_size),
          " (GPU ", device, "; ",
          format_size(device_total), " total capacity; ",
          format_size(stats.allocated_bytes[static_cast<size_t>(StatType::AGGREGATE)].current),
          " already allocated; ",
          format_size(device_free), " free; ",
          format_size(stats.reserved_bytes[static_cast<size_t>(StatType::AGGREGATE)].current),
          " reserved in total by PyTorch)");
      } else {
        C10_CUDA_CHECK(params.err);
      }
    }

    Block* block = params.block;
    Block* remaining = nullptr;
    AT_ASSERT(block);

    const bool already_split = block->is_split();
    if (should_split(block, size)) {
      remaining = block;

      block = new Block(device, stream, size, &pool, block->ptr);
      block->prev = remaining->prev;
      if (block->prev) {
        block->prev->next = block;
      }
      block->next = remaining;

      remaining->prev = block;
      remaining->ptr = static_cast<char*>(remaining->ptr) + size;
      remaining->size -= size;
      pool.insert(remaining);

      if (already_split) {
        // An already-split inactive block is being shrunk by size bytes.
        update_stat_array(stats.inactive_split_bytes, -block->size, params.stat_types);
      } else {
        // A new split inactive block is being created from a previously unsplit block,
        // size remaining->size bytes.
        update_stat_array(stats.inactive_split_bytes, remaining->size, params.stat_types);
        update_stat_array(stats.inactive_split, 1, params.stat_types);
      }
    } else if (already_split) {
      // An already-split block is becoming active
      update_stat_array(stats.inactive_split_bytes, -block->size, params.stat_types);
      update_stat_array(stats.inactive_split, -1, params.stat_types);
    }

    block->allocated = true;
    active_blocks.insert(block);

    update_stat_array(stats.allocation, 1, params.stat_types);
    update_stat_array(stats.allocated_bytes, block->size, params.stat_types);
    update_stat_array(stats.active, 1, params.stat_types);
    update_stat_array(stats.active_bytes, block->size, params.stat_types);
    update_stat_array(stats.pinned, 1, params.stat_types);
    update_stat_array(stats.pinned_bytes, block->size, params.stat_types);
    stats.alloc_distribution[static_cast<size_t>(params.source)] += 1;

    return block;
  }

  void free(Block* block)
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    block->allocated = false;

    StatTypes stat_types;
    stat_types[static_cast<size_t>(StatType::AGGREGATE)] = true;
    stat_types[static_cast<size_t>(get_stat_type_for_pool(*(block->pool)))] = true;
    update_stat_array(stats.allocation, -1, {stat_types});
    update_stat_array(stats.allocated_bytes, -block->size, {stat_types});

    if (!block->stream_uses.empty()) {
      insert_events(block);
    } else {
      free_block(block);
    }
  }

  void* getBaseAllocation(Block* block, size_t* outSize) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    while (block->prev) {
      block = block->prev;
    }
    void *basePtr = block->ptr;
    if (outSize) {
      size_t size = 0;
      while (block) {
        size += block->size;
        block = block->next;
      }
      *outSize = size;
    }
    return basePtr;
  }

  void recordStream(Block* block, cuda::CUDAStream stream) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (stream.stream() == block->stream) {
      // ignore uses on the allocation stream, since those don't require any
      // special synchronization
      return;
    }
    block->stream_uses.insert(stream);
  }

  /** returns cached blocks to the system allocator **/
  void emptyCache() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    free_cached_blocks();
  }

  /** Retrieves info (total size + largest block) of the memory cache **/
  void cacheInfo(size_t* total, size_t* largest)
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    cache_info_aux(large_blocks, total, largest);
    cache_info_aux(small_blocks, total, largest);
  }

  /** Returns a copy of the memory allocator stats **/
  DeviceStats getStats() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return stats;
  }

  /** Resets the historical accumulation stats for the device **/
  void resetAccumulatedStats() {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    for (size_t statType = 0; statType < static_cast<size_t>(StatType::NUM_TYPES); ++statType) {
      reset_accumulated_stat(stats.allocation[statType]);
      reset_accumulated_stat(stats.segment[statType]);
      reset_accumulated_stat(stats.active[statType]);
      reset_accumulated_stat(stats.inactive_split[statType]);
      reset_accumulated_stat(stats.pinned[statType]);
      reset_accumulated_stat(stats.allocated_bytes[statType]);
      reset_accumulated_stat(stats.reserved_bytes[statType]);
      reset_accumulated_stat(stats.active_bytes[statType]);
      reset_accumulated_stat(stats.inactive_split_bytes[statType]);
      reset_accumulated_stat(stats.pinned_bytes[statType]);
    }

    stats.num_alloc_retries = 0;
    stats.num_ooms = 0;
    stats.reclaimed = 0;
    stats.reclaimed_bytes = 0;
    stats.alloc_distribution.fill(0);
  }

  /** Resets the historical peak stats for the device **/
  void resetPeakStats() {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    for (size_t statType = 0; statType < static_cast<size_t>(StatType::NUM_TYPES); ++statType) {
      reset_peak_stat(stats.allocation[statType]);
      reset_peak_stat(stats.segment[statType]);
      reset_peak_stat(stats.active[statType]);
      reset_peak_stat(stats.inactive_split[statType]);
      reset_peak_stat(stats.pinned[statType]);
      reset_peak_stat(stats.allocated_bytes[statType]);
      reset_peak_stat(stats.reserved_bytes[statType]);
      reset_peak_stat(stats.active_bytes[statType]);
      reset_peak_stat(stats.inactive_split_bytes[statType]);
      reset_peak_stat(stats.pinned_bytes[statType]);
    }
  }

  /** Dump a complete snapshot of the memory held by the allocator. Potentially VERY expensive. **/
  std::vector<SegmentInfo> snapshot() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    std::vector<SegmentInfo> result;
    const auto all_blocks = get_all_blocks();

    for (const Block* const head_block : all_blocks) {
      if (head_block->prev != nullptr) {
        continue;
      }
      result.emplace_back();
      SegmentInfo& segment_info = result.back();
      segment_info.device = head_block->device;
      segment_info.address = reinterpret_cast<int64_t>(head_block->ptr);
      segment_info.is_large = (head_block->pool == &large_blocks);

      const Block* block = head_block;
      while (block != nullptr) {
        segment_info.blocks.emplace_back();
        BlockInfo& block_info = segment_info.blocks.back();

        block_info.size = block->size;
        block_info.allocated = block->allocated;
        block_info.active = block->allocated || (block->event_count > 0);

        segment_info.total_size += block_info.size;
        if (block_info.allocated) {
          segment_info.allocated_size += block_info.size;
        }
        if (block_info.active) {
          segment_info.active_size += block_info.size;
        }

        block = block->next;
      }
    }

    std::sort(result.begin(), result.end(), [](const SegmentInfo& a, const SegmentInfo& b) {
      return a.address < b.address;
    });

    return result;
  }

  static size_t round_size(size_t size) {
    if (size < kMinBlockSize) {
      return kMinBlockSize;
    } else {
      return kMinBlockSize * ((size + kMinBlockSize - 1) / kMinBlockSize);
    }
  }

  void reclaim_list_add(LmsStorageImpl* lmsStorage) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Block* block = static_cast<CudaLmsStorageImpl*>(lmsStorage)->block();

    StatTypes stat_types;
    stat_types[static_cast<size_t>(StatType::AGGREGATE)] = true;
    stat_types[static_cast<size_t>(get_stat_type_for_size(block->size))] = true;
    update_stat_array(stats.pinned, -1, stat_types);
    update_stat_array(stats.pinned_bytes, -block->size, stat_types);

    lmsStorage->list_add(&reclaim_list);
  }

  bool reclaim_list_remove(LmsStorageImpl* lmsStorage) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return reclaim_list_remove_internal(lmsStorage);
  }

  void reclaimInactive() {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (!reclaim_list.empty()) {
      cudaStream_t stream = cuda::getCurrentCUDAStream().stream();
      cudaEvent_t sync_event;

      C10_CUDA_CHECK(cudaEventCreate(&sync_event));
      C10_CUDA_CHECK(cudaEventRecord(sync_event, stream));
      reclaim_all(sync_event);
      C10_CUDA_CHECK(cudaEventDestroy(sync_event));
    }
  }

 private:

  // All private methods do not acquire the allocator mutex.

  std::vector<const Block*> get_all_blocks() const {
    std::vector<const Block*> blocks;
    blocks.insert(blocks.end(), small_blocks.begin(), small_blocks.end());
    blocks.insert(blocks.end(), large_blocks.begin(), large_blocks.end());
    blocks.insert(blocks.end(), active_blocks.begin(), active_blocks.end());
    return blocks;
  }

  /** moves a block into a pool of cached free blocks */
  void free_block(Block* block)
  {
    AT_ASSERT(!block->allocated && block->event_count == 0);

    size_t original_block_size = block->size;

    auto& pool = *block->pool;
    int64_t net_change_inactive_split_blocks = 0;
    int64_t net_change_inactive_split_size = 0;

    const std::array<Block*, 2> merge_candidates = {block->prev, block->next};
    for (Block* merge_candidate : merge_candidates) {
      const int64_t subsumed_size = try_merge_blocks(block, merge_candidate, pool);
      if (subsumed_size > 0) {
        net_change_inactive_split_blocks -= 1;
        net_change_inactive_split_size -= subsumed_size;
      }
    }

    active_blocks.erase(block);
    pool.insert(block);

    if (block->is_split()) {
      net_change_inactive_split_blocks += 1;
      net_change_inactive_split_size += block->size;
    }

    StatTypes stat_types;
    stat_types[static_cast<size_t>(StatType::AGGREGATE)] = true;
    stat_types[static_cast<size_t>(get_stat_type_for_pool(*(block->pool)))] = true;
    update_stat_array(stats.inactive_split, net_change_inactive_split_blocks, stat_types);
    update_stat_array(stats.inactive_split_bytes, net_change_inactive_split_size, stat_types);
    update_stat_array(stats.active, -1, stat_types);
    update_stat_array(stats.active_bytes, -original_block_size, stat_types);
    update_stat_array(stats.pinned, -1, stat_types);
    update_stat_array(stats.pinned_bytes, -original_block_size, stat_types);
  }

  /** combine previously split blocks. returns the size of the subsumed block, or 0 on failure. */
  size_t try_merge_blocks(Block* dst, Block* src, BlockPool& pool)
  {
    if (!src || src->allocated || src->event_count > 0) {
      return 0;
    }

    AT_ASSERT(dst->is_split() && src->is_split());

    if (dst->prev == src) {
      dst->ptr = src->ptr;
      dst->prev = src->prev;
      if (dst->prev) {
        dst->prev->next = dst;
      }
    } else {
      dst->next = src->next;
      if (dst->next) {
        dst->next->prev = dst;
      }
    }

    const size_t subsumed_size = src->size;
    dst->size += subsumed_size;
    pool.erase(src);
    delete src;

    return subsumed_size;
  }

  BlockPool& get_pool(size_t size) {
    if (size <= kSmallSize) {
      return small_blocks;
    } else {
      return large_blocks;
    }
  }

  StatType get_stat_type_for_pool(const BlockPool& pool) {
    if (&pool == &small_blocks) {
      return StatType::SMALL_POOL;
    } else if (&pool == &large_blocks) {
      return StatType::LARGE_POOL;
    } else {
      AT_ERROR("get_stat_type_for_pool: invalid pool");
    }
  }

  StatType get_stat_type_for_size(size_t size) {
    return get_stat_type_for_pool(get_pool(size));
  }

  bool should_split(const Block* block, size_t size) {
    size_t remaining = block->size - size;
    if (block->pool == &small_blocks) {
      return remaining >= kMinBlockSize;
    } else if (block->pool == &large_blocks) {
      return remaining > kSmallSize;
    } else {
      AT_ERROR("should_split: invalid pool");
    }
  }

  static size_t get_allocation_size(size_t size) {
    if (size <= kSmallSize) {
      return kSmallBuffer;
    } else if (size < kMinLargeAlloc) {
      return kLargeBuffer;
    } else {
      return kRoundLarge * ((size + kRoundLarge - 1) / kRoundLarge);
    }
  }

  bool get_free_block(AllocParams& p, AllocSource source) {
    BlockPool& pool = *p.pool;
    auto it = pool.lower_bound(&p.search_key);
    if (it == pool.end() || (*it)->stream != p.stream())
      return false;
    p.block = *it;
    p.source = source;
    pool.erase(it);
    return true;
  }

  bool trigger_free_memory_callbacks(AllocParams& p) {
    bool freed_memory = false;
    for (const auto& name : FreeCudaMemoryCallbacksRegistry()->Keys()) {
      freed_memory |=
        FreeCudaMemoryCallbacksRegistry()->Create(name)->Execute();
    }
    return freed_memory;
  }

  bool alloc_block(AllocParams& p, bool record_error, AllocSource source) {
    size_t size = p.alloc_size;
    void* ptr;
    cudaError_t err;

    if (source == AllocSource::CUDAMALLOC_RETRY) {
      stats.num_alloc_retries += 1;
    }

    err = cudaMalloc(&ptr, size);
    if (err != cudaSuccess) {
      if (record_error) p.err = err; else cudaGetLastError();
      return false;
    }

    p.block = new Block(p.device(), p.stream(), size, p.pool, (char*)ptr);
    update_stat_array(stats.segment, 1, p.stat_types);
    update_stat_array(stats.reserved_bytes, size, p.stat_types);
    p.source = source;

    return (p.block != nullptr);
  }

  bool free_cached_blocks()
  {
    // First ensure that all blocks that can't currently be allocated due to
    // outstanding events are returned to the pool.
    synchronize_and_free_events();

    // Free all non-split cached blocks
    free_blocks(large_blocks);
    free_blocks(small_blocks);
    return true;
  }

  void free_blocks(BlockPool& blocks)
  {
    // Frees all non-split blocks
    auto it = blocks.begin();
    while (it != blocks.end()) {
      Block* block = *it;
      if (!block->prev && !block->next) {
        C10_CUDA_CHECK(cudaFree((void*)block->ptr));

        StatTypes stat_types;
        stat_types[static_cast<size_t>(StatType::AGGREGATE)] = true;
        stat_types[static_cast<size_t>(get_stat_type_for_pool(*(block->pool)))] = true;
        update_stat_array(stats.segment, -1, stat_types);
        update_stat_array(stats.reserved_bytes, -block->size, stat_types);

        auto cur = it;
        ++it;
        blocks.erase(cur);
        delete block;
      } else {
        ++it;
      }
    }
  }

  void synchronize_and_free_events() {
    // Synchronize on outstanding events and then free associated blocks.

    for (auto& e : cuda_events) {
      cudaEvent_t event = e.first;
      Block* block = e.second;

      C10_CUDA_CHECK(cudaEventSynchronize(event));
      C10_CUDA_CHECK(cudaEventDestroy(event));

      block->event_count--;
      if (block->event_count == 0) {
        free_block(block);
      }
    }

    cuda_events.clear();
  }

  void insert_events(Block* block)
  {
    int prev_device;
    C10_CUDA_CHECK(cudaGetDevice(&prev_device));

    stream_set streams(std::move(block->stream_uses));
    AT_ASSERT(block->stream_uses.empty());
    for (auto it = streams.begin(); it != streams.end(); ++it) {
      C10_CUDA_CHECK(cudaSetDevice(it->device_index()));

      cudaEvent_t event;
      C10_CUDA_CHECK(cudaEventCreateWithFlags(&event, cudaEventDisableTiming));
      C10_CUDA_CHECK(cudaEventRecord(event, it->stream()));

      block->event_count++;
      cuda_events.emplace_back(event, block);
    }

    C10_CUDA_CHECK(cudaSetDevice(prev_device));
  }

  void process_events()
  {
    // Process outstanding cudaEvents. Events that are completed are removed
    // from the queue, and the 'event_count' for the corresponding allocation
    // is decremented. Stops at the first event which has not been completed.
    // Since events on different devices or streams may occur out of order,
    // the processing of some events may be delayed.
    while (!cuda_events.empty()) {
      auto& e = cuda_events.front();
      cudaEvent_t event = e.first;
      Block* block = e.second;

      cudaError_t err = cudaEventQuery(event);
      if (err == cudaErrorNotReady) {
        // ignore and clear the error if not ready
        cudaGetLastError();
        break;
      } else if (err != cudaSuccess) {
        C10_CUDA_CHECK(err);
      }

      C10_CUDA_CHECK(cudaEventDestroy(event));

      block->event_count--;
      if (block->event_count == 0) {
        free_block(block);
      }
      cuda_events.pop_front();
    }
  }

  // Accumulates sizes of all memory blocks for given device in given pool
  void cache_info_aux(BlockPool& blocks, size_t* total, size_t* largest)
  {
    for (auto it = blocks.begin(); it != blocks.end(); ++it) {
      size_t blocksize = (*it)->size;
      *total += blocksize;
      if (blocksize > *largest) {
        *largest = blocksize;
      }
    }
  }

  bool reclaim_list_remove_internal(LmsStorageImpl* lmsStorage, bool reclaimed = false) {
    if (!lmsStorage->list_remove())
      return false;

    Block* block = static_cast<CudaLmsStorageImpl*>(lmsStorage)->block();

    StatTypes stat_types;
    stat_types[static_cast<size_t>(StatType::AGGREGATE)] = true;
    stat_types[static_cast<size_t>(get_stat_type_for_size(block->size))] = true;
    update_stat_array(stats.pinned, 1, stat_types);
    update_stat_array(stats.pinned_bytes, block->size, stat_types);
    if (reclaimed) {
      stats.reclaimed += 1;
      stats.reclaimed_bytes += lmsStorage->capacity();
    }

    return true;
  }

  bool reclaim_one(size_t size, cudaEvent_t sync_event) {
    LmsStorageImpl *best = nullptr;
    size_t best_size = ULONG_MAX;

    if (!reclaim_list.empty()) {
      auto hook = reclaim_list.head();
      auto end = reclaim_list.terminator();
      do {
        LmsStorageImpl *lmsStorage = hook->elem();
        hook = hook->next();

        Block* block = static_cast<CudaLmsStorageImpl*>(lmsStorage)->block();
        if (block->size >= size && block->size < best_size) {
          best = lmsStorage;
          best_size = block->size;
          if (!should_split(block, size)) {
            // close enough
            break;
          }
        }
      } while (hook != end);
    }

    if (best == nullptr)
      return false;

    reclaim_list_remove_internal(best, true);
    best->pageout(sync_event);
    best->pageout_sync();

    return true;
  }

  static inline void process_pageout_sync(at::LmsStorageList* iodone_queue) {
    while (!iodone_queue->empty()) {
      auto hook = iodone_queue->head();
      LmsStorageImpl *lmsStorage = hook->elem();
      lmsStorage->pageout_sync(iodone_queue);
    }
  }

  bool reclaim_fragments(size_t size, cudaEvent_t sync_event) {
    at::LmsStorageList iodone_queue;
    size_t alloc_size;
    int count = 0;

    if (!reclaim_list.empty()) {
      auto hook = reclaim_list.head();
      auto end = reclaim_list.terminator();
      do {
        LmsStorageImpl *lmsStorage = hook->elem();
        hook = hook->next();

        Block* block = static_cast<CudaLmsStorageImpl*>(lmsStorage)->block();
        getBaseAllocation(block, &alloc_size);
        if (alloc_size >= size) {
          reclaim_list_remove_internal(lmsStorage, true);
          lmsStorage->pageout(sync_event, &iodone_queue);
          count++;
        }
      } while (hook != end);
    }

    if (count == 0)
      return false;

    process_pageout_sync(&iodone_queue);
    return true;
  }

  bool reclaim_all(cudaEvent_t sync_event) {
    at::LmsStorageList iodone_queue;
    int count = 0;

    if (!reclaim_list.empty()) {
      auto hook = reclaim_list.head();
      auto end = reclaim_list.terminator();
      do {
        LmsStorageImpl *lmsStorage = hook->elem();
        hook = hook->next();

        reclaim_list_remove_internal(lmsStorage, true);
        lmsStorage->pageout(sync_event, &iodone_queue);
        count++;
      } while (hook != end);
    }

    if (count == 0)
      return false;

    process_pageout_sync(&iodone_queue);
    return true;
  }

  bool try_lms_reclaim(AllocParams& p) {
    size_t size = p.size();
    cudaStream_t stream = p.stream();
    cudaEvent_t sync_event;

    AT_ASSERT(stream == cuda::getCurrentCUDAStream().stream());
    C10_CUDA_CHECK(cudaEventCreate(&sync_event));
    C10_CUDA_CHECK(cudaEventRecord(sync_event, stream));

    bool found =
      // a. Search reclaim list for a suitable inactive allocation
      (reclaim_one(size, sync_event) && get_free_block(p, AllocSource::RECLAIM_ONE))
      // b. Reclaim fragments of suitable allocations
      || (reclaim_fragments(size, sync_event) && get_free_block(p, AllocSource::RECLAIM_FRAGMENTS))
      // c. Attempt allocate (if not done earlier due to limit)
      || (p.limit_alloc && alloc_block(p, false, AllocSource::CUDAMALLOC_OVER_LIMIT))
      // d. Reclaim everything else
      || (reclaim_all(sync_event) && get_free_block(p, AllocSource::RECLAIM_ALL));

    C10_CUDA_CHECK(cudaEventDestroy(sync_event));

    return found;
  }
};

class THCCachingAllocator {

 private:

  std::mutex mutex;

  // allocated blocks by device pointer
  std::unordered_map<void*, Block*> allocated_blocks;

  // lock around calls to cudaFree (to prevent deadlocks with NCCL)
  mutable std::mutex cuda_free_mutex;

  void add_allocated_block(Block* block) {
    std::lock_guard<std::mutex> lock(mutex);
    allocated_blocks[block->ptr] = block;
  }

 public:

  std::vector<DeviceCachingAllocator*> device_allocator;
  LMSSettings lms_settings;

  std::mutex* getCudaFreeMutex() const {
    return &cuda_free_mutex;
  }

  Block* get_allocated_block(void *ptr, bool remove=false) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = allocated_blocks.find(ptr);
    if (it == allocated_blocks.end()) {
      return nullptr;
    }
    Block* block = it->second;
    if (remove) {
      allocated_blocks.erase(it);
    }
    return block;
  }

  void init(int device_count, at::Allocator* host_allocator) {
    int size = device_allocator.size();
    if (size < device_count) {
      device_allocator.resize(device_count);
      for (int i = size; i < device_count; i++) {
        device_allocator[i] = new DeviceCachingAllocator();
      }
    }
    lms_settings.set_host_allocator(host_allocator);
  }

  /** allocates a block which is safe to use from the provided stream */
  void malloc(void** devPtr, size_t size, cudaStream_t stream) {
    int device;
    C10_CUDA_CHECK(cudaGetDevice(&device));
    Block* block = device_allocator[device]->malloc(device, size, stream, &lms_settings);
    add_allocated_block(block);
    *devPtr = (void*)block->ptr;
  }

  void free(void* ptr) {
    if (!ptr) {
      return;
    }
    Block* block = get_allocated_block(ptr, true);
    if (!block) {
      AT_ERROR("invalid device pointer: ", ptr);
    }
    device_allocator[block->device]->free(block);
  }

  void emptyCache() {
    int count = device_allocator.size();
    for (int i = 0; i < count; i++)
      device_allocator[i]->emptyCache();
  }

  void* getBaseAllocation(void* ptr, size_t* outSize)
  {
    Block* block = get_allocated_block(ptr);
    if (!block) {
      AT_ERROR("invalid device pointer: ", ptr);
    }
    return device_allocator[block->device]->getBaseAllocation(block, outSize);
  }

  void recordStream(void* ptr, cuda::CUDAStream stream)
  {
    // Empty tensor's storage().data() might be a null ptr. As there is no
    // blocks associated with those tensors, it is fine to do nothing here.
    if (!ptr) {
      return;
    }

    Block* block = get_allocated_block(ptr);
    // block could be nullptr in some cases, e.g., tensor loaded from blob, or
    // shared from another process, or not pointing to a CUDA tensor.
    if (block) {
      device_allocator[block->device]->recordStream(block, stream);
    }
  }

  std::vector<SegmentInfo> snapshot() {
    std::vector<SegmentInfo> result;
    int count = device_allocator.size();
    for (int i = 0; i < count; i++) {
      auto snap = device_allocator[i]->snapshot();
      result.insert(result.end(), snap.begin(), snap.end());
    }

    return result;
  }

  void reclaimInactive() {
    int count = device_allocator.size();
    for (int i = 0; i < count; i++)
      device_allocator[i]->reclaimInactive();
  }
};

THCCachingAllocator caching_allocator;

CudaLmsStorageImpl::CudaLmsStorageImpl(at::StorageImpl* storage) :
  at::LmsStorageImpl(storage, caching_allocator.lms_settings.host_allocator()),
  block_(nullptr), stream_(LMS_INVALID_STREAM) {}

void CudaLmsStorageImpl::reclaim_list_add() {
  if (capacity() > 0) {
    block_ = caching_allocator.get_allocated_block(ptr());
    caching_allocator.device_allocator[device().index()]->reclaim_list_add(this);
  }
}

bool CudaLmsStorageImpl::reclaim_list_remove() {
  return caching_allocator.device_allocator[device().index()]->reclaim_list_remove(this);
}

static void CudaCachingDeleter(void* ptr) {
  caching_allocator.free(ptr);
}

// NB: I decided not to fold this into THCCachingAllocator, because the latter
// has a lot more methods and it wasn't altogether clear that they should
// actually be publicly exposed
struct CudaCachingAllocator : public Allocator {
  DataPtr allocate(size_t size) const override {
    int device;
    C10_CUDA_CHECK(cudaGetDevice(&device));
    void* r = nullptr;
    if (size != 0) {
      caching_allocator.malloc(&r, size, cuda::getCurrentCUDAStream(device));
    }
    return {r, r, &CudaCachingDeleter, Device(DeviceType::CUDA, device)};
  }
  DeleterFnPtr raw_deleter() const override {
    return &CudaCachingDeleter;
  }
  at::LmsStorageImpl* AsLmsStorage(at::StorageImpl* storage) const {
    return caching_allocator.lms_settings.enabled() ? new CudaLmsStorageImpl(storage) : nullptr;
  }
};

CudaCachingAllocator device_allocator;

Allocator* get(void)
{
  return &device_allocator;
}

void init(int device_count, at::Allocator* host_allocator) {
  caching_allocator.init(device_count, host_allocator);
}

void emptyCache(void) {
  caching_allocator.emptyCache();
}

void cacheInfo(int dev_id, size_t* cachedAndFree, size_t* largestBlock) {
  caching_allocator.device_allocator[dev_id]->cacheInfo(cachedAndFree, largestBlock);
}

void* getBaseAllocation(void *ptr, size_t *size)
{
  return caching_allocator.getBaseAllocation(ptr, size);
}

void recordStream(void *ptr, cuda::CUDAStream stream)
{
  caching_allocator.recordStream(ptr, stream);
}

std::mutex* getFreeMutex()
{
  return caching_allocator.getCudaFreeMutex();
}

static inline void assertValidDevice(int device) {
  int device_num = device_count();
  AT_ASSERTM(0 <= device && device < device_num, "Invalid device argument.");
}

DeviceStats getDeviceStats(int device) {
  assertValidDevice(device);
  return caching_allocator.device_allocator[device]->getStats();
}

void resetAccumulatedStats(int device) {
  assertValidDevice(device);
  caching_allocator.device_allocator[device]->resetAccumulatedStats();
}

void resetPeakStats(int device) {
  assertValidDevice(device);
  caching_allocator.device_allocator[device]->resetPeakStats();
}

std::vector<SegmentInfo> snapshot() {
  return caching_allocator.snapshot();
}

void setUserEnabledLMS(bool enable) {
  caching_allocator.lms_settings.set_enabled(enable);
}

bool userEnabledLMS(void) {
  return caching_allocator.lms_settings.enabled();
}

void setUserLimitLMS(size_t limit) {
  caching_allocator.lms_settings.set_limit(limit);
}

size_t userLimitLMS(void) {
  return caching_allocator.lms_settings.limit();
}

void reclaimInactive(void) {
  caching_allocator.reclaimInactive();
}

//
// In CUDA IPC, sender sends a tensor to receiver, getIpcDevPtr
// is called by the receiving process to map the CUDA memory from the sending
// process into its own address space.
//
// CUDA IPC only allows sharing a big memory block associated with a cudaIpcMemHandle_t
// and it can be opened only **once** per context per process. There can be
// multiple types of storage in the same IPC mem block, so we must cache the
// device ptr to construct typed storage as it comes.
//
// ipcMemHandle_to_devptr maps a cudaIpcMemHandle_t to a device pointer in the process
// that can be used to access the memory block in the sender process.
// It only saves a weak_ptr of the device pointer in the map, the shared_ptr
// will be used to reconstruct all storages in this CudaMalloc allocation.
// And it will deleted in cudaIpcCloseMemHandle when its reference count is 0.
//
namespace {
  std::mutex IpcMutex;
  std::unordered_map<std::string, std::weak_ptr<void>> ipcMemHandle_to_devptr;
}

std::shared_ptr<void> getIpcDevPtr(std::string handle) {
  std::lock_guard<std::mutex> lock(IpcMutex);

  auto iter = ipcMemHandle_to_devptr.find(handle);
  if (iter != ipcMemHandle_to_devptr.end()) {
    auto devptr = iter->second.lock();
    if (devptr) return devptr;
  }
  // This ipcMemHandle hasn't been opened, or already expired, open it to
  // enable IPC access to that mem block.
  void *dev = nullptr;
  auto ipc_handle = reinterpret_cast<const cudaIpcMemHandle_t*>(handle.c_str());
  C10_CUDA_CHECK(cudaIpcOpenMemHandle(&dev, *ipc_handle, cudaIpcMemLazyEnablePeerAccess));
  // devPtr has to be deleted in same device when created.
  int curr_device;
  C10_CUDA_CHECK(cudaGetDevice(&curr_device));
  auto sp = std::shared_ptr<void>(
      dev,
      [handle, curr_device](void *ptr) {
        cuda::CUDAGuard device_guard(curr_device);
        std::lock_guard<std::mutex> deleter_lock(IpcMutex);
        C10_CUDA_CHECK(cudaIpcCloseMemHandle(ptr));
        ipcMemHandle_to_devptr.erase(handle);});
  std::weak_ptr<void> wp = sp;
  // To eliminate an additional search, we can use insert().
  // It doesn't overwrite when key already exists(ptr expired).
  // But in the deleter for sp we erased the entry,
  // this should be safe to do now.
  ipcMemHandle_to_devptr.insert(iter, {handle, wp});

  return sp;
}

void* raw_alloc(size_t nbytes) {
  if (nbytes == 0) {
    return nullptr;
  }
  int device;
  C10_CUDA_CHECK(cudaGetDevice(&device));
  void* r = nullptr;
  caching_allocator.malloc(&r, nbytes, cuda::getCurrentCUDAStream(device));
  return r;
}

void raw_delete(void* ptr) {
  caching_allocator.free(ptr);
}

} // namespace CUDACachingAllocator

}} // namespace c10::cuda
