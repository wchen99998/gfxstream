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

#include "GoldfishPipeService.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "FrameBuffer.h"
#include "gfxstream/common/logging.h"
#include "render-utils/RenderChannel.h"
#include "render-utils/Renderer.h"

// ============================================================================
// Internal pipe types
// ============================================================================

// Defined in virtio-gpu-gfxstream-renderer.cpp with C linkage.
extern "C" const GoldfishPipeHwFuncs* stream_renderer_get_service_hw_funcs(void);

namespace gfxstream {
namespace host {
namespace {

// Global weak reference to the renderer, set during init.
static std::weak_ptr<Renderer> s_renderer;

// ---------------------------------------------------------------------------
// Base class for host-side pipe instances.
// ---------------------------------------------------------------------------

class HostPipe {
   public:
    virtual ~HostPipe() = default;
    virtual GoldfishPipePollFlags poll() = 0;
    virtual int recv(GoldfishPipeBuffer* bufs, int nbufs) = 0;
    virtual int send(const GoldfishPipeBuffer* bufs, int nbufs) = 0;
    virtual void wakeOn(GoldfishPipeWakeFlags) {}
    virtual void save(void*) {}
    virtual bool isConnector() const { return false; }
    virtual void onSwappedIn() {}  // called after connector swap completes

    // Set by the device when the pipe is created.
    void setHwPipe(GoldfishHwPipe* hw) { mHwPipe = hw; }
    GoldfishHwPipe* hwPipe() const { return mHwPipe; }

   protected:
    // Signal the QEMU device that this pipe has wake events.
    void signalWake(GoldfishPipeWakeFlags flags) {
        if (!mHwPipe) return;
        const auto* hw = stream_renderer_get_service_hw_funcs();
        if (hw && hw->signal_wake) {
            hw->signal_wake(mHwPipe, flags);
        }
    }

   private:
    GoldfishHwPipe* mHwPipe = nullptr;
};

// ---------------------------------------------------------------------------
// ConnectorPipe – accumulates the service name then creates the real pipe.
// ---------------------------------------------------------------------------

class ConnectorPipe : public HostPipe {
   public:
    ConnectorPipe() = default;

    GoldfishPipePollFlags poll() override {
        return GOLDFISH_PIPE_POLL_OUT;
    }

    int recv(GoldfishPipeBuffer*, int) override {
        return GOLDFISH_PIPE_ERROR_IO;
    }

    bool isConnector() const override { return true; }

    int send(const GoldfishPipeBuffer* bufs, int nbufs) override;

    // After a successful send() that completed the connector handshake,
    // this returns the newly created service pipe (ownership transferred).
    HostPipe* takeSwappedPipe() {
        return mSwappedPipe.release();
    }

   private:
    std::string mAccum;
    std::unique_ptr<HostPipe> mSwappedPipe;
};

// ---------------------------------------------------------------------------
// RefcountPipe – mirrors build/aemu RefcountPipe exactly.
// ---------------------------------------------------------------------------

class RefcountPipe : public HostPipe {
   public:
    RefcountPipe() = default;

    ~RefcountPipe() override {
        if (mHandle != 0) {
            FrameBuffer* fb = FrameBuffer::getFB();
            if (fb) {
                fb->onLastColorBufferRef(mHandle);
            }
        }
    }

    GoldfishPipePollFlags poll() override {
        return GOLDFISH_PIPE_POLL_OUT;
    }

    int recv(GoldfishPipeBuffer*, int) override {
        return GOLDFISH_PIPE_ERROR_IO;
    }

    int send(const GoldfishPipeBuffer* bufs, int nbufs) override {
        int result = 0;
        char tmp[4] = {};
        while (nbufs > 0 && sizeof(tmp) - result >= bufs->size) {
            memcpy(tmp + result, bufs->data, bufs->size);
            result += static_cast<int>(bufs->size);
            bufs++;
            nbufs--;
        }
        if (result == sizeof(tmp)) {
            memcpy(&mHandle, tmp, sizeof(tmp));
        }
        return result;
    }

