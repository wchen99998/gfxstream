// Copyright 2023 The Android Open Source Project
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

#include "gfxstream/goldfish_pipe.h"
#include "gfxstream/virtio-gpu-gfxstream-renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the host service ops table.  Returns the previously installed
 * table, or NULL.  The caller must ensure the ops table outlives any
 * active pipe connections. */
VG_EXPORT const GoldfishPipeServiceOps* stream_renderer_set_service_ops(
    const GoldfishPipeServiceOps* ops);

/* Retrieve the currently active service ops table (may be NULL). */
VG_EXPORT const GoldfishPipeServiceOps* stream_renderer_get_service_ops(void);

/* Install hardware-side callbacks so the host service bridge can signal
 * wake events back to the virtual device.  Returns the previous pointer. */
VG_EXPORT const GoldfishPipeHwFuncs* stream_renderer_set_service_hw_funcs(
    const GoldfishPipeHwFuncs* hw_funcs);

#ifdef __cplusplus
}  // extern "C"
#endif
