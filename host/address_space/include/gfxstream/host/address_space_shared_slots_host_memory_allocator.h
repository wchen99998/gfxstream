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

#pragma once

#include <map>
#include <unordered_map>

#include "gfxstream/host/address_space_service.h"

namespace gfxstream {
namespace host {

class AddressSpaceSharedSlotsHostMemoryAllocatorContext : public AddressSpaceDeviceContext {
  public:
    enum class HostMemoryAllocatorCommand {
        Allocate = 1,
        Unallocate = 2,
        CheckIfSharedSlotsSupported = 3,
    };

    struct MemBlock {
        using FreeSubblocks = std::map<uint32_t, uint32_t>;

        MemBlock() = default;
        MemBlock(const address_space_device_control_ops* ops,
                 const AddressSpaceHwFuncs* hw,
                 uint32_t size);
        MemBlock(MemBlock&& rhs);
        MemBlock& operator=(MemBlock rhs);
        ~MemBlock();

        friend void swap(MemBlock& lhs, MemBlock& rhs);

        bool isAllFree() const;
        uint64_t allocate(size_t requestedSize);
        void unallocate(uint64_t physAddr, uint32_t subblockSize);

        static FreeSubblocks::iterator findFreeSubblock(FreeSubblocks* freeSubblocks,
                                                        size_t size);

        static FreeSubblocks::iterator tryMergeSubblocks(FreeSubblocks* freeSubblocks,
                                                         FreeSubblocks::iterator ret,
                                                         FreeSubblocks::iterator lhs,
                                                         FreeSubblocks::iterator rhs);

        void save(gfxstream::Stream* stream) const;
        static bool load(gfxstream::Stream* stream,
                         const address_space_device_control_ops* ops,
                         const AddressSpaceHwFuncs* hw,
                         MemBlock* block);

        const address_space_device_control_ops* ops = nullptr;
        const AddressSpaceHwFuncs* hw = nullptr;
        uint64_t physBase = 0;
        uint64_t physBaseLoaded = 0;
        void* bits = nullptr;
        uint32_t bitsSize = 0;
        FreeSubblocks freeSubblocks;

        MemBlock(const MemBlock&) = delete;
        MemBlock& operator=(const MemBlock&) = delete;
    };

    AddressSpaceSharedSlotsHostMemoryAllocatorContext(const address_space_device_control_ops* ops,
                                                      const AddressSpaceHwFuncs* hw);
    ~AddressSpaceSharedSlotsHostMemoryAllocatorContext() override;

    void perform(AddressSpaceDevicePingInfo* info) override;

    AddressSpaceDeviceType getDeviceType() const override;
    void save(gfxstream::Stream* stream) const override;
    bool load(gfxstream::Stream* stream) override;

    static void globalStateSave(gfxstream::Stream* stream);
    static bool globalStateLoad(gfxstream::Stream* stream,
                                const address_space_device_control_ops* ops,
                                const AddressSpaceHwFuncs* hw);
    static void globalStateClear();

  private:
    uint64_t allocate(AddressSpaceDevicePingInfo* info);
    uint64_t unallocate(uint64_t physAddr);
    void gcEmptyBlocks(int allowedEmpty);
    void clear();

    uint64_t populatePhysAddr(AddressSpaceDevicePingInfo* info,
                              uint64_t physAddr,
                              uint32_t alignedSize,
                              MemBlock* owner);

    std::unordered_map<uint64_t, std::pair<uint32_t, MemBlock*>> mAllocations;
    const address_space_device_control_ops* mOps;
    const AddressSpaceHwFuncs* mHw;
};

}  // namespace host
}  // namespace gfxstream