    void save(void*) override {
        // Snapshot: write mHandle. For now we don't persist across snapshot.
    }

    uint32_t handle() const { return mHandle; }
    void setHandle(uint32_t h) { mHandle = h; }

   private:
    uint32_t mHandle = 0;
};

// ---------------------------------------------------------------------------
// ProcessPipe – mirrors VirtioGpuProcessPipe.
// ---------------------------------------------------------------------------

class ProcessPipe : public HostPipe {
   public:
    ProcessPipe() {
        static std::atomic<uint64_t> sNextId{1};
        mUniqueId = sNextId++;

        auto renderer = s_renderer.lock();
        if (!renderer) {
            GFXSTREAM_ERROR("ProcessPipe: renderer not available");
            return;
        }

        renderer->onGuestGraphicsProcessCreate(mUniqueId);
        mRegistered = true;
    }

    ~ProcessPipe() override {
        if (!mRegistered) {
            return;
        }

        auto renderer = s_renderer.lock();
        if (!renderer) {
            return;
        }

        renderer->cleanupProcGLObjects(mUniqueId);
    }

    GoldfishPipePollFlags poll() override {
        unsigned flags = GOLDFISH_PIPE_POLL_OUT;
        if (!mSentId) {
            flags |= GOLDFISH_PIPE_POLL_IN;
        }
        return static_cast<GoldfishPipePollFlags>(flags);
    }

    int recv(GoldfishPipeBuffer* bufs, int nbufs) override {
        if (mSentId) return GOLDFISH_PIPE_ERROR_IO;
        if (nbufs < 1 || bufs[0].size < sizeof(mUniqueId))
            return GOLDFISH_PIPE_ERROR_INVAL;
        memcpy(bufs[0].data, &mUniqueId, sizeof(mUniqueId));
        mSentId = true;
        return sizeof(mUniqueId);
    }

    int send(const GoldfishPipeBuffer* bufs, int nbufs) override {
        if (mGotConfirmation) return GOLDFISH_PIPE_ERROR_IO;
        if (nbufs < 1 || bufs[0].size < 4) return GOLDFISH_PIPE_ERROR_INVAL;
        int32_t confirmation = 0;
        memcpy(&confirmation, bufs[0].data, sizeof(confirmation));
        if (confirmation != 100) return GOLDFISH_PIPE_ERROR_INVAL;
        mGotConfirmation = true;
        return 4;
    }

   private:
    uint64_t mUniqueId = 0;
    bool mSentId = false;
    bool mGotConfirmation = false;
    bool mRegistered = false;
};

// ---------------------------------------------------------------------------
// RenderThreadPipe – mirrors VirtioGpuRenderThreadPipe.
// ---------------------------------------------------------------------------

class RenderThreadPipe : public HostPipe {
   public:
    static std::unique_ptr<RenderThreadPipe> Create() {
        auto renderer = s_renderer.lock();
        if (!renderer) {
            GFXSTREAM_ERROR("RenderThreadPipe: renderer not available");
            return nullptr;
        }

        auto channel = renderer->createRenderChannel(nullptr);
        if (!channel) {
            GFXSTREAM_ERROR("RenderThreadPipe: failed to create RenderChannel");
            return nullptr;
        }

        return std::unique_ptr<RenderThreadPipe>(
            new RenderThreadPipe(std::move(channel)));
    }

    ~RenderThreadPipe() override {
        if (mChannel) {
            mChannel->setEventCallback(nullptr);
            mChannel->stop();
        }
    }

    void onSwappedIn() override { installWakeCallback(); }

    void installWakeCallback() {
        if (!mChannel) return;
        mChannel->setWantedEvents(
            static_cast<RenderChannel::State>(
                static_cast<int>(RenderChannel::State::CanRead) |
                static_cast<int>(RenderChannel::State::CanWrite)));
        mChannel->setEventCallback(
            [this](RenderChannel::State state) {
                unsigned wake = 0;
                if (static_cast<int>(state) &
                    static_cast<int>(RenderChannel::State::CanRead))
                    wake |= GOLDFISH_PIPE_WAKE_READ;
                if (static_cast<int>(state) &
                    static_cast<int>(RenderChannel::State::CanWrite))
                    wake |= GOLDFISH_PIPE_WAKE_WRITE;
                if (static_cast<int>(state) &
                    static_cast<int>(RenderChannel::State::Stopped))
                    wake |= GOLDFISH_PIPE_WAKE_CLOSED;
                if (wake) {
                    signalWake(static_cast<GoldfishPipeWakeFlags>(wake));
                }
            });
    }

