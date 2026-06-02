# ps5debug-NG - Wire Protocol Reference

Developer-facing reference for every command exposed by the debugger payload.
The opcodes, struct layouts, sizes, and behaviours below are taken directly from
the sources under `debugger/source/`, `common/`, and the client
`Cheater-NG/CheaterNG/PS4DBG.cs`, with file + function citations. Where a server
value and the on-the-wire value differ (see the bit-swap note in 1.6), both are
given.

This document reflects `ps5debug-NG v1.2.6` (`common/include/version.h`,
`PS5DEBUG_NG_VERSION_STR`). The on-wire **protocol** version reported by
`CMD_VERSION` is a separate string, currently `"1.3"`.

---

## 1. Wire protocol fundamentals

### 1.1 Core constants

| Symbol             | Value                            | Source                                                   |
|--------------------|----------------------------------|----------------------------------------------------------|
| protocol version   | `"1.3"`                          | `meta.c` `handle_version` (local `char ver[]="1.3"`)     |
| branding string    | `"ps5debug-NG by OSR v1.2.6"`    | `version.h` `PS5DEBUG_NG_BRAND_STR` via `meta.c` `handle_branding` |
| `PACKET_MAGIC`     | `0xFFAABBCC`                     | `main.c:50` (local `#define`)                            |
| broadcast magic    | `0xFFFFAAAA`                     | `main.c` `broadcast_thread` (raw literal)                |
| auth magic         | `0xBB40E64D`                     | `protocol.h:249` (`CMD_PROC_AUTH_MAGIC`)                 |

None of these are defined in `protocol.h` except the auth magic. There is no
`server.h` / `server.c` in this tree; the framing, dispatch, and discovery code
lives in `main.c` and `meta.c`.

### 1.2 Ports

| Port | Protocol | Role                                                     | Source                              |
|------|----------|----------------------------------------------------------|-------------------------------------|
| 744  | TCP      | Main command server                                      | `main.c` `start_server` (`SERVER_PORT`) |
| 755  | TCP      | Async debug-interrupt channel (server connects out to client) | `debug.c` `connect_debugger` (`htons(755)`) |
| 3232 | TCP      | Kernel-log forwarder (server listens; streams `/dev/klog`) | `console.c` `klog_server_thread` (`sin_port=0xA00C`) |
| 1010 | UDP      | Discovery broadcast                                      | `main.c` `broadcast_thread` (port `0x3F2`) |

### 1.3 Byte order

All multi-byte integers are **little-endian** (native x86-64 layout).
`__attribute__((packed))` is used on every wire struct, so there is no implicit
padding between fields. (Exception: status words are bit-swapped on the wire,
see 1.6.)

### 1.4 Packet framing

Every request on port 744 begins with a fixed 12-byte header. `void *data` in the
C struct is NOT on the wire - it is a server-side pointer assigned after the
body is received (`protocol.h:26-31`, `main.c` `handle_client`):

```
struct cmd_packet {        // 12 bytes on the wire
    uint32_t magic;        // must equal PACKET_MAGIC (0xFFAABBCC)
    uint32_t cmd;          // command ID (see 4)
    uint32_t datalen;      // length of request body that follows the header
    void    *data;         // NOT transmitted; server-side pointer only
};
#define CMD_PACKET_SIZE 12
```

Framing loop (`main.c` `handle_client`, lines 70-155):

1. Each connection is serviced on its own thread. The thread `select()`s the
   socket and blocking-reads 12 bytes into a `cmd_packet` header.
2. Validates `magic == PACKET_MAGIC`; on mismatch the packet is silently dropped
   and the loop continues (no error response).
3. If `datalen != 0`: when `datalen > 0x100000` (1 MiB) the server sends the raw
   status literal `0xF0000002` (which is `CMD_ERROR` on the wire, see 1.6) and
   continues; otherwise it `malloc(datalen)`s a buffer and reads exactly
   `datalen` bytes into `packet.data`. (It uses `malloc`, not `net_alloc_buffer`.)
4. If the command is in the debug namespace (`(cmd & 0xFFFF0000) == 0xBDBB0000`),
   it is dispatched while holding both `g_proc_rw_mutex` and `g_server_mutex`.
5. `cmd_handler` dispatches to the namespace handler. A handler may read
   *additional* trailing data from the socket beyond the fixed request struct
   (writes, ELF images, register blobs, scan payloads), and several handlers
   reply with an intermediate `CMD_SUCCESS` *before* the data phase and a final
   status *after* it (see 8).

### 1.5 Dispatch routing (`meta.c` `cmd_handler`, lines 54-73)

`cmd_handler` inlines five infrequent commands, then routes everything else by
testing the command's **middle byte** (bits 16-23) against the `VALID_*_CMD`
macros:

| Namespace      | Predicate           | Dispatch target  | File        |
|----------------|---------------------|------------------|-------------|
| Info/ping      | n/a (inlined)       | `cmd_handler`    | `meta.c` (5 cases) |
| Process        | `VALID_PROC_CMD`    | `proc_handle`    | `proc.c`    |
| Debug          | `VALID_DEBUG_CMD`   | `debug_handle`   | `debug.c`   |
| Kernel R/W     | `VALID_KERN_CMD`    | `kern_handle`    | `kern.c`    |
| Console        | `VALID_CONSOLE_CMD` | `console_handle` | `console.c` |

`VALID_CMD(cmd)` first requires the top byte to be `0xBD`
(`protocol.h:20`); a command failing that returns immediately. `debug_handle` is
a weak symbol and is only called if linked in.

**Attach special case:** in `main.c handle_client` (lines 112-115), when
`cmd == CMD_DEBUG_ATTACH` and no session is active (`g_debug_attached == 0`), the
server records the current client slot into the globals `curdbgcli` /
`curdbgctx` before dispatching. The attach itself is then handled normally by
`debug_attach_handle` (see 2.3). There is no separate `_svc` handler.

### 1.6 Status codes and the bit-swap

The server transmits every `uint32_t` status word through `net_send_int32`
(`net.c:39`), which bit-swaps adjacent even/odd bit positions:

```c
static inline uint32_t bitswap32(uint32_t x) {
    return ((x >> 1) & 0x55555555u) | ((x << 1) & 0xAAAAAAAAu);  // involution
}
```

So the **server-side macro value** (`protocol.h:9-12`, `CMD_INVALID_INDEX` at
`:271`) and the **on-the-wire value** a client compares against differ. The swap is applied **only** to status
words; response payloads, length prefixes, struct bodies, and raw memory are
sent unswapped. Clients do **not** un-swap - they compare the raw wire word
directly to the post-swap constants (`PS4DBG.cs` `CMD_STATUS` + `ReceiveStatus`).

| Name                | Server macro (`protocol.h`) | On-the-wire value (clients compare this) |
|---------------------|-----------------------------|------------------------------------------|
| `CMD_SUCCESS`       | `0x40000000`                | `0x80000000`                             |
| `CMD_ERROR`         | `0xF0000002`                | `0xF0000001`                             |
| `CMD_DATA_NULL`     | `0xF0000003`                | `0xF0000003`                             |
| `CMD_ALREADY_DEBUG` | `0xF0000008`                | `0xF0000004`                             |
| `CMD_INVALID_INDEX` | `0xF000000A`                | `0xF0000005`                             |

Notes:
- There is **no** `CMD_TOO_MUCH_DATA` macro and **no** `CMD_FATAL_STATUS` macro
  in this tree. The over-size guard (1.4 step 3) sends the literal `0xF0000002`,
  which is `CMD_ERROR` on the wire (`0xF0000001`). The client's
  `CMD_TOO_MUCH_DATA = 0xF0000002` enum value is therefore never actually
  received.
