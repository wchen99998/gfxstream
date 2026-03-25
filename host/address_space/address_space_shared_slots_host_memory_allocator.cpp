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

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <utility>

#include "gfxstream/AlignedBuf.h"
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

using SharedAllocatorContext = AddressSpaceSharedSlotsHostMemoryAllocatorContext;
using MemBlock = SharedAllocatorContext::MemBlock;

#if defined(__APPLE__) && defined(__arm64__)
constexpr uint32_t kAllocAlignment = 16384;
#else
constexpr uint32_t kAllocAlignment = 4096;
#endif

uint64_t AllocateAddressSpaceBlock(const AddressSpaceHwFuncs* hw, uint32_t size) {
    uint64_t offset = 0;
    if (!hw || hw->allocSharedHostRegionLocked(size, &offset)) {
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_ERROR("ASG shared-slots alloc-block failed hw=%p size=0x%x", hw, size);
        }
        return 0;
    }
    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO("ASG shared-slots alloc-block size=0x%x offset=0x%llx phys=0x%llx", size,
                       static_cast<unsigned long long>(offset),
                       static_cast<unsigned long long>(hw->getPhysAddrStartLocked() + offset));
    }
    return hw->getPhysAddrStartLocked() + offset;
}

uint64_t AllocateAddressSpaceBlockFixed(uint64_t gpa, const AddressSpaceHwFuncs* hw, uint32_t size) {
    uint64_t start;
    uint64_t offset;

    if (!hw) {
        return 0;
    }

    start = hw->getPhysAddrStartLocked();
    if (gpa < start) {
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_ERROR(
                "ASG shared-slots alloc-block-fixed invalid gpa=0x%llx start=0x%llx size=0x%x",
                static_cast<unsigned long long>(gpa),
                static_cast<unsigned long long>(start), size);
        }
        return 0;
    }

    offset = gpa - start;
    if (hw->allocSharedHostRegionFixedLocked(size, offset)) {
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_ERROR(
                "ASG shared-slots alloc-block-fixed failed gpa=0x%llx size=0x%x offset=0x%llx",
                static_cast<unsigned long long>(gpa), size,
                static_cast<unsigned long long>(offset));
        }
        return 0;
    }
    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO(
            "ASG shared-slots alloc-block-fixed gpa=0x%llx size=0x%x offset=0x%llx",
            static_cast<unsigned long long>(gpa), size,
            static_cast<unsigned long long>(offset));
    }
    return start + offset;
}

int FreeAddressBlock(const AddressSpaceHwFuncs* hw, uint64_t phys) {
    const uint64_t start = hw->getPhysAddrStartLocked();
    if (phys < start) {
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_ERROR("ASG shared-slots free-block invalid phys=0x%llx start=0x%llx",
                            static_cast<unsigned long long>(phys),
                            static_cast<unsigned long long>(start));
        }
        return -1;
    }
    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO("ASG shared-slots free-block phys=0x%llx offset=0x%llx",
                       static_cast<unsigned long long>(phys),
                       static_cast<unsigned long long>(phys - start));
    }
    return hw->freeSharedHostRegionLocked(phys - start);
}

std::map<uint64_t, MemBlock> gBlocks;
std::mutex gBlocksMutex;

std::pair<uint64_t, MemBlock*> TranslatePhysAddr(uint64_t phys) {
    for (auto& [_, block] : gBlocks) {
        if (phys >= block.physBaseLoaded && phys < block.physBaseLoaded + block.bitsSize) {
            return {block.physBase + (phys - block.physBaseLoaded), &block};
        }
    }

    return {0, nullptr};
}

}  // namespace

