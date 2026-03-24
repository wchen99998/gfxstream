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

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include "gfxstream/host/gfxstream_format.h"

namespace gfxstream {
namespace host {
namespace vk {

struct DisplayLeaseInfoVk {
    uint32_t colorBufferHandle = 0;
    uint64_t leaseGeneration = 0;
    VkImage image = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageCreateInfo imageCreateInfo = {};
    GfxstreamFormat imageFormat = GfxstreamFormat::UNKNOWN;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
};

}  // namespace vk
}  // namespace host
}  // namespace gfxstream