- The auth magic uses the same swap: the server checks
  `bitswap32(packet magic) == CMD_PROC_AUTH_MAGIC_BSWAP (0x7780D98E)`, i.e. the
  client sends the raw `0xBB40E64D`.

### 1.7 Discovery (`main.c` `broadcast_thread`, lines 174-218)

A UDP server on port 1010 receives 4-byte datagrams. If the payload equals
`0xFFFFAAAA`, the server echoes the same 4 bytes back to the sender. No other
payload is acted on; there is no authentication on this channel. (`PS4DBG.cs`
`FindPlayStation` broadcasts the magic and matches the echo.)

### 1.8 Authentication (`auth.c` `proc_auth_handle`)

A single **global** flag word `g_proc_auth_state` (`auth.c:13`, default
`0x14000`) gates the *stateful* scan trio. Bit 1 (`0x02`) is the scan-enabled
bit; `CMD_PROC_SCAN_START` / `_COUNT` / `_GET` check `g_proc_auth_state & 2` on
entry (`scan.c:351`, `:475`, `:620`). The legacy/AOB scans
(`CMD_PROC_SCAN`, `CMD_PROC_SCAN_AOB`, `CMD_PROC_SCAN_AOB_MULTI`) do **not**
check it. The flag is global and is **not** reset on disconnect.

`CMD_PROC_AUTH` (`0xBDAACCFF`) handshake:

1. Client sends `struct cmd_proc_auth_packet` (`protocol.h:251`):
   `uint32_t magic` (raw `0xBB40E64D`) + `uint32_t flags`.
2. Server validates the magic (1.6) and replies `CMD_SUCCESS`.
3. Server sends `uint16_t challenge_length = 64`, then 64 challenge bytes
   (derived from an internal xorshift-128 generator).
4. Client XORs the challenge against a fixed keystream (xorshift-128 seeded with
   `200,300,400,500`; see `PS4DBG.cs` `GenerateKeystream`) and sends 64 response
   bytes back.
5. On a byte-exact match the server sets the flag word - `flags & 1` ->
   `0x14001`, `flags & 2` -> `0x14002` (scan-enabled) - and replies
   `CMD_SUCCESS`; otherwise `CMD_DATA_NULL`. (`PS4DBG.cs` `ValidateProcess`
   passes `flags = maxLength`, whose bit 1 is set, enabling scans.)

### 1.9 Asynchronous interrupt channel (port 755)

On `CMD_DEBUG_ATTACH`, the server opens a TCP connection *outbound* to the
client's IP on port 755 (`debug.c` `connect_debugger`, `htons(755)`). The client
must be listening on 755 before attaching (`PS4DBG.cs` `DebuggerThread` binds
`0.0.0.0:755`).

Breakpoint/watchpoint hits and single-step completions are delivered as
fixed-size 1184-byte (`0x4A0`) packets. There is **no** named struct or size
macro in the server source for this - the packet is assembled by hand at fixed
offsets in `debug.c` `dispatch_debug_events` (lines 1189-1240). The layout is:

```
offset  size  field
0x000     4   uint32_t lwpid
0x004     4   uint32_t status                 // raw wait4 status word
0x008    40   char     tdname[40]             // ~24 bytes copied from PT_LWPINFO pl_tdname
0x030   176   struct reg   reg64              // FreeBSD amd64 GP registers
0x0E0   832   struct fpu   savefpu            // FPU + YMM
0x420   128   struct dbreg dbreg64            // 16 x uint64_t
                                              // total = 0x4A0 = 1184
```

The matching client struct is `PS4DBG.cs` `DebuggerInterruptPacket`
(`tdname[40]`, `regs` 176, `fpregs` 832, `dbregs` 128). `savefpu` lands at offset
`0xE0` (224); the struct is packed so it is not 64-byte aligned, which is
intentional.

---

## 2. Command reference

58 opcodes have handlers. Only a handful are given `CMD_*` macros in
`protocol.h` (`CMD_VERSION`, `CMD_FW_VERSION`, `CMD_BRANDING`, `CMD_PLATFORM_ID`,
`CMD_PROC_NOP`, `CMD_DEBUG_ATTACH`, `CMD_DEBUG_SET_BREAKPOINT`,
`CMD_DEBUG_SET_WATCHPOINT`); the rest are bare hex literals inside the
per-namespace `switch` statements. Three opcodes have no symbolic name at all:
`0xBDAA000D`, `0xBDAA0024`, `0xBDDD0006`.

### 2.1 Info & ping - inlined in `cmd_handler` (`meta.c:60-64`)

#### `CMD_VERSION = 0xBD000001`
- **Request body:** none.
- **Response:** `uint32_t length`, then `length` bytes of the protocol version
  string ("1.3"). No status word precedes it.

#### `CMD_FW_VERSION = 0xBD000500`
- **Request body:** none.
- **Response:** `uint16_t` firmware as a **decimal** number (the BCD high word of
  `kernel_get_fw_version()` converted digit-by-digit), e.g. `900` for 9.00,
  `1240` for 12.40, `505` for 5.05. No status word precedes it.

#### `CMD_BRANDING = 0xBD000501`
- **Request body:** none.
- **Response:** `uint32_t length`, then `length` bytes of the branding string
  ("ps5debug-NG by OSR v1.2.6"). No status word precedes it. (The client calls
  this `CMD_EXT_VERSION`.)

#### `CMD_PLATFORM_ID = 0xBD000502`
- **Request body:** none.
- **Response:** `uint16_t`, hardcoded `5`. No status word precedes it. (Present
  in the server only; the CheaterNG client does not invoke it.)

#### `CMD_PROC_NOP = 0xBDAACC06`
- **Request body:** none.
- **Response:** `CMD_SUCCESS`. Keepalive / liveness probe (client `Ping`).

---

### 2.2 Process commands - dispatched by `proc_handle` (`proc.c:958-993`)

Unless noted, the request body is read into `packet->data` up front; "trailing
data" is read from the socket by the handler after a leading `CMD_SUCCESS`.

#### `CMD_PROC_LIST = 0xBDAA0001` (`proc.c:364`)
- **Request body:** none.
- **Response:** `CMD_SUCCESS`, `uint32_t num` (4 bytes), then `num` x
  `struct proc_list_entry { char name[32]; int32_t pid; }` (36 bytes each,
  `protocol.h:43`). The list is built by walking the kernel `allproc` chain.

#### `CMD_PROC_READ = 0xBDAA0002` (`proc.c:440`)
- **Request body:** `struct cmd_proc_read_packet` (16 bytes).
- **Response:** `CMD_SUCCESS`, then up to `length` bytes in 64 KiB (`0x10000`)
  chunks.

#### `CMD_PROC_WRITE = 0xBDAA0003` (`proc.c:563`)
- **Request body:** `struct cmd_proc_write_packet` (16 bytes).
- **Response:** `CMD_SUCCESS` (before the data phase), then the server reads
  `length` bytes in 64 KiB chunks, then `CMD_SUCCESS` again. **Two** status
  words (see 8).

#### `CMD_PROC_MAPS = 0xBDAA0004` (`proc.c:597`)
- **Request body:** `struct cmd_proc_maps_packet` (4 bytes, `pid`).
- **Response:** `CMD_SUCCESS`, `uint32_t num`, then `num` x
  `struct proc_vm_map_entry` (**58 bytes** each, `protocol.h:154`):
  `{ char name[32]; uint64_t start; uint64_t end; uint64_t offset; uint16_t prot; }`.
  The map may be augmented by a kernel page-table walk (`proc_ptwalk_augment`)
  before sending.

