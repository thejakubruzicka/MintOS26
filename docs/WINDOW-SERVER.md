# Window server design: GUI apps as separate processes (next step after 26.5.014.0)

## Today

MintDE is one ring-3 process. It draws every window into the kernel's shared
display canvas and presents dirty rectangles itself, so all built-in apps
(Files, Notes, Terminal, Web…) live inside that process.

26.5.014.0 added the pieces a multi-process GUI needs:

- separate processes started from MintOS HD;
- pipes, wait and kill;
- per-task FPU/SSE state and timer preemption;
- demand-zero memory.

A crash in a program no longer affects the desktop. A GUI program, however,
still has no way to show a window.

## Goal

A program started from MintOS HD can:

- open one or more windows that MintDE composites like its own;
- receive input events for them;
- crash without taking MintDE down.

MintDE stays the window server; no separate server process is added.

## Design

### 1. Shared window surfaces

- **Allocation.** A new kernel object, the *surface*, is a physically contiguous run of pages. It is allocated by the kernel from the frame pool and is not owned by any one address space.
- **Mapping.** It is mapped into both the client and MintDE with the existing borrowed-mapping mechanism (`map_shared`, `MINT_ASPACE_REGION_SHARED`). The client maps it read/write and MintDE maps it read-only.
- **Lifetime.** Surfaces are reference counted. They are freed when the last mapping goes away, so a crashing client cannot free pages MintDE is still reading.
- **Syscalls** (reserved app-platform range):
  - `SURFACE_CREATE(width, height, format)` returns a handle and a user address;
  - `SURFACE_SHARE(handle, pid)` maps it into the window server;
  - `SURFACE_DESTROY(handle)`.

### 2. Message channel

- **Transport.** Each GUI client gets a pair of message pipes to MintDE. These reuse the 26.5.014.0 pipe objects with a message-framing flag, so a read returns whole messages.
- **Client to server:**
  - `WINDOW_CREATE{surface, title, w, h, flags}`
  - `WINDOW_DAMAGE{x, y, w, h}`
  - `WINDOW_SET_TITLE`
  - `WINDOW_CLOSE`
  - `CURSOR_SHAPE`
- **Server to client:**
  - `CONFIGURE{w, h, scale, theme}` (on resize the client must reallocate its surface)
  - `KEY`, `POINTER`, `SCROLL`, `FOCUS`
  - `CLOSE_REQUEST`
  - `FRAME_DONE` (paces redraws)
- **Encoding.** Messages are fixed-size little-endian records with a version field. They are validated like syscall arguments: bounded sizes, and handles checked against the sender.

### 3. MintDE side

- **Proxy windows.** A client window becomes a normal MintDE window whose paint function copies the client's damaged region from the read-only surface mapping into the canvas. This is the same path the Web app uses to blit its page bitmap.
- **Decorations.** Title bar, traffic-light buttons, shadows and vibrancy stay in MintDE. Clients draw only their content area.
- **Focus.** Input for a focused client window is translated to window-local coordinates and written to its event pipe.
- **Disconnects.** If the pipe closes or the process exits (seen through `wait` with `MINT_WAIT_NOHANG`), MintDE removes the window and shows "*App* quit unexpectedly" without restarting itself.

### 4. Client library ("MintKit" seed)

- A small static library linked into SDK programs, next to musl. Because SDK programs run on the Linux-compat entry, it reaches the MintOS services through `int 0x80` wrappers, as `sysinfo` already does.
- It provides `mk_window_open`, `mk_window_present`, `mk_next_event` and a software 2D painter (rects, text with the Inter font atlas, images through `stb_image`).

## Safety and robustness

- **Untrusted clients.** MintDE treats every client as untrusted:
  - message sizes and counts are bounded;
  - damage rectangles are clipped to the surface;
  - a client that stops answering `FRAME_DONE` is throttled, not waited for.
- **Memory limits.** Surfaces count against the client's owned-page limit. The server never blocks on a client pipe and always uses non-blocking reads, as Terminal does today.
- **Kernel stays non-preemptive.** Only ring-3 code is preempted, so the window server needs no kernel locking changes.

## Steps

1. Surface object with reference counting, and `SURFACE_*` syscalls, with host tests for its lifetime rules.
2. Message-framed pipes, and a host test for the codec.
3. MintDE proxy window and input routing, with QEMU evidence: a client window next to Finder.
4. MintKit library plus a demo app (clock or Lua with a canvas), and crash-isolation evidence: kill the client, the desktop survives.
5. Later:
   - shared-memory audio (personality topic);
   - clipboard and drag and drop over the same channel;
   - an SSE/AVX2 compositor, now possible because FPU state is switched.
