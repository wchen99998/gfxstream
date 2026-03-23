// Copyright 2019 The Android Open Source Project
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

#include "gfxstream/host/address_space_device.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "gfxstream/host/address_space_graphics.h"
#include "gfxstream/host/address_space_host_memory_allocator.h"
#include "gfxstream/host/address_space_service.h"
#include "gfxstream/host/address_space_shared_slots_host_memory_allocator.h"
#include "gfxstream/host/vm_operations.h"
#include "gfxstream/common/logging.h"
#include "render-utils/address_space_operations.h"

namespace gfxstream {
namespace host {

using gfxstream::Stream;

static const address_space_device_control_ops* GetAsgOperationsPtr();

namespace {

bool IsAsgTraceEnabled() {
    static int enabled = -1;
    if (enabled == -1) {
        const char* env = std::getenv("QEMU_RUTABAGA_TRACE_ASG");
        enabled = (env && env[0] && !(env[0] == '0' && env[1] == '\0')) ? 1 : 0;
    }
    return enabled;
}

}  // namespace

class AddressSpaceDeviceState {
  public:
    AddressSpaceDeviceState() = default;
    ~AddressSpaceDeviceState() = default;

    uint32_t genHandle() {
        std::lock_guard<std::mutex> lock(mContextsMutex);

        auto res = mHandleIndex;
        if (!res) {
            ++res;
            mHandleIndex += 2;
        } else {
            ++mHandleIndex;
        }

        return res;
    }

    void destroyHandle(uint32_t handle) {
        std::unique_ptr<AddressSpaceDeviceContext> context;

        {
            std::lock_guard<std::mutex> lock(mContextsMutex);

            auto contextDescriptionIt = mContexts.find(handle);
            if (contextDescriptionIt == mContexts.end()) return;
            auto& contextDescription = contextDescriptionIt->second;

            context = std::move(contextDescription.device_context);

            mContexts.erase(contextDescriptionIt);
        }

        // Destroy `context` without holding the lock.
    }

    void createInstance(const struct AddressSpaceCreateInfo& create) {
        std::lock_guard<std::mutex> lock(mContextsMutex);
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_INFO(
                "ASG createInstance handle=%u type=%u phys=0x%llx fromSnapshot=%d",
                create.handle, create.type,
                static_cast<unsigned long long>(create.physAddr),
                create.fromSnapshot ? 1 : 0);
        }
        auto& contextDesc = mContexts[create.handle];
        contextDesc.device_context = buildAddressSpaceDeviceContext(create);
    }

    void tellPingInfo(uint32_t handle, uint64_t gpa) {
        std::lock_guard<std::mutex> lock(mContextsMutex);
        auto& contextDesc = mContexts[handle];

        contextDesc.pingInfo = reinterpret_cast<AddressSpaceDevicePingInfo*>(
            gfxstream::get_gfxstream_vm_operations().lookup_user_memory(gpa));
        contextDesc.pingInfoGpa = gpa;

        if (IsAsgTraceEnabled()) {
            GFXSTREAM_INFO("ASG tellPingInfo handle=%u gpa=0x%llx hva=%p", handle,
                           static_cast<unsigned long long>(gpa), contextDesc.pingInfo);
        }

        if (!contextDesc.pingInfo) {
            GFXSTREAM_FATAL("Could not resolve ping info GPA 0x%llx for handle %u",
                            static_cast<unsigned long long>(gpa), handle);
        }
    }

    void ping(uint32_t handle) {
        std::lock_guard<std::mutex> lock(mContextsMutex);
        auto& contextDesc = mContexts[handle];
        AddressSpaceDevicePingInfo* pingInfo = contextDesc.pingInfo;

        const uint64_t phys_addr = pingInfo->phys_addr;
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_INFO(
                "ASG ping handle=%u phys=0x%llx size=0x%llx metadata=0x%llx hasContext=%d",
                handle, static_cast<unsigned long long>(phys_addr),
                static_cast<unsigned long long>(pingInfo->size),
                static_cast<unsigned long long>(pingInfo->metadata),
                contextDesc.device_context ? 1 : 0);
        }

        AddressSpaceDeviceContext *device_context = contextDesc.device_context.get();
        if (device_context) {
            device_context->perform(pingInfo);
        } else {
            // The first ioctl establishes the device type
            struct AddressSpaceCreateInfo create = {};
            create.type = static_cast<uint32_t>(pingInfo->metadata);
            create.physAddr = phys_addr;

            contextDesc.device_context = buildAddressSpaceDeviceContext(create);
            pingInfo->metadata = contextDesc.device_context ? 0 : -1;
            if (IsAsgTraceEnabled()) {
                GFXSTREAM_INFO(
                    "ASG ping first-create handle=%u type=%u phys=0x%llx success=%d",
                    handle, create.type, static_cast<unsigned long long>(create.physAddr),
                    contextDesc.device_context ? 1 : 0);
            }
        }
    }

