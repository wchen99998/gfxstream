// Copyright 2026 The Android Open Source Project
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

#include <gtest/gtest.h>

#include <future>

#include "gfxstream/host/gfxstream_format.h"
#include "host/color_buffer.h"
#include "host/sync_thread.h"
#include "display_vk.h"
#include "vk_common_operations.h"
#include "vk_decoder_global_state.h"
#include "vulkan_dispatch.h"

namespace gfxstream {
namespace host {
namespace vk {
namespace {

constexpr uint32_t kColorBufferWidth = 32;
constexpr uint32_t kColorBufferHeight = 32;
constexpr uint32_t kColorBufferMipLevels = 1;

gfxstream::host::FeatureSet getTestFeatures() {
    gfxstream::host::FeatureSet features;
    features.GlDirectMem.enabled = true;
    return features;
}

class LeaseTestHelper {
   public:
    LeaseTestHelper()
        : mVk(vkDispatch(/*forTesting=*/true)),
          mEmu(VkEmulation::create(mVk, {}, getTestFeatures())) {
        SyncThread::initialize(/*hasGl=*/false);
    }

    ~LeaseTestHelper() {
        VkDecoderGlobalState::reset();
        mEmu.reset();
        SyncThread::destroy();
    }

    void initialize(VkEmulation::Features features = {}) {
        ASSERT_NE(mVk, nullptr);
        ASSERT_NE(mEmu, nullptr);
        VkDecoderGlobalState::initialize(mEmu.get());
        mEmu->initFeatures(std::move(features));
    }

    VkEmulation* emu() const { return mEmu.get(); }