#### `CMD_PROC_INTALL = 0xBDAA0005` (`proc.c:648`)
Literal spelling `INTALL` (typo preserved on the wire).
- **Request body:** `struct cmd_proc_install_packet` (4 bytes, `pid`).
- **Response:** `CMD_SUCCESS`, then `uint64_t rpcstub = 0`.
- **Note:** in this build the handler is a **stub** - it returns `rpcstub = 0`
  and performs no injection.

#### `CMD_PROC_CALL = 0xBDAA0006` (`proc.c:659`)
- **Request body:** `struct cmd_proc_call_packet` (68 bytes):
  `uint32_t pid; uint64_t rpcstub, rpc_rip, rpc_rdi, rpc_rsi, rpc_rdx, rpc_rcx, rpc_r8, rpc_r9;`
- **Response:** `CMD_SUCCESS`, then `struct cmd_proc_call_response`
  `{ uint32_t pid; uint64_t rpc_rax; }` (12 bytes). On backend failure the
  handler returns `-1` with no status word (the connection is then torn down).

#### `CMD_PROC_ELF = 0xBDAA0007` (`proc.c:691`)
- **Request body:** `struct cmd_proc_elf_packet` `{ uint32_t pid; uint32_t length; }` (8 bytes).
- **Response:** `CMD_SUCCESS` (before data), reads `length` bytes of ELF, then
  `CMD_SUCCESS` or `CMD_ERROR`. **Two** status words.

#### `CMD_PROC_PROTECT = 0xBDAA0008` (`proc.c:769`)
- **Request body:** `struct cmd_proc_protect_packet` (20 bytes, defined in
  `proc.c:762`): `{ uint32_t pid; uint64_t address; uint32_t length; uint32_t prot; }`.
- **Response:** `CMD_SUCCESS` or `CMD_ERROR` (single status).

#### `CMD_PROC_SCAN = 0xBDAA0009` (`scan.c:82`)
Legacy single-pass scan. **No auth required.**
- **Request body:** `struct cmd_proc_scan_packet` (10 bytes):
  `{ uint32_t pid; uint8_t valueType; uint8_t compareType; uint32_t lenData; }`.
- **Trailing data:** `lenData` bytes of comparison value(s).
- **Response:** `CMD_SUCCESS` (ack), the server reads the value, scans, sends
  `CMD_SUCCESS` again, then **streams** matching addresses as `uint64_t`s
  terminated by an all-`0xFF` sentinel `0xFFFFFFFFFFFFFFFF`. No count prefix.
  (Compare via `scan_compare.c` `proc_scan_legacy_compareValues`.)

#### `CMD_PROC_INFO = 0xBDAA000A` (`proc.c:790`)
- **Request body:** `struct cmd_proc_info_packet` (4 bytes, `pid`).
- **Response:** `CMD_SUCCESS`, `struct cmd_proc_info_response` (**188 bytes**,
  `protocol.h:259`): `{ uint32_t pid; char name[40]; char path[64]; char titleid[16]; char contentid[64]; }`.
  `titleid`/`contentid` are resolved by `sceKernelGetAppInfo` plus a pattern scan
  of the proc struct, falling back to per-firmware offsets
  (`proc_field_offsets.c`).

#### `CMD_PROC_ALLOC = 0xBDAA000B` (`proc.c:853`)
- **Request body:** `struct cmd_proc_alloc_packet` (8 bytes, `{ pid, length }`).
- **Response:** `CMD_SUCCESS`, `struct cmd_proc_alloc_response` `{ uint64_t address; }` (8 bytes).

#### `CMD_PROC_FREE = 0xBDAA000C` (`proc.c:880`)
- **Request body:** `struct cmd_proc_free_packet` (16 bytes, `{ pid, address, length }`).
- **Response:** `CMD_SUCCESS`.

#### `0xBDAA000D` - first-map probe (`proc.c:901`, `proc_unknown_d_handle`)
Raw literal, no `CMD_*` macro.
- **Request body:** `struct cmd_proc_unknown_d_packet` (4 bytes, `pid`).
- **Response:** `CMD_SUCCESS` then `uint64_t` = the `start` address of the
  target's first VM map entry, or `CMD_ERROR` if the map could not be read.

#### `CMD_PROC_ALLOC_HINTED = 0xBDAA000E` (`proc.c:929`)
- **Request body:** `struct cmd_proc_alloc_hinted_packet` (16 bytes, `{ pid, hint, length }`).
- **Response:** `CMD_SUCCESS` + `struct cmd_proc_alloc_response` on success; on
  failure it sends `CMD_ERROR` **and still** the (zeroed/partial) response struct.

#### `CMD_PROC_ELF_RPC = 0xBDAA0010` (`proc.c:724`)
Like `CMD_PROC_ELF` but returns the entry address instead of jumping into it.
- **Request body:** `struct cmd_proc_elf_rpc_packet` (8 bytes, `{ pid, length }`).
- **Response:** `CMD_SUCCESS` (before data), reads `length` ELF bytes, then
  `CMD_SUCCESS` + `struct cmd_proc_elf_rpc_response` `{ uint64_t entry; }` (8
  bytes). **Two** status words.

#### `CMD_PROC_DISASM_REGION = 0xBDAA0020` (`proc.c:197`)
- **Request body:** `struct cmd_proc_disasm_packet` (20 bytes, `{ pid, address, length, max_entries }`).
- **Validation:** `max_entries` must be `1 .. 1000000`, else `CMD_ERROR`.
- **Response:** `CMD_SUCCESS`, then a stream of `struct disasm_instr_entry` (32
  bytes each), terminated by a 32-byte sentinel with all bytes `0xFF`.
- **Backend:** Zydis (`third_party/zydis/Zydis.c`).

`disasm_instr_entry` (`protocol.h:235`):
```
struct disasm_instr_entry {   // 32 bytes
    uint64_t addr;              // instruction start
    uint64_t rip_rel_target;    // resolved absolute addr if RIP-relative; else 0
    int64_t  mem_disp;          // [base + index*scale + disp]
    uint8_t  length;
    uint8_t  kind;              // bitmask below
    uint8_t  mem_base_reg;      // ZydisRegister low byte; 0 if none
    uint8_t  mem_index_reg;
    uint8_t  mem_scale;         // 1/2/4/8; 0 if none
    uint8_t  mnemonic_lo;       // ZydisMnemonic low byte
    uint16_t pad;
};
```

Kind bitmask (`proc.c` `disasm_fill_entry`):
| Bit  | Meaning                            |
|------|------------------------------------|
| 0x01 | `CALL`                             |
| 0x02 | `RET`                              |
| 0x04 | Unconditional `JMP`                |
| 0x08 | Conditional branch                 |
| 0x10 | Has a memory operand               |
| 0x20 | Memory operand is RIP-relative     |
| 0x40 | Memory read                        |
| 0x80 | Memory write                       |

#### `CMD_PROC_EXTRACT_CODE_XREFS = 0xBDAA0021` (`proc.c:262`)
- **Request body:** `struct cmd_proc_disasm_packet` (20 bytes); if `max_entries`
  is 0 a default of 10000000 is used.
- **Response:** `CMD_SUCCESS`, then a **stream** of `uint64_t` RIP-relative
  target addresses (one per RIP-relative memory operand encountered; not
  de-duplicated by the server), terminated by a `0xFFFFFFFFFFFFFFFF` sentinel.
  No count prefix.

#### `CMD_PROC_FIND_XREFS_TO = 0xBDAA0022` (`proc.c:325`)
- **Request body:** `struct cmd_proc_xrefs_to_packet` (24 bytes):
  `{ uint32_t pid; uint64_t scan_address; uint32_t scan_length; uint64_t target_address; }`.
- **Response:** `CMD_SUCCESS`, then a **stream** of `uint64_t` instruction
  addresses whose resolved RIP-relative target equals `target_address`,
  terminated by a `0xFFFFFFFFFFFFFFFF` sentinel. No count prefix, no kind byte.