    GoldfishPipePollFlags poll() override {
        unsigned flags = 0;
        auto state = mChannel->state();
        if (static_cast<int>(state) &
            static_cast<int>(RenderChannel::State::CanRead))
            flags |= GOLDFISH_PIPE_POLL_IN;
        if (static_cast<int>(state) &
            static_cast<int>(RenderChannel::State::CanWrite))
            flags |= GOLDFISH_PIPE_POLL_OUT;
        if (static_cast<int>(state) &
            static_cast<int>(RenderChannel::State::Stopped))
            flags |= GOLDFISH_PIPE_POLL_HUP;
        return static_cast<GoldfishPipePollFlags>(flags);
    }

    int recv(GoldfishPipeBuffer* bufs, int nbufs) override {
        // Drain from read buffer first, then try channel.
        int total = 0;
        for (int i = 0; i < nbufs; i++) {
            size_t want = bufs[i].size;
            size_t got = 0;
            while (got < want) {
                if (mReadBuf.empty()) {
                    auto result = mChannel->tryRead(&mReadBuf);
                    if (result == RenderChannel::IoResult::TryAgain) {
                        if (total + got > 0) {
                            total += got;
                            return total;
                        }
                        return GOLDFISH_PIPE_ERROR_AGAIN;
                    }
                    if (result != RenderChannel::IoResult::Ok) {
                        return total > 0 ? total : GOLDFISH_PIPE_ERROR_IO;
                    }
                    mReadPos = 0;
                }
                size_t avail = mReadBuf.size() - mReadPos;
                size_t copy = std::min(want - got, avail);
                memcpy(static_cast<char*>(bufs[i].data) + got,
                       mReadBuf.data() + mReadPos, copy);
                got += copy;
                mReadPos += copy;
                if (mReadPos == mReadBuf.size()) {
                    mReadBuf.clear();
                    mReadPos = 0;
                }
            }
            total += got;
        }
        return total;
    }

    int send(const GoldfishPipeBuffer* bufs, int nbufs) override {
        int total = 0;
        for (int i = 0; i < nbufs; i++) {
            RenderChannel::Buffer cb;
            cb.resize_noinit(bufs[i].size);
            memcpy(cb.data(), bufs[i].data, bufs[i].size);
            auto result = mChannel->tryWrite(std::move(cb));
            if (result == RenderChannel::IoResult::TryAgain) {
                return total > 0 ? total : GOLDFISH_PIPE_ERROR_AGAIN;
            }
            if (result != RenderChannel::IoResult::Ok) {
                return total > 0 ? total : GOLDFISH_PIPE_ERROR_IO;
            }
            total += bufs[i].size;
        }
        return total;
    }

    void wakeOn(GoldfishPipeWakeFlags flags) override {
        // Re-arm the RenderChannel event subscription so we get notified
        // again when the channel becomes readable/writable.
        if (!mChannel) return;
        int wanted = 0;
        if (flags & GOLDFISH_PIPE_WAKE_READ)
            wanted |= static_cast<int>(RenderChannel::State::CanRead);
        if (flags & GOLDFISH_PIPE_WAKE_WRITE)
            wanted |= static_cast<int>(RenderChannel::State::CanWrite);
        if (wanted) {
            mChannel->setWantedEvents(
                static_cast<RenderChannel::State>(wanted));
        }
    }

   private:
    explicit RenderThreadPipe(RenderChannelPtr ch)
        : mChannel(std::move(ch)) {}

