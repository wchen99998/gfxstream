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

#include <unordered_map>

#include "gfxstream/host/address_space_service.h"

namespace gfxstream {
namespace host {

class AddressSpaceHostMemoryAllocatorContext : public AddressSpaceDeviceContext {
  public:
    enum class HostMemoryAllocatorCommand {
        Allocate = 1,
        Unallocate = 2,
    };

    AddressSpaceHostMemoryAllocatorContext(const address_space_device_control_ops* ops,
                                           const AddressSpaceHwFuncs* hw);
    ~AddressSpaceHostMemoryAllocatorContext() override;

    void perform(AddressSpaceDevicePingInfo* info) override;

    AddressSpaceDeviceType getDeviceType() const override;
    void save(gfxstream::Stream* stream) const override;
    bool load(gfxstream::Stream* stream) override;

  private:
    uint64_t allocate(AddressSpaceDevicePingInfo* info);
    uint64_t unallocate(AddressSpaceDevicePingInfo* info);
    void* allocateImpl(uint64_t physAddr, uint64_t size);
    void clear();

    std::unordered_map<uint64_t, std::pair<void*, size_t>> mPhysAddrToPtr;
    const address_space_device_control_ops* mOps;
    const AddressSpaceHwFuncs* mHw;
};

}  // namespace host
}  // namespace gfxstream
