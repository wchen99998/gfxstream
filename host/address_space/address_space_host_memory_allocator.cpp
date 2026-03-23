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

#include <cstdlib>

#include "gfxstream/common/logging.h"

namespace gfxstream {
namespace host {
namespace {

size_t AlignTo(size_t value, size_t alignment) {
    return (value + alignment - 1) & (~(alignment - 1));
}

bool IsAsgTraceEnabled() {
    static int enabled = -1;
    if (enabled == -1) {
        const char* env = std::getenv("QEMU_RUTABAGA_TRACE_ASG");
        enabled = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    }
    return enabled;
}

}  // namespace

AddressSpaceHostMemoryAllocatorContext::AddressSpaceHostMemoryAllocatorContext(
    const address_space_device_control_ops* ops, const AddressSpaceHwFuncs* hw)
    : mOps(ops), mHw(hw) {}

AddressSpaceHostMemoryAllocatorContext::~AddressSpaceHostMemoryAllocatorContext() {
    clear();
}

void AddressSpaceHostMemoryAllocatorContext::perform(AddressSpaceDevicePingInfo* info) {
    uint64_t result = ~0ULL;
    const auto command = static_cast<HostMemoryAllocatorCommand>(info->metadata);

    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO(
            "ASG host-allocator perform command=%llu phys=0x%llx size=0x%llx",
            static_cast<unsigned long long>(info->metadata),
            static_cast<unsigned long long>(info->phys_addr),
            static_cast<unsigned long long>(info->size));
    }

    switch (command) {
        case HostMemoryAllocatorCommand::Allocate:
            result = allocate(info);
            break;
        case HostMemoryAllocatorCommand::Unallocate:
            result = unallocate(info);
            break;
        default:
            break;
    }

    info->metadata = result;
    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO("ASG host-allocator result=0x%llx",
                       static_cast<unsigned long long>(result));
    }
}

void* AddressSpaceHostMemoryAllocatorContext::allocateImpl(uint64_t physAddr, uint64_t size) {
    if (!mOps || !mHw || !mOps->get_host_ptr) {
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_ERROR("ASG host-allocator missing ops=%p hw=%p get_host_ptr=%p", mOps,
                            mHw, mOps ? reinterpret_cast<const void*>(mOps->get_host_ptr) : nullptr);
        }
        return nullptr;
    }
    const uint64_t alignedSize = AlignTo(size, mHw->getGuestPageSize());

    void* hostPtr = mOps->get_host_ptr(physAddr);
    if (!hostPtr) {
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_ERROR(
                "ASG host-allocator could not resolve host ptr phys=0x%llx size=0x%llx aligned=0x%llx",
                static_cast<unsigned long long>(physAddr),
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(alignedSize));
        }
        return nullptr;
    }

    auto insertion = mPhysAddrToPtr.insert({physAddr, {hostPtr, alignedSize}});
    if (!insertion.second) {
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_ERROR("ASG host-allocator duplicate phys=0x%llx",
                            static_cast<unsigned long long>(physAddr));
        }
        return nullptr;
    }

    if (!mOps->add_memory_mapping(physAddr, hostPtr, alignedSize)) {
        mPhysAddrToPtr.erase(insertion.first);
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_ERROR(
                "ASG host-allocator add_memory_mapping failed phys=0x%llx ptr=%p size=0x%llx",
                static_cast<unsigned long long>(physAddr), hostPtr,
                static_cast<unsigned long long>(alignedSize));
        }
        return nullptr;
    }

    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO(
            "ASG host-allocator mapped phys=0x%llx ptr=%p size=0x%llx aligned=0x%llx",
            static_cast<unsigned long long>(physAddr), hostPtr,
            static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(alignedSize));
    }
    return hostPtr;
}

uint64_t AddressSpaceHostMemoryAllocatorContext::allocate(AddressSpaceDevicePingInfo* info) {
    return allocateImpl(info->phys_addr, info->size) ? 0 : ~0ULL;
}

uint64_t AddressSpaceHostMemoryAllocatorContext::unallocate(AddressSpaceDevicePingInfo* info) {
    const uint64_t physAddr = info->phys_addr;
    const auto it = mPhysAddrToPtr.find(physAddr);
    if (it == mPhysAddrToPtr.end()) {
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_ERROR("ASG host-allocator unallocate missing phys=0x%llx",
                            static_cast<unsigned long long>(physAddr));
        }
        return ~0ULL;
    }

    void* hostPtr = it->second.first;
    const uint64_t size = it->second.second;

    if (!mOps->remove_memory_mapping(physAddr, hostPtr, size)) {
        GFXSTREAM_FATAL(
            "Failed to remove host memory mapping phys=0x%llx ptr=%p size=0x%llx",
            static_cast<unsigned long long>(physAddr), hostPtr,
            static_cast<unsigned long long>(size));
    }

    mPhysAddrToPtr.erase(it);
    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO("ASG host-allocator unmapped phys=0x%llx ptr=%p size=0x%llx",
                       static_cast<unsigned long long>(physAddr), hostPtr,
                       static_cast<unsigned long long>(size));
    }
    return 0;
}

AddressSpaceDeviceType AddressSpaceHostMemoryAllocatorContext::getDeviceType() const {
    return AddressSpaceDeviceType::HostMemoryAllocator;
}

void AddressSpaceHostMemoryAllocatorContext::save(gfxstream::Stream* stream) const {
    stream->putBe32(mPhysAddrToPtr.size());

    for (const auto& [physAddr, ptrAndSize] : mPhysAddrToPtr) {
        stream->putBe64(physAddr);
        stream->putBe64(ptrAndSize.second);
        stream->write(ptrAndSize.first, ptrAndSize.second);
    }
}

bool AddressSpaceHostMemoryAllocatorContext::load(gfxstream::Stream* stream) {
    clear();

    const size_t numAddrs = stream->getBe32();
    for (size_t i = 0; i < numAddrs; ++i) {
        const uint64_t physAddr = stream->getBe64();
        const uint64_t size = stream->getBe64();
        void* mem = allocateImpl(physAddr, size);
        if (!mem) {
            return false;
        }
        if (stream->read(mem, size) != static_cast<ssize_t>(size)) {
            return false;
        }
    }

    return true;
}

void AddressSpaceHostMemoryAllocatorContext::clear() {
    for (const auto& [physAddr, ptrAndSize] : mPhysAddrToPtr) {
        void* hostPtr = ptrAndSize.first;
        const uint64_t size = ptrAndSize.second;
        if (!mOps->remove_memory_mapping(physAddr, hostPtr, size)) {
            GFXSTREAM_FATAL(
                "Failed to remove host memory mapping phys=0x%llx ptr=%p size=0x%llx",
                static_cast<unsigned long long>(physAddr), hostPtr,
                static_cast<unsigned long long>(size));
        }
    }
    mPhysAddrToPtr.clear();
}

}  // namespace host
}  // namespace gfxstream