#### `CMD_PROC_READ_STACK = 0xBDAA0023` (`proc.c:485`)
Server-side RBP-chain walk: the server walks the frame chain and bundles each
frame's saved RBP, return address, frame-local bytes, and a code window around
the return address into one response.
- **Request body:** `struct cmd_proc_read_stack_packet` (24 bytes):
  `{ uint32_t pid; uint64_t rbp; uint64_t rsp; uint32_t depth; }`
  - `rbp`/`rsp` describe the local (top) frame; `depth` caps frames returned.
- **Response:** `CMD_SUCCESS`, `uint32_t bundle_len`, then `bundle_len` bytes:
  ```
  u32 n_frames
  per frame {
      u64 rbp; u64 rsp; u64 saved_rbp; u64 ret_addr;
      u32 flags;            // bit0: frame-locals omitted (oversized/invalid)
      u32 frame_locals_len;
      u32 code_len;         // bytes of code at (ret_addr - 10); 0 if unavailable
      u8  frame_locals[frame_locals_len];
      u8  code[code_len];
  }
  ```
  - Caps (`protocol.h:89-93`): `MAX_DEPTH = 64`, `LOCALS_CAP = 0x1000` per frame,
    `CODE_OFF = 10`, `CODE_LEN = 200`.

#### `0xBDAA0024` - assemble x86-64 (`assemble.c` `proc_assemble_handle`)
Raw literal, no `CMD_*` macro. Assembles x86-64 text into machine bytes using the
embedded Keystone (LLVM-MC) assembler. Pure userspace; no attached process
required.
- **Request body** (in `packet->data`; `datalen` must be `>= 12`): a 12-byte
  header `struct cmd_proc_assemble_packet` `{ uint64_t base_addr; uint32_t ks_opt_syntax; }`
  followed by `(datalen - 12)` bytes of asm text (NUL-termination not required).
  - `base_addr` is what Keystone resolves PC-relative operands against.
  - `ks_opt_syntax = 0` keeps the engine default (Intel); a non-zero value is
    passed straight to `ks_option(KS_OPT_SYNTAX, ...)`.
- **Response on success:** `CMD_SUCCESS`, `struct cmd_proc_assemble_ok`
  `{ uint32_t byte_len; uint32_t insn_count; }` (8 bytes), then `byte_len`
  machine-code bytes.
- **Response on assembler error:** `CMD_ERROR`, `struct cmd_proc_assemble_err`
  `{ uint32_t ks_errno; uint32_t msg_len; }` (8 bytes), then `msg_len` bytes of
  the `ks_strerror` message.

#### `CMD_PROC_SCAN_AOB = 0xBDAA0501` (`scan.c:151`)
Array-of-bytes single scan. **No auth required.**
- **Request body:** `struct cmd_proc_scan_aob_packet` (22 bytes):
  `{ uint32_t pid; uint64_t address; uint32_t length; uint8_t max_matches; uint8_t stop_flag; uint32_t pattern_length; }`.
- **Trailing data:** `pattern_length` pattern bytes, then `pattern_length` mask
  bytes (mask byte `1` = must match; any other value = wildcard).
- **Response:** `CMD_SUCCESS` (ack), reads pattern+mask, `CMD_SUCCESS` again,
  scans, then sends a single `uint64_t` = the `max_matches`-th match address
  (or `0` if not found, or if `stop_flag == 1` and the match is not unique),
  then `CMD_SUCCESS`.

#### `CMD_PROC_SCAN_AOB_MULTI = 0xBDAA0502` (`scan.c:237`)
Multi-pattern AOB scan. **No auth required.** (Client name: `CMD_PROC_SCAN_AOB_ALL`.)
- **Request body:** `struct cmd_proc_scan_aob_multi_packet` (21 bytes):
  `{ uint32_t pid; uint64_t address; uint32_t length; uint8_t stop_flag; uint32_t patterns_length; }`.
- **Trailing data:** `patterns_length` bytes of concatenated patterns; each
  pattern is `[uint8_t target_count][uint32_t pattern_length][pattern_length pattern bytes][pattern_length mask bytes]`.
- **Response:** `CMD_SUCCESS` (ack), reads the blob, `CMD_SUCCESS` again, scans,
  then sends `pat_count` x `uint64_t` (one match address per pattern, `0` if
  none / invalidated), then `CMD_SUCCESS`. (The client supplies `pat_count` and
  reads exactly that many `uint64_t`s.)

#### `CMD_PROC_AUTH = 0xBDAACCFF` (`auth.c:60`)
See 1.8.

#### `CMD_PROC_SCAN_START = 0xBDAACC01` (`scan.c:350`) - **requires auth bit 1**
First pass of the staged scanner. (Client name: `CMD_PROC_SCAN_INIT`.)
- **Request body:** `struct cmd_proc_scan_start_packet` (23 bytes):
  `{ uint32_t pid; uint64_t address; uint32_t length; uint8_t valueType; uint8_t compareType; uint8_t alignment; uint32_t lenData; }`.
- **Trailing data:** the seed value(s) when the compare type needs them, plus a
  mask when `valueType == 10` (arrBytes).
- **Response:** `CMD_SUCCESS` (ack), reads value/mask, `CMD_SUCCESS` again, then
  **streams** result blocks - each block is `uint64_t block_len` followed by
  `block_len` bytes of `(uint32_t offset, value)` entries (`offset` is relative
  to `address`) - terminated by a `0xFFFFFFFFFFFFFFFF` sentinel, then a final
  `CMD_SUCCESS`. The result set lives **on the client**; the server keeps no
  per-pid session.

#### `CMD_PROC_SCAN_COUNT = 0xBDAACC02` (`scan.c:474`) - **requires auth bit 1**
Filtering pass: the client streams its current candidate list back and the
server returns the survivors. (Client name: `CMD_PROC_SCAN_NEXT`.)
- **Request body:** `struct cmd_proc_scan_count_packet` (18 bytes):
  `{ uint32_t pid; uint64_t base_address; uint8_t valueType; uint8_t compareType; uint32_t lenData; }`.
- **Trailing data:** value/mask as for `SCAN_START`, then a client-driven loop of
  chunks: `uint32_t chunk_len` + `chunk_len` bytes of `(uint32_t offset[, prev_value])`
  entries. `chunk_len == 0xFFFFFFFF` ends the loop.
- **Response:** for each chunk the server re-reads memory, re-compares, and sends
  surviving `(offset, value)` entries as `uint64_t block_len`-prefixed blocks
  followed by a per-chunk `0xFFFFFFFFFFFFFFFF` sentinel. After the loop ends it
  sends a final `CMD_SUCCESS`.

#### `CMD_PROC_SCAN_GET = 0xBDAACC03` (`scan.c:619`) - **requires auth bit 1**
Bulk memory fetch for a candidate list. (Client name: `CMD_PROC_READ_MULTI`.)
- **Request body:** `struct cmd_proc_scan_get_packet` (8 bytes, `{ pid, count }`).
- **Trailing data:** `count` x `{ uint64_t address; uint32_t length; }` (12 bytes
  each).
- **Response:** `CMD_SUCCESS`, then the raw memory bytes for each entry
  concatenated in order, then a `0xFFFFFFFFFFFFFFFF` sentinel. (Returns raw
  memory, not addresses.)

For the `valueType` / `compareType` values, see 7.

---

### 2.3 Debug commands - dispatched by `debug_handle` (`debug.c:1529-1552`)

Most handlers check `g_debug_attached` (and a non-zero pid) themselves and reply
`CMD_ERROR` if no session is active (`CMD_DEBUG_PROCESS_STOP` is the exception -
it works without a session). An unknown debug opcode replies `CMD_ERROR`. The
dispatcher does not silently drop.

