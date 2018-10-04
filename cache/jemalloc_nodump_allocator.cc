//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "cache/jemalloc_nodump_allocator.h"

#include <string>

#include "util/logging.h"
#include "util/string_util.h"

namespace rocksdb {

#ifdef ROCKSDB_JEMALLOC_NODUMP_ALLOCATOR

std::atomic<extent_alloc_t*> JemallocNodumpAllocator::original_alloc_{nullptr};

JemallocNodumpAllocator::JemallocNodumpAllocator(
    unsigned arena_index, int flags, std::unique_ptr<extent_hooks_t>&& hooks,
    const std::shared_ptr<Logger>& info_log)
    : arena_index_(arena_index),
      flags_(flags),
      hooks_(std::move(hooks)),
      info_log_(info_log) {
  assert(arena_index != 0);
}

void* JemallocNodumpAllocator::Allocate(size_t size) {
  return mallocx(size, flags_);
}

void JemallocNodumpAllocator::Deallocate(void* p) { dallocx(p, flags_); }

void* JemallocNodumpAllocator::Alloc(extent_hooks_t* extent, void* new_addr,
                                     size_t size, size_t alignment, bool* zero,
                                     bool* commit, unsigned arena_ind) {
  extent_alloc_t* original_alloc =
      original_alloc_.load(std::memory_order_relaxed);
  assert(original_alloc != nullptr);
  void* result = original_alloc(extent, new_addr, size, alignment, zero, commit,
                                arena_ind);
  if (result != nullptr) {
    int ret = madvise(result, size, MADV_DONTDUMP);
    if (ret != 0) {
      fprintf(
          stderr,
          "JemallocNodumpAllocator failed to set MADV_DONTDUMP, error code: %d",
          ret);
      assert(false);
    }
  }
  return result;
}

JemallocNodumpAllocator::~JemallocNodumpAllocator() {
  assert(arena_index_ != 0);
  std::string key = "arena." + ToString(arena_index_) + ".destroy";
  size_t zero = 0;
  int ret = mallctl(key.c_str(), nullptr, &zero, nullptr, 0);
  if (ret != 0) {
    ROCKS_LOG_ERROR(info_log_, "Failed to destroy arena, error code: %d", ret);
  }
}

size_t JemallocNodumpAllocator::UsableSize(void* p,
                                           size_t /*allocation_size*/) const {
  return malloc_usable_size(static_cast<void*>(p));
}

#endif  // ROCKSDB_JEMALLOC_NODUMP_ALLOCATOR

Status NewJemallocNodumpAllocator(
    const JemallocNodumpAllocatorOptions& options,
    std::shared_ptr<CacheAllocator>* cache_allocator) {
#ifndef ROCKSDB_JEMALLOC_NODUMP_ALLOCATOR
  (void)options;
  (void)cache_allocator;
  return Status::NotSupported(
      "JemallocNodumpAllocator only available with jemalloc version >= 5 "
      "and MADV_DONTDUMP is available.");
#else
  if (cache_allocator == nullptr) {
    return Status::InvalidArgument("cache_allocator is nullptr.");
  }

  // Create arena.
  unsigned arena_index = 0;
  size_t arena_index_size = sizeof(arena_index);
  int ret =
      mallctl("arenas.create", &arena_index, &arena_index_size, nullptr, 0);
  if (ret != 0) {
    ROCKS_LOG_ERROR(options.info_log,
                    "Failed to create jemalloc arena, error code: %d", ret);
    return Status::Incomplete("Failed to create jemalloc arena, error code: " +
                              ToString(ret));
  }
  assert(arena_index != 0);
  int flags = MALLOCX_ARENA(arena_index) | MALLOCX_TCACHE_NONE;
  std::string key = "arena." + ToString(arena_index) + ".extent_hooks";

  // Read existing hooks.
  extent_hooks_t* hooks;
  size_t hooks_size = sizeof(hooks);
  ret = mallctl(key.c_str(), &hooks, &hooks_size, nullptr, 0);
  if (ret != 0) {
    std::string msg =
        "Failed to read existing hooks, error code: " + ToString(ret);
    ROCKS_LOG_ERROR(options.info_log,
                    "Failed to read existing hooks, error code: %d", ret);
    return Status::Incomplete("Failed to read existing hooks, error code: " +
                              ToString(ret));
  }

  // Store existing alloc.
  extent_alloc_t* original_alloc = hooks->alloc;
  extent_alloc_t* expected = nullptr;
  bool success __attribute__((__unused__)) =
      JemallocNodumpAllocator::original_alloc_.compare_exchange_strong(
          expected, original_alloc);
  assert(success || original_alloc == expected);

  // Set the custom hook.
  std::unique_ptr<extent_hooks_t> new_hooks(new extent_hooks_t(*hooks));
  new_hooks->alloc = &JemallocNodumpAllocator::Alloc;
  extent_hooks_t* hooks_ptr = new_hooks.get();
  ret = mallctl(key.c_str(), nullptr, nullptr, &hooks_ptr, sizeof(hooks_ptr));
  if (ret != 0) {
    ROCKS_LOG_ERROR(options.info_log,
                    "Failed to set custom hook, error code: %d", ret);
    return Status::Incomplete("Failed to set custom hook, error code: " +
                              ToString(ret));
  }

  // Create cache allocator.
  *cache_allocator = std::make_shared<JemallocNodumpAllocator>(
      arena_index, flags, std::move(new_hooks), options.info_log);
  return Status::OK();
#endif  // ROCKSDB_JEMALLOC_NODUMP_ALLOCATOR
}

}  // namespace rocksdb