MemBlock::MemBlock(const address_space_device_control_ops* blockOps,
                   const AddressSpaceHwFuncs* blockHw, uint32_t size)
    : ops(blockOps), hw(blockHw) {
    bits = gfxstream::aligned_buf_alloc(kAllocAlignment, size);
    bitsSize = size;
    physBase = AllocateAddressSpaceBlock(hw, size);
    if (!physBase) {
        GFXSTREAM_FATAL("AllocateAddressSpaceBlock failed");
    }
    if (!ops->add_memory_mapping(physBase, bits, bitsSize)) {
        GFXSTREAM_FATAL("add_memory_mapping failed");
    }
    if (!freeSubblocks.insert({0, size}).second) {
        GFXSTREAM_FATAL("freeSubblocks insert failed");
    }
    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO("ASG shared-slots new-block phys=0x%llx bits=%p size=0x%x",
                       static_cast<unsigned long long>(physBase), bits, bitsSize);
    }
}

MemBlock::MemBlock(MemBlock&& rhs)
    : ops(std::exchange(rhs.ops, nullptr)),
      hw(std::exchange(rhs.hw, nullptr)),
      physBase(std::exchange(rhs.physBase, 0)),
      physBaseLoaded(std::exchange(rhs.physBaseLoaded, 0)),
      bits(std::exchange(rhs.bits, nullptr)),
      bitsSize(std::exchange(rhs.bitsSize, 0)),
      freeSubblocks(std::move(rhs.freeSubblocks)) {}

MemBlock& MemBlock::operator=(MemBlock rhs) {
    swap(*this, rhs);
    return *this;
}

MemBlock::~MemBlock() {
    if (!physBase) {
        return;
    }

    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO("ASG shared-slots destroy-block phys=0x%llx bits=%p size=0x%x",
                       static_cast<unsigned long long>(physBase), bits, bitsSize);
    }
    ops->remove_memory_mapping(physBase, bits, bitsSize);
    FreeAddressBlock(hw, physBase);
    gfxstream::aligned_buf_free(bits);
}

void swap(MemBlock& lhs, MemBlock& rhs) {
    using std::swap;

    swap(lhs.ops, rhs.ops);
    swap(lhs.hw, rhs.hw);
    swap(lhs.physBase, rhs.physBase);
    swap(lhs.physBaseLoaded, rhs.physBaseLoaded);
    swap(lhs.bits, rhs.bits);
    swap(lhs.bitsSize, rhs.bitsSize);
    swap(lhs.freeSubblocks, rhs.freeSubblocks);
}

bool MemBlock::isAllFree() const {
    if (freeSubblocks.size() != 1) {
        return false;
    }

    const auto [offset, size] = *freeSubblocks.begin();
    return offset == 0 && size == bitsSize;
}

uint64_t MemBlock::allocate(size_t requestedSize) {
    auto it = findFreeSubblock(&freeSubblocks, requestedSize);
    if (it == freeSubblocks.end()) {
        return 0;
    }

    const uint32_t subblockOffset = it->first;
    const uint32_t subblockSize = it->second;

    freeSubblocks.erase(it);
    if (subblockSize > requestedSize &&
        !freeSubblocks.insert({subblockOffset + requestedSize,
                               subblockSize - requestedSize}).second) {
        GFXSTREAM_FATAL("freeSubblocks insert failed");
    }

    return physBase + subblockOffset;
}

void MemBlock::unallocate(uint64_t physAddr, uint32_t subblockSize) {
    if (physAddr >= physBase + bitsSize) {
        GFXSTREAM_FATAL("physAddr outside MemBlock");
    }

    auto insertion = freeSubblocks.insert({physAddr - physBase, subblockSize});
    if (!insertion.second) {
        GFXSTREAM_FATAL("freeSubblocks insert failed");
    }

    auto it = insertion.first;
    if (it != freeSubblocks.begin()) {
        it = tryMergeSubblocks(&freeSubblocks, it, std::prev(it), it);
    }
    auto next = std::next(it);
    if (next != freeSubblocks.end()) {
        tryMergeSubblocks(&freeSubblocks, it, it, next);
    }
}