#### `CMD_DEBUG_ATTACH = 0xBDBB0001` (`debug.c:544`, `debug_attach_handle`)
This is the real attach handler (there is no stub / `_svc` variant).
- **Request body:** `struct cmd_debug_attach_packet` (**4 bytes**, `{ uint32_t pid; }`).
  The client's IP comes from the connection slot recorded in 1.5, not from the
  packet.
- **Side effects:** elevates, `ptrace(PT_ATTACH)`s the target, resumes the app,
  and opens the outbound interrupt connection to the client on port 755 (1.9).
- **Response:** `CMD_SUCCESS`, or `CMD_ALREADY_DEBUG` / `CMD_DATA_NULL` /
  `CMD_ERROR`.

#### `CMD_DEBUG_DETACH = 0xBDBB0002` (`debug.c:604`)
- **Request body:** none.
- **Response:** `CMD_SUCCESS`. Runs `debug_full_teardown` (clears breakpoints /
  watchpoints, resumes, detaches, closes the 755 connection).

#### `CMD_DEBUG_SET_BREAKPOINT = 0xBDBB0003` (`debug.c:613`)
- **Request body:** `struct cmd_debug_breakpt_packet` (16 bytes, `{ index, enabled, address }`).
- **Response:** `CMD_SUCCESS`, or `CMD_INVALID_INDEX` if `index > 29`.
- **Backend:** software breakpoint - writes `0xCC` via `proc_write_mem` (which
  uses the DMAP path on FW >= 9), saving the original byte.
  `MAX_BREAKPOINTS = 30` (`protocol.h:273`). (The CheaterNG client caps its own
  index at 10.)

#### `CMD_DEBUG_SET_WATCHPOINT = 0xBDBB0004` (`debug.c:671`)
- **Request body:** `struct cmd_debug_watchpt_packet` (24 bytes):
  `{ index, enabled, length, breaktype, address }`.
- **Response:** `CMD_SUCCESS`, or `CMD_INVALID_INDEX` if `index > 3`.
- **Backend:** hardware breakpoint via DR0-DR3 / DR7 (written through the kernel
  dbreg path on firmwares that need it). `MAX_WATCHPOINTS = 4` (`protocol.h:274`).
  `breaktype` and `length` use the x86 DR7 encodings in 7.3.

#### `CMD_DEBUG_GET_THREAD_LIST = 0xBDBB0005` (`debug.c:764`)
- **Request body:** none.
- **Response:** `CMD_SUCCESS`, `uint32_t num`, then `num` x `uint32_t lwpid`.

#### `CMD_DEBUG_SUSPEND_THREAD = 0xBDBB0006` (`debug.c:804`)
- **Request body:** `struct cmd_debug_stopthr_packet` (4 bytes, `lwpid`).
- **Response:** `CMD_SUCCESS` or `CMD_ERROR` (`PT_SUSPEND`).

#### `CMD_DEBUG_RESUME_THREAD = 0xBDBB0007` (`debug.c:817`)
- **Request body:** `struct cmd_debug_resumethr_packet` (4 bytes, `lwpid`).
- **Response:** `CMD_SUCCESS` or `CMD_ERROR` (`PT_RESUME`).

#### `CMD_DEBUG_GETREGS = 0xBDBB0008` (`debug.c:830`)
- **Request body:** `{ uint32_t lwpid; }` (4 bytes).
- **Response:** `CMD_SUCCESS`, then a **176-byte** GP-register blob (FreeBSD
  amd64 `struct reg`).

#### `CMD_DEBUG_SETREGS = 0xBDBB0009` (`debug.c:850`)
- **Request body:** `struct cmd_debug_setregs_packet` (8 bytes, `{ lwpid, length }`).
- **Response:** `CMD_SUCCESS` (ack), reads `length` bytes (must be 176), then
  `CMD_SUCCESS`. **Two** status words.

#### `CMD_DEBUG_GETFPREGS = 0xBDBB000A` (`debug.c:871`)
- **Request body:** `{ uint32_t lwpid; }` (4 bytes).
- **Response:** `CMD_SUCCESS`, then an **832-byte** FPU/YMM blob.

#### `CMD_DEBUG_SETFPREGS = 0xBDBB000B` (`debug.c:891`)
- **Request body:** `{ uint32_t lwpid; uint32_t length; }` (8 bytes).
- **Response:** `CMD_SUCCESS` (ack), reads `length` bytes (832), then
  `CMD_SUCCESS`. **Two** status words.

#### `CMD_DEBUG_GETDBREGS = 0xBDBB000C` (`debug.c:912`)
- **Request body:** `{ uint32_t lwpid; }` (4 bytes).
- **Response:** `CMD_SUCCESS`, then a **128-byte** debug-register blob.

#### `CMD_DEBUG_SETDBREGS = 0xBDBB000D` (`debug.c:949`)
- **Request body:** `{ uint32_t lwpid; uint32_t length; }` (8 bytes).
- **Response:** `CMD_SUCCESS` (ack), reads `length` bytes (128), then
  `CMD_SUCCESS`. **Two** status words.

#### `CMD_DEBUG_CONTINUE = 0xBDBB0010` (`debug.c:977`)
- **Request body:** `struct cmd_debug_stopgo_packet` (4 bytes); the first byte is
  the action.
- `0` -> resume; `1` -> stop (`SIGSTOP`); `2` -> kill (`SIGKILL`) - routed
  through `debug_stopgo_handle`. (Client name: `CMD_DEBUG_STOPGO`.)
- **Response:** `CMD_SUCCESS`.

#### `CMD_DEBUG_THREAD_INFO = 0xBDBB0011` (`debug.c:1022`)
- **Request body:** `struct cmd_debug_thrinfo_packet` (4 bytes, `lwpid`).
- **Response:** `CMD_SUCCESS`, `struct dbg_thrinfo_response` (40 bytes,
  `{ uint32_t lwpid; uint32_t priority; char tdname[32]; }`).

#### `CMD_DEBUG_STEP = 0xBDBB0012` (`debug.c:1050`)
- **Request body:** none.
- **Response:** `CMD_SUCCESS`. `PT_STEP` on the whole process. (Client name:
  `CMD_DEBUG_SINGLESTEP`.)

#### `CMD_DEBUG_STEP_THREAD = 0xBDBB0013` (`debug.c:1066`)
- **Request body:** `struct cmd_debug_stopthr_packet` (4 bytes, `lwpid`).
- **Response:** `CMD_SUCCESS` or `CMD_ERROR`. `PT_STEP` on a single LWP.
  (Server-only; not invoked by the CheaterNG client.)

#### `CMD_DEBUG_PROCESS_STOP = 0xBDBB0500` (`debug.c:526`)
- **Request body:** 5 raw bytes: `uint32_t pid; uint8_t state;`. Returns
  `CMD_ERROR` if `state > 2` or `pid == 0`. (Client name: `CMD_DEBUG_EXT`.)
- **Response:** `CMD_SUCCESS`, `CMD_DATA_NULL`, or `CMD_ERROR`.
- **Behaviour** (`debug.c` `debug_stopgo_handle`): if a session is active, a
  signal is staged for the next event dispatch (`0` -> deferred resume,
  `1` -> `SIGSTOP` (17), `2` -> `SIGKILL` (9)). Otherwise `kill(pid, sig)` is
  called directly using the table `{ 19 = SIGCONT (resume), 17 = SIGSTOP, 9 = SIGKILL }`.

---

### 2.4 Kernel R/W commands - dispatched by `kern_handle` (`kern.c:53-60`)

#### `CMD_KERN_BASE = 0xBDCC0001` (`kern.c:8`)
- **Request body:** none.
- **Response:** `CMD_SUCCESS`, then `uint64_t` = the kernel `.data` base
  (`KERNEL_ADDRESS_DATA_BASE`).