    void pingAtHva(uint32_t handle, AddressSpaceDevicePingInfo* pingInfo) {
        std::lock_guard<std::mutex> lock(mContextsMutex);
        auto& contextDesc = mContexts[handle];

        const uint64_t phys_addr = pingInfo->phys_addr;
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_INFO(
                "ASG pingAtHva handle=%u phys=0x%llx size=0x%llx metadata=0x%llx hva=%p hasContext=%d",
                handle, static_cast<unsigned long long>(phys_addr),
                static_cast<unsigned long long>(pingInfo->size),
                static_cast<unsigned long long>(pingInfo->metadata), pingInfo,
                contextDesc.device_context ? 1 : 0);
        }

        AddressSpaceDeviceContext *device_context = contextDesc.device_context.get();
        if (device_context) {
            device_context->perform(pingInfo);
        } else {
            struct AddressSpaceCreateInfo create = {};
            create.type = static_cast<uint32_t>(pingInfo->metadata);
            create.physAddr = phys_addr;

            contextDesc.device_context = buildAddressSpaceDeviceContext(create);
            pingInfo->metadata = contextDesc.device_context ? 0 : -1;
            if (IsAsgTraceEnabled()) {
                GFXSTREAM_INFO(
                    "ASG pingAtHva first-create handle=%u type=%u phys=0x%llx success=%d",
                    handle, create.type, static_cast<unsigned long long>(create.physAddr),
                    contextDesc.device_context ? 1 : 0);
            }
        }
    }

    void registerDeallocationCallback(uint64_t gpa, void* context, address_space_device_deallocation_callback_t func) {
        std::lock_guard<std::mutex> lock(mContextsMutex);
        auto& currentCallbacks = mDeallocationCallbacks[gpa];

        DeallocationCallbackEntry entry = {
            context,
            func,
        };

        currentCallbacks.push_back(entry);
    }

    void runDeallocationCallbacks(uint64_t gpa) {
        std::lock_guard<std::mutex> lock(mContextsMutex);

        auto it = mDeallocationCallbacks.find(gpa);
        if (it == mDeallocationCallbacks.end()) return;

        auto& callbacks = it->second;

        for (auto& entry: callbacks) {
            entry.func(entry.context, gpa);
        }

        mDeallocationCallbacks.erase(gpa);
    }

    AddressSpaceDeviceContext* handleToContext(uint32_t handle) {
        std::lock_guard<std::mutex> lock(mContextsMutex);
        if (mContexts.find(handle) == mContexts.end()) return nullptr;

        auto& contextDesc = mContexts[handle];
        return contextDesc.device_context.get();
    }

    void save(Stream* stream) const {
        // Pre-save
        for (const auto &kv : mContexts) {
            const AddressSpaceContextDescription &desc = kv.second;
            const AddressSpaceDeviceContext *device_context = desc.device_context.get();
            if (device_context) {
                device_context->preSave();
            }
        }

        AddressSpaceGraphicsContext::globalStatePreSave();
        AddressSpaceSharedSlotsHostMemoryAllocatorContext::globalStateSave(stream);
        AddressSpaceGraphicsContext::globalStateSave(stream);

        stream->putBe32(mHandleIndex);
        stream->putBe32(mContexts.size());

        for (const auto &kv : mContexts) {
            const uint32_t handle = kv.first;
            const AddressSpaceContextDescription &desc = kv.second;
            const AddressSpaceDeviceContext *device_context = desc.device_context.get();

            stream->putBe32(handle);
            stream->putBe64(desc.pingInfoGpa);

            if (device_context) {
                stream->putByte(1);
                stream->putBe32(static_cast<uint32_t>(device_context->getDeviceType()));
                device_context->save(stream);
            } else {
                stream->putByte(0);
            }
        }

        // Post save

        AddressSpaceGraphicsContext::globalStatePostSave();

        for (const auto &kv : mContexts) {
            const AddressSpaceContextDescription &desc = kv.second;
            const AddressSpaceDeviceContext *device_context = desc.device_context.get();
            if (device_context) {
                device_context->postSave();
            }
        }
    }

    void setLoadResources(AddressSpaceDeviceLoadResources resources) {
        mLoadResources = std::move(resources);
    }

    bool load(Stream* stream) {
        // First destroy all contexts, because
        // this can be done while an emulator is running
        clear();

        if (!AddressSpaceSharedSlotsHostMemoryAllocatorContext::globalStateLoad(
                stream, GetAsgOperationsPtr(), gfxstream_address_space_get_hw_funcs())) {
            return false;
        }

        if (!AddressSpaceGraphicsContext::globalStateLoad(stream, mLoadResources)) {
            return false;
        }

        const uint32_t handleIndex = stream->getBe32();
        const size_t size = stream->getBe32();

        std::unordered_map<uint32_t, AddressSpaceContextDescription> contexts;
        for (size_t i = 0; i < size; ++i) {
            const uint32_t handle = stream->getBe32();
            const uint64_t pingInfoGpa = stream->getBe64();

            std::unique_ptr<AddressSpaceDeviceContext> context;
            switch (stream->getByte()) {
                case 0:
                    break;
                case 1: {
                    struct AddressSpaceCreateInfo create = {};
                    create.type = static_cast<uint32_t>(stream->getBe32());
                    create.physAddr = pingInfoGpa;
                    create.fromSnapshot = true;

                    context = buildAddressSpaceDeviceContext(create);
                    if (!context || !context->load(stream)) {
                        return false;
                    }
                    break;
                }
                default:
                    return false;
            }

            auto &desc = contexts[handle];
            desc.pingInfoGpa = pingInfoGpa;
            if (desc.pingInfoGpa == ~0ULL) {
                GFXSTREAM_WARNING("Restoring hva-only ping.");
            } else {
                desc.pingInfo = nullptr;
            }
            desc.device_context = std::move(context);
        }

        {
           std::lock_guard<std::mutex> lock(mContextsMutex);
           mHandleIndex = handleIndex;
           mContexts = std::move(contexts);
        }

        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mContextsMutex);
        mContexts.clear();
        AddressSpaceSharedSlotsHostMemoryAllocatorContext::globalStateClear();

        std::vector<std::pair<uint64_t, uint64_t>> gpasSizesToErase;
        for (auto& mapping : mMemoryMappings) {
            auto gpa = mapping.first;
            auto size = mapping.second.second;
            gpasSizesToErase.push_back({gpa, size});
        }
        for (const auto& gpaSize : gpasSizesToErase) {
            removeMemoryMappingLocked(gpaSize.first, gpaSize.second);
        }
        mMemoryMappings.clear();
    }

    bool addMemoryMapping(uint64_t gpa, void *ptr, uint64_t size) {
        std::lock_guard<std::mutex> lock(mMemoryMappingsLock);
        return addMemoryMappingLocked(gpa, ptr, size);
    }

    bool removeMemoryMapping(uint64_t gpa, uint64_t size) {
        std::lock_guard<std::mutex> lock(mMemoryMappingsLock);
        return removeMemoryMappingLocked(gpa, size);
    }

    void *getHostPtr(uint64_t gpa) const {
        std::lock_guard<std::mutex> lock(mMemoryMappingsLock);
        return getHostPtrLocked(gpa);
    }

  private:
    std::unique_ptr<AddressSpaceDeviceContext> buildAddressSpaceDeviceContext(
            const struct AddressSpaceCreateInfo& create) {
        if (IsAsgTraceEnabled()) {
            GFXSTREAM_INFO("ASG buildContext type=%u handle=%u phys=0x%llx hw=%p",
                           create.type, create.handle,
                           static_cast<unsigned long long>(create.physAddr),
                           gfxstream_address_space_get_hw_funcs());
        }
        switch (static_cast<AddressSpaceDeviceType>(create.type)) {
            case AddressSpaceDeviceType::Graphics:
            case AddressSpaceDeviceType::VirtioGpuGraphics:
                return std::unique_ptr<AddressSpaceDeviceContext>(
                    new AddressSpaceGraphicsContext(create));
            case AddressSpaceDeviceType::HostMemoryAllocator:
                return std::unique_ptr<AddressSpaceDeviceContext>(
                    new AddressSpaceHostMemoryAllocatorContext(GetAsgOperationsPtr(),
                                                               gfxstream_address_space_get_hw_funcs()));
            case AddressSpaceDeviceType::SharedSlotsHostMemoryAllocator:
                return std::unique_ptr<AddressSpaceDeviceContext>(
                    new AddressSpaceSharedSlotsHostMemoryAllocatorContext(
                        GetAsgOperationsPtr(), gfxstream_address_space_get_hw_funcs()));
            default:
                GFXSTREAM_FATAL("Unhandled address space context type: %d", create.type);
                return nullptr;
        }
    }

    bool addMemoryMappingLocked(uint64_t gpa, void *ptr, uint64_t size) {
        if (mMemoryMappings.insert({gpa, {ptr, size}}).second) {
            if (IsAsgTraceEnabled()) {
                GFXSTREAM_INFO("ASG addMemoryMapping gpa=0x%llx ptr=%p size=0x%llx",
                               static_cast<unsigned long long>(gpa), ptr,
                               static_cast<unsigned long long>(size));
            }
            gfxstream::get_gfxstream_vm_operations().map_user_memory(gpa, ptr, size);
            return true;
        } else {
            GFXSTREAM_ERROR("Failed: hva %p -> gpa [0x%llx 0x%llx]",
                            ptr, (unsigned long long)gpa, (unsigned long long)size);
            return false;
        }
    }

    bool removeMemoryMappingLocked(uint64_t gpa, uint64_t size) {
        if (mMemoryMappings.erase(gpa) > 0) {
            if (IsAsgTraceEnabled()) {
                GFXSTREAM_INFO("ASG removeMemoryMapping gpa=0x%llx size=0x%llx",
                               static_cast<unsigned long long>(gpa),
                               static_cast<unsigned long long>(size));
            }
            gfxstream::get_gfxstream_vm_operations().unmap_user_memory(gpa, size);
            return true;
        } else {
            GFXSTREAM_FATAL("Failed: gpa [0x%llx 0x%llx]",
                            (unsigned long long)gpa, (unsigned long long)size);
            return false;
        }
    }

    void *getHostPtrLocked(uint64_t gpa) const {
        auto i = mMemoryMappings.lower_bound(gpa); // i->first >= gpa (or i==end)
        if ((i != mMemoryMappings.end()) && (i->first == gpa)) {
            return i->second.first;  // gpa is exactly the beginning of the range
        }

        if (i != mMemoryMappings.begin()) {
            --i;

            if ((i->first + i->second.second) > gpa) {
                // move the host ptr by +(gpa-base)
                if (IsAsgTraceEnabled()) {
                    GFXSTREAM_INFO(
                        "ASG getHostPtr local-hit gpa=0x%llx base=0x%llx size=0x%llx",
                        static_cast<unsigned long long>(gpa),
                        static_cast<unsigned long long>(i->first),
                        static_cast<unsigned long long>(i->second.second));
                }
                return static_cast<char *>(i->second.first) + (gpa - i->first);
            }
        }

        if (IsAsgTraceEnabled()) {
            GFXSTREAM_INFO("ASG getHostPtr vm-lookup gpa=0x%llx",
                           static_cast<unsigned long long>(gpa));
        }
        return gfxstream::get_gfxstream_vm_operations().lookup_user_memory(gpa);
    }


    uint32_t mHandleIndex = 1;

    mutable std::mutex mContextsMutex;
    std::unordered_map<uint32_t, AddressSpaceContextDescription> mContexts;

    mutable std::mutex mMemoryMappingsLock;
    std::map<uint64_t, std::pair<void *, uint64_t>> mMemoryMappings;  // do not save/load

    struct DeallocationCallbackEntry {
        void* context;
        address_space_device_deallocation_callback_t func;
    };
    std::map<uint64_t, std::vector<DeallocationCallbackEntry>> mDeallocationCallbacks; // do not save/load, users re-register on load

    // Not saved/loaded. Externally owned resources used during load.
    std::optional<AddressSpaceDeviceLoadResources> mLoadResources;
};

