// Copyright 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "gfxstream/host/address_space_shared_slots_host_memory_allocator.h"

#include <gtest/gtest.h>

namespace gfxstream {
namespace host {
namespace {

using SharedAllocatorContext = AddressSpaceSharedSlotsHostMemoryAllocatorContext;
using MemBlock = SharedAllocatorContext::MemBlock;
using FreeSubblocks = MemBlock::FreeSubblocks;

int AddMemoryMapping(uint64_t, void*, uint64_t) {
    return 1;
}

int RemoveMemoryMapping(uint64_t, void*, uint64_t) {
    return 1;
}

address_space_device_control_ops CreateDeviceControlOps() {
    address_space_device_control_ops ops = {};
    ops.add_memory_mapping = &AddMemoryMapping;
    ops.remove_memory_mapping = &RemoveMemoryMapping;
    return ops;
}

uint64_t GetPhysAddrStartLocked() {
    return 2020;
}

int AllocSharedHostRegionLocked(uint64_t pageAlignedSize, uint64_t* offset) {
    *offset = pageAlignedSize * 10;
    return 0;
}

int FreeSharedHostRegionLocked(uint64_t) {
    return 0;
}

AddressSpaceHwFuncs CreateHwFuncs() {
    AddressSpaceHwFuncs hw = {};
    hw.allocSharedHostRegionLocked = &AllocSharedHostRegionLocked;
    hw.freeSharedHostRegionLocked = &FreeSharedHostRegionLocked;
    hw.getPhysAddrStartLocked = &GetPhysAddrStartLocked;
    return hw;
}

}  // namespace

TEST(MemBlockTest, FindFreeSubblock) {
    FreeSubblocks freeSubblocks;
    EXPECT_EQ(MemBlock::findFreeSubblock(&freeSubblocks, 11), freeSubblocks.end());

    freeSubblocks[100] = 10;
    EXPECT_EQ(MemBlock::findFreeSubblock(&freeSubblocks, 11), freeSubblocks.end());

    auto it = MemBlock::findFreeSubblock(&freeSubblocks, 7);
    ASSERT_NE(it, freeSubblocks.end());
    EXPECT_EQ(it->first, 100u);
    EXPECT_EQ(it->second, 10u);

    freeSubblocks[200] = 6;
    it = MemBlock::findFreeSubblock(&freeSubblocks, 7);
    ASSERT_NE(it, freeSubblocks.end());
    EXPECT_EQ(it->first, 100u);
    EXPECT_EQ(it->second, 10u);

    freeSubblocks[300] = 8;
    it = MemBlock::findFreeSubblock(&freeSubblocks, 7);
    ASSERT_NE(it, freeSubblocks.end());
    EXPECT_EQ(it->first, 300u);
    EXPECT_EQ(it->second, 8u);
}

TEST(MemBlockTest, TryMergeSubblocksNoMerge) {
    FreeSubblocks freeSubblocks;
    auto it = freeSubblocks.insert({10, 5}).first;
    auto jt = freeSubblocks.insert({20, 5}).first;

    auto merged = MemBlock::tryMergeSubblocks(&freeSubblocks, it, it, jt);

    EXPECT_EQ(freeSubblocks.size(), 2u);
    EXPECT_EQ(freeSubblocks[10], 5u);
    EXPECT_EQ(freeSubblocks[20], 5u);
    EXPECT_EQ(merged, it);
}

TEST(MemBlockTest, TryMergeSubblocksMerge) {
    FreeSubblocks freeSubblocks;
    auto it = freeSubblocks.insert({10, 10}).first;
    auto jt = freeSubblocks.insert({20, 5}).first;

    auto merged = MemBlock::tryMergeSubblocks(&freeSubblocks, it, it, jt);

    EXPECT_EQ(freeSubblocks.size(), 1u);
    EXPECT_EQ(freeSubblocks[10], 15u);
    ASSERT_NE(merged, freeSubblocks.end());
    EXPECT_EQ(merged->first, 10u);
    EXPECT_EQ(merged->second, 15u);
}

TEST(MemBlockTest, Allocate) {
    const auto ops = CreateDeviceControlOps();
    const auto hw = CreateHwFuncs();

    MemBlock block(&ops, &hw, 100);
    EXPECT_TRUE(block.isAllFree());
    EXPECT_EQ(block.physBase, 2020u + 100u * 10u);

    EXPECT_EQ(block.allocate(110), 0u);

    auto physAddr = block.allocate(50);
    EXPECT_GE(physAddr, 2020u + 100u * 10u);

    physAddr = block.allocate(47);
    EXPECT_GE(physAddr, 2020u + 100u * 10u);

    physAddr = block.allocate(2);
    EXPECT_GE(physAddr, 2020u + 100u * 10u);

    EXPECT_EQ(block.allocate(2), 0u);
}

TEST(MemBlockTest, Unallocate) {
    const auto ops = CreateDeviceControlOps();
    const auto hw = CreateHwFuncs();

    MemBlock block(&ops, &hw, 100);
    const auto physBase = block.physBase;

    const auto a = block.allocate(30);
    const auto b = block.allocate(40);
    const auto c = block.allocate(20);

    EXPECT_FALSE(block.isAllFree());

    block.unallocate(b, 40);
    EXPECT_FALSE(block.isAllFree());

    block.unallocate(a, 30);
    EXPECT_FALSE(block.isAllFree());

    block.unallocate(c, 20);
    EXPECT_TRUE(block.isAllFree());
    EXPECT_EQ(block.physBase, physBase);
}

}  // namespace host
}  // namespace gfxstream
