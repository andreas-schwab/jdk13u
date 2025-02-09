/*
 * Copyright (c) 2015, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#ifndef OS_CPU_LINUX_RISCV64_GC_Z_ZBACKINGFILE_LINUX_RISCV64_HPP
#define OS_CPU_LINUX_RISCV64_GC_Z_ZBACKINGFILE_LINUX_RISCV64_HPP

#include "memory/allocation.hpp"

class ZErrno;

class ZBackingFile {
private:
  int      _fd;
  size_t   _size;
  uint64_t _filesystem;
  size_t   _block_size;
  size_t   _available;
  bool     _initialized;

  int create_mem_fd(const char* name) const;
  int create_file_fd(const char* name) const;
  int create_fd(const char* name) const;

  bool is_tmpfs() const;
  bool is_hugetlbfs() const;
  bool tmpfs_supports_transparent_huge_pages() const;

  ZErrno fallocate_compat_ftruncate(size_t size) const;
  ZErrno fallocate_compat_mmap(size_t offset, size_t length, bool reserve_only) const;
  ZErrno fallocate_compat_pwrite(size_t offset, size_t length) const;
  ZErrno fallocate_fill_hole_compat(size_t offset, size_t length);
  ZErrno fallocate_fill_hole_syscall(size_t offset, size_t length);
  ZErrno fallocate_fill_hole(size_t offset, size_t length);
  ZErrno fallocate_punch_hole(size_t offset, size_t length);
  ZErrno split_and_fallocate(bool punch_hole, size_t offset, size_t length);
  ZErrno fallocate(bool punch_hole, size_t offset, size_t length);

  bool commit_inner(size_t offset, size_t length);

public:
  ZBackingFile();

  bool is_initialized() const;

  int fd() const;
  size_t size() const;
  size_t available() const;

  size_t commit(size_t offset, size_t length);
  size_t uncommit(size_t offset, size_t length);
};

#endif // OS_CPU_LINUX_RISCV64_GC_Z_ZBACKINGFILE_LINUX_RISCV64_HPP