static AddressSpaceDeviceState* sAddressSpaceDeviceState() {
    static AddressSpaceDeviceState* s = new AddressSpaceDeviceState;
    return s;
}

static const AddressSpaceHwFuncs* sAddressSpaceHwFuncs = nullptr;

static uint32_t sAddressSpaceDeviceGenHandle() {
    return sAddressSpaceDeviceState()->genHandle();
}

static void sAddressSpaceDeviceDestroyHandle(uint32_t handle) {
    sAddressSpaceDeviceState()->destroyHandle(handle);
}

static void sAddressSpaceDeviceCreateInstance(const struct AddressSpaceCreateInfo& create) {
    sAddressSpaceDeviceState()->createInstance(create);
}

static void sAddressSpaceDeviceTellPingInfo(uint32_t handle, uint64_t gpa) {
    sAddressSpaceDeviceState()->tellPingInfo(handle, gpa);
}

static void sAddressSpaceDevicePing(uint32_t handle) {
    sAddressSpaceDeviceState()->ping(handle);
}

int sAddressSpaceDeviceAddMemoryMapping(uint64_t gpa, void *ptr, uint64_t size) {
    return sAddressSpaceDeviceState()->addMemoryMapping(gpa, ptr, size) ? 1 : 0;
}

int sAddressSpaceDeviceRemoveMemoryMapping(uint64_t gpa, void*, uint64_t size) {
    return sAddressSpaceDeviceState()->removeMemoryMapping(gpa, size) ? 1 : 0;
}

