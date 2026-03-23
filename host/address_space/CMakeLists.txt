# Copyright 2025 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if (NOT TARGET gfxstream_host_address_space)
    add_library(
        gfxstream_host_address_space.headers
        INTERFACE)
    target_include_directories(
        gfxstream_host_address_space.headers
        INTERFACE
        include)
    target_link_libraries(
        gfxstream_host_address_space.headers
        INTERFACE
        gfxstream_backend_headers)

    add_library(
        gfxstream_host_address_space
        address_space_device.cpp
        address_space_host_memory_allocator.cpp
        address_space_graphics.cpp
        address_space_shared_slots_host_memory_allocator.cpp
        ring_buffer.cpp
        sub_allocator.cpp)
    target_link_libraries(
        gfxstream_host_address_space
        PUBLIC
        gfxstream_common_base
        gfxstream_host_address_space.headers
        gfxstream_common_logging)

    if (ENABLE_VKCEREAL_TESTS)
        add_executable(
            gfxstream_host_address_space_unittests
            address_space_host_memory_allocator_unittest.cpp
            address_space_shared_slots_host_memory_allocator_unittest.cpp
            ring_buffer_unittest.cpp)
        target_link_libraries(
            gfxstream_host_address_space_unittests
            PUBLIC
            gfxstream_host_address_space
            gtest_main)
    endif()
endif()
