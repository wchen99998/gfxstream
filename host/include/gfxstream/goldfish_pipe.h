// Copyright 2020 The Android Open Source Project
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

// Owned goldfish pipe ABI header.
//
// This header is the single source of truth for the goldfish-pipe service
// operations ABI shared by QEMU, rutabaga FFI, and gfxstream host.  It is
// intentionally C-only and does not depend on QEMUFile or any other QEMU
// internal type so that it can be compiled by all three projects through the
// local sysroot.

#ifndef GFXSTREAM_GOLDFISH_PIPE_H
#define GFXSTREAM_GOLDFISH_PIPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer descriptor for pipe send/recv operations. */
typedef struct GoldfishPipeBuffer {
    void* data;
    size_t size;
} GoldfishPipeBuffer;

/* Bitflags returned by guest_poll(). */
typedef enum {
    GOLDFISH_PIPE_POLL_IN = (1 << 0),   /* guest can read */
    GOLDFISH_PIPE_POLL_OUT = (1 << 1),  /* guest can write */
    GOLDFISH_PIPE_POLL_HUP = (1 << 2),  /* closed by host */
} GoldfishPipePollFlags;

/* Bitflags used with goldfish_pipe_signal_wake() and guest_wake_on(). */
typedef enum {
    GOLDFISH_PIPE_WAKE_CLOSED = (1 << 0),
    GOLDFISH_PIPE_WAKE_READ = (1 << 1),
    GOLDFISH_PIPE_WAKE_WRITE = (1 << 2),
    GOLDFISH_PIPE_WAKE_UNLOCK_DMA = (1 << 3),
} GoldfishPipeWakeFlags;

/* Error values returned by guest_recv() / guest_send(). */
typedef enum {
    GOLDFISH_PIPE_ERROR_INVAL = -1,
    GOLDFISH_PIPE_ERROR_AGAIN = -2,
    GOLDFISH_PIPE_ERROR_NOMEM = -3,
    GOLDFISH_PIPE_ERROR_IO = -4,
} GoldfishPipeError;

/* Reasons why the pipe is closed. */
typedef enum {
    GOLDFISH_PIPE_CLOSE_GRACEFUL = 0,
    GOLDFISH_PIPE_CLOSE_REBOOT = 1,
    GOLDFISH_PIPE_CLOSE_LOAD_SNAPSHOT = 2,
    GOLDFISH_PIPE_CLOSE_ERROR = 3,
} GoldfishPipeCloseReason;

/* Opaque handle for the hardware (device) side of a pipe connection. */
typedef struct GoldfishHwPipe GoldfishHwPipe;

/* Opaque handle for the host (service) side of a pipe connection. */
typedef struct GoldfishHostPipe GoldfishHostPipe;

/*
 * Hardware-side callbacks that the device provides to the host service layer.
 * These let the host signal events back to the guest.
 */
typedef struct GoldfishPipeHwFuncs {
    /* Tell the device to wake the guest on |flags| events. */
    void (*signal_wake)(GoldfishHwPipe* hw_pipe, GoldfishPipeWakeFlags flags);

    /* Close a pipe from the host side. */
    void (*close_from_host)(GoldfishHwPipe* hw_pipe);

    /* Replace the host-pipe associated with |hw_pipe|.
     * Used during the connector→service-pipe swap. */
    void (*reset_pipe)(GoldfishHwPipe* hw_pipe, GoldfishHostPipe* new_host_pipe);

    /* Return a unique integer ID for this hw_pipe (for snapshot). */
    int (*get_id)(GoldfishHwPipe* hw_pipe);

    /* Lookup a hw_pipe by its integer ID (for snapshot restore). */
    GoldfishHwPipe* (*lookup_by_id)(int id);
} GoldfishPipeHwFuncs;

/*
 * Service-side callbacks provided by the host to the virtual device.
 *
 * The device calls these when the guest performs I/O on a pipe.
 * Snapshot callbacks use opaque stream pointers (void*) rather than
 * QEMUFile* to keep the ABI independent of QEMU internals.
 */
typedef struct GoldfishPipeServiceOps {
    /* Create a new host-side pipe instance. */
    GoldfishHostPipe* (*guest_open)(GoldfishHwPipe* hw_pipe);
    GoldfishHostPipe* (*guest_open_with_flags)(GoldfishHwPipe* hw_pipe,
                                               uint32_t flags);

    /* Close and free a pipe. */
    void (*guest_close)(GoldfishHostPipe* host_pipe,
                        GoldfishPipeCloseReason reason);

    /* Snapshot lifecycle (called once per snapshot operation). */
    void (*guest_pre_load)(void* stream);
    void (*guest_post_load)(void* stream);
    void (*guest_pre_save)(void* stream);
    void (*guest_post_save)(void* stream);

    /* Per-pipe snapshot. */
    GoldfishHostPipe* (*guest_load)(void* stream, GoldfishHwPipe* hw_pipe,
                                    char* force_close);
    void (*guest_save)(GoldfishHostPipe* host_pipe, void* stream);

    /* Poll for readable / writable state. */
    GoldfishPipePollFlags (*guest_poll)(GoldfishHostPipe* host_pipe);

    /* Guest→host data transfer. Returns bytes transferred or negative error. */
    int (*guest_recv)(GoldfishHostPipe* host_pipe, GoldfishPipeBuffer* buffers,
                      int num_buffers);
    void (*wait_guest_recv)(GoldfishHostPipe* host_pipe);

    /* Host→guest data transfer. Returns bytes transferred or negative error.
     * |host_pipe| is a double-pointer to allow the connector→service swap. */
    int (*guest_send)(GoldfishHostPipe** host_pipe,
                      const GoldfishPipeBuffer* buffers, int num_buffers);
    void (*wait_guest_send)(GoldfishHostPipe* host_pipe);

    /* Wake-on registration. */
    void (*guest_wake_on)(GoldfishHostPipe* host_pipe,
                          GoldfishPipeWakeFlags wake_flags);

    /* DMA buffer plumbing (may be NULL when DMA is not used). */
    void (*dma_add_buffer)(void* pipe, uint64_t guest_paddr, uint64_t sz);
    void (*dma_remove_buffer)(uint64_t guest_paddr);
    void (*dma_invalidate_host_mappings)(void);
    void (*dma_reset_host_mappings)(void);
    void (*dma_save_mappings)(void* stream);
    void (*dma_load_mappings)(void* stream);
} GoldfishPipeServiceOps;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* GFXSTREAM_GOLDFISH_PIPE_H */