MemBlock::FreeSubblocks::iterator MemBlock::findFreeSubblock(FreeSubblocks* freeSubblocks,
                                                             size_t size) {
    if (freeSubblocks->empty()) {
        return freeSubblocks->end();
    }

    auto best = freeSubblocks->end();
    size_t bestSize = ~size_t(0);
    for (auto it = freeSubblocks->begin(); it != freeSubblocks->end(); ++it) {
        if (it->second >= size && it->second < bestSize) {
            best = it;
            bestSize = it->second;
        }
    }

    return best;
}

MemBlock::FreeSubblocks::iterator MemBlock::tryMergeSubblocks(
    FreeSubblocks* freeSubblocks, FreeSubblocks::iterator ret, FreeSubblocks::iterator lhs,
    FreeSubblocks::iterator rhs) {
    if (lhs->first + lhs->second != rhs->first) {
        return ret;
    }

    const uint32_t subblockOffset = lhs->first;
    const uint32_t mergedSize = lhs->second + rhs->second;
    freeSubblocks->erase(lhs);
    freeSubblocks->erase(rhs);
    auto insertion = freeSubblocks->insert({subblockOffset, mergedSize});
    if (!insertion.second) {
        GFXSTREAM_FATAL("freeSubblocks insert failed");
    }
    return insertion.first;
}

void MemBlock::save(gfxstream::Stream* stream) const {
    stream->putBe64(physBase);
    stream->putBe32(bitsSize);
    stream->write(bits, bitsSize);
    stream->putBe32(freeSubblocks.size());
    for (const auto& [offset, size] : freeSubblocks) {
        stream->putBe32(offset);
        stream->putBe32(size);
    }
}

bool MemBlock::load(gfxstream::Stream* stream, const address_space_device_control_ops* blockOps,
                    const AddressSpaceHwFuncs* blockHw, MemBlock* block) {
    const uint64_t physBaseLoaded = stream->getBe64();
    const uint32_t bitsSize = stream->getBe32();
    void* bits = gfxstream::aligned_buf_alloc(kAllocAlignment, bitsSize);
    if (!bits) {
        return false;
    }
    if (stream->read(bits, bitsSize) != static_cast<ssize_t>(bitsSize)) {
        gfxstream::aligned_buf_free(bits);
        return false;
    }

    const uint64_t physBase = AllocateAddressSpaceBlockFixed(physBaseLoaded, blockHw, bitsSize);
    if (!physBase) {
        gfxstream::aligned_buf_free(bits);
        return false;
    }
    if (!blockOps->add_memory_mapping(physBase, bits, bitsSize)) {
        FreeAddressBlock(blockHw, physBase);
        gfxstream::aligned_buf_free(bits);
        return false;
    }

    FreeSubblocks freeSubblocks;
    for (uint32_t freeCount = stream->getBe32(); freeCount > 0; --freeCount) {
        const uint32_t offset = stream->getBe32();
        const uint32_t size = stream->getBe32();
        if (!freeSubblocks.insert({offset, size}).second) {
            GFXSTREAM_FATAL("freeSubblocks insert failed");
        }
    }

    block->ops = blockOps;
    block->hw = blockHw;
    block->physBase = physBase;
    block->physBaseLoaded = physBaseLoaded;
    block->bits = bits;
    block->bitsSize = bitsSize;
    block->freeSubblocks = std::move(freeSubblocks);
    return true;
}

AddressSpaceSharedSlotsHostMemoryAllocatorContext::AddressSpaceSharedSlotsHostMemoryAllocatorContext(
    const address_space_device_control_ops* ops, const AddressSpaceHwFuncs* hw)
    : mOps(ops), mHw(hw) {}

AddressSpaceSharedSlotsHostMemoryAllocatorContext::
    ~AddressSpaceSharedSlotsHostMemoryAllocatorContext() {
    clear();
}

