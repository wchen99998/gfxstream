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

#include "gfxstream/host/address_space_host_memory_allocator.h"

#include <array>

#include <gtest/gtest.h>

namespace gfxstream {
namespace host {
namespace {

constexpr uint64_t kBadGpa = 0x1234000;
constexpr uint64_t kGoodGpa1 = 0x10001000;
constexpr uint64_t kGoodGpa2 = 0x20002000;
constexpr uint64_t kMissingGpa = 0x30003000;

alignas(4096) std::array<uint8_t, 4096> kHostMem1 = {};
alignas(4096) std::array<uint8_t, 4096> kHostMem2 = {};
alignas(4096) std::array<uint8_t, 4096> kHostMemBad = {};

int AddMemoryMapping(uint64_t gpa, void*, uint64_t) {
    return gpa == kBadGpa ? 0 : 1;
}

int RemoveMemoryMapping(uint64_t, void*, uint64_t) {
    return 1;
}

void* GetHostPtr(uint64_t gpa) {
    switch (gpa) {
        case kGoodGpa1:
            return kHostMem1.data();
        case kGoodGpa2:
            return kHostMem2.data();
        case kBadGpa:
            return kHostMemBad.data();
        default:
            return nullptr;
    }
}

uint32_t GetGuestPageSize() {
    return 4096;
}

address_space_device_control_ops CreateDeviceControlOps() {
    address_space_device_control_ops ops = {};
    ops.add_memory_mapping = &AddMemoryMapping;
    ops.remove_memory_mapping = &RemoveMemoryMapping;
    ops.get_host_ptr = &GetHostPtr;
    return ops;
}

AddressSpaceHwFuncs CreateDeviceHwFuncs() {
    AddressSpaceHwFuncs hwFuncs = {};
    hwFuncs.getGuestPageSize = &GetGuestPageSize;
    return hwFuncs;
}

AddressSpaceDevicePingInfo CreateAllocateRequest(uint64_t physAddr) {
    AddressSpaceDevicePingInfo req = {};
    req.metadata = static_cast<uint64_t>(
        AddressSpaceHostMemoryAllocatorContext::HostMemoryAllocatorCommand::Allocate);
    req.phys_addr = physAddr;
    req.size = 1;
    return req;
}

AddressSpaceDevicePingInfo CreateUnallocateRequest(uint64_t physAddr) {
    AddressSpaceDevicePingInfo req = {};
    req.metadata = static_cast<uint64_t>(
        AddressSpaceHostMemoryAllocatorContext::HostMemoryAllocatorCommand::Unallocate);
    req.phys_addr = physAddr;
    return req;
}

}  // namespace

TEST(AddressSpaceHostMemoryAllocatorContext, GetDeviceType) {
    auto ops = CreateDeviceControlOps();
    auto hwFuncs = CreateDeviceHwFuncs();

    AddressSpaceHostMemoryAllocatorContext ctx(&ops, &hwFuncs);

    EXPECT_EQ(ctx.getDeviceType(), AddressSpaceDeviceType::HostMemoryAllocator);
}

TEST(AddressSpaceHostMemoryAllocatorContext, AllocateDeallocate) {
    auto ops = CreateDeviceControlOps();
    auto hwFuncs = CreateDeviceHwFuncs();

    AddressSpaceHostMemoryAllocatorContext ctx(&ops, &hwFuncs);

    auto req = CreateAllocateRequest(kGoodGpa1);
    ctx.perform(&req);
    EXPECT_EQ(req.metadata, 0u);

    req = CreateUnallocateRequest(kGoodGpa1);
    ctx.perform(&req);
    EXPECT_EQ(req.metadata, 0u);
}

TEST(AddressSpaceHostMemoryAllocatorContext, AllocateSamePhysAddr) {
    auto ops = CreateDeviceControlOps();
    auto hwFuncs = CreateDeviceHwFuncs();

    AddressSpaceHostMemoryAllocatorContext ctx(&ops, &hwFuncs);

    auto req = CreateAllocateRequest(kGoodGpa1);
    ctx.perform(&req);
    EXPECT_EQ(req.metadata, 0u);

    req = CreateAllocateRequest(kGoodGpa1);
    ctx.perform(&req);
    EXPECT_NE(req.metadata, 0u);

    req = CreateAllocateRequest(kGoodGpa2);
    ctx.perform(&req);
    EXPECT_EQ(req.metadata, 0u);

    req = CreateUnallocateRequest(kGoodGpa2);
    ctx.perform(&req);
    EXPECT_EQ(req.metadata, 0u);

    req = CreateUnallocateRequest(kGoodGpa1);
    ctx.perform(&req);
    EXPECT_EQ(req.metadata, 0u);

    req = CreateAllocateRequest(kGoodGpa1);
    ctx.perform(&req);
    EXPECT_EQ(req.metadata, 0u);

    req = CreateUnallocateRequest(kGoodGpa1);
    ctx.perform(&req);
    EXPECT_EQ(req.metadata, 0u);
}

TEST(AddressSpaceHostMemoryAllocatorContext, AllocateMappingFail) {
    auto ops = CreateDeviceControlOps();
    auto hwFuncs = CreateDeviceHwFuncs();

    AddressSpaceHostMemoryAllocatorContext ctx(&ops, &hwFuncs);

    auto req = CreateAllocateRequest(kBadGpa);
    ctx.perform(&req);
    EXPECT_NE(req.metadata, 0u);
}

TEST(AddressSpaceHostMemoryAllocatorContext, AllocateMissingHostPtrFail) {
    auto ops = CreateDeviceControlOps();
    auto hwFuncs = CreateDeviceHwFuncs();

    AddressSpaceHostMemoryAllocatorContext ctx(&ops, &hwFuncs);

    auto req = CreateAllocateRequest(kMissingGpa);
    ctx.perform(&req);
    EXPECT_NE(req.metadata, 0u);
}

TEST(AddressSpaceHostMemoryAllocatorContext, UnallocateTwice) {
    auto ops = CreateDeviceControlOps();
    auto hwFuncs = CreateDeviceHwFuncs();

    AddressSpaceHostMemoryAllocatorContext ctx(&ops, &hwFuncs);

    auto req = CreateAllocateRequest(kGoodGpa1);
    ctx.perform(&req);
    EXPECT_EQ(req.metadata, 0u);

    req = CreateUnallocateRequest(kGoodGpa1);
    ctx.perform(&req);
    EXPECT_EQ(req.metadata, 0u);

    req = CreateUnallocateRequest(kGoodGpa1);
    ctx.perform(&req);
    EXPECT_NE(req.metadata, 0u);
}

}  // namespace host
}  // namespace gfxstream