#### `CMD_KERN_READ = 0xBDCC0002` (`kern.c:16`)
- **Request body:** `struct cmd_kern_read_packet` (12 bytes, `{ address, length }`).
- **Response:** `CMD_SUCCESS`, then `length` bytes (`kernel_copyout_fast`).

#### `CMD_KERN_WRITE = 0xBDCC0003` (`kern.c:34`)
- **Request body:** `struct cmd_kern_write_packet` (12 bytes, `{ address, length }`).
- **Response:** `CMD_SUCCESS` (before the data phase), reads `length` bytes,
  `kernel_copyin_fast`, then `CMD_SUCCESS` again. **Two** status words.

---

### 2.5 Console commands - dispatched by `console_handle` (`console.c:312-322`)

#### `CMD_CONSOLE_REBOOT = 0xBDDD0001` (`console.c:30`)
- **Request body:** none.
- **Behaviour:** calls `sceKernelReboot()` and returns; the console reboots.

#### `CMD_CONSOLE_END = 0xBDDD0002` (`console.c:36`, `console_end_handle`)
- **Request body:** none.
- **Response:** `CMD_SUCCESS`.

#### `CMD_CONSOLE_PRINT = 0xBDDD0003` (`console.c:42`)
- **Request body:** `struct cmd_console_print_packet` `{ uint32_t length; }` (4 bytes).
- **Trailing data:** `length` bytes of text.
- **Response:** `CMD_SUCCESS`. (Writes via `klog_puts`.)

#### `CMD_CONSOLE_NOTIFY = 0xBDDD0004` (`console.c:61`)
- **Request body:** `struct cmd_console_notify_packet` `{ uint32_t messageType; uint32_t length; }` (8 bytes).
- **Trailing data:** `length` bytes of text (the client appends a trailing `\0`).
- **Response:** `CMD_SUCCESS`.
- **Behaviour:** calls `sceKernelSendNotificationRequest` with the text.
  `messageType` is read off the wire but **not used** by this handler.

#### `CMD_CONSOLE_INFO = 0xBDDD0005` (`console.c:88`, `console_info_handle`)
- **Request body:** none.
- **Response:** `CMD_SUCCESS`. No side effect (ping variant).

#### `0xBDDD0006` - foreground-app metadata (`console.c:183`, `console_foreground_app_handle`)
Raw literal, no `CMD_*` macro. (Client name: `CMD_CONSOLE_FOREGROUND_APP`.)
- **Request body:** none.
- **Response:** `CMD_SUCCESS`, `struct cmd_console_foreground_app_response`
  (**140 bytes**, `console.c:144`):
  ```
  u32  pid;
  char titleid[16];
  char contentid[64];
  char name[40];
  char app_ver[16];     // version string (app_ver at byte offset 124)
  ```
- **Behaviour:** the foreground `eboot.bin` process is found by walking
  `allproc`. `titleid`/`contentid` are resolved by pattern-scanning the proc
  struct (FW-agnostic), falling back to per-firmware offsets. The version is read
  from the title's `param.sfo` (`APP_VER` / `VERSION`, the lexically larger of
  the two), falling back to `param.json` (`contentVersion` /
  `originContentVersion`, else `masterVersion`). If there is no foreground game
  all fields are zeroed (`pid == 0`).

---

## 3. In-process kernel/target primitives

ps5debug-NG runs the entire wire-protocol server inside SceShellCore (the
installer injects the embedded debugger ELF as a SceShellCore-internal thread).
"Kernel-side" operations are performed from userland using primitives from
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) plus short kernel
patches applied at install time:

| Concern                              | How ps5debug-NG implements it                                              |
|--------------------------------------|----------------------------------------------------------------------------|
| Process enumeration (`CMD_PROC_LIST`)| Walk the kernel `allproc` linked list with `kernel_copyout_fast` (`proc.c` `proc_list_handle`). |
| Target memory read                   | `sys_proc_rw_w0` (mdbg), with a DMAP page-table-walk read for Sony-aux regions (`proc.c` `proc_read_mem`). |
| Target memory write                  | DMAP page-table-walk write on FW >= 9 (mdbg write is `EPERM`-gated there), mdbg `sys_proc_rw_w1` otherwise (`proc.c` `proc_write_mem`). |
| `mprotect` / alloc / free / call / ELF | `sys_proc_cmd` syscall variants (`kdbg.h` `SYS_PROC_*`). |
| Kernel base / kernel R/W             | `KERNEL_ADDRESS_DATA_BASE`, `kernel_copyout_fast` / `kernel_copyin_fast` (`kern.c`). |
| VM map (`CMD_PROC_MAPS`)             | Read `vmspace` + map entries via `kernel_copyout_fast`, augmented by a page-table walk (`scan.c` `sys_proc_vm_map`, `proc_ptwalk_augment`). |
| Console reboot / notify / print      | `sceKernelReboot`, `sceKernelSendNotificationRequest`, `klog_puts`.        |

The installer's role is bootstrap-only: find SceShellCore, apply the FW-specific
kernel patch, set up the target's execution state, and hand control to the
embedded debugger. After that the installer exits and the debugger services TCP.

---

## 4. Full command ID table

Auth = requires `g_proc_auth_state & 2` (set via `CMD_PROC_AUTH`).