   private:
    VulkanDispatch* mVk = nullptr;
    std::unique_ptr<VkEmulation> mEmu;
};

void createColorBuffer(VkEmulation* emu, uint32_t colorBufferHandle) {
    ASSERT_NE(emu, nullptr);
    ASSERT_TRUE(emu->createVkColorBuffer(kColorBufferWidth, kColorBufferHeight,
                                         GfxstreamFormat::R8G8B8A8_UNORM, colorBufferHandle,
                                         /*vulkanOnly=*/true,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                         kColorBufferMipLevels));
}

TEST(VkCommonOperationsTest, StaleDisplayLeaseDoesNotReleaseCurrentGeneration) {
    LeaseTestHelper vkTest;
    vkTest.initialize();

    auto* emu = vkTest.emu();
    constexpr uint32_t kColorBufferHandle = 1001;
    createColorBuffer(emu, kColorBufferHandle);

    auto firstLease = emu->acquireColorBufferDisplayLease(kColorBufferHandle);
    ASSERT_NE(firstLease, nullptr);
    EXPECT_TRUE(emu->isColorBufferDisplayLeaseCurrent(*firstLease));
    EXPECT_EQ(emu->releaseColorBufferDisplayLease(*firstLease, /*waitForCompletion=*/false),
              VkEmulation::DisplayLeaseReleaseResult::kReleased);

    auto secondLease = emu->acquireColorBufferDisplayLease(kColorBufferHandle);
    ASSERT_NE(secondLease, nullptr);
    EXPECT_NE(firstLease->leaseGeneration, secondLease->leaseGeneration);
    EXPECT_FALSE(emu->isColorBufferDisplayLeaseCurrent(*firstLease));
    EXPECT_TRUE(emu->isColorBufferDisplayLeaseCurrent(*secondLease));

    EXPECT_EQ(emu->releaseColorBufferDisplayLease(*firstLease, /*waitForCompletion=*/false),
              VkEmulation::DisplayLeaseReleaseResult::kNotLeased);
    EXPECT_TRUE(emu->isColorBufferDisplayLeaseCurrent(*secondLease));

    EXPECT_EQ(emu->releaseColorBufferDisplayLease(*secondLease, /*waitForCompletion=*/false),
              VkEmulation::DisplayLeaseReleaseResult::kReleased);
    EXPECT_TRUE(emu->teardownVkColorBuffer(kColorBufferHandle));
}

TEST(VkCommonOperationsTest, ReleaseColorBufferDisplayLeaseHonorsDisplayCompletion) {
    LeaseTestHelper vkTest;
    vkTest.initialize();

    auto* emu = vkTest.emu();
    constexpr uint32_t kColorBufferHandle = 1002;
    createColorBuffer(emu, kColorBufferHandle);

    auto lease = emu->acquireColorBufferDisplayLease(kColorBufferHandle);
    ASSERT_NE(lease, nullptr);

    std::promise<CancelableFutureStatus> displayCompletion;
    emu->setColorBufferLatestDisplayUseCompletion(*lease, displayCompletion.get_future().share());

    EXPECT_EQ(emu->releaseColorBufferDisplayLease(*lease, /*waitForCompletion=*/false),
              VkEmulation::DisplayLeaseReleaseResult::kBusy);

    displayCompletion.set_value(CancelableFutureStatus::kSuccess);

    EXPECT_EQ(emu->releaseColorBufferDisplayLease(*lease, /*waitForCompletion=*/false),
              VkEmulation::DisplayLeaseReleaseResult::kReleased);
    EXPECT_TRUE(emu->teardownVkColorBuffer(kColorBufferHandle));
}

TEST(VkCommonOperationsTest, ReleaseColorBufferDisplayLeaseWaitsForPreviousUseAcrossImmediatePost) {
    LeaseTestHelper vkTest;
    vkTest.initialize(VkEmulation::Features{
        .useVulkanNativeSwapchain = true,
    });

    auto* emu = vkTest.emu();
    ASSERT_NE(emu, nullptr);

    auto* display = emu->getDisplay();
    ASSERT_NE(display, nullptr);

    auto colorBuffer =
        ColorBuffer::create(/*emulationGl=*/nullptr, emu, kColorBufferWidth, kColorBufferHeight,
                            GfxstreamFormat::R8G8B8A8_UNORM, /*handle=*/1008, /*stream=*/nullptr);
    ASSERT_NE(colorBuffer, nullptr);

    auto lease = emu->acquireColorBufferDisplayLease(colorBuffer->getHndl());
    ASSERT_NE(lease, nullptr);

    std::promise<CancelableFutureStatus> firstDisplayCompletion;
    emu->setColorBufferLatestDisplayUseCompletion(*lease,
                                                  firstDisplayCompletion.get_future().share());

    auto post = display->postColorBuffer(colorBuffer, /*rotationDegrees=*/0.0f,
                                         /*colorTransform=*/std::nullopt);
    ASSERT_TRUE(post.success);
    post.postCompletedWaitable.wait();

    EXPECT_EQ(emu->releaseColorBufferDisplayLease(*lease, /*waitForCompletion=*/false),
              VkEmulation::DisplayLeaseReleaseResult::kBusy);

    firstDisplayCompletion.set_value(CancelableFutureStatus::kSuccess);

    EXPECT_EQ(emu->releaseColorBufferDisplayLease(*lease, /*waitForCompletion=*/true),
              VkEmulation::DisplayLeaseReleaseResult::kReleased);
}

TEST(VkCommonOperationsTest, ReleaseColorBufferDisplayLeaseHandlesCanceledDisplayCompletion) {
    LeaseTestHelper vkTest;
    vkTest.initialize();

    auto* emu = vkTest.emu();
    constexpr uint32_t kColorBufferHandle = 1009;
    createColorBuffer(emu, kColorBufferHandle);

    auto lease = emu->acquireColorBufferDisplayLease(kColorBufferHandle);
    ASSERT_NE(lease, nullptr);

    CancelableFuture canceledCompletion;
    {
        auto canceledPromise = std::make_unique<AutoCancelingPromise>();
        canceledCompletion = canceledPromise->GetFuture();
    }
    emu->setColorBufferLatestDisplayUseCompletion(*lease, canceledCompletion);

    EXPECT_EQ(emu->releaseColorBufferDisplayLease(*lease, /*waitForCompletion=*/true),
              VkEmulation::DisplayLeaseReleaseResult::kReleased);
    EXPECT_TRUE(emu->teardownVkColorBuffer(kColorBufferHandle));
}

TEST(VkCommonOperationsTest, TeardownActiveDisplayLeaseFailsFast) {
    LeaseTestHelper vkTest;
    vkTest.initialize();

    auto* emu = vkTest.emu();
    constexpr uint32_t kColorBufferHandle = 1005;
    createColorBuffer(emu, kColorBufferHandle);

    auto lease = emu->acquireColorBufferDisplayLease(kColorBufferHandle);
    ASSERT_NE(lease, nullptr);

    ASSERT_DEATH(
        {
            emu->teardownVkColorBuffer(kColorBufferHandle);
        },
        "Destroying ColorBuffer");
}

TEST(VkCommonOperationsTest, DisplayVkRetainsColorBufferWhileLeaseIsActive) {
    LeaseTestHelper vkTest;
    vkTest.initialize(VkEmulation::Features{
        .useVulkanNativeSwapchain = true,
    });

    auto* emu = vkTest.emu();
    ASSERT_NE(emu, nullptr);

    auto* display = emu->getDisplay();
    ASSERT_NE(display, nullptr);

    auto firstColorBuffer =
        ColorBuffer::create(/*emulationGl=*/nullptr, emu, kColorBufferWidth, kColorBufferHeight,
                            GfxstreamFormat::R8G8B8A8_UNORM, /*handle=*/1006, /*stream=*/nullptr);
    auto secondColorBuffer =
        ColorBuffer::create(/*emulationGl=*/nullptr, emu, kColorBufferWidth, kColorBufferHeight,
                            GfxstreamFormat::R8G8B8A8_UNORM, /*handle=*/1007, /*stream=*/nullptr);
    ASSERT_NE(firstColorBuffer, nullptr);
    ASSERT_NE(secondColorBuffer, nullptr);
    EXPECT_EQ(firstColorBuffer.use_count(), 1);
    EXPECT_EQ(secondColorBuffer.use_count(), 1);

    auto firstPost = display->postColorBuffer(firstColorBuffer, /*rotationDegrees=*/0.0f,
                                              /*colorTransform=*/std::nullopt);
    ASSERT_TRUE(firstPost.success);
    firstPost.postCompletedWaitable.wait();
    EXPECT_EQ(firstColorBuffer.use_count(), 2);
    EXPECT_EQ(secondColorBuffer.use_count(), 1);

    auto secondPost = display->postColorBuffer(secondColorBuffer, /*rotationDegrees=*/0.0f,
                                               /*colorTransform=*/std::nullopt);
    ASSERT_TRUE(secondPost.success);
    secondPost.postCompletedWaitable.wait();
    EXPECT_EQ(firstColorBuffer.use_count(), 1);
    EXPECT_EQ(secondColorBuffer.use_count(), 2);

    display->unbindFromSurface();
    EXPECT_EQ(secondColorBuffer.use_count(), 1);
}

TEST(VkCommonOperationsTest, ReleaseAllDisplayLeasesClearsEveryActiveLease) {
    LeaseTestHelper vkTest;
    vkTest.initialize();

    auto* emu = vkTest.emu();
    constexpr uint32_t kFirstColorBufferHandle = 1003;
    constexpr uint32_t kSecondColorBufferHandle = 1004;
    createColorBuffer(emu, kFirstColorBufferHandle);
    createColorBuffer(emu, kSecondColorBufferHandle);

    auto firstLease = emu->acquireColorBufferDisplayLease(kFirstColorBufferHandle);
    auto secondLease = emu->acquireColorBufferDisplayLease(kSecondColorBufferHandle);
    ASSERT_NE(firstLease, nullptr);
    ASSERT_NE(secondLease, nullptr);

    emu->releaseAllDisplayLeases();

    EXPECT_FALSE(emu->isColorBufferDisplayLeaseCurrent(*firstLease));
    EXPECT_FALSE(emu->isColorBufferDisplayLeaseCurrent(*secondLease));
    EXPECT_TRUE(emu->teardownVkColorBuffer(kFirstColorBufferHandle));
    EXPECT_TRUE(emu->teardownVkColorBuffer(kSecondColorBufferHandle));
}

}  // namespace
}  // namespace vk
}  // namespace host
}  // namespace gfxstream