void* sAddressSpaceDeviceGetHostPtr(uint64_t gpa) {
    return sAddressSpaceDeviceState()->getHostPtr(gpa);
}

static void* sAddressSpaceHandleToContext(uint32_t handle) {
    return (void*)(sAddressSpaceDeviceState()->handleToContext(handle));
}

static void sAddressSpaceDeviceClear() {
    sAddressSpaceDeviceState()->clear();
}

static uint64_t sAddressSpaceDeviceHostmemRegister(const struct MemEntry *entry) {
    GFXSTREAM_FATAL("Unexpected call to hostmem register.");
    return -1;
}

static void sAddressSpaceDeviceHostmemUnregister(uint64_t id) {
    GFXSTREAM_FATAL("Unexpected call to hostmem unregister.");
}

static void sAddressSpaceDevicePingAtHva(uint32_t handle, void* hva) {
    sAddressSpaceDeviceState()->pingAtHva(handle, (AddressSpaceDevicePingInfo*)hva);
}

static void sAddressSpaceDeviceRegisterDeallocationCallback(
    void* context, uint64_t gpa, address_space_device_deallocation_callback_t func) {
    sAddressSpaceDeviceState()->registerDeallocationCallback(gpa, context, func);
}

static void sAddressSpaceDeviceRunDeallocationCallbacks(uint64_t gpa) {
    sAddressSpaceDeviceState()->runDeallocationCallbacks(gpa);
}