void AddressSpaceSharedSlotsHostMemoryAllocatorContext::perform(AddressSpaceDevicePingInfo* info) {
    uint64_t result = ~0ULL;
    const auto command = static_cast<HostMemoryAllocatorCommand>(info->metadata);

    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO(
            "ASG shared-slots perform command=%llu phys=0x%llx size=0x%llx",
            static_cast<unsigned long long>(info->metadata),
            static_cast<unsigned long long>(info->phys_addr),
            static_cast<unsigned long long>(info->size));
    }

    switch (command) {
        case HostMemoryAllocatorCommand::Allocate:
            result = allocate(info);
            break;
        case HostMemoryAllocatorCommand::Unallocate:
            result = unallocate(info->phys_addr);
            break;
        case HostMemoryAllocatorCommand::CheckIfSharedSlotsSupported:
            result = 0;
            break;
        default:
            break;
    }

    info->metadata = result;
    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO(
            "ASG shared-slots result=0x%llx phys=0x%llx size=0x%llx",
            static_cast<unsigned long long>(result),
            static_cast<unsigned long long>(info->phys_addr),
            static_cast<unsigned long long>(info->size));
    }
}

uint64_t AddressSpaceSharedSlotsHostMemoryAllocatorContext::allocate(AddressSpaceDevicePingInfo* info) {
    const uint32_t alignedSize = AlignTo(info->size, mHw->getGuestPageSize());
    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO("ASG shared-slots allocate requested=0x%llx aligned=0x%x",
                       static_cast<unsigned long long>(info->size), alignedSize);
    }

    std::lock_guard<std::mutex> lock(gBlocksMutex);
    for (auto& [_, block] : gBlocks) {
        uint64_t physAddr = block.allocate(alignedSize);
        if (physAddr) {
            if (IsAsgTraceEnabled()) {
                GFXSTREAM_INFO("ASG shared-slots reuse-block phys=0x%llx aligned=0x%x",
                               static_cast<unsigned long long>(physAddr), alignedSize);
            }
            return populatePhysAddr(info, physAddr, alignedSize, &block);
        }
    }

    constexpr uint32_t kDefaultBlockSize = 64u << 20;
    MemBlock newBlock(mOps, mHw, std::max(alignedSize, kDefaultBlockSize));
    const uint64_t physAddr = newBlock.allocate(alignedSize);
    if (!physAddr) {
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_ERROR("ASG shared-slots new-block allocation failed aligned=0x%x",
                            alignedSize);
        }
        return ~0ULL;
    }

    const uint64_t physBase = newBlock.physBase;
    auto insertion = gBlocks.insert({physBase, std::move(newBlock)});
    if (!insertion.second) {
        GFXSTREAM_FATAL("gBlocks insert failed");
    }

    return populatePhysAddr(info, physAddr, alignedSize, &insertion.first->second);
}

uint64_t AddressSpaceSharedSlotsHostMemoryAllocatorContext::unallocate(uint64_t physAddr) {
    // physAddr here is the offset from the BAR base as returned to the guest by
    // populatePhysAddr().  mAllocations is keyed by actual GPA, so convert back.
    const uint64_t actualGPA = physAddr + mHw->getPhysAddrStartLocked();

    std::lock_guard<std::mutex> lock(gBlocksMutex);

    const auto it = mAllocations.find(actualGPA);
    if (it == mAllocations.end()) {
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_ERROR("ASG shared-slots unallocate missing phys=0x%llx (actualGPA=0x%llx)",
                            static_cast<unsigned long long>(physAddr),
                            static_cast<unsigned long long>(actualGPA));
        }
        return ~0ULL;
    }

    const uint32_t allocationSize = it->second.first;
    MemBlock* block = it->second.second;
    block->unallocate(actualGPA, allocationSize);
    mAllocations.erase(it);

    if (block->isAllFree()) {
        gcEmptyBlocks(1);
    }

    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO("ASG shared-slots unallocated phys=0x%llx size=0x%x",
                       static_cast<unsigned long long>(physAddr), allocationSize);
    }

    return 0;
}

void AddressSpaceSharedSlotsHostMemoryAllocatorContext::gcEmptyBlocks(int allowedEmpty) {
    auto it = gBlocks.begin();
    while (it != gBlocks.end()) {
        if (it->second.isAllFree()) {
            if (allowedEmpty > 0) {
                --allowedEmpty;
                ++it;
            } else {
                it = gBlocks.erase(it);
            }
        } else {
            ++it;
        }
    }
}