    RenderChannelPtr mChannel;
    RenderChannel::Buffer mReadBuf;
    size_t mReadPos = 0;
};

// ============================================================================
// Service registry
// ============================================================================

using ServiceFactory = std::function<std::unique_ptr<HostPipe>()>;

static std::unordered_map<std::string, ServiceFactory>& serviceMap() {
    static auto* m = new std::unordered_map<std::string, ServiceFactory>;
    return *m;
}

static void registerBuiltinServices() {
    serviceMap()["refcount"] = []() -> std::unique_ptr<HostPipe> {
        return std::make_unique<RefcountPipe>();
    };
    serviceMap()["opengles"] = []() -> std::unique_ptr<HostPipe> {
        return RenderThreadPipe::Create();
    };
    serviceMap()["GLProcessPipe"] = []() -> std::unique_ptr<HostPipe> {
        return std::make_unique<ProcessPipe>();
    };
}

static std::unique_ptr<HostPipe> createService(const std::string& name) {
    auto it = serviceMap().find(name);
    if (it == serviceMap().end()) {
        GFXSTREAM_ERROR("Goldfish pipe: unknown service '%s'", name.c_str());
        return nullptr;
    }
    auto pipe = it->second();
    if (!pipe) {
        GFXSTREAM_ERROR("Goldfish pipe: failed to create service '%s'",
                        name.c_str());
    }
    return pipe;
}

// ---------------------------------------------------------------------------
// ConnectorPipe::send – parse the service name and swap in the real pipe.
// ---------------------------------------------------------------------------

int ConnectorPipe::send(const GoldfishPipeBuffer* bufs, int nbufs) {
    int total = 0;
    for (int i = 0; i < nbufs; i++) {
        auto* data = static_cast<const char*>(bufs[i].data);
        for (size_t j = 0; j < bufs[i].size; j++) {
            if (data[j] == '\0') {
                total += j + 1;
                goto parse;
            }
            mAccum.push_back(data[j]);
        }
        total += bufs[i].size;
    }
    return total;

parse:
    // Expected format: "pipe:<service>" or "pipe:<service>:<args>"
    std::string name = mAccum;
    if (name.compare(0, 5, "pipe:") == 0) {
        name = name.substr(5);
    }
    auto colon = name.find(':');
    if (colon != std::string::npos) {
        name = name.substr(0, colon);
    }

    GFXSTREAM_DEBUG("Goldfish pipe connector: service='%s'", name.c_str());

    mSwappedPipe = createService(name);
    if (!mSwappedPipe) {
        return GOLDFISH_PIPE_ERROR_INVAL;
    }

    // The swapped pipe is ready. svc_guest_send() will pick it up via
    // takeSwappedPipe() and update the device-side pointer through the
    // double-pointer mechanism.
    return total;
}

// ============================================================================
// GoldfishPipeServiceOps callbacks
// ============================================================================

static GoldfishHostPipe* svc_guest_open(GoldfishHwPipe* hw_pipe) {
    auto pipe = std::make_unique<ConnectorPipe>();
    pipe->setHwPipe(hw_pipe);
    return reinterpret_cast<GoldfishHostPipe*>(pipe.release());
}

static GoldfishHostPipe* svc_guest_open_with_flags(GoldfishHwPipe* hw_pipe,
                                                   uint32_t) {
    return svc_guest_open(hw_pipe);
}

static void svc_guest_close(GoldfishHostPipe* host_pipe,
                            GoldfishPipeCloseReason) {
    delete reinterpret_cast<HostPipe*>(host_pipe);
}

static GoldfishPipePollFlags svc_guest_poll(GoldfishHostPipe* host_pipe) {
    return reinterpret_cast<HostPipe*>(host_pipe)->poll();
}

static int svc_guest_recv(GoldfishHostPipe* host_pipe,
                          GoldfishPipeBuffer* bufs, int nbufs) {
    return reinterpret_cast<HostPipe*>(host_pipe)->recv(bufs, nbufs);
}

static void svc_wait_guest_recv(GoldfishHostPipe*) {
    // Blocking wait not needed for the current services.
}

static int svc_guest_send(GoldfishHostPipe** host_pipe,
                          const GoldfishPipeBuffer* bufs, int nbufs) {
    HostPipe* current = reinterpret_cast<HostPipe*>(*host_pipe);
    int result = current->send(bufs, nbufs);

    // If this was a ConnectorPipe that completed the service handshake,
    // swap in the real service pipe through the double-pointer.
    if (current->isConnector()) {
        auto* connector = static_cast<ConnectorPipe*>(current);
        HostPipe* swapped = connector->takeSwappedPipe();
        if (swapped) {
            swapped->setHwPipe(connector->hwPipe());
            *host_pipe = reinterpret_cast<GoldfishHostPipe*>(swapped);
            swapped->onSwappedIn();
            delete connector;
        }
    }

    return result;
}

static void svc_wait_guest_send(GoldfishHostPipe*) {
    // Blocking wait not needed for the current services.
}

static void svc_guest_wake_on(GoldfishHostPipe* host_pipe,
                              GoldfishPipeWakeFlags flags) {
    reinterpret_cast<HostPipe*>(host_pipe)->wakeOn(flags);
}

// Snapshot: deferred per PLAN.md §4 "Snapshot policy".
//
// First-pass behavior:
//   - save: per-pipe save() is a no-op (state is not persisted).
//   - load: always force-closes the pipe.  The guest kernel interprets
//     force_close=1 by closing the fd, which triggers re-open after
//     resume.  This is the documented fallback for services that
//     cannot preserve state across snapshots.
//   - refcount pipes restore correctly because the guest re-opens
//     them; the handle is re-sent by the guest allocator.
//
// Future work: persist refcount handle in save/load, persist
// RenderChannel state for opengles pipes.

static void svc_guest_pre_load(void*) {}
static void svc_guest_post_load(void*) {}
static void svc_guest_pre_save(void*) {}
static void svc_guest_post_save(void*) {}

static GoldfishHostPipe* svc_guest_load(void*, GoldfishHwPipe*, char* fc) {
    if (fc) *fc = 1;
    return nullptr;
}

static void svc_guest_save(GoldfishHostPipe* host_pipe, void* stream) {
    reinterpret_cast<HostPipe*>(host_pipe)->save(stream);
}

// DMA stubs – goldfish_pipe DMA is not used in our stack.
static void svc_dma_add_buffer(void*, uint64_t, uint64_t) {}
static void svc_dma_remove_buffer(uint64_t) {}
static void svc_dma_invalidate(void) {}
static void svc_dma_reset(void) {}
static void svc_dma_save(void*) {}
static void svc_dma_load(void*) {}

static const GoldfishPipeServiceOps s_service_ops = {
    .guest_open = svc_guest_open,
    .guest_open_with_flags = svc_guest_open_with_flags,
    .guest_close = svc_guest_close,
    .guest_pre_load = svc_guest_pre_load,
    .guest_post_load = svc_guest_post_load,
    .guest_pre_save = svc_guest_pre_save,
    .guest_post_save = svc_guest_post_save,
    .guest_load = svc_guest_load,
    .guest_save = svc_guest_save,
    .guest_poll = svc_guest_poll,
    .guest_recv = svc_guest_recv,
    .wait_guest_recv = svc_wait_guest_recv,
    .guest_send = svc_guest_send,
    .wait_guest_send = svc_wait_guest_send,
    .guest_wake_on = svc_guest_wake_on,
    .dma_add_buffer = svc_dma_add_buffer,
    .dma_remove_buffer = svc_dma_remove_buffer,
    .dma_invalidate_host_mappings = svc_dma_invalidate,
    .dma_reset_host_mappings = svc_dma_reset,
    .dma_save_mappings = svc_dma_save,
    .dma_load_mappings = svc_dma_load,
};

}  // anonymous namespace

const GoldfishPipeServiceOps* goldfish_pipe_service_init(RendererPtr renderer) {
    s_renderer = renderer;
    registerBuiltinServices();
    GFXSTREAM_INFO("Goldfish pipe services registered: refcount, opengles, GLProcessPipe");
    return &s_service_ops;
}

}  // namespace host
}  // namespace gfxstream