| Hex          | Server handler name              | File / location          | Auth? |
|--------------|----------------------------------|--------------------------|-------|
| `0xBD000001` | `handle_version`                 | `meta.c` (inline)        |       |
| `0xBD000500` | `handle_fw_version`              | `meta.c` (inline)        |       |
| `0xBD000501` | `handle_branding`                | `meta.c` (inline)        |       |
| `0xBD000502` | `handle_platform_id`             | `meta.c` (inline)        |       |
| `0xBDAACC06` | `CMD_PROC_NOP`                   | `meta.c` (inline)        |       |
| `0xBDAA0001` | `proc_list_handle`               | `proc.c`                 |       |
| `0xBDAA0002` | `proc_read_handle`               | `proc.c`                 |       |
| `0xBDAA0003` | `proc_write_handle`              | `proc.c`                 |       |
| `0xBDAA0004` | `proc_maps_handle`               | `proc.c`                 |       |
| `0xBDAA0005` | `proc_install_handle` (stub)     | `proc.c`                 |       |
| `0xBDAA0006` | `proc_call_handle`               | `proc.c`                 |       |
| `0xBDAA0007` | `proc_elf_handle`                | `proc.c`                 |       |
| `0xBDAA0008` | `proc_protect_handle`            | `proc.c`                 |       |
| `0xBDAA0009` | `proc_scan_handle`               | `scan.c`                 |       |
| `0xBDAA000A` | `proc_info_handle`               | `proc.c`                 |       |
| `0xBDAA000B` | `proc_alloc_handle`              | `proc.c`                 |       |
| `0xBDAA000C` | `proc_free_handle`               | `proc.c`                 |       |
| `0xBDAA000D` | `proc_unknown_d_handle`          | `proc.c` (raw literal)   |       |
| `0xBDAA000E` | `proc_alloc_hinted_handle`       | `proc.c`                 |       |
| `0xBDAA0010` | `proc_elf_rpc_handle`            | `proc.c`                 |       |
| `0xBDAA0020` | `proc_disasm_region_handle`      | `proc.c`                 |       |
| `0xBDAA0021` | `proc_extract_code_xrefs_handle` | `proc.c`                 |       |
| `0xBDAA0022` | `proc_find_xrefs_to_handle`      | `proc.c`                 |       |
| `0xBDAA0023` | `proc_read_stack_handle`         | `proc.c`                 |       |
| `0xBDAA0024` | `proc_assemble_handle`           | `assemble.c` (raw literal) |     |
| `0xBDAA0501` | `proc_scan_aob_handle`           | `scan.c`                 |       |
| `0xBDAA0502` | `proc_scan_aob_multi_handle`     | `scan.c`                 |       |
| `0xBDAACCFF` | `proc_auth_handle`               | `auth.c`                 |       |
| `0xBDAACC01` | `proc_scan_start_handle`         | `scan.c`                 | bit 1 |
| `0xBDAACC02` | `proc_scan_count_handle`         | `scan.c`                 | bit 1 |
| `0xBDAACC03` | `proc_scan_get_handle`           | `scan.c`                 | bit 1 |
| `0xBDBB0001` | `debug_attach_handle`            | `debug.c`                |       |
| `0xBDBB0002` | `debug_detach_handle`            | `debug.c`                |       |
| `0xBDBB0003` | `debug_set_breakpoint_handle`    | `debug.c`                |       |
| `0xBDBB0004` | `debug_set_watchpoint_handle`    | `debug.c`                |       |
| `0xBDBB0005` | `debug_get_thread_list_handle`   | `debug.c`                |       |
| `0xBDBB0006` | `debug_suspend_thread_handle`    | `debug.c`                |       |
| `0xBDBB0007` | `debug_resume_thread_handle`     | `debug.c`                |       |
| `0xBDBB0008` | `debug_getregs_handle`           | `debug.c`                |       |
| `0xBDBB0009` | `debug_setregs_handle`           | `debug.c`                |       |
| `0xBDBB000A` | `debug_getfpregs_handle`         | `debug.c`                |       |
| `0xBDBB000B` | `debug_setfpregs_handle`         | `debug.c`                |       |
| `0xBDBB000C` | `debug_getdbregs_handle`         | `debug.c`                |       |
| `0xBDBB000D` | `debug_setdbregs_handle`         | `debug.c`                |       |
| `0xBDBB0010` | `debug_continue_handle`          | `debug.c`                |       |
| `0xBDBB0011` | `debug_thread_info_handle`       | `debug.c`                |       |
| `0xBDBB0012` | `debug_step_handle`              | `debug.c`                |       |
| `0xBDBB0013` | `debug_step_thread_handle`       | `debug.c`                |       |
| `0xBDBB0500` | `debug_process_stop_handle`      | `debug.c`                |       |
| `0xBDCC0001` | `kern_base_handle`               | `kern.c`                 |       |
| `0xBDCC0002` | `kern_read_handle`               | `kern.c`                 |       |
| `0xBDCC0003` | `kern_write_handle`              | `kern.c`                 |       |
| `0xBDDD0001` | `console_reboot_handle`          | `console.c`              |       |
| `0xBDDD0002` | `console_end_handle`             | `console.c`              |       |
| `0xBDDD0003` | `console_print_handle`           | `console.c`              |       |
| `0xBDDD0004` | `console_notify_handle`          | `console.c`              |       |
| `0xBDDD0005` | `console_info_handle`            | `console.c`              |       |
| `0xBDDD0006` | `console_foreground_app_handle`  | `console.c` (raw literal) |      |

---

## 5. Request packet appendix

