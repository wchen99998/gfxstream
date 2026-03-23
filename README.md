# Gfxstream

Graphics Streaming Kit, colloquially known as Gfxstream and previously known as
Vulkan Cereal, is a collection of code generators and libraries for streaming
rendering APIs from one place to another.

-   From a virtual machine guest to host for virtualized graphics
-   From one process to another for IPC graphics
-   From one computer to another via network sockets

## Table of Contents

-   [Architecture Overview](#architecture-overview)
-   [Repository Structure](#repository-structure)
-   [Core Components](#core-components)
    -   [Host (Server)](#host-server)
    -   [Guest (Client)](#guest-client)
    -   [Common](#common)
    -   [Code Generation](#code-generation)
    -   [Third-Party Dependencies](#third-party-dependencies)
-   [Data Flow](#data-flow)
    -   [Guest-to-Host Rendering Pipeline](#guest-to-host-rendering-pipeline)
    -   [Wire Protocol](#wire-protocol)
    -   [Transport Mechanisms](#transport-mechanisms)
-   [Key Subsystems](#key-subsystems)
    -   [FrameBuffer](#framebuffer)
    -   [VirtioGpuFrontend](#virtiogpufrontend)
    -   [RenderThread](#renderthread)
    -   [GLES Translation](#gles-translation)
    -   [Vulkan Streaming](#vulkan-streaming)
    -   [Snapshots](#snapshots)
    -   [Tracing](#tracing)
-   [Design Patterns](#design-patterns)
-   [Build Systems](#build-systems)
-   [Building](#building)
-   [Codegen](#codegen)
-   [Testing](#testing)
-   [CI/CD](#cicd)
-   [Contributing](#contributing)
-   [Notice](#notice)

---

## Architecture Overview

Gfxstream implements a **split client/server architecture** for streaming
graphics API calls (OpenGL ES, EGL, Vulkan, and a custom RenderControl
protocol) from a **guest** environment to a **host** environment where they are
executed on real (or software-emulated) GPU hardware.

```
┌──────────────────────────────────────────────────────────┐
│                     GUEST (VM / Process)                 │
│                                                          │
│  ┌──────────┐  ┌──────────┐  ┌───────────────────┐      │
│  │ GLESv1   │  │ GLESv2   │  │ RenderControl     │      │
│  │ Encoder  │  │ Encoder  │  │ Encoder           │      │
│  └────┬─────┘  └────┬─────┘  └────────┬──────────┘      │
│       │              │                 │                  │
│       └──────────────┼─────────────────┘                  │
│                      ▼                                    │
│          ┌───────────────────────┐                        │
│          │  OpenglSystemCommon   │                        │
│          │  (HostConnection)     │                        │
│          └───────────┬───────────┘                        │
│                      │                                    │
│              ┌───────┴───────┐                            │
│              │   Transport   │                            │
│              │  (Pipe/Ring)  │                            │
│              └───────┬───────┘                            │
└──────────────────────┼───────────────────────────────────┘
                       │  byte stream (opcodes + payloads)
┌──────────────────────┼───────────────────────────────────┐
│                      ▼           HOST (Emulator)         │
│          ┌───────────────────────┐                        │
│          │  VirtioGpuFrontend /  │                        │
│          │  GoldfishPipe         │                        │
│          └───────────┬───────────┘                        │
│                      ▼                                    │
│          ┌───────────────────────┐                        │
│          │    FrameBuffer        │                        │
│          │  (Global GPU State)   │                        │
│          └─────┬─────┬──────────┘                        │
│                │     │                                    │
│         ┌──────┘     └──────┐                             │
│         ▼                   ▼                             │
│  ┌─────────────┐   ┌──────────────┐                      │
│  │ GLES Path   │   │ Vulkan Path  │                      │
│  │ (Decoders + │   │ (vk_decoder  │                      │
│  │ Translator) │   │  + Cereal)   │                      │
│  └──────┬──────┘   └──────┬───────┘                      │
│         │                 │                               │
│         ▼                 ▼                               │
│  ┌──────────────────────────────┐                        │
│  │  Real GPU (via EGL/Vulkan)   │                        │
│  └──────────────────────────────┘                        │
└──────────────────────────────────────────────────────────┘
```

**Key principles:**

-   **Encoders** on the guest serialize API calls into an opcode-based wire
    protocol.
-   **Transport** carries the byte stream between guest and host (virtio-gpu
    rings, QEMU pipes, address-space graphics, or network sockets).
-   **Decoders** on the host deserialize the stream and invoke the corresponding
    native graphics API calls.
-   **Code generation** (emugen / apigen) produces much of the repetitive
    encoder/decoder boilerplate from API specification files.

---

## Repository Structure

```
gfxstream/
├── host/                   # Host-side renderer, decoders, and GPU backend
│   ├── gl/                 # GLES path: translator, decoders, dispatch
│   │   ├── glestranslator/ # EGL/GLES1/GLES2 → native GL translation
│   │   ├── gles1_dec/      # GLES 1.x decoder (generated)
│   │   ├── gles2_dec/      # GLES 2.x decoder (generated)
│   │   ├── glsnapshot/     # GL state snapshot support
│   │   └── OpenGLESDispatch/  # Dynamic loading of host GL functions
│   ├── vulkan/             # Vulkan path: decoder, dispatch, emulation
│   │   ├── cereal/         # Vulkan marshaling and generated wire code
│   │   └── emulated_textures/  # Texture format emulation
│   ├── renderControl_dec/  # RenderControl decoder (generated)
│   ├── common/             # Host APIs: display, streams, sync, VM ops
│   ├── include/            # Public host headers (C ABI)
│   ├── address_space/      # Address-space graphics integration
│   ├── compressed_textures/  # ASTC/ETC2 texture decompression
│   ├── decoder_common/     # Shared decoder utilities
│   ├── features/           # Feature flag management
│   ├── iostream/           # I/O stream abstractions
│   ├── library/            # RenderLib implementation
│   ├── snapshot/           # Host-side snapshot/restore support
│   ├── tracing/            # Perfetto tracing integration
│   ├── tests/              # Host unit tests
│   └── testlibs/           # Test helper libraries
│
├── guest/                  # Guest-side encoders and client libraries
│   ├── GLESv1_enc/         # GLES 1.x encoder (generated)
│   ├── GLESv2_enc/         # GLES 2.x encoder (generated)
│   ├── GLESv1/             # Guest GLES 1.x library
│   ├── GLESv2/             # Guest GLES 2.x library
│   ├── egl/                # Guest EGL implementation
│   ├── rendercontrol/      # Guest RenderControl interface
│   ├── renderControl_enc/  # RenderControl encoder (generated)
│   ├── OpenglCodecCommon/  # Shared codec utilities (checksums, state)
│   ├── OpenglSystemCommon/ # HostConnection, transport selection
│   ├── android-emu/        # Android emulator base (threads, ring buffer)
│   ├── qemupipe/           # QEMU pipe transport
│   ├── gralloc_cb/         # Gralloc (graphics allocator) stubs
│   ├── fuchsia/            # Fuchsia OS port
│   └── include/            # Guest-side headers
│
├── common/                 # Code shared between host and guest
│   ├── base/               # Cross-platform base library (threads, files, sync)
│   ├── logging/            # Logging infrastructure
│   ├── utils/              # General utilities
│   ├── image/              # Image format helpers
│   ├── detector/           # Graphics capability detection
│   ├── testenv/            # Test environment setup (Bazel)
│   └── etc/                # Miscellaneous shared code
│
├── codegen/                # API specification files and code generator
│   ├── generic-apigen/     # The emugen code generator tool
│   ├── gles1/              # GLES 1.x API specs (.in, .types, .attrib)
│   ├── gles2/              # GLES 2.x API specs
│   └── renderControl/      # RenderControl API specs
│
├── scripts/                # Build and codegen helper scripts
│   ├── generate-apigen-sources.sh    # Regenerate GLES/RenderControl code
│   ├── generate-gfxstream-vulkan.sh  # Regenerate Vulkan code
│   ├── generate-dispatch-headers.sh  # Regenerate dispatch headers
│   └── ...                           # Shader compilation, other utilities
│
├── third_party/            # Vendored headers and libraries
│   ├── vulkan/             # Vulkan headers
│   ├── opengl/             # OpenGL/GLES/EGL headers
│   ├── glm/                # OpenGL Mathematics library
│   ├── stb/                # STB single-file libraries
│   ├── drm/                # Direct Rendering Manager headers
│   ├── astc-encoder/       # ASTC texture codec
│   ├── renderdoc/          # RenderDoc integration headers
│   ├── x11/, xcb/          # X11/XCB windowing headers
│   └── ...                 # Platform-specific headers (QNX, Fuchsia, etc.)
│
├── tests/                  # Integration and end-to-end tests
│   ├── end2end/            # Full-stack GL/Vulkan integration tests
│   └── cuttlefish/         # Android Cuttlefish test helpers
│
├── toolchain/              # Build system toolchain setup
│   ├── bazel/              # Bazel installation and toolchain rules
│   ├── cmake/              # CMake helpers
│   └── meson/              # Meson helpers
│
├── docs/                   # Additional documentation
│   ├── design.md           # Design notes (guest Vulkan architecture)
│   ├── snapshot.md         # Vulkan snapshot/restore design
│   ├── tracing.md          # Perfetto tracing guide
│   └── ideas.md            # Future project ideas
│
├── .github/                # GitHub configuration
│   ├── workflows/          # CI workflows (presubmit, postsubmit)
│   └── CODEOWNERS          # Code ownership
│
├── CMakeLists.txt          # Root CMake project configuration
├── meson.build             # Root Meson project configuration
├── meson_options.txt       # Meson build options
├── MODULE.bazel            # Bazel module and external dependencies
├── BUILD.bazel             # Root Bazel package
├── .bazelrc                # Bazel defaults (graphics drivers, sanitizers)
├── Android.bp              # Android Soong build configuration
├── .clang-format           # C++ formatting rules (Google style, 4-space)
├── CONTRIBUTING.md         # Contribution guidelines
└── LICENSE                 # Apache 2.0
```

---

## Core Components

### Host (Server)

The host is the side that has access to real GPU hardware (or a software
renderer like SwiftShader). It receives encoded graphics commands, decodes them,
and executes them against the native graphics stack.

#### Entry Points

The host exposes two primary C-ABI surfaces:

1.  **Render API** (`host/render_api.cpp`) — `gfxstream::initLibrary()` returns
    a `RenderLibPtr` that provides the object-oriented interface for managing
    render channels, address-space graphics, and snapshots.

2.  **Virtio-GPU Stream Renderer**
    (`host/virtio_gpu_gfxstream_renderer.cpp`) — Implements the
    `virtio-gpu-gfxstream-renderer.h` C ABI consumed by hypervisors (QEMU,
    crosvm) and virtual device tooling (Cuttlefish, Goldfish). This is the
    primary integration point for VM-based use cases.

#### Host Internal Modules

| Module | Location | Responsibility |
|--------|----------|----------------|
| **VirtioGpuFrontend** | `host/virtio_gpu_frontend.*` | Handles virtio-gpu commands: resource creation, context management, blob operations, fence signaling, timeline tracking, scanout display, and snapshot/restore |
| **FrameBuffer** | `host/frame_buffer.*` | Global singleton managing emulated display state, color buffers, data buffers, display surfaces, multi-display support, and coordination between GL and Vulkan paths |
| **RendererImpl** | `host/renderer_impl.*` | Manages render channels, process cleanup, post callbacks, and window configuration |
| **RenderThread** | `host/render_thread.*` | Per-channel worker thread that reads the byte stream and drives GLES/Vulkan decoders; supports both RenderChannel and ring buffer transport modes |
| **GLES Decoders** | `host/gl/gles1_dec/`, `host/gl/gles2_dec/` | Generated code that decodes GLES 1.x/2.x wire protocol opcodes and dispatches to the host GLES implementation |
| **GLES Translator** | `host/gl/glestranslator/` | Translates GLES calls to desktop OpenGL; includes EGL, GLES 1.x, and GLES 2.x sub-translators |
| **OpenGLESDispatch** | `host/gl/OpenGLESDispatch/` | Dynamic loader for host-side EGL/GLES/GL functions via `dlopen`/`dlsym` |
| **Vulkan Server** | `host/vulkan/` | Vulkan decoder (`vk_decoder.cpp`), dispatch, swap chain, compositor, and emulated texture format support |
| **Vulkan Cereal** | `host/vulkan/cereal/` | Generated Vulkan marshaling/unmarshaling code for the wire protocol |
| **RenderControl Decoder** | `host/renderControl_dec/` | Decodes RenderControl protocol commands (display queries, color buffer management, sync) |
| **Address Space** | `host/address_space/` | Address-space device and graphics integration for shared-memory transport |
| **Snapshot** | `host/snapshot/` | Host-side state serialization for VM snapshot/restore |
| **Tracing** | `host/tracing/` | Optional Perfetto tracing integration |
| **Features** | `host/features/` | Feature flag management for enabling/disabling capabilities |
| **Compressed Textures** | `host/compressed_textures/` | ASTC/ETC2 texture decompression on the host |

### Guest (Client)

The guest runs inside a VM (or another process) and provides standard graphics
API libraries that encode calls and send them to the host.

| Module | Location | Responsibility |
|--------|----------|----------------|
| **GLES 1.x Encoder** | `guest/GLESv1_enc/` | Generated encoder that serializes GLES 1.x calls into wire protocol bytes |
| **GLES 2.x Encoder** | `guest/GLESv2_enc/` | Generated encoder that serializes GLES 2.x calls into wire protocol bytes |
| **GLES Libraries** | `guest/GLESv1/`, `guest/GLESv2/` | Guest-side GLES shared libraries providing standard API entry points |
| **EGL** | `guest/egl/` | Guest EGL implementation that routes through encoders |
| **RenderControl Encoder** | `guest/renderControl_enc/` | Encodes display/buffer management commands for the host |
| **RenderControl** | `guest/rendercontrol/` | Guest RenderControl client interface |
| **OpenglCodecCommon** | `guest/OpenglCodecCommon/` | Shared codec utilities: checksums, texture helpers, client state tracking |
| **OpenglSystemCommon** | `guest/OpenglSystemCommon/` | `HostConnection` class that selects and manages the transport layer |
| **QEMU Pipe** | `guest/qemupipe/` | QEMU pipe transport backend |
| **Android-emu Base** | `guest/android-emu/` | Android emulator base utilities: threads, ring buffers, synchronization |
| **Gralloc** | `guest/gralloc_cb/` | Graphics allocator callback stubs for Android |

#### HostConnection and Transport Selection

`HostConnection` (in `guest/OpenglSystemCommon/`) is the central guest-side
class responsible for establishing and managing the connection to the host. It
selects the transport mechanism based on Android system properties or the
`GFXSTREAM_TRANSPORT` environment variable:

-   **QEMU Pipe** — Traditional Goldfish emulator pipe
-   **Virtio-GPU Address Space** — Shared memory ring buffer (io_uring style)
    for virtio-gpu contexts
-   **Network Sockets** — For remote rendering scenarios

### Common

The `common/` directory contains code shared between host and guest:

| Module | Location | Responsibility |
|--------|----------|----------------|
| **Base Library** | `common/base/` | Cross-platform primitives: threads, locks, file utilities, string formatting, shared memory, system detection (Linux/macOS/Windows/QNX) |
| **Logging** | `common/logging/` | Unified logging with configurable levels |
| **Utils** | `common/utils/` | General-purpose utilities |
| **Image** | `common/image/` | Image format conversion helpers |
| **Detector** | `common/detector/` | Runtime GPU capability detection (GLES/Vulkan/EGL support probing) |
| **Test Environment** | `common/testenv/` | Bazel test environment for selecting graphics drivers at test time |

### Code Generation

The `codegen/` directory contains the **emugen** (also called **apigen**) tool
and API specification files that drive automatic code generation for
encoder/decoder pairs.

#### How Emugen Works

Emugen reads three input files per API:

1.  **`basename.in`** — Function signatures (return type, name, parameters)
2.  **`basename.types`** — Type definitions (size, printf format, is-pointer)
3.  **`basename.attrib`** — Additional attributes (pointer directions, data
    lengths, custom packing, flags)

From these, it generates:

-   **Encoder side:** opcode definitions, entry points, client context/dispatch
    table, encoder implementation that serializes calls into the wire format
-   **Decoder side:** opcode definitions, server context/dispatch table, decoder
    implementation that deserializes and invokes server-side functions
-   **Wrapper side:** optional dispatch-table-based wrapper libraries

API specifications exist for:
-   `codegen/gles1/` — OpenGL ES 1.x
-   `codegen/gles2/` — OpenGL ES 2.x
-   `codegen/renderControl/` — Custom display/buffer management protocol

Vulkan code generation uses a separate pipeline
(`scripts/generate-gfxstream-vulkan.sh`) that produces the marshaling code in
`host/vulkan/cereal/`.

### Third-Party Dependencies

Managed via `third_party/` (vendored headers) and `MODULE.bazel` (external
fetches):

| Dependency | Purpose |
|-----------|---------|
| **ANGLE** | GLES-on-Vulkan for test/reference rendering |
| **SwiftShader** | Software Vulkan implementation for CI and headless testing |
| **SPIRV-Headers/Tools** | Vulkan shader tooling |
| **Mesa** | Vulkan dispatch and object management (guest-side integration) |
| **Abseil** | C++ utility library |
| **Protobuf** | Serialization for snapshots and telemetry |
| **GoogleTest** | Testing framework |
| **GLM** | OpenGL mathematics |
| **STB** | Image loading utilities |
| **DRM** | Direct Rendering Manager headers |
| **ASTC-Encoder** | Adaptive Scalable Texture Compression codec |
| **RenderDoc** | Graphics debugger integration |
| **Rutabaga** | Cross-VMM GPU resource management |
| **zlib** | Compression |

---

## Data Flow

### Guest-to-Host Rendering Pipeline

The end-to-end flow of a single graphics API call:

```
1. Application calls GLES/Vulkan
         │
         ▼
2. Guest entry point (generated: gl2_entry.cpp / gl_entry.cpp)
         │
         ▼
3. Encoder serializes call into wire format
   ┌─────────────────────────────────────┐
   │  opcode (4B) │ packet_len (4B) │ …  │
   │  parameter1  │ parameter2  │ …      │
   └─────────────────────────────────────┘
         │
         ▼
4. HostConnection selects transport and writes bytes
         │
         ▼
5. Transport carries bytes to host
   (virtio-gpu ring / QEMU pipe / address-space / socket)
         │
         ▼
6. RenderThread reads stream on host
         │
         ▼
7. Decoder deserializes opcodes and invokes host-side functions
         │
         ├───► GLES path: glestranslator → native OpenGL
         │
         └───► Vulkan path: vk_decoder → native Vulkan
                  │
                  ▼
8. FrameBuffer coordinates display, composition, sync
         │
         ▼
9. Results (return values, out-pointers) sent back via transport
```

### Wire Protocol

The wire protocol is opcode-based with minimal overhead:

**Encoder → Decoder packet:**
```
┌──────────┬────────────┬──────────┬──────────┬───┐
│  opcode  │ packet_len │  param1  │  param2  │ … │
│  (4B)    │  (4B)      │  (var)   │  (var)   │   │
└──────────┴────────────┴──────────┴──────────┴───┘
```

**Decoder → Encoder reply** (return values and out-pointers only, no headers):
```
┌────────────────┬──────────┐
│  out-ptr data  │  retval  │
│  (var)         │  (var)   │
└────────────────┴──────────┘
```

Pointer data includes direction annotations (`in`, `out`, `in_out`). The
endianness is determined by the client side; the server must detect and handle
byte order.

### Transport Mechanisms

| Transport | Use Case | Mechanism |
|-----------|----------|-----------|
| **Virtio-GPU + Address Space Graphics** | Modern VM integration (Cuttlefish, crosvm) | Shared memory ring buffer (io_uring style) with virtio-gpu capsets for context and resource management |
| **QEMU Pipe (Goldfish)** | Legacy Android emulator | Named pipe between guest kernel driver and host |
| **RenderChannel** | In-process / direct channel | Direct memory channel managed by `RendererImpl` |
| **Network Sockets** | Remote rendering | TCP/UDP for cross-machine streaming |

---

## Key Subsystems

### FrameBuffer

`FrameBuffer` (`host/frame_buffer.h`) is the **global singleton** managing all
host-side emulated GPU state. Despite its name, it functions more as a
**Display** or **GPU State Manager**.

**Responsibilities:**
-   Manages **ColorBuffers** (GPU textures representing guest framebuffers) with
    reference counting
-   Manages **Buffers** (Vulkan-backed data buffers)
-   Coordinates between **GLES** and **Vulkan** rendering paths
-   Handles **multi-display** support (up to 11 displays)
-   Manages **sub-window** display for embedding in emulator UI
-   Coordinates **EGL contexts**, **window surfaces**, and **EGL images** (when
    GLES is enabled)
-   Supports **Vsync** timing
-   Handles **process resource cleanup** when guest processes terminate
-   Provides **screenshot** capture
-   Supports **snapshot save/restore** (serialization of GPU state)

**Lifecycle:** Must be initialized via `FrameBuffer::initialize()` before use.
Retrieved globally via `FrameBuffer::getFB()`.

### VirtioGpuFrontend

`VirtioGpuFrontend` (`host/virtio_gpu_frontend.h`) handles the virtio-gpu
protocol layer, translating virtio-gpu commands into Gfxstream operations.

**Responsibilities:**
-   **Context management** — Creating and destroying GPU contexts for guest
    processes
-   **Resource management** — Creating, importing, and destroying GPU resources
    (textures, buffers, blobs)
-   **Command submission** — Processing guest command streams via `submitCmd()`
-   **Fence and timeline management** — Tracking GPU work completion via
    `VirtioGpuTimelines`
-   **Blob operations** — Managing host-mappable memory blobs for zero-copy data
    sharing
-   **Display integration** — Scanout binding, native surface management,
    resource flushing
-   **Snapshot/Restore** — Serializing frontend state for VM migration

**Singleton access:** The static `sFrontend()` function in
`virtio_gpu_gfxstream_renderer.cpp` provides the global instance.

### RenderThread

`RenderThread` (`host/render_thread.h`) is a **per-channel worker thread** that
processes the encoded byte stream from a single guest client.

**Responsibilities:**
-   Reads encoded commands from either a `RenderChannel` or a `RingStream`
    (address-space graphics)
-   Drives the appropriate decoder (GLES 1.x, GLES 2.x, or Vulkan)
-   Maintains per-thread GL/Vulkan context via `RenderThreadInfo`
-   Supports **snapshot pause/resume** for live migration
-   Manages its own lifecycle with controlled exit signaling (to work around
    driver bugs with concurrent destroy operations)

**Threading model:** 1:1 — each guest encoder thread gets a dedicated host
decoder thread.

### GLES Translation

The GLES path (`host/gl/`) translates guest GLES calls to host desktop OpenGL:

```
Guest GLES call
    │
    ▼
GLES Decoder (gles1_dec / gles2_dec)
    │  (generated: opcode → function dispatch)
    ▼
GLES Translator (glestranslator/)
    │  ├── EGL Translator
    │  ├── GLES 1.x Translator (CM)
    │  └── GLES 2.x Translator (v2)
    │
    ▼
OpenGLESDispatch
    │  (dynamic loading via dlopen/dlsym)
    ▼
Host Desktop OpenGL / ANGLE / SwiftShader
```

The **GLES Translator** maintains full GLES state tracking and translates GLES
semantics that don't have direct desktop GL equivalents.
**OpenGLESDispatch** dynamically loads the host's EGL and GL/GLES libraries at
runtime.

### Vulkan Streaming

The Vulkan path (`host/vulkan/`) streams Vulkan API calls:

```
Guest Vulkan call (via Mesa + gfxstream guest driver)
    │
    ▼
Ring Buffer Transport (io_uring style)
    │
    ▼
Vulkan Decoder (vk_decoder.cpp)
    │  (generated cereal code for marshaling)
    ▼
Vulkan Dispatch (host Vulkan loader)
    │
    ▼
Host Vulkan Driver (GPU vendor / SwiftShader)
```

**Key design aspects** (from `docs/design.md`):
-   **1:1 threading model** — each guest encoder thread gets a host decoder
    thread
-   Supports **virtio-gpu, Goldfish, and testing transports**
-   Supports **Android, Fuchsia, and Linux** guests
-   Uses **ring buffer** (io_uring style) for command streaming
-   **Mesa integration** for Vulkan dispatch and object management on the guest
-   Dual object model: Mesa objects (for dispatch) contain keys to gfxstream
    internal objects; gfxstream objects are being phased out in favor of
    Mesa-only objects

### Snapshots

Vulkan snapshot support (`docs/snapshot.md`, `host/snapshot/`) enables saving
and restoring GPU state for VM hibernation.

**Mechanism:**
-   A `DependencyGraph` tracks Vulkan API calls and resulting objects
-   **ApiNodes** represent specific API call invocations
-   **DepNodes** represent Vulkan objects or command dependency markers
-   On **save**: nodes are serialized in chronological order
-   On **load**: saved API calls are replayed to reconstruct device state
-   `vkDestroy*()` calls prune nodes and descendants from the graph
-   Command buffer state is tracked through begin/end/reset cycles

### Tracing

Optional **Perfetto** tracing support (`host/tracing/`, `docs/tracing.md`)
provides host-side performance instrumentation. Enabled by default on Android
builds via `GFXSTREAM_BUILD_WITH_TRACING`. See `docs/tracing.md` for capture
instructions.

---

## Design Patterns

| Pattern | Usage |
|---------|-------|
| **Singleton** | `FrameBuffer::getFB()` for global GPU state; static `sFrontend()` for `VirtioGpuFrontend` |
| **Factory** | `gfxstream::initLibrary()` creates `RenderLibImpl`; `Renderer::createRenderChannel()` creates channels |
| **Thread-per-Channel** | `RenderThread` — one worker thread per guest client stream |
| **Strategy / Pluggable Transport** | `HostConnection` selects transport (pipe, ring, socket) based on configuration |
| **Code Generation** | emugen/apigen produces encoder/decoder boilerplate from API specs; Vulkan cereal for Vulkan marshaling |
| **Observer / Callback** | Post callbacks, fence completion callbacks, process cleanup callbacks |
| **PIMPL** | `FrameBuffer` uses `Impl` pattern to hide implementation details |
| **Dispatch Table** | Both encoders and decoders use function-pointer dispatch tables that can be overridden |
| **Reference Counting** | `ColorBuffer` instances use open/close reference counting for lifecycle management |
| **Dependency Graph** | Vulkan snapshot system tracks object dependencies for ordered save/restore |

---

## Build Systems

Gfxstream supports four build systems for different platforms and use cases:

| Build System | Primary Use Case | Configuration |
|-------------|------------------|---------------|
| **Bazel** | Standalone host builds, CI, testing | `MODULE.bazel`, `BUILD.bazel`, `.bazelrc` |
| **CMake** | Goldfish host backend, standalone builds | `CMakeLists.txt` (root and per-directory) |
| **Meson** | Linux guest-on-Linux host | `meson.build`, `meson_options.txt` |
| **Android Soong** | Guest components for Android virtual devices | `Android.bp` (throughout tree) |

Most directories contain build files for all four systems.

**Languages:** C++17 (primary), C11, Objective-C/C++ (macOS), Python (scripts),
Protocol Buffers (snapshots/telemetry), GLSL (shaders), Rust (transitively via
Rutabaga).

---

## Building

### Linux

#### Bazel

The Bazel build currently supports building the host server which is typically
used for Android virtual device host tooling.

```
cd <gfxstream project>

bazel build ...

bazel test ...
```

#### CMake

The CMake build has historically been used for building the host backend for
Goldfish.

The CMake build can be used from either a standalone Gfxstream checkout or from
inside an Android repo.

Then,

```
mkdir build

cd build

cmake .. -G Ninja

ninja
```

For validating a Goldfish build,

```
cd <aosp/emu-main-dev repo>

cd external/qemu

python android/build/python/cmake.py --gfxstream
```

#### Meson

The Meson build has historically been used for building the backend for Linux
guest on Linux host use cases.

```
cd <gfxstream project>

meson setup \
    -Ddefault_library=static \
    -Dgfxstream-build=host \
    build

meson compile -C build
```

#### Soong

The Android Soong build is used for building the guest components for virtual
device (Cuttlefish, Goldfish, etc) images and was previously used for building
the host backend for virtual device host tools.

Please follow the instructions
[here](https://source.android.com/docs/setup/start) for getting started with
Android development and setting up a repo.

Then,

```
m libgfxstream_backend
```

and `libgfxstream_backend.so` can be found in `out/host`.

For validating changes, consider running

```
cd hardware/google/gfxstream

mma
```

to build everything inside of the Gfxstream directory.

### Windows

Make sure the latest CMake is installed. Make sure Visual Studio 2019 is
installed on your system along with all the Clang C++ toolchain components.
Then:

```
mkdir build

cd build

cmake . ../ -A x64 -T ClangCL
```

A solution file should be generated. Then open the solution file in Visual
studio and build the `gfxstream_backend` target.

## Codegen

### Regenerating GLES/RenderControl code

Run:

```
scripts/generate-apigen-sources.sh
```

### Regenerating Vulkan code

To re-generate both guest and Vulkan code, please run:

```
scripts/generate-gfxstream-vulkan.sh
```

## Testing

### Linux

#### Bazel

```
bazel test ...
```

or

```
bazel test tests/end2end:gfxstream_end2end_tests
```

#### Soong

From within an Android repo, run:

```
atest --host GfxstreamEnd2EndTests
```

### Windows

There are a bunch of test executables generated. They require `libEGL.dll` and
`libGLESv2.dll` and `vulkan-1.dll` to be available, possibly from your GPU
vendor or ANGLE, in the `%PATH%`.

---

## CI/CD

GitHub Actions workflows under `.github/workflows/`:

| Workflow | Trigger | Actions |
|----------|---------|---------|
| **`presubmit.yaml`** | Pull requests and pushes (non-`main`) | Runs Bazel build+test (with ASan variant), CMake+Ninja build, Meson build (Linux + Windows/MSYS2), ARM Bazel build (build-only) |
| **`presubmit_bazel.yml`** | Called by `presubmit.yaml` | Installs Bazel and toolchains, restores disk cache, runs `bazel build ...` with ANGLE+SwiftShader graphics drivers, then runs selected test targets |
| **`postsubmit.yaml`** | Push to `main` + scheduled (every 3 hours) | Populates Bazel disk cache for x86_64 and ARM |

**Tested targets in CI:**
-   `host:gfxstream_framebuffer_tests`
-   `host/vulkan:gfxstream_compositorvk_tests`
-   `host/vulkan:gfxstream_emulatedphysicalmemory_tests`
-   `tests/end2end:gfxstream_end2end_tests`

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines including CLA
requirements, community guidelines, and the code review process via GitHub pull
requests.

## Notice

This is not an officially supported Google product. This project is not eligible
for the
[Google Open Source Software Vulnerability Rewards Program](https://bughunters.google.com/open-source-security).
