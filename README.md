# ps5debug-NG

A debugger payload for jailbroken PlayStation 5 consoles. Ships a userland
command server that runs inside SceShellCore, letting remote clients inspect
and manipulate running processes, the kernel itself, and the system UI over a
simple TCP protocol.

ps5debug-NG is inspired by Ctn's `ps5debug 1.0b5` and is wire-compatible with
it - existing clients should work without modification. It is licensed under
GPL-3.

Discord Server: [Team Reaper](https://discord.gg/7bjtgZf4PY)

---

## Supported firmwares

The kernel kpatch routine in [installer/source/main.c](installer/source/main.c)
recognises the following firmware families. Booting on an unsupported FW prints
`port_outer: kpatch SKIP - unsupported FW magic 0x...` to the kernel log and
aborts cleanly.

| Family             | Label in code              |
|--------------------|----------------------------|
| 3.xx               | `FW 3.x`                   |
| 4.xx               | `FW 4.x`                   |
| 5.xx               | `FW 5.x`                   |
| 6.xx               | `FW 6.x`                   |
| 7.xx               | `FW 7.x`                   |
| 7.5x               | `FW 7.5x`                  |

Each family covers several point releases - see the switch in
[installer/source/main.c](installer/source/main.c) for the exact set of FW
magic values recognised per family. Clients can read the running FW with
`CMD_FW_VERSION` (returns the kernel's raw FW magic word).

---

## Primary Features

### Process inspection and manipulation
- **Enumerate processes** (`p_comm` + pid list).
- **Read and write target memory** in streamed chunks.
- **List virtual memory maps** - ranges, protections, backing names.
- **Query process metadata** - name, path, titleId, contentId.
- **Identify the foreground app** (`0xBDDD0006`) - returns pid + titleid +
  contentid + process name + the game's version, parsed server-side from the
  title's `param.sfo`. Useful for clients that need to know what's currently
  running without listing every process.
- **Server-side stack walk** (`CMD_PROC_READ_STACK`) - the server walks the
  RBP chain itself (up to 64 frames) and bundles each frame's saved-RBP,
  return address, frame-local bytes, and a 200-byte code window around the
  return address into one response. Clients avoid paying many TCP round-trips
  per stack frame.
- **Change memory protection** on arbitrary target regions.
- **Allocate / free / hint-allocate** memory inside any target process.

### In-target code execution
- **Install an RPC stub** (`CMD_PROC_INSTALL`) - injects a reusable trampoline
  with its own thread into the target.
- **Call arbitrary functions** with up to six SysV ABI register arguments and
  read back `rax` (`CMD_PROC_CALL`).
- **Load ELFs** into a target process - either jump to the entry point
  immediately (`CMD_PROC_ELF`) or return the entry for later invocation
  (`CMD_PROC_ELF_RPC`).

### Full userland debugger
- **Attach** to a single target with `CMD_DEBUG_ATTACH` (sets up an async
  interrupt channel back to the client).
- **Software breakpoints** - up to **30** slots, transparent `0xCC` injection.
- **Hardware watchpoints** - up to **4** DR0-DR3 slots with read / write /
  read-write and 1/2/4/8-byte granularity.
- **Thread control** - list, suspend, resume, single-step, per-thread step.
- **Full register access** - general-purpose, FPU + YMM, and debug registers.
- **Continue / stop / halt** the whole process from one command.
- **Asynchronous interrupt packets** delivered on a separate TCP connection so
  the client never polls.

### Kernel access
- Get the **kernel base address**.
- **Read** arbitrary kernel memory.
- **Write** arbitrary kernel memory.

### Built-in Zydis disassembler
Large memory regions never leave the PS5. Three server-side decoder commands
keep bandwidth low:
- `CMD_PROC_DISASM_REGION` - packed 32-byte-per-instruction stream with
  control-flow, memory-operand, and RIP-relative metadata.
- `CMD_PROC_EXTRACT_CODE_XREFS` - all resolved RIP-relative branch/call
  targets in a region, deduplicated.
- `CMD_PROC_FIND_XREFS_TO` - only instructions that reference a specific
  target address.

### Built-in Keystone assembler (x86-64)
A cross-compiled LLVM-MC Keystone (x86-only, no exceptions / no RTTI, static
~4 MB) is embedded in the payload, exposed via the raw-literal opcode
`0xBDAA0024`. Lets clients assemble asm text into machine code on the console
itself.
- Pure userspace - needs no attached process and no `CMD_PROC_AUTH` handshake.
- Request: `u64 base_addr; u32 ks_opt_syntax;` + asm text (NUL not required).
  `ks_opt_syntax` defaults to Intel; pass 1/2/4/8/0x10 for Intel/ATT/NASM/MASM/GAS.
- Response: `CMD_SUCCESS` + `u32 byte_len; u32 insn_count;` + machine bytes,
  or `CMD_ERROR` + `u32 ks_errno; u32 msg_len;` + Keystone's human-readable error.
- The opcode is deliberately a raw literal (no `CMD_*` macro) so the published
  `CMD_*` set that some clients enumerate stays unchanged.

### Memory scanning
- **Value scan** (`CMD_PROC_SCAN`) - single-pass, 12 value types × 13 compare
  modes (exact, fuzzy, bigger/smaller, between, increased, decreased, changed,
  etc.).
- **Iterative scan session** (`SCAN_START` → `SCAN_COUNT` → `SCAN_GET`) - lets
  clients narrow a result set server-side over many passes.
- **AOB scan** (`CMD_PROC_SCAN_AOB`) - byte patterns with `??` wildcards.
- **Multi-pattern AOB scan** (`CMD_PROC_SCAN_AOB_MULTI`) - many patterns in
  one pass.
- **Auth-gated** - scan commands require a prior `CMD_PROC_AUTH` handshake.

### System UI integration
- **Push notifications** to the user's screen with arbitrary UTF-8 text.
- **Print** to the kernel console.
- **Reboot** the console.

### Klog forwarder
- TCP **3232** streams the kernel log to a connected client (host-side
  `klog reader` style). Survives suspend/resume the same as the main server.

### Discovery
- A UDP broadcast responder on port `1010` echoes a handshake magic
  (`0xFFFFAAAA`) so clients can find the PS5 on the LAN without hard-coding
  an IP.

### Rest-mode support
- The payload **survives suspend / resume** without needing to be reloaded.
  A supervisory loop polls the network periodically: when the console drops
  into rest mode the server exits cleanly, and as soon as the network comes
  back the server restarts and a fresh "online" notification fires.
- Clients see a clean disconnect on port 744 when rest mode begins and can
  simply reconnect after wake.

### Performance-oriented design
- Non-blocking sockets with `TCP_NODELAY`, `SO_KEEPALIVE`, large transfer
  chunks.
- Zydis amalgamation compiled at `-O3 -DNDEBUG` for maximum decode throughput.
- Link-time dead stripping (`-ffunction-sections -fdata-sections
  -Wl,--gc-sections`).
- Interrupt packets streamed over a dedicated side channel to avoid blocking
  the command loop.

---

## Architecture

The deployable artifact `ps5debug-NG.elf` is a two-component build:

```
┌──────────────────────────────────────────────────────────────┐
│                       ps5debug-NG.elf                        │
│                                                              │
│   ┌───────────────────┐    injects   ┌───────────────────┐   │
│   │   installer ELF   │─────────────▶│   debugger ELF    │   │
│   │ (umtx-loaded PIE) │              │ (in SceShellCore) │   │
│   └───────────────────┘              └────────┬──────────┘   │
│                                               │              │
│                              ┌────────────────▼─────────┐    │
│                              │  - TCP server   :744     │    │
│                              │  - debug async  :755     │    │
│                              │  - klog forward :3232    │    │
│                              │  - UDP bcast    :1010    │    │
│                              └──────────────────────────┘    │
└──────────────────────────────────────────────────────────────┘
```

- **installer** - umtx-loaded SDK PIE. Finds SceShellCore, sets up the
  target's KEX state and syscall-origin filter, then calls the SCE-side
  `inject_remote_thread_create` primitive to run the embedded debugger as
  a SceShellCore-internal thread.
- **debugger** - runs inside SceShellCore once injected. Implements the
  wire protocol, breakpoints / watchpoints / single-step, memory scan, RPC,
  and ELF inject. Built into `debugger/build/debugger.elf`, then embedded as
  a `.rodata` blob into the installer via `.incbin`.

Running the wire protocol from inside SceShellCore is what lets `PT_ATTACH`
on game pids look kernel-side like an SCE-originated debug attach, which
PS5's AppContext gating allows. A standalone process doing `PT_ATTACH` gets
the game flagged and stops progressing.

---

## Network protocol at a glance

| Port  | Proto | Direction      | Purpose                              |
|-------|-------|----------------|--------------------------------------|
| 744   | TCP   | client → PS5   | Command server                       |
| 755   | TCP   | PS5 → client   | Async debug interrupts               |
| 3232  | TCP   | PS5 → client   | Kernel log forwarder                 |
| 1010  | UDP   | bidirectional  | Discovery beacon (`0xFFFFAAAA`)      |

Every command begins with a 12-byte header:

```c
struct cmd_packet {
    uint32_t magic;      // 0xFFAABBCC
    uint32_t cmd;        // 0xBDAA..., 0xBDBB..., 0xBDCC..., 0xBDDD...
    uint32_t datalen;    // length of request body that follows
};
```

Followed by the command's fixed request struct (if any), any trailing
variable-length payload, and a `uint32_t` status code reply.

**Note on status words.** The status `uint32_t` on PS5 is transmitted with
its bit pairs swapped (`net_send_int32` swaps even/odd-bit positions). Clients
must un-bitswap incoming status values before comparing to `CMD_SUCCESS` /
`CMD_ERROR` / `CMD_DATA_NULL` / `CMD_ALREADY_DEBUG`. Subsequent payload bytes
are sent raw.

**Full protocol specification:** [PROTOCOL.md](PROTOCOL.md) - every command,
every packet struct, every enum, every status code, with `file:line`
citations.

---

## Command coverage

| Namespace     | Count | Examples                                                   |
|---------------|-------|------------------------------------------------------------|
| Info / ping   | 5     | `VERSION`, `FW_VERSION`, `BRANDING`, `PLATFORM_ID`, `NOP`  |
| Process       | 26    | `READ`, `WRITE`, `MAPS`, `CALL`, `SCAN_*`, `DISASM_*`      |
| Debug         | 18    | `ATTACH`, `SET_BREAKPOINT`, `GETREGS`, `STEP`, `CONTINUE`  |
| Kernel R/W    | 3     | `KERN_BASE`, `KERN_READ`, `KERN_WRITE`                     |
| Console       | 6     | `NOTIFY`, `PRINT`, `REBOOT`, `INFO`, `END`, `FOREGROUND_APP` |
| **Total**     | **58**|                                                            |

---

## Building

Prerequisites (Ubuntu / Debian):

```sh
sudo apt install bash clang-18 lld-18
```

Build:

```sh
./build.sh
```

This builds the SDK first (one-time, cached), then the debugger, then the
installer (which embeds the debugger), then publishes `ps5debug-NG.elf` at
the top level. Subsequent runs only rebuild what changed.

Clean (including the SDK install):

```sh
./build.sh clean
```

---

## Deploying

`ps5debug-NG.elf` is loaded onto the PS5 via a umtx-based ELF loader (e.g.
elfldr from etaHEN-class loaders).

You should see a system notification confirming the payload is alive:

```
ps5debug-NG by OSR v1.2.3 loaded!
Coded by OpenSourcereR
Special thanks to
golden, Ctn & SiSTRo! ♥
```

---

## Writing your own client

The protocol is deliberately simple - a raw TCP client in any language can
drive it. Example: pinging the server and reading its branding string, in
Python (don't forget the bit-pair swap on the status word):

```python
import socket, struct

PACKET_MAGIC = 0xFFAABBCC
CMD_BRANDING = 0xBD000501
CMD_SUCCESS  = 0x40000000

def bitswap32(x):
    x &= 0xFFFFFFFF
    return ((x << 1) & 0xAAAAAAAA) | ((x >> 1) & 0x55555555)

s = socket.create_connection(("<PS5_IP>", 744))
s.sendall(struct.pack("<III", PACKET_MAGIC, CMD_BRANDING, 0))
(status_raw,) = struct.unpack("<I", s.recv(4))
assert bitswap32(status_raw) == CMD_SUCCESS
(length,) = struct.unpack("<I", s.recv(4))
print("server branding:", s.recv(length).decode())
```

See [PROTOCOL.md](PROTOCOL.md) for the exact byte layout of every command,
response, and async interrupt packet.

---

## Source layout

```
.
├── build.sh                 # one-command full build
│
├── common/                  # headers + sources shared by both components
│   ├── include/             # protocol.h, sdk_shim.h, net.h, proc.h, ...
│   └── source/
│
├── debugger/                # in-SceShellCore wire-protocol debugger
│   ├── Makefile  source/  include/
│   └── third_party/         # Zydis (decoder) + Keystone (assembler)
│
├── installer/               # umtx-loaded SceShellCore installer
│   ├── Makefile  source/
│   └── source/embedded_inner.S   # embeds debugger.elf via .incbin
│
├── ps5-payload-sdk/         # vendored John Törnblom SDK
└── third_party/             # keystone-0.9.2 full source (for rebuilds)
```

Three source files (`kern_rw_fast.c`, `proc_elf.c`, `proc_remote.c`) and
`main.c` exist in **both** `debugger/source/` and `installer/source/` because
they genuinely diverge between the two builds - same code specialized for
each component's role.

---

## SDK pin

The vendored SDK is **ps5-payload-sdk v0.38** (commit
`6ae1470fd50c5791e8a8bb728627e657e36eb55a`, dated 2026-04-02). Upstream:
https://github.com/ps5-payload-dev/sdk

To upgrade the SDK:

```sh
./build.sh clean
rm -rf ps5-payload-sdk
curl -fsSL https://github.com/ps5-payload-dev/sdk/archive/refs/tags/<TAG>.tar.gz \
  | tar xz -C /tmp
mv /tmp/sdk-<TAG_WITHOUT_v> ps5-payload-sdk
./build.sh
```

---

## Credits

- **jogolden** - original public `ps4debug` and the wire protocol this project
  indirectly inherits.
- **Ctn & SiSTRo** - `ps5debug` authors; this project is wire-compatible with
  their implementation.
- **DeathRGH** - Frame4 author. Inspiration.
- **John Törnblom** - `ps5-payload-sdk`, the vendored SDK / toolchain.
- **Zydis** - x86 disassembler used in decoder-only mode (`ZYAN_NO_LIBC`,
  `-DNDEBUG`). Third-party, unmodified; MIT-licensed.
- **Keystone** - LLVM-MC-based assembler; cross-compiled here for the PS5
  payload (x86-only, `-fno-exceptions -fno-rtti`, static).
- **OSR** (OpenSourcereR) - author.

---

## License

Licensed under the **GNU General Public License v3.0** - see [LICENSE.txt](LICENSE.txt)
for the full text.

In short:
- You may use, study, modify, and redistribute this software freely.
- If you distribute a modified binary, you **must** also make the complete
  corresponding source code available under the same license.
- The software is provided **without warranty** of any kind.