All structs are `__attribute__((packed))`; most are defined in
`common/include/protocol.h` (a few are local to their handler's `.c`).

| Struct                                  | Size | Fields                                                                    |
|-----------------------------------------|------|---------------------------------------------------------------------------|
| `cmd_packet` (wire portion)             | 12   | `u32 magic; u32 cmd; u32 datalen;`                                        |
| `cmd_proc_read_packet`                  | 16   | `u32 pid; u64 address; u32 length;`                                       |
| `cmd_proc_write_packet`                 | 16   | `u32 pid; u64 address; u32 length;`                                       |
| `cmd_proc_maps_packet`                  | 4    | `u32 pid;`                                                                |
| `cmd_proc_install_packet`               | 4    | `u32 pid;`                                                                |
| `cmd_proc_unknown_d_packet`             | 4    | `u32 pid;`                                                                |
| `cmd_proc_call_packet`                  | 68   | `u32 pid; u64 rpcstub,rpc_rip,rpc_rdi,rpc_rsi,rpc_rdx,rpc_rcx,rpc_r8,rpc_r9;` |
| `cmd_proc_elf_packet`                   | 8    | `u32 pid; u32 length;`                                                    |
| `cmd_proc_elf_rpc_packet`               | 8    | `u32 pid; u32 length;`                                                    |
| `cmd_proc_protect_packet` (in `proc.c`) | 20   | `u32 pid; u64 address; u32 length; u32 prot;`                            |
| `cmd_proc_scan_packet`                  | 10   | `u32 pid; u8 valueType, compareType; u32 lenData;`                        |
| `cmd_proc_scan_start_packet`            | 23   | `u32 pid; u64 address; u32 length; u8 valueType, compareType, alignment; u32 lenData;` |
| `cmd_proc_scan_count_packet`            | 18   | `u32 pid; u64 base_address; u8 valueType, compareType; u32 lenData;`      |
| `cmd_proc_scan_aob_packet`              | 22   | `u32 pid; u64 address; u32 length; u8 max_matches, stop_flag; u32 pattern_length;` |
| `cmd_proc_scan_aob_multi_packet`        | 21   | `u32 pid; u64 address; u32 length; u8 stop_flag; u32 patterns_length;`    |
| `cmd_proc_auth_packet`                  | 8    | `u32 magic (0xBB40E64D); u32 flags;`                                      |
| `cmd_proc_scan_get_packet`              | 8    | `u32 pid; u32 count;`                                                     |
| `cmd_proc_info_packet`                  | 4    | `u32 pid;`                                                                |
| `cmd_proc_alloc_packet`                 | 8    | `u32 pid; u32 length;`                                                    |
| `cmd_proc_alloc_hinted_packet`          | 16   | `u32 pid; u64 hint; u32 length;`                                          |
| `cmd_proc_free_packet`                  | 16   | `u32 pid; u64 address; u32 length;`                                       |
| `cmd_proc_disasm_packet`                | 20   | `u32 pid; u64 address; u32 length; u32 max_entries;`                      |
| `cmd_proc_xrefs_to_packet`              | 24   | `u32 pid; u64 scan_address; u32 scan_length; u64 target_address;`         |
| `cmd_proc_read_stack_packet`            | 24   | `u32 pid; u64 rbp; u64 rsp; u32 depth;`                                   |
| `cmd_proc_assemble_packet`              | 12   | `u64 base_addr; u32 ks_opt_syntax;` (asm text follows in the same body)   |
| `cmd_debug_attach_packet`               | 4    | `u32 pid;`                                                                |
| `cmd_debug_breakpt_packet`              | 16   | `u32 index, enabled; u64 address;`                                        |
| `cmd_debug_watchpt_packet`              | 24   | `u32 index, enabled, length, breaktype; u64 address;`                     |
| `cmd_debug_stopthr_packet`              | 4    | `u32 lwpid;`                                                              |
| `cmd_debug_setregs_packet`              | 8    | `u32 lwpid; u32 length;`                                                  |
| `cmd_debug_stopgo_packet`               | 4    | `u32 stop;` (first byte = action 0/1/2)                                   |
| `cmd_debug_thrinfo_packet`              | 4    | `u32 lwpid;`                                                              |
| `cmd_kern_read_packet`                  | 12   | `u64 address; u32 length;`                                                |
| `cmd_kern_write_packet`                 | 12   | `u64 address; u32 length;`                                                |
| `cmd_console_print_packet` (in `console.c`)  | 4 | `u32 length;`                                                          |
| `cmd_console_notify_packet` (in `console.c`) | 8 | `u32 messageType; u32 length;`                                         |

## 6. Response packet appendix

| Struct                                  | Size | Fields                                                                    |
|-----------------------------------------|------|---------------------------------------------------------------------------|
| `proc_list_entry` (streamed)            | 36   | `char name[32]; int32_t pid;`                                            |
| `proc_vm_map_entry` (streamed)          | 58   | `char name[32]; u64 start; u64 end; u64 offset; u16 prot;`               |
| `cmd_proc_call_response`                | 12   | `u32 pid; u64 rpc_rax;`                                                   |
| `cmd_proc_elf_rpc_response`             | 8    | `u64 entry;`                                                             |
| `cmd_proc_alloc_response`               | 8    | `u64 address;`                                                           |
| `cmd_proc_info_response`                | 188  | `u32 pid; char name[40], path[64], titleid[16], contentid[64];`          |
| `cmd_proc_assemble_ok`                  | 8    | `u32 byte_len; u32 insn_count;` (then `byte_len` machine bytes)           |
| `cmd_proc_assemble_err`                 | 8    | `u32 ks_errno; u32 msg_len;` (then `msg_len` chars of `ks_strerror`)     |
| `cmd_console_foreground_app_response`   | 140  | `u32 pid; char titleid[16], contentid[64], name[40], app_ver[16];`       |
| `dbg_thrinfo_response`                  | 40   | `u32 lwpid, priority; char tdname[32];`                                  |
| `disasm_instr_entry` (streamed)         | 32   | see 2.2                                                                   |
| register blobs (raw, not structs)       | 176 / 832 / 128 | GP regs / FPU+YMM / debug regs                                |

(`CMD_PROC_INTALL` returns a bare `u64 rpcstub`, which is always `0` in this build.)

---

## 7. Enum reference

The `valueType` / `compareType` / DR7 enums are **not** defined in
`protocol.h`. The server uses bare integers (`scan_compare.c`); the symbolic
names live in the client (`PS4DBG.cs`). The values below are the wire integers.

### 7.1 Scan value type (server sizes from `scan_compare.c` `proc_scan_getSizeOfValueType`)

| Value | Name               | Size |
|-------|--------------------|------|
| 0     | `valTypeUInt8`     | 1    |
| 1     | `valTypeInt8`      | 1    |
| 2     | `valTypeUInt16`    | 2    |
| 3     | `valTypeInt16`     | 2    |
| 4     | `valTypeUInt32`    | 4    |
| 5     | `valTypeInt32`     | 4    |
| 6     | `valTypeUInt64`    | 8    |
| 7     | `valTypeInt64`     | 8    |
| 8     | `valTypeFloat`     | 4    |
| 9     | `valTypeDouble`    | 8    |
| 10    | `valTypeArrBytes`  | from `lenData` (mask-driven) |
| 11    | `valTypeString`    | from `lenData` |

(The client additionally defines `valTypePointer = 12` and `valTypeNone = 13`
sentinels; the server treats any value `> 9` as size 0.)

### 7.2 Scan compare type (`scan_compare.c` `proc_scan_compareValues`, cmpType 0-12)

| Value | Name                               |
|-------|------------------------------------|
| 0     | `ExactValue`                       |
| 1     | `FuzzyValue`                       |
| 2     | `BiggerThan`                       |
| 3     | `SmallerThan`                      |
| 4     | `ValueBetween`                     |
| 5     | `IncreasedValue`                   |
| 6     | `IncreasedValueBy`                 |
| 7     | `DecreasedValue`                   |
| 8     | `DecreasedValueBy`                 |
| 9     | `ChangedValue`                     |
| 10    | `UnchangedValue`                   |
| 11    | `UnknownInitialValue`              |
| 12    | `UnknownInitialLowValue`           |

(The client additionally defines `PointerValue = 13` and `None = 14`.)

### 7.3 DR7 encoding for hardware watchpoints (`PS4DBG.cs` `WATCHPT_*`; server applies raw shifts in `debug.c`)

`breaktype` in `cmd_debug_watchpt_packet`:

| Value | Name              |
|-------|-------------------|
| 0     | `DBREG_DR7_EXEC`  |
| 1     | `DBREG_DR7_WRONLY`|
| 3     | `DBREG_DR7_RDWR`  |

`length` (reused as the DR7 length field):

| Value | Bytes |
|-------|-------|
| 0     | 1     |
| 1     | 2     |
| 2     | 8     |
| 3     | 4     |

---

## 8. Response framing patterns

Reply shapes used across handlers:

1. **Fixed status:** a single `uint32_t` status (usually `CMD_SUCCESS`). Used by
   protect, breakpoint/watchpoint management, thread suspend/resume, continue,
   step, etc.
2. **Status + fixed struct:** status then a `struct cmd_*_response` of known
   size. Used by info, call, alloc, alloc-hinted, elf-rpc, thread-info,
   foreground-app.
3. **Status + count + array:** status, `uint32_t count`, then `count` elements.
   Used by process list, VM maps, and the debug thread list.
4. **Streamed with sentinel:** status, then a sequence of records terminated by a
   sentinel. `CMD_PROC_DISASM_REGION` uses a 32-byte all-`0xFF` record;
   `CMD_PROC_EXTRACT_CODE_XREFS` / `CMD_PROC_FIND_XREFS_TO`, the legacy
   `CMD_PROC_SCAN`, and `CMD_PROC_SCAN_START` / `_COUNT` / `_GET` stream
   `uint64_t`s (or `uint64_t`-length-prefixed blocks) terminated by
   `0xFFFFFFFFFFFFFFFF`. (The two AOB scans instead return a fixed count of
   `uint64_t` results with no sentinel.)
5. **Ack + data phase + final status (two status words):** the handler sends
   `CMD_SUCCESS`, reads the trailing payload, then sends a final status. Used by
   `CMD_PROC_WRITE`, `CMD_PROC_ELF`, `CMD_PROC_ELF_RPC`, `CMD_KERN_WRITE`, and
   `CMD_DEBUG_SETREGS` / `SETFPREGS` / `SETDBREGS`. Clients must consume both
   status words.
6. **Multi-phase (scans and the auth handshake):** these interleave several
   `CMD_SUCCESS` acks with read/stream phases. The number and placement of status
   words varies per command (two to three for the legacy and AOB scans and
   `SCAN_START` / `_COUNT`; a single status for `SCAN_GET`); see each command's
   entry in 2.2 and the auth handshake in 1.8 for the exact sequence.

---

## 9. Known source-code oddities

Documented so a developer does not mistake them for bugs:

- `CMD_PROC_INTALL` is misspelled (missing `S`), preserved on the wire; the
  handler is also a stub that returns `rpcstub = 0` without injecting anything.
- Three opcodes have no `CMD_*` macro and are bare hex literals in their switch:
  `0xBDAA000D` (first-map probe), `0xBDAA0024` (assemble), `0xBDDD0006`
  (foreground app).
- Status words are bit-swapped on the wire by `net_send_int32` (1.6); payloads
  are not. The server-side `CMD_SUCCESS` macro is `0x40000000`, which is
  `0x80000000` on the wire.
- The 1184-byte debug interrupt packet (1.9) has no named struct or size macro in
  the server; it is built by hand at fixed offsets. `savefpu` lands at offset
  224, which is not 64-byte aligned (intentional).
- `g_proc_auth_state` is a single global (not per-connection) and is not reset on
  disconnect.
- The disassembler amalgamation (`third_party/zydis/Zydis.c`) is built with a
  Makefile override (`-O3 -DNDEBUG -w -DZYAN_NO_LIBC`) distinct from the rest of
  the payload.

---

*This document reflects the `ps5debug-NG v1.2.6` payload
(`common/include/version.h`). Line numbers cite the sources as of this writing
and may drift; the source under `common/include/protocol.h`,
`debugger/source/`, and `common/source/` is the authoritative reference.*