static const struct AddressSpaceHwFuncs* sAddressSpaceDeviceControlGetHwFuncs() {
    return gfxstream_address_space_get_hw_funcs();
}

static const address_space_device_control_ops* GetAsgOperationsPtr() {
    static const address_space_device_control_ops kAsgOperations = {
        &sAddressSpaceDeviceGenHandle,                     // gen_handle
        &sAddressSpaceDeviceDestroyHandle,                 // destroy_handle
        &sAddressSpaceDeviceTellPingInfo,                  // tell_ping_info
        &sAddressSpaceDevicePing,                          // ping
        &sAddressSpaceDeviceAddMemoryMapping,              // add_memory_mapping
        &sAddressSpaceDeviceRemoveMemoryMapping,           // remove_memory_mapping
        &sAddressSpaceDeviceGetHostPtr,                    // get_host_ptr
        &sAddressSpaceHandleToContext,                     // handle_to_context
        &sAddressSpaceDeviceClear,                         // clear
        &sAddressSpaceDeviceHostmemRegister,               // hostmem register
        &sAddressSpaceDeviceHostmemUnregister,             // hostmem unregister
        &sAddressSpaceDevicePingAtHva,                     // ping_at_hva
        &sAddressSpaceDeviceRegisterDeallocationCallback,  // register_deallocation_callback
        &sAddressSpaceDeviceRunDeallocationCallbacks,      // run_deallocation_callbacks
        &sAddressSpaceDeviceControlGetHwFuncs,             // control_get_hw_funcs
        &sAddressSpaceDeviceCreateInstance,                // create_instance
    };

    return &kAsgOperations;
}

address_space_device_control_ops GetAsgOperations() {
    return *GetAsgOperationsPtr();
};

int gfxstream_address_space_set_load_resources(AddressSpaceDeviceLoadResources resources) {
    sAddressSpaceDeviceState()->setLoadResources(std::move(resources));
    return 0;
}

int gfxstream_address_space_save_memory_state(gfxstream::Stream *stream) {
    sAddressSpaceDeviceState()->save(stream);
    return 0;
}

int gfxstream_address_space_load_memory_state(gfxstream::Stream *stream) {
    return sAddressSpaceDeviceState()->load(stream) ? 0 : 1;
}

const AddressSpaceHwFuncs* gfxstream_address_space_set_hw_funcs(const AddressSpaceHwFuncs* hwFuncs) {
    const AddressSpaceHwFuncs* previous = sAddressSpaceHwFuncs;
    sAddressSpaceHwFuncs = hwFuncs;
    if (IsAsgTraceEnabled()) {
        GFXSTREAM_INFO("ASG set_hw_funcs previous=%p new=%p", previous, hwFuncs);
    }
    return previous;
}

const AddressSpaceHwFuncs* gfxstream_address_space_get_hw_funcs() {
    return sAddressSpaceHwFuncs;
}

}  // namespace host
}  // namespace gfxstream
