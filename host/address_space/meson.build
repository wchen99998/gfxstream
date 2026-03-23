# Copyright 2025 Android Open Source Project
# SPDX-License-Identifier: Apache-2.0

inc_host_address_space = include_directories('include')
inc_host_backend_public = include_directories('../backend/include')

files_lib_host_address_space = files(
  'address_space_device.cpp',
  'address_space_host_memory_allocator.cpp',
  'address_space_graphics.cpp',
  'address_space_shared_slots_host_memory_allocator.cpp',
  'ring_buffer.cpp',
  'sub_allocator.cpp',
)

lib_host_address_space = static_library(
  'host_address_space',
  files_lib_host_address_space,
  include_directories: [
    inc_host_address_space,
    inc_host_backend_public,
    inc_common_base,
    inc_gfxstream_server,
    inc_common_logging,
  ],
  link_with: [lib_common_base, lib_common_logging],
)
