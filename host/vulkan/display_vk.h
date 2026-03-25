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
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either expresso or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef DISPLAY_VK_H
#define DISPLAY_VK_H

#include <deque>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "gfxstream/host/borrowed_image.h"
#include "compositor_vk.h"
#include "display_lease_vk.h"
#include "gfxstream/host/display.h"
#include "display_surface_vk.h"
#include "host/hwc2.h"
#include "swap_chain_state_vk.h"
#include "gfxstream/synchronization/Lock.h"
#include "goldfish_vk_dispatch.h"

// The DisplayVk class holds the Vulkan and other states required to draw a
// frame in a host window.

namespace gfxstream {
namespace host {
class ColorBuffer;

namespace vk {

class VkEmulation;
class PostWorkerVk;
struct BorrowedImageInfoVk;

class DisplayVk : public Display {
   public:
    DisplayVk(const VulkanDispatch&, VkPhysicalDevice, VkDevice, CompositorVk* compositorVk,
              uint32_t compositorQueueFamilyIndex, VkQueue compositorVkQueue,
              std::shared_ptr<gfxstream::base::Lock> compositorVkQueueLock,
              uint32_t swapChainQueueFamilyIndex, VkQueue swapChainVkQueue,
              std::shared_ptr<gfxstream::base::Lock> swapChainVkQueueLock,
              VkEmulation* vkEmulation = nullptr);
    ~DisplayVk();

    PostResult post(const BorrowedImageInfo* info, float rotationDegrees,
                    const std::optional<std::array<float, 16>>& colorTransform);
    PostResult post(const DisplayLeaseInfoVk& info, float rotationDegrees,
                    const std::optional<std::array<float, 16>>& colorTransform);
    PostResult postColorBuffer(const std::shared_ptr<ColorBuffer>& colorBuffer,
                               float rotationDegrees,
                               const std::optional<std::array<float, 16>>& colorTransform);

    void drainQueues();
    void clear();

    uint64_t getSourceBorrowSubmitCountForTesting() const;
    std::optional<uint32_t> getCachedLeaseColorBufferHandleForTesting() const;

   protected:
    void bindToSurfaceImpl(DisplaySurface* surface) override;
    void surfaceUpdated(DisplaySurface* surface) override;
    void unbindFromSurfaceImpl() override;

   private:
    friend class PostWorkerVk;

    class PostResource;
    struct PendingPost;
    struct SourceImageBarriers {
        std::vector<VkImageMemoryBarrier> preUseQueueTransferBarriers;
        std::vector<VkImageMemoryBarrier> preUseLayoutTransitionBarriers;
        std::vector<VkImageMemoryBarrier> postUseLayoutTransitionBarriers;
        std::vector<VkImageMemoryBarrier> postUseQueueTransferBarriers;
    };

    static SourceImageBarriers buildSourceImageBarriers(const BorrowedImageInfoVk& info,
                                                        uint32_t usedQueueFamilyIndex);
    void destroySwapchain();
    bool recreateSwapchain();
    bool reclaimPendingPost(std::optional<PendingPost>& pendingPost, bool waitForCompletion);
    void reclaimPendingPosts(bool waitForCompletion);
    void retireActiveColorBufferLease();
    void reclaimRetiredColorBufferLeases(bool waitForCompletion);
    void releaseRetainedColorBufferLeases(bool waitForCompletion);
    bool isActiveColorBufferLeaseCurrent(uint32_t colorBufferHandle) const;

    // The success component of the result is false when the swapchain is no longer valid and
    // bindToSurface() needs to be called again. When the success component is true, the waitable
    // component of the returned result is a future that will complete when the GPU side of work
    // completes. The caller is responsible to guarantee the synchronization and the layout of
    // ColorBufferCompositionInfo::m_vkImage is VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL.
    PostResult postBorrowedImageImpl(const BorrowedImageInfoVk& info, float rotationDegrees,
                                     const std::optional<std::array<float, 16>>& colorTransform);
    PostResult postLeaseImpl(const DisplayLeaseInfoVk& info, float rotationDegrees,
                             const std::optional<std::array<float, 16>>& colorTransform);
    PostResult postSourceImageImpl(VkImage image, VkImageView imageView,
                                   const VkImageCreateInfo& imageCreateInfo,
                                   const SourceImageBarriers& sourceImageBarriers,
                                   float rotationDegrees,
                                   const std::optional<std::array<float, 16>>& colorTransform);

    VkFormatFeatureFlags getFormatFeatures(VkFormat, VkImageTiling);
    bool canPost(const VkImageCreateInfo&);

    const VulkanDispatch& m_vk;
    VkPhysicalDevice m_vkPhysicalDevice;
    VkDevice m_vkDevice;
    CompositorVk* m_compositorVk;  // TODO(b/442394091): temporary addition, refactor compositor to
                                   // separate drawing routines like TextureDraw in GL side

    uint32_t m_compositorQueueFamilyIndex;
    VkQueue m_compositorVkQueue;
    std::shared_ptr<gfxstream::base::Lock> m_compositorVkQueueLock;
    uint32_t m_swapChainQueueFamilyIndex;
    VkQueue m_swapChainVkQueue;
    std::shared_ptr<gfxstream::base::Lock> m_swapChainVkQueueLock;
    VkCommandPool m_vkCommandPool;
    VkEmulation* const m_vkEmulation;

    class PostResource {
       public:
        const VkFence m_swapchainImageReleaseFence;
        const VkSemaphore m_swapchainImageAcquireSemaphore;
        const VkSemaphore m_swapchainImageReleaseSemaphore;
        const VkCommandBuffer m_vkCommandBuffer;
        static std::shared_ptr<PostResource> create(const VulkanDispatch&, VkDevice, VkCommandPool);
        ~PostResource();
        DISALLOW_COPY_ASSIGN_AND_MOVE(PostResource);

       private:
        PostResource(const VulkanDispatch&, VkDevice, VkCommandPool,
                     VkFence swapchainImageReleaseFence, VkSemaphore swapchainImageAcquireSemaphore,
                     VkSemaphore swapchainImageReleaseSemaphore, VkCommandBuffer);
        const VulkanDispatch& m_vk;
        const VkDevice m_vkDevice;
        const VkCommandPool m_vkCommandPool;
    };

    struct PendingPost {
        VkFence m_completeFence = VK_NULL_HANDLE;
        std::shared_future<std::shared_ptr<PostResource>> m_postResourceFuture;
    };

    struct RetainedDisplayLease {
        DisplayLeaseInfoVk info;
        std::shared_ptr<ColorBuffer> colorBuffer;
    };

    std::deque<std::shared_ptr<PostResource>> m_freePostResources;
    std::vector<std::optional<PendingPost>> m_pendingPosts;
    int m_inFlightFrameIndex;

    std::unique_ptr<SwapChainStateVk> m_swapChainStateVk;
    bool m_needToRecreateSwapChain = true;

    // The display lease must retain the ColorBuffer lifetime. Otherwise a guest-process cleanup
    // can destroy the backing VkImage while DisplayVk still owns the lease for it.
    std::optional<RetainedDisplayLease> m_activeColorBufferLease;
    std::vector<RetainedDisplayLease> m_retiredColorBufferLeases;

    std::unordered_map<VkFormat, VkFormatProperties> m_vkFormatProperties;
};

}  // namespace vk
}  // namespace host
}  // namespace gfxstream

#endif
