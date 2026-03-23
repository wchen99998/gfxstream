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

#include <memory>

#include "gfxstream/goldfish_pipe.h"
#include "render-utils/Renderer.h"

namespace gfxstream {
namespace host {

// Initialize the goldfish pipe service registry.
// |renderer| is used by the opengles service to create RenderChannels.
// Registers the built-in services (refcount, opengles, GLProcessPipe) and
// returns a GoldfishPipeServiceOps table that can be installed into the
// virtual device via stream_renderer_set_service_ops().
//
// The returned pointer is a process-lifetime singleton – it must not be freed.
const GoldfishPipeServiceOps* goldfish_pipe_service_init(RendererPtr renderer);

}  // namespace host
}  // namespace gfxstream