uint64_t AddressSpaceSharedSlotsHostMemoryAllocatorContext::populatePhysAddr(
    AddressSpaceDevicePingInfo* info, uint64_t physAddr, uint32_t alignedSize, MemBlock* owner) {
    info->phys_addr = physAddr - mHw->getPhysAddrStartLocked();
    info->size = alignedSize;
    if (!mAllocations.insert({physAddr, {alignedSize, owner}}).second) {
        GFXSTREAM_FATAL("mAllocations insert failed");
    }
    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO(
            "ASG shared-slots populate phys=0x%llx exported-offset=0x%llx size=0x%x owner=%p",
            static_cast<unsigned long long>(physAddr),
            static_cast<unsigned long long>(info->phys_addr), alignedSize, owner);
    }
    return 0;
}

AddressSpaceDeviceType AddressSpaceSharedSlotsHostMemoryAllocatorContext::getDeviceType() const {
    return AddressSpaceDeviceType::SharedSlotsHostMemoryAllocator;
}

void AddressSpaceSharedSlotsHostMemoryAllocatorContext::save(gfxstream::Stream* stream) const {
    std::lock_guard<std::mutex> lock(gBlocksMutex);

    stream->putBe32(mAllocations.size());
    for (const auto& [physAddr, sizeAndOwner] : mAllocations) {
        stream->putBe64(physAddr);
        stream->putBe32(sizeAndOwner.first);
    }
}

bool AddressSpaceSharedSlotsHostMemoryAllocatorContext::load(gfxstream::Stream* stream) {
    clear();

    std::lock_guard<std::mutex> lock(gBlocksMutex);
    for (uint32_t allocCount = stream->getBe32(); allocCount > 0; --allocCount) {
        const uint64_t physAddr = stream->getBe64();
        const uint32_t size = stream->getBe32();
        const auto translated = TranslatePhysAddr(physAddr);
        if (!translated.first) {
            GFXSTREAM_FATAL("TranslatePhysAddr failed for 0x%llx",
                            static_cast<unsigned long long>(physAddr));
        }
        if (!mAllocations.insert({translated.first, {size, translated.second}}).second) {
            GFXSTREAM_FATAL("mAllocations insert failed");
        }
    }

    return true;
}

void AddressSpaceSharedSlotsHostMemoryAllocatorContext::clear() {
    std::lock_guard<std::mutex> lock(gBlocksMutex);
    for (const auto& [physAddr, sizeAndOwner] : mAllocations) {
        sizeAndOwner.second->unallocate(physAddr, sizeAndOwner.first);
    }
    mAllocations.clear();
}

void AddressSpaceSharedSlotsHostMemoryAllocatorContext::globalStateSave(gfxstream::Stream* stream) {
    std::lock_guard<std::mutex> lock(gBlocksMutex);

    stream->putBe32(gBlocks.size());
    for (const auto& [_, block] : gBlocks) {
        block.save(stream);
    }
}

bool AddressSpaceSharedSlotsHostMemoryAllocatorContext::globalStateLoad(
    gfxstream::Stream* stream, const address_space_device_control_ops* ops,
    const AddressSpaceHwFuncs* hw) {
    if (!ops || !hw) {
        return false;
    }

    std::lock_guard<std::mutex> lock(gBlocksMutex);
    for (uint32_t blockCount = stream->getBe32(); blockCount > 0; --blockCount) {
        MemBlock block;
        if (!MemBlock::load(stream, ops, hw, &block)) {
            return false;
        }
        const uint64_t physBase = block.physBase;
        if (!gBlocks.insert({physBase, std::move(block)}).second) {
            GFXSTREAM_FATAL("gBlocks insert failed");
        }
    }

    return true;
}

void AddressSpaceSharedSlotsHostMemoryAllocatorContext::globalStateClear() {
    std::lock_guard<std::mutex> lock(gBlocksMutex);
    gBlocks.clear();
}

}  // namespace host
}  // namespace gfxstream
