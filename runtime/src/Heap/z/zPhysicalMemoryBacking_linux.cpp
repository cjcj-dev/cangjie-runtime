// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/linux/gc/z/zPhysicalMemoryBacking_linux.cpp:53-707. Differences
// that are HotSpot infrastructure: ZInitialize::error → LOG + _initialized
// stays false; is_init_completed() (no VM init flag: the hugetlbfs ENOSPC
// retry always runs its three attempts); SafeFetch32 (no signal-handler
// safe fetch: the tmpfs compat touch is a plain volatile read); os::Linux
// NUMA/THP helpers are the underlying syscalls.

#include "Heap/z/zPhysicalMemoryBacking_linux.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "Base/Globals.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zErrno.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zLargePages.inline.hpp"
#include "Heap/z/zMountPoint_linux.hpp"
#include "Heap/z/zNUMA.inline.hpp"

namespace MapleRuntime {

//
// Support for building on older Linux systems
//

// memfd_create(2) flags
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC                      0x0001U
#endif
#ifndef MFD_HUGETLB
#define MFD_HUGETLB                      0x0004U
#endif
#ifndef MFD_HUGE_2MB
#define MFD_HUGE_2MB                     0x54000000U
#endif

// open(2) flags
#ifndef O_TMPFILE
#define O_TMPFILE                        (020000000 | O_DIRECTORY)
#endif

// fallocate(2) flags
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE              0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE             0x02
#endif

// Filesystem types, see statfs(2)
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC                      0x01021994
#endif
#ifndef HUGETLBFS_MAGIC
#define HUGETLBFS_MAGIC                  0x958458f6
#endif

// Filesystem names
#define ZFILESYSTEM_TMPFS                "tmpfs"
#define ZFILESYSTEM_HUGETLBFS            "hugetlbfs"

// Proc file entry for max map mount
#define ZFILENAME_PROC_MAX_MAP_COUNT     "/proc/sys/vm/max_map_count"

// Sysfs file for transparent huge page on tmpfs
#define ZFILENAME_SHMEM_ENABLED          "/sys/kernel/mm/transparent_hugepage/shmem_enabled"

// Cangjie heap filename
#define ZFILENAME_HEAP                   "cangjie_heap"

// Preferred tmpfs mount points, ordered by priority
static const char* ZPreferredTmpfsMountpoints[] = {
  "/dev/shm",
  "/run/shm",
  nullptr
};

// Preferred hugetlbfs mount points, ordered by priority
static const char* ZPreferredHugetlbfsMountpoints[] = {
  "/dev/hugepages",
  "/hugepages",
  nullptr
};

static int z_fallocate_hugetlbfs_attempts = 3;
static bool z_fallocate_supported = true;

static int z_memfd_create(const char* name, unsigned int flags) {
  return static_cast<int>(syscall(SYS_memfd_create, name, flags));
}

static int z_fallocate(int fd, int mode, size_t offset, size_t length) {
  return static_cast<int>(syscall(SYS_fallocate, fd, mode, static_cast<off_t>(offset), static_cast<off_t>(length)));
}

ZPhysicalMemoryBacking::ZPhysicalMemoryBacking(size_t max_capacity)
  : _fd(-1),
    _size(0),
    _filesystem(0),
    _block_size(0),
    _available(0),
    _initialized(false) {

  // Create backing file
  _fd = create_fd(ZFILENAME_HEAP);
  if (_fd == -1) {
    LOG(RTLOG_ERROR, "Failed to create heap backing file");
    return;
  }

  // Truncate backing file
  while (ftruncate(_fd, static_cast<off_t>(max_capacity)) == -1) {
    if (errno != EINTR) {
      ZErrno err;
      LOG(RTLOG_ERROR, "Failed to truncate backing file (%s)", err.to_string());
      return;
    }
  }

  _size = max_capacity;

  // Get filesystem statistics
  struct statfs buf;
  if (fstatfs(_fd, &buf) == -1) {
    ZErrno err;
    LOG(RTLOG_ERROR, "Failed to determine filesystem type for backing file (%s)", err.to_string());
    return;
  }

  _filesystem = static_cast<uint64_t>(buf.f_type);
  _block_size = static_cast<size_t>(buf.f_bsize);
  _available = static_cast<size_t>(buf.f_bavail) * _block_size;

  VLOG(REPORT, "Heap Backing Filesystem: %s (0x%llx)",
       is_tmpfs() ? ZFILESYSTEM_TMPFS : is_hugetlbfs() ? ZFILESYSTEM_HUGETLBFS : "other",
       static_cast<unsigned long long>(_filesystem));

  // Make sure the filesystem type matches requested large page type
  if (ZLargePages::is_transparent() && !is_tmpfs()) {
    LOG(RTLOG_ERROR, "cjUseTransparentHugePages can only be enabled when using a %s filesystem",
        ZFILESYSTEM_TMPFS);
    return;
  }

  if (ZLargePages::is_transparent() && !tmpfs_supports_transparent_huge_pages()) {
    LOG(RTLOG_ERROR, "cjUseTransparentHugePages on a %s filesystem not supported by kernel",
        ZFILESYSTEM_TMPFS);
    return;
  }

  if (ZLargePages::is_explicit() && !is_hugetlbfs()) {
    LOG(RTLOG_ERROR, "cjUseLargePages (without cjUseTransparentHugePages) can only be enabled "
        "when using a %s filesystem", ZFILESYSTEM_HUGETLBFS);
    return;
  }

  if (!ZLargePages::is_explicit() && is_hugetlbfs()) {
    LOG(RTLOG_ERROR, "cjUseLargePages must be enabled when using a %s filesystem",
        ZFILESYSTEM_HUGETLBFS);
    return;
  }

  // Make sure the filesystem block size is compatible
  if (ZBackingGranuleSize % _block_size != 0) {
    LOG(RTLOG_ERROR, "Filesystem backing the heap has incompatible block size (%zu)",
        _block_size);
    return;
  }

  if (is_hugetlbfs() && _block_size != ZGranuleSize) {
    LOG(RTLOG_ERROR, "%s filesystem has unexpected block size %zu (expected %zu)",
        ZFILESYSTEM_HUGETLBFS, _block_size, ZGranuleSize);
    return;
  }

  // Successfully initialized
  _initialized = true;
}

ZPhysicalMemoryBacking::~ZPhysicalMemoryBacking() {
  // ZGC keeps the backing file for the whole VM lifetime; gtests here build
  // and tear down backings, so release the descriptor.
  if (_fd != -1) {
    close(_fd);
    _fd = -1;
  }
}

int ZPhysicalMemoryBacking::create_mem_fd(const char* name) const {
  assert(ZGranuleSize == 2 * MB);

  // Create file name
  char filename[PATH_MAX];
  snprintf(filename, sizeof(filename), "%s%s", name, ZLargePages::is_explicit() ? ".hugetlb" : "");

  // Create file
  const unsigned int extra_flags = ZLargePages::is_explicit() ? (MFD_HUGETLB | MFD_HUGE_2MB) : 0;
  const int fd = z_memfd_create(filename, MFD_CLOEXEC | extra_flags);
  if (fd == -1) {
    ZErrno err;
    DLOG(REPORT, "Failed to create memfd file (%s)",
         (ZLargePages::is_explicit() && (err == EINVAL || err == ENODEV)) ?
         "Hugepages (2M) not available" : err.to_string());
    return -1;
  }

  VLOG(REPORT, "Heap Backing File: /memfd:%s", filename);

  return fd;
}

int ZPhysicalMemoryBacking::create_file_fd(const char* name) const {
  const char* const filesystem = ZLargePages::is_explicit()
                                 ? ZFILESYSTEM_HUGETLBFS
                                 : ZFILESYSTEM_TMPFS;
  const char** const preferred_mountpoints = ZLargePages::is_explicit()
                                             ? ZPreferredHugetlbfsMountpoints
                                             : ZPreferredTmpfsMountpoints;

  // Find mountpoint
  ZMountPoint mountpoint(filesystem, preferred_mountpoints);
  if (mountpoint.get() == nullptr) {
    LOG(RTLOG_ERROR, "Use cjAllocateHeapAt to specify the path to a %s filesystem", filesystem);
    return -1;
  }

  // Try to create an anonymous file using the O_TMPFILE flag. Note that this
  // flag requires kernel >= 3.11. If this fails we fall back to open/unlink.
  const int fd_anon = open(mountpoint.get(), O_TMPFILE|O_EXCL|O_RDWR|O_CLOEXEC, S_IRUSR|S_IWUSR);
  if (fd_anon == -1) {
    ZErrno err;
    DLOG(REPORT, "Failed to create anonymous file in %s (%s)", mountpoint.get(),
         (err == EINVAL ? "Not supported" : err.to_string()));
  } else {
    // Get inode number for anonymous file
    struct stat stat_buf;
    if (fstat(fd_anon, &stat_buf) == -1) {
      ZErrno err;
      LOG(RTLOG_ERROR, "Failed to determine inode number for anonymous file (%s)", err.to_string());
      return -1;
    }

    VLOG(REPORT, "Heap Backing File: %s/#%llu", mountpoint.get(), static_cast<unsigned long long>(stat_buf.st_ino));

    return fd_anon;
  }

  DLOG(REPORT, "Falling back to open/unlink");

  // Create file name
  char filename[PATH_MAX];
  snprintf(filename, sizeof(filename), "%s/%s.%d", mountpoint.get(), name, static_cast<int>(getpid()));

  // Create file
  const int fd = open(filename, O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC, S_IRUSR|S_IWUSR);
  if (fd == -1) {
    ZErrno err;
    LOG(RTLOG_ERROR, "Failed to create file %s (%s)", filename, err.to_string());
    return -1;
  }

  // Unlink file
  if (unlink(filename) == -1) {
    ZErrno err;
    LOG(RTLOG_ERROR, "Failed to unlink file %s (%s)", filename, err.to_string());
    return -1;
  }

  VLOG(REPORT, "Heap Backing File: %s", filename);

  return fd;
}

int ZPhysicalMemoryBacking::create_fd(const char* name) const {
  if (getenv("cjAllocateHeapAt") == nullptr) {
    // If the path is not explicitly specified, then we first try to create a memfd file
    // instead of looking for a tmpfd/hugetlbfs mount point. Note that memfd_create() might
    // not be supported at all (requires kernel >= 3.17), or it might not support large
    // pages (requires kernel >= 4.14). If memfd_create() fails, then we try to create a
    // file on an accessible tmpfs or hugetlbfs mount point.
    const int fd = create_mem_fd(name);
    if (fd != -1) {
      return fd;
    }

    DLOG(REPORT, "Falling back to searching for an accessible mount point");
  }

  return create_file_fd(name);
}

bool ZPhysicalMemoryBacking::is_initialized() const {
  return _initialized;
}

void ZPhysicalMemoryBacking::warn_available_space(size_t max_capacity) const {
  // Note that the available space on a tmpfs or a hugetlbfs filesystem
  // will be zero if no size limit was specified when it was mounted.
  if (_available == 0) {
    // No size limit set, skip check
    VLOG(REPORT, "Available space on backing filesystem: N/A");
    return;
  }

  VLOG(REPORT, "Available space on backing filesystem: %zuM", _available / MB);

  // Warn if the filesystem doesn't currently have enough space available to hold
  // the max heap size. The max heap size will be capped if we later hit this limit
  // when trying to expand the heap.
  if (_available < max_capacity) {
    LOG(RTLOG_WARNING, "***** WARNING! INCORRECT SYSTEM CONFIGURATION DETECTED! *****");
    LOG(RTLOG_WARNING, "Not enough space available on the backing filesystem to hold the current max Cangjie heap");
    LOG(RTLOG_WARNING, "size (%zuM). Please adjust the size of the backing filesystem accordingly "
        "(available", max_capacity / MB);
    LOG(RTLOG_WARNING, "space is currently %zuM). Continuing execution with the current filesystem "
        "size could", _available / MB);
    LOG(RTLOG_WARNING, "lead to a premature OutOfMemoryError being thrown, due to failure to commit memory.");
  }
}

void ZPhysicalMemoryBacking::warn_max_map_count(size_t max_capacity) const {
  const char* const filename = ZFILENAME_PROC_MAX_MAP_COUNT;
  FILE* const file = fopen(filename, "r");
  if (file == nullptr) {
    // Failed to open file, skip check
    DLOG(REPORT, "Failed to open %s", filename);
    return;
  }

  size_t actual_max_map_count = 0;
  const int result = fscanf(file, "%zu", &actual_max_map_count);
  fclose(file);
  if (result != 1) {
    // Failed to read file, skip check
    DLOG(REPORT, "Failed to read %s", filename);
    return;
  }

  // The required max map count is impossible to calculate exactly since subsystems
  // other than ZGC are also creating memory mappings, and we have no control over that.
  // However, ZGC tends to create the most mappings and dominate the total count.
  // In the worst cases, ZGC will map each granule three times, i.e. once per heap view.
  // We speculate that we need another 20% to allow for non-ZGC subsystems to map memory.
  const size_t required_max_map_count = static_cast<size_t>((max_capacity / ZGranuleSize) * 3 * 1.2);
  if (actual_max_map_count < required_max_map_count) {
    LOG(RTLOG_WARNING, "***** WARNING! INCORRECT SYSTEM CONFIGURATION DETECTED! *****");
    LOG(RTLOG_WARNING, "The system limit on number of memory mappings per process might be too low for the given");
    LOG(RTLOG_WARNING, "max Cangjie heap size (%zuM). Please adjust %s to allow for at",
        max_capacity / MB, filename);
    LOG(RTLOG_WARNING, "least %zu mappings (current limit is %zu). Continuing execution "
        "with the current", required_max_map_count, actual_max_map_count);
    LOG(RTLOG_WARNING, "limit could lead to a premature OutOfMemoryError being thrown, due to failure to map memory.");
  }
}

void ZPhysicalMemoryBacking::warn_commit_limits(size_t max_capacity) const {
  // Warn if available space is too low
  warn_available_space(max_capacity);

  // Warn if max map count is too low
  warn_max_map_count(max_capacity);
}

bool ZPhysicalMemoryBacking::is_tmpfs() const {
  return _filesystem == TMPFS_MAGIC;
}

bool ZPhysicalMemoryBacking::is_hugetlbfs() const {
  return _filesystem == HUGETLBFS_MAGIC;
}

bool ZPhysicalMemoryBacking::tmpfs_supports_transparent_huge_pages() const {
  // If the shmem_enabled file exists and is readable then we
  // know the kernel supports transparent huge pages for tmpfs.
  return access(ZFILENAME_SHMEM_ENABLED, R_OK) == 0;
}

static void pretouch_memory(char* start, char* end, size_t page_size) {
  for (char* p = start; p < end; p += page_size) {
    volatile char* const vp = p;
    *vp = *vp;
  }
}

ZErrno ZPhysicalMemoryBacking::fallocate_compat_mmap_hugetlbfs(zbacking_offset offset, size_t length, bool touch) const {
  // On hugetlbfs, mapping a file segment will fail immediately, without
  // the need to touch the mapped pages first, if there aren't enough huge
  // pages available to back the mapping.
  void* const addr = mmap(nullptr, length, PROT_READ|PROT_WRITE, MAP_SHARED, _fd, static_cast<off_t>(untype(offset)));
  if (addr == MAP_FAILED) {
    // Failed
    return errno;
  }

  // Once mapped, the huge pages are only reserved. We need to touch them
  // to associate them with the file segment. Note that we can not punch
  // hole in file segments which only have reserved pages.
  if (touch) {
    char* const start = (char*)addr;
    char* const end = start + length;
    pretouch_memory(start, end, _block_size);
  }

  // Unmap again. From now on, the huge pages that were mapped are allocated
  // to this file. There's no risk of getting a SIGBUS when mapping and
  // touching these pages again.
  if (munmap(addr, length) == -1) {
    // Failed
    return errno;
  }

  // Success
  return 0;
}

static bool safe_touch_mapping(void* addr, size_t length, size_t page_size) {
  char* const start = (char*)addr;
  char* const end = start + length;

  // HotSpot uses SafeFetch32 to survive a SIGBUS here. Without a
  // signal-handler based safe fetch a failed backing allocation on tmpfs
  // (fallocate(2) unsupported, kernel < 3.5) terminates the process.
  for (char *p = start; p < end; p += page_size) {
    volatile int* const vp = reinterpret_cast<volatile int*>(p);
    (void)*vp;
  }

  // Success
  return true;
}

ZErrno ZPhysicalMemoryBacking::fallocate_compat_mmap_tmpfs(zbacking_offset offset, size_t length) const {
  // On tmpfs, we need to touch the mapped pages to figure out
  // if there are enough pages available to back the mapping.
  void* const addr = mmap(nullptr, length, PROT_READ|PROT_WRITE, MAP_SHARED, _fd, static_cast<off_t>(untype(offset)));
  if (addr == MAP_FAILED) {
    // Failed
    return errno;
  }

  // Maybe madvise the mapping to use transparent huge pages
  if (ZLargePages::is_transparent()) {
    (void)madvise(addr, length, MADV_HUGEPAGE);
  }

  // Touch the mapping (safely) to make sure it's backed by memory
  const bool backed = safe_touch_mapping(addr, length, _block_size);

  // Unmap again. If successfully touched, the backing memory will
  // be allocated to this file. There's no risk of getting a SIGBUS
  // when mapping and touching these pages again.
  if (munmap(addr, length) == -1) {
    // Failed
    return errno;
  }

  // Success
  return backed ? 0 : ENOMEM;
}

ZErrno ZPhysicalMemoryBacking::fallocate_compat_pwrite(zbacking_offset offset, size_t length) const {
  uint8_t data = 0;

  // Allocate backing memory by writing to each block
  for (zbacking_offset pos = offset; pos < offset + length; pos += _block_size) {
    if (pwrite(_fd, &data, sizeof(data), static_cast<off_t>(untype(pos))) == -1) {
      // Failed
      return errno;
    }
  }

  // Success
  return 0;
}

ZErrno ZPhysicalMemoryBacking::fallocate_fill_hole_compat(zbacking_offset offset, size_t length) const {
  // fallocate(2) is only supported by tmpfs since Linux 3.5, and by hugetlbfs
  // since Linux 4.3. When fallocate(2) is not supported we emulate it using
  // mmap/munmap (for hugetlbfs and tmpfs with transparent huge pages) or pwrite
  // (for tmpfs without transparent huge pages and other filesystem types).
  if (ZLargePages::is_explicit()) {
    return fallocate_compat_mmap_hugetlbfs(offset, length, false /* touch */);
  } else if (ZLargePages::is_transparent()) {
    return fallocate_compat_mmap_tmpfs(offset, length);
  } else {
    return fallocate_compat_pwrite(offset, length);
  }
}

ZErrno ZPhysicalMemoryBacking::fallocate_fill_hole_syscall(zbacking_offset offset, size_t length) const {
  const int mode = 0; // Allocate
  const int res = z_fallocate(_fd, mode, untype(offset), length);
  if (res == -1) {
    // Failed
    return errno;
  }

  // Success
  return 0;
}

ZErrno ZPhysicalMemoryBacking::fallocate_fill_hole(zbacking_offset offset, size_t length) const {
  // Using compat mode is more efficient when allocating space on hugetlbfs.
  // Note that allocating huge pages this way will only reserve them, and not
  // associate them with segments of the file. We must guarantee that we at
  // some point touch these segments, otherwise we can not punch hole in them.
  // Also note that we need to use compat mode when using transparent huge pages,
  // since we need to use madvise(2) on the mapping before the page is allocated.
  if (z_fallocate_supported && !ZLargePages::is_enabled()) {
     const ZErrno err = fallocate_fill_hole_syscall(offset, length);
     if (!err) {
       // Success
       return 0;
     }

     if (err != ENOSYS && err != EOPNOTSUPP) {
       // Failed
       return err;
     }

     // Not supported
     DLOG(REPORT, "Falling back to fallocate() compatibility mode");
     z_fallocate_supported = false;
  }

  return fallocate_fill_hole_compat(offset, length);
}

ZErrno ZPhysicalMemoryBacking::fallocate_punch_hole(zbacking_offset offset, size_t length) const {
  if (ZLargePages::is_explicit()) {
    // We can only punch hole in pages that have been touched. Non-touched
    // pages are only reserved, and not associated with any specific file
    // segment. We don't know which pages have been previously touched, so
    // we always touch them here to guarantee that we can punch hole.
    const ZErrno err = fallocate_compat_mmap_hugetlbfs(offset, length, true /* touch */);
    if (err) {
      // Failed
      return err;
    }
  }

  const int mode = FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE;
  if (z_fallocate(_fd, mode, untype(offset), length) == -1) {
    // Failed
    return errno;
  }

  // Success
  return 0;
}

ZErrno ZPhysicalMemoryBacking::split_and_fallocate(bool punch_hole, zbacking_offset offset, size_t length) const {
  // Try first half
  const zbacking_offset offset0 = offset;
  const size_t length0 = AlignUp(length / 2, _block_size);
  const ZErrno err0 = fallocate(punch_hole, offset0, length0);
  if (err0) {
    return err0;
  }

  // Try second half
  const zbacking_offset offset1 = offset0 + length0;
  const size_t length1 = length - length0;
  const ZErrno err1 = fallocate(punch_hole, offset1, length1);
  if (err1) {
    return err1;
  }

  // Success
  return 0;
}

ZErrno ZPhysicalMemoryBacking::fallocate(bool punch_hole, zbacking_offset offset, size_t length) const {
  assert(untype(offset) % _block_size == 0);
  assert(length % _block_size == 0);

  const ZErrno err = punch_hole ? fallocate_punch_hole(offset, length) : fallocate_fill_hole(offset, length);
  if (err == EINTR && length > _block_size) {
    // Calling fallocate(2) with a large length can take a long time to
    // complete. When running profilers, such as VTune, this syscall will
    // be constantly interrupted by signals. Expanding the file in smaller
    // steps avoids this problem.
    return split_and_fallocate(punch_hole, offset, length);
  }

  return err;
}

bool ZPhysicalMemoryBacking::commit_inner(zbacking_offset offset, size_t length) const {
  DLOG(ALLOC, "Committing memory: %zuM-%zuM (%zuM)",
       untype(offset) / MB, untype(to_zbacking_offset_end(offset, length)) / MB, length / MB);

retry:
  const ZErrno err = fallocate(false /* punch_hole */, offset, length);
  if (err) {
    if (err == ENOSPC && ZLargePages::is_explicit() && z_fallocate_hugetlbfs_attempts-- > 0) {
      // If we fail to allocate during initialization, due to lack of space on
      // the hugetlbfs filesystem, then we wait and retry a few times before
      // giving up. Otherwise there is a risk that running JVMs back-to-back
      // will fail, since there is a delay between process termination and the
      // huge pages owned by that process being returned to the huge page pool
      // and made available for new allocations.
      DLOG(REPORT, "Failed to commit memory (%s), retrying", err.to_string());

      // Wait and retry in one second, in the hope that huge pages will be
      // available by then.
      sleep(1);
      goto retry;
    }

    // Failed
    LOG(RTLOG_ERROR, "Failed to commit memory (%s)", err.to_string());
    return false;
  }

  // Success
  return true;
}

size_t ZPhysicalMemoryBacking::commit_numa_preferred(zbacking_offset offset, size_t length, uint32_t numa_id) const {
  // Setup NUMA policy to allocate memory from a preferred node
  // (os::Linux::numa_set_preferred → set_mempolicy(MPOL_PREFERRED)). The
  // numa_id → node mapping is the sealed topology until A03n (ZNUMA::numa_id_to_node).
  constexpr int kMpolPreferred = 1;
  constexpr unsigned long kMaxNumaNodes = sizeof(unsigned long) * 8;
  const uint32_t node = NumaTopology::SealProcessTopology().NodeAt(numa_id);
  unsigned long mask = node < kMaxNumaNodes ? (1UL << node) : 0UL;
  if (syscall(SYS_set_mempolicy, kMpolPreferred, &mask, kMaxNumaNodes) != 0) {
    LOG(RTLOG_WARNING, "backing NUMA preference failed: %d", errno);
  }

  const size_t committed = commit_default(offset, length);

  // Restore NUMA policy
  if (syscall(SYS_set_mempolicy, kMpolPreferred, nullptr, 0UL) != 0) {
    LOG(RTLOG_WARNING, "backing NUMA preference reset failed: %d", errno);
  }

  return committed;
}

size_t ZPhysicalMemoryBacking::commit_default(zbacking_offset offset, size_t length) const {
  // Try to commit the whole region
  if (commit_inner(offset, length)) {
    // Success
    return length;
  }

  // Failed, try to commit as much as possible
  zbacking_offset start = offset;
  zbacking_offset_end end = to_zbacking_offset_end(offset, length);

  for (;;) {
    length = AlignDown((end - start) / 2, ZBackingGranuleSize);
    if (length < ZBackingGranuleSize) {
      // Done, don't commit more
      return start - offset;
    }

    if (commit_inner(start, length)) {
      // Success, try commit more
      start += length;
    } else {
      // Failed, try commit less
      end -= length;
    }
  }
}

size_t ZPhysicalMemoryBacking::commit(zbacking_offset offset, size_t length, uint32_t numa_id) const {
  if (NumaTopology::SealProcessTopology().Count() > 1 && !ZLargePages::is_explicit()) {
    // The memory is required to be preferred at the time it is paged in. As a
    // consequence we must prefer the memory when committing non-large pages.
    return commit_numa_preferred(offset, length, numa_id);
  }

  return commit_default(offset, length);
}

size_t ZPhysicalMemoryBacking::uncommit(zbacking_offset offset, size_t length) const {
  DLOG(ALLOC, "Uncommitting memory: %zuM-%zuM (%zuM)",
       untype(offset) / MB, untype(to_zbacking_offset_end(offset, length)) / MB, length / MB);

  const ZErrno err = fallocate(true /* punch_hole */, offset, length);
  if (err) {
    LOG(RTLOG_ERROR, "Failed to uncommit memory (%s)", err.to_string());
    return 0;
  }

  return length;
}

void ZPhysicalMemoryBacking::map(zaddress_unsafe addr, size_t size, zbacking_offset offset) const {
  const void* const res = mmap((void*)untype(addr), size, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_SHARED, _fd, static_cast<off_t>(untype(offset)));
  if (res == MAP_FAILED) {
    ZErrno err;
    LOG(RTLOG_FATAL, "Failed to map memory (%s)", err.to_string());
  }
}

void ZPhysicalMemoryBacking::unmap(zaddress_unsafe addr, size_t size) const {
  // Note that we must keep the address space reservation intact and just detach
  // the backing memory. For this reason we map a new anonymous, non-accessible
  // and non-reserved page over the mapping instead of actually unmapping.
  const void* const res = mmap((void*)untype(addr), size, PROT_NONE, MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
  if (res == MAP_FAILED) {
    ZErrno err;
    LOG(RTLOG_FATAL, "Failed to map memory (%s)", err.to_string());
  }
}

} // namespace MapleRuntime
