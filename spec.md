# `tmux2` Technical Specification
**An mTLS-Native, Detachable, Persistent Multi-Session Terminal Multiplexer**

- **Status:** Complete Specification (v1.0 — C++23 Cross-Platform Architecture)
- **Target Architecture:** Direct Client-to-Daemon over TLS 1.3 (mTLS) via OpenSSL 3.5+ QUIC and TCP, plus local `AF_UNIX` auto-spawn mode.
- **Core Goal:** Eliminate the legacy `ssh -> remote PTY -> tmux attach` workflow by replacing local-only `AF_UNIX` + `SCM_RIGHTS` IPC with a network-native, mTLS-authenticated multiplexing protocol supporting multiple sessions, windows, and panes across Linux and macOS.

---

## 1. Problem Statement & Motivation

### 1.1 Why Traditional `tmux` Requires SSH First
Standard `tmux` uses a local client-server model over a Unix domain socket (`/tmp/tmux-<uid>/default`) via OpenBSD's `imsg` framing protocol. During attachment (`MSG_IDENTIFY_STDIN`), the `tmux` client passes its local terminal file descriptor (`STDIN_FILENO`) directly into the `tmux` server process using `sendmsg(2)` ancillary data (`SCM_RIGHTS`). The `tmux` server then performs `ioctl(TIOCGWINSZ)` and writes `terminfo` escape sequences directly to that kernel file descriptor.

Because `SCM_RIGHTS` file-descriptor passing cannot cross a network socket:
1. **Mandatory SSH Prerequisite:** Users must first authenticate via `ssh`, allocate a remote pseudo-terminal (PTY) under `sshd`, and then invoke `tmux new -A -s <session>`.
2. **Double-PTY Overhead & Escape Delays:** Every keystroke and screen update traverses two PTY layers (`sshd` PTY + `tmux` pane PTY), introducing latency, `$TERM` capability mismatches, `escape-time` ambiguity on `ESC`, and broken OSC 52 / image passthrough.
3. **Fragile Roaming:** When a client IP changes or a laptop sleeps, the outer SSH TCP connection stalls or dies, forcing manual reconnection and re-attachment.

### 1.2 Landscape & Prior Art Gap Analysis

| System | Transport & Auth | Multi-Session / Windows / Panes | Direct Connect (No SSH Needed) | Works in Any Terminal Emulator | Roaming / Auto-Resume |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`tmux` + `ssh`** | TCP (SSH) + Unix Socket | Yes (Sessions, Windows, Panes) | No (Requires `sshd`) | Yes | No (Drops on IP change) |
| **Mosh** | UDP (SSP / AES-OCB) | No (Single shell; needs `tmux` inside) | No (Requires `ssh` to bootstrap key) | Yes (Loses native scrollback) | Yes |
| **Eternal Terminal (`et`)** | TCP (Custom framing) | No (Single pipe; needs `tmux` inside) | No (Requires `ssh` to bootstrap token) | Yes | Yes |
| **WezTerm Mux Server** | TCP + TLS / mTLS | Yes (Workspaces, Tabs, Panes) | Yes (With manual/CA certs) | No (Requires WezTerm GUI client) | Yes |
| **Zellij / `shpool`** | Unix Socket (+ Web UI in Zellij) | Zellij: Yes / `shpool`: Sessions only | CLI requires `ssh` (Web uses HTTPS) | Yes | Via SSH reconnect |
| **`tmux2` (C++23 v1)** | **QUIC + TCP with TLS 1.3 mTLS & AF_UNIX** | **Yes (Full `tmux` hierarchy)** | **Yes (Direct mTLS to daemon)** | **Yes (Pure TUI/CLI client)** | **Yes (Connection migration + sequence resume)** |

---

## 2. System Architecture

`tmux2` is built as a single, unified C++23 binary (`tmux2`) that acts as both the client and the background daemon (`tmux2d` as a symlink or `tmux2 daemon` subcommand).

```
+-------------------------------------------------------------------------+
| Local Workstation (Any Terminal: Kitty, Ghostty, Alacritty, iTerm2...)  |
|                                                                         |
|  +-------------------------------------------------------------------+  |
|  | `tmux2` Client Mode (TUI)                                         |  |
|  |  - POSIX raw-mode TTY RAII wrapper (tcgetattr / tcsetattr)        |  |
|  |  - High-performance VT/SGR input parser (keys, mouse, paste)      |  |
|  |  - Configurable Prefix Engine (Ctrl-b) & Interactive Overlays     |  |
|  |  - Atomic Frame Renderer with DECSET 2026 synchronized output     |  |
|  |  - Outbound input ring buffer for zero-drop seamless reconnect    |  |
|  +-------------------------------------------------------------------+  |
+--------------------+-------------------------------+--------------------+
                     |                               |
                     | Local 0600 AF_UNIX            | TLS 1.3 Mutual TLS (mTLS)
                     | Socket (Auto-Spawn)           | Primary: OpenSSL QUIC (UDP/8443)
                     |                               | Fallback: OpenSSL TCP (TCP/8443)
                     v                               v
+-------------------------------------------------------------------------+
| Remote or Local Host (`tmux2 daemon` / `tmux2d`)                        |
|                                                                         |
|  +-------------------------------------------------------------------+  |
|  | Single-Threaded Lock-Free C++23 Reactor                           |  |
|  |  - Linux: epoll(7) / macOS: kqueue(2) + poll(2) TTY fallback       |  |
|  |  - Min-heap timer queue driving SSL_get_event_timeout             |  |
|  |  - Zero-allocation non-blocking socket dispatch                   |  |
|  +-------------------------------------------------------------------+  |
|                                     |                                   |
|  +-------------------------------------------------------------------+  |
|  | Per-User Session Engine (Rootless Mode A)                         |  |
|  |                                                                   |  |
|  |  +-- Session "work" ------------------+  +-- Session "deploy" -+  |  |
|  |  |  +-- Window 0: "editor" ---------+ |  |  +-- Window 0 ----+ |  |  |
|  |  |  | Pane 0 (PTY + VteScreen Grid) | |  |  | Pane 0 (PTY)   | |  |  |
|  |  |  | Pane 1 (PTY + VteScreen Grid) | |  |  +----------------+ |  |  |
|  |  |  +-------------------------------+ |  +---------------------+  |  |
|  |  |  +-- Window 1: "logs" -----------+ |                           |  |
|  |  +------------------------------------+                           |  |
|  +-------------------------------------------------------------------+  |
+-------------------------------------------------------------------------+
```

### 2.1 Object Hierarchy
`tmux2` preserves and refines the traditional 3-tier hierarchy:
1. **Session (`SessionId`, `name`):** Named persistent workspace (e.g., `dev`, `infra`, `ml-train`). Holds an ordered map of Windows, active window pointer, last window pointer, session environment variables, options, and a set of attached `ClientAttachment` handles.
2. **Window (`WindowId`, `index`, `name`):** Holds a split layout tree (horizontal split, vertical split, tiled, zoomed pane) and dimensions governed by sizing policy (`latest`, `smallest`, `largest`, or `manual`). In v1, `window-size = latest` dynamically sizes the window to the most recently active attached client.
3. **Pane (`PaneId`):**
   - Owns a server-side PTY master/slave pair (`openpty(3)` via `<pty.h>` on Linux and `<util.h>` on macOS) and child process group (`pid`).
   - Owns a server-side **`VteScreen` Virtual Terminal Emulator** powered by a C++23 RAII wrapper around `libvterm` (grid cells, styles, 24-bit Truecolor RGB, cursor tracking, alternate screen buffer, mouse reporting, and OSC 52 clipboard callbacks).
   - Owns a high-performance **`ScrollbackBuffer`** ring buffer utilizing `libzstd` chunked compression for memory-efficient multi-thousand-line storage (e.g., 50,000 lines).

### 2.2 Local Auto-Spawn Mode (`AF_UNIX`)
When executed locally without remote host flags (`tmux2`, `tmux2 new`, `tmux2 attach`), `tmux2` connects via a secure local Unix domain socket (`0600` permissions at `$XDG_RUNTIME_DIR/tmux2/default.sock` or `~/.local/state/tmux2/default.sock` / `/tmp/tmux2-<uid>/default.sock`). If the socket does not exist or fails connection, the client automatically daemonizes and spawns the background daemon using platform-native binary resolution:
- **macOS:** `_NSGetExecutablePath()`
- **Linux:** `/proc/self/exe`

The client polls the socket with a brief exponential backoff (up to 1.5s total), ensuring instantaneous, zero-configuration local startup matching `tmux` ergonomics.

### 2.3 Server-Side State vs. Client-Side Rendering
Unlike raw byte-stream forwarders, `tmux2` parses all child PTY output into its in-memory VTE grid on the server:
- **Instant Reattachment:** If a pane outputs 10 GB of logs while detached, reattaching sends only the current visible screen snapshot (~10–50 KB compressed) plus scrollback metadata in `< 20 ms`.
- **Terminal Independence:** The wire protocol transmits structured **Cell Grid Snapshots & Row Diffs** rather than raw `terminfo` escape strings. Two clients attached to the same session with different terminal sizes or color palettes render cleanly and independently.
- **In-Band Session Switching:** Switching between sessions (`Prefix + s` or `:switch-client -t <name>`) occurs over the existing mTLS or local socket connection by updating subscription state and receiving a new `ScreenSnapshot`.
- **Multi-Client Detach Semantics:** Running `attach -d` or connecting with the detach flag cleanly detaches all other clients attached to that session, notifying them via `MsgDetachNotice`.

---

## 3. Security, Authentication, and Privilege Model (mTLS)

### 3.1 TLS 1.3 Mutual Authentication
All network communication uses **TLS 1.3** (RFC 8446) with mandatory mutual certificate authentication (mTLS):
- **Cipher Suites:** `TLS_AES_256_GCM_SHA384`, `TLS_CHACHA20_POLY1305_SHA256`, `TLS_AES_128_GCM_SHA256`.
- **Key Types:** Ed25519 X.509v3 certificates (primary, fast, small footprint) or ECDSA P-256. RSA keys `< 3072 bits` are rejected.
- **ALPN Identifier:** `tmux2/1`.
- **0-RTT Policy:** TLS 1.3 0-RTT early data is disabled for interactive input commands to prevent replay attacks; resumption relies on TLS session tickets combined with application-level `client_seq` deduplication.

### 3.2 Certificate Verification & Identity Mapping
1. **Server Verification (by Client):**
   - The client validates the server certificate against the configured Root CA (`ca_cert` in profile).
   - Hostname/IP is matched against the server certificate's X.509 Subject Alternative Name (`SAN`).
2. **Client Verification (by Server):**
   - The server verifies the client certificate chain against `client_ca_cert`.
   - Identity is resolved via X.509 `SAN` URI (`tmux2://user/<username>`), SAN DNS/email, or Common Name (`CN`).
3. **Deferred Extensions (v2+):**
   - Trust-On-First-Use (TOFU) automatic pinning without CA bundles is explicitly deferred to v2+.
   - Fine-grained certificate-based Role-Based Access Control (RBAC) extensions (e.g. read-only spectator certs or session whitelists) are deferred to v2+. In v1, authorized client profiles carry standard user identities verified against local user access lists.

### 3.3 Daemon Deployment Modes

#### Mode A: Rootless Per-User Daemon (v1 Primary)
- Runs directly under the target user account without root privileges.
- Supports background execution via `--daemonize`.
- Built-in generators for service managers:
  - **Linux:** `systemd --user` unit (`tmux2 --print-systemd-unit` / `tmux2 daemon --print-systemd-unit` -> `~/.config/systemd/user/tmux2.service`).
  - **macOS:** `LaunchAgent` plist generator (`tmux2 --print-launchd-plist` / `tmux2 daemon --print-launchd-plist` -> `~/Library/LaunchAgents/com.tmux2.daemon.plist`).
- Default network listen port: UDP/TCP `8443`.
- Local socket created with `0600` permissions accessible only to the owner UID.

#### Mode B: Privilege-Separated Multi-User Root Daemon (Deferred to v2+)
- Multi-user privilege-separated root daemon (`tmux2d-listener` / `tmux2d-tls` / `tmux2d-user`) designed to replace system `sshd` is deferred to v2+. In v1, multi-user systems run independent rootless Mode A daemons per user on designated user ports or via user-space port forwarding.

### 3.4 Built-In PKI Tooling (`tmux2 pki`)
`tmux2` includes a complete OpenSSL 3.5+ Ed25519 X.509v3 PKI engine:
- `tmux2 pki init --host dev.example.com`: Generates a dedicated Root CA (`ca.key`, `ca.crt`), server certificate (`server.key`, `server.crt`), and initial client certificate (`client.key`, `client.crt`) with proper SAN entries.
- `tmux2 pki export-client <name> --server dev.example.com:8443 > dev.tmux2-profile`: Bundles the CA cert, client cert, private key, and connection endpoint into a single TOML profile file saved with strict `0600` permissions.
- `tmux2 profile import dev.tmux2-profile`: Installs the profile into `~/.config/tmux2/profiles/dev.toml` with `0600` permissions.

---

## 4. Transport & Wire Protocol Specification

### 4.1 Dual-Transport Abstraction (QUIC Primary, TCP Fallback)
`tmux2` uses a unified transport abstraction over OpenSSL 3.5+:

1. **QUIC Transport (Default, UDP/8443):**
   - Utilizes OpenSSL 3.5+ native QUIC API (`OSSL_QUIC_client_method()`).
   - Stream 0: Bidirectional Control & Interactive Input.
   - Stream 2: Server-to-Client High-Priority Render Stream (`ScreenSnapshot`, `CellRowDiff`).
   - Dynamic Streams: On-demand bulk transfers (scrollback slice paging, OSC 52 sync) without head-of-line blocking.
   - Connection Migration: Survives network switches (Wi-Fi to LTE) without session drops.

2. **TCP + TLS 1.3 Transport (Fallback, TCP/8443):**
   - Automatically engaged when UDP is blocked or throttled, coordinated via a Happy Eyeballs parallel connection race.
   - Employs a framing header over the TLS 1.3 stream.

3. **Local AF_UNIX Transport:**
   - Raw binary frame transport over local non-blocking Unix domain socket with `0600` file mode.

### 4.2 Zero-Codegen Little-Endian Binary Wire Codec
The wire protocol uses a zero-copy, zero-codegen binary format using standard C++23 `<bit>`, `std::span`, and `std::expected`. All multibyte integers are strictly Little-Endian.

#### Frame Header (8 bytes)
```
+-------------------+-------------------+-------------------+-----------------------+
|  Magic (u16)      |   Type (u8)       |   Flags (u8)      |   Payload Len (u32)   |
|   0x5432 ("T2")   |   MsgType         |   Bitmask         |   0 .. 64 MiB         |
+-------------------+-------------------+-------------------+-----------------------+
```
- `Magic`: `0x5432` (`'2'` in low byte, `'T'` in high byte).
- `Flags`: `0x01 = Compressed (zstd)`, `0x02 = Urgent`.

#### Core Message Types
| Value | Type | Description |
| :--- | :--- | :--- |
| `0x01` | `ClientHello` | Handshake: client protocol version, instance ID, terminal capabilities, resume token. |
| `0x02` | `ServerHello` | Handshake response: assigned client ID, session list, resume token. |
| `0x03` | `ListSessionsReq` | Query running sessions. |
| `0x04` | `ListSessionsResp` | Session metadata array (`id`, `name`, `windows`, `created_at`). |
| `0x05` | `NewSessionReq` | Create session (`name`, `window_name`, `cols`, `rows`, `command`). |
| `0x06` | `NewSessionResp` | Session creation result (`session_id`). |
| `0x07` | `AttachSessionReq` | Attach client to session (`session_id`/`name`, `read_only`, `detach_others`). |
| `0x08` | `AttachSessionResp` | Attach acknowledgment (`layout_tree`, active pane). |
| `0x09` | `DetachReq` | Clean client detach. |
| `0x0A` | `DetachNotice` | Server notice to client (e.g. kicked or session terminated). |
| `0x0B` | `SwitchSessionReq` | In-band session switch (`target_session_id`). |
| `0x0C` | `SwitchSessionResp`| Switch acknowledgment. |
| `0x0D` | `KillSessionReq` | Terminate session and its child PTY processes. |
| `0x0E` | `KillSessionResp` | Termination status. |
| `0x10` | `KeyEvent` | Client keystroke (`client_seq`, `pane_id`, `key_code`, `modifiers`, `raw_bytes`). |
| `0x11` | `MouseEvent` | Mouse click/drag/scroll (`client_seq`, `pane_id`, `button`, `kind`, `col`, `row`). |
| `0x12` | `PasteEvent` | Bracketed paste payload (`client_seq`, `pane_id`, `data`). |
| `0x13` | `ClientResize` | Window/pane resize notification (`cols`, `rows`, `pixel_w`, `pixel_h`). |
| `0x20` | `LayoutUpdate` | Layout tree update (`session_id`, `window_id`, `tree`, `active_pane_id`). |
| `0x21` | `ScreenSnapshot` | Full grid snapshot (`server_seq`, `pane_id`, `cols`, `rows`, `cursor`, `cells`). |
| `0x22` | `FrameDiff` | Incremental dirty row diffs (`server_seq`, `acked_client_seq`, `diffs`). |
| `0x23` | `ScrollbackSliceReq` | Copy Mode history request (`pane_id`, `start_line`, `line_count`). |
| `0x24` | `ScrollbackSliceResp`| Compressed lines slice (`pane_id`, `start_line`, `lines`). |
| `0x25` | `Passthrough` | Out-of-band events (OSC 52 clipboard payload, bell, window title). |
| `0x30` | `Ping` | Heartbeat keep-alive (`timestamp`). |
| `0x31` | `Pong` | Heartbeat response (`timestamp`). |

#### Cell Binary Serialization
Each terminal cell is serialized into an 8-byte fixed structure + optional extended styles:
- `codepoint`: `u32` (UTF-32 codepoint; `0` = empty space).
- `width`: `u8` (0 = combining char, 1 = normal, 2 = wide/CJK/emoji).
- `flags`: `u16` (bitmask: Bold, Italic, Underline, Blink, Reverse, Hidden, Strikethrough).
- `fg_type`, `fg_r`, `fg_g`, `fg_b`: Foreground color (Default, Indexed, or Truecolor RGB).
- `bg_type`, `bg_r`, `bg_g`, `bg_b`: Background color.

Runs of identical cells or snapshot grids are RLE encoded and compressed via `libzstd` before wire transmission.

---

## 5. Persistence, Roaming & Reconnection Mechanics

### 5.1 Decoupled Session Lifecycle
- **Immortality:** Sessions and child PTY processes continue executing indefinitely regardless of client connectivity.
- **Client Session State:** The daemon maintains a per-client record:
  - `client_instance_id` (128-bit UUID)
  - `resume_token` (256-bit cryptographic token valid for a configurable TTL, default 24h)
  - Ring buffer of recent `FrameDiff` messages and last `acked_client_seq`.

### 5.2 Fast Reconnect & Catch-Up Protocol
When a client experiences a network glitch, Wi-Fi migration, or laptop sleep:
1. **Client TUI Hold:** The client keeps the current screen intact and overlays a status indicator (`[Reconnecting... attempt 2]`). Outbound keystrokes are buffered in a local queue tagged with `client_seq`.
2. **Exponential Backoff:** Retries at 100ms, 250ms, 500ms, 1s, up to 5s max.
3. **Resume Handshake:** The client presents its `resume_token` and `last_server_seq` in `ClientHello`.
4. **Delta vs. Snapshot:**
   - If `last_server_seq` is within the server's ring buffer, the daemon transmits only the missed `FrameDiff`s.
   - If the client was offline too long, the daemon sends a fresh `ScreenSnapshot`.
5. **Input Deduplication:** The client resends any unacknowledged keystrokes (`client_seq > last_client_seq_acked`). The daemon drops any already processed `client_seq`s, preventing duplicated keystrokes.
6. **Multi-Client Sizing:** `window-size = latest` resizes the window and child PTYs (`TIOCSWINSZ`) to the dimensions of the client that most recently sent an input event.

---

## 6. User Experience, CLI & Configuration

### 6.1 Unified Binary CLI
`tmux2` provides an intuitive command-line interface mirroring standard `tmux` syntax while adding network targeting:

```bash
# Local session management (auto-spawns daemon if needed)
tmux2 new -s dev
tmux2 attach -t dev
tmux2 ls
tmux2 kill-session -t dev

# Remote session management via mTLS profiles
tmux2 -H dev.example.com new -s main
tmux2 attach dev:main
tmux2 -H dev ls

# Detach other clients on attach
tmux2 attach -d -t dev

# Run detached command
tmux2 new-session -d -s build "make -j8"

# Daemon management
tmux2 daemon --daemonize
tmux2 daemon --print-systemd-unit  # or --print-launchd-plist

# Built-in PKI
tmux2 pki init --host dev.example.com
tmux2 pki export-client alice --server dev.example.com:8443 > alice.tmux2-profile
tmux2 profile import alice.tmux2-profile
```

### 6.2 Daily-Driver Interactive TUI
- **Prefix Engine:** Configurable default `Ctrl-b` prefix.
- **Session Chooser Overlay (`Prefix + s`):** Interactive modal overlay listing all sessions and windows with live preview. Fast selection sends `MsgSwitchSessionReq` without dropping the connection.
- **Copy Mode (`Prefix + [`):**
  - Full vi navigation keys (`h, j, k, l`, `Ctrl-u`, `Ctrl-d`, `0`, `$`).
  - Search forward (`/`) and backward (`?`).
  - Visual selection (`v`, `V`).
  - History paging fetching compressed slices from the daemon's `ScrollbackBuffer`.
  - Yanking (`y`) emits an `OSC 52` sequence to update the local workstation clipboard.
- **Synchronized Rendering (`DECSET 2026`):** All screen paints are enclosed in atomic frame delimiters (`\033[?2026h ... \033[?2026l`), eliminating flicker and partial tearing in modern terminals.
- **Full Mouse Support:** Direct pane click-to-focus, split border drag-to-resize, and mouse scroll-wheel passing.
- **Native OSC 52 Passthrough:** Pane-generated clipboard copies are intercepted by `VteScreen` callbacks and forwarded cleanly to the client terminal.

### 6.3 Configuration (`config.toml` & `server.toml`)
Configurations use TOML parsed via `tomlplusplus`:

#### Client Configuration (`~/.config/tmux2/config.toml`)
```toml
default_host = "dev"
prefix_key = "C-b"
escape_time_ms = 0
mouse = true
clipboard = "osc52"

[hosts.dev]
address = "dev.example.com:8443"
transport = "auto"          # "quic" | "tcp" | "auto"
ca_cert = "~/.config/tmux2/pki/dev/ca.crt"
client_cert = "~/.config/tmux2/pki/dev/client.crt"
client_key = "~/.config/tmux2/pki/dev/client.key"
auto_reconnect = true
```

#### Server Configuration (`~/.config/tmux2/server.toml`)
```toml
listen_addrs = ["0.0.0.0:8443", "[::]:8443"]
local_socket = "/run/user/1000/tmux2/default.sock"
scrollback_lines = 50000
resume_token_ttl = "24h"
window_size_policy = "latest"

[tls]
server_cert = "~/.config/tmux2/pki/server.crt"
server_key = "~/.config/tmux2/pki/server.key"
client_ca_cert = "~/.config/tmux2/pki/ca.crt"
require_client_cert = true
min_version = "1.3"
```

---

## 7. C++23 Cross-Platform Architecture & Implementation Stack

### 7.1 Cross-Platform Core Technology Stack
`tmux2` is implemented in **portable modern C++23** targeting both Linux and macOS:

- **Language Standard:** `-std=c++23` using portable, header-based standard library features (`std::span`, `std::expected`, `std::optional`, `std::string_view`, `<bit>`, `<format>`, `<chrono>`). C++20/23 modules are explicitly avoided to guarantee seamless compilation across:
  - Apple Clang (Xcode 16.0+) on macOS (Apple Silicon & Intel)
  - GCC 14.1+ on Linux
  - LLVM Clang 18.1+ on Linux and macOS
- **Build System:** CMake 3.25+ supporting both system package managers (`pkg-config`) and CMake `FetchContent`.
- **Binary Output:** Unified executable `tmux2` with symlink support (`tmux2d`).
- **Cryptography & Network TLS:**
  - **OpenSSL 3.5+**: Powers the unified cryptography stack, providing native TLS 1.3 mTLS, `OSSL_QUIC_*` client/server connections, and Ed25519 X.509v3 certificate generation and validation.
- **Event Loop & Reactor:**
  - High-performance, single-threaded lock-free Reactor.
  - **Linux:** `epoll(7)` with `EPOLLIN`, `EPOLLOUT`, `EPOLLRDHUP`, and `EPOLLET` (edge-triggered) support.
  - **macOS (Darwin):** `kqueue(2)` with `EVFILT_READ` and `EVFILT_WRITE`. Uses `poll(2)` fallback specifically for `/dev/tty` master/slave descriptors where Darwin kernel kqueue does not reliably report EOF/hangup conditions.
  - **Timer Engine:** Min-heap timer queue coordinating timeouts and driving `SSL_get_event_timeout()` for OpenSSL QUIC event scheduling.
- **Terminal Emulation (VTE):**
  - Vendored/system `libvterm` wrapped in a C++23 RAII abstraction (`tmux2::vte::VteScreen`).
  - Supports full 24-bit Truecolor RGB, indexed colors, alternate screen buffer, UTF-8 streaming, wide chars (CJK, emojis), cursor visibility/shapes, and terminal bell/title/selection callbacks.
- **Scrollback & Compression:**
  - Custom C++23 ring buffer (`tmux2::vte::ScrollbackBuffer`) using `libzstd` for block compression of inactive lines, achieving an 85%+ memory footprint reduction for large scrollbacks.
- **PTY Subsystem:**
  - Master/slave pseudo-terminal allocation (`openpty(3)`) via `<pty.h>` (Linux) and `<util.h>` (macOS).
  - Terminfo capability detection with fallback chain (`tmux-256color` -> `xterm-256color` -> `vt100`).
- **Wire Codec:**
  - Zero-codegen binary encoder/decoder (`tmux2::protocol::BinaryWriter` / `BinaryReader`).
- **Configuration & Testing:**
  - `tomlplusplus` (`toml++`) for configuration parsing.
  - `doctest` (2.4+) for comprehensive unit test execution.

### 7.2 Directory & Module Structure

```
tmux2/
├── CMakeLists.txt              # Cross-platform CMake 3.25+ build definition
├── spec.md                     # Technical specification (this document)
├── include/                    # Public API / shared headers
│   └── tmux2/
│       ├── config/
│       │   └── config.hpp
│       ├── pki/
│       │   └── pki.hpp
│       └── reactor/            # Single-threaded Reactor abstractions
│           ├── backend.hpp
│           ├── epoll_backend.hpp
│           ├── frame_coalescer.hpp
│           ├── kqueue_backend.hpp
│           ├── poll_backend.hpp
│           ├── reactor.hpp
│           ├── signal_handler.hpp
│           ├── timer_heap.hpp
│           └── types.hpp
├── src/
│   ├── app/                    # Daemon runner, client runner & CLI PKI commands
│   │   ├── client_runner.hpp/.cpp
│   │   ├── daemon_runner.hpp/.cpp
│   │   └── pki_commands.hpp/.cpp
│   ├── client/                 # TUI client, keybindings, copy mode, renderer
│   │   ├── client.hpp
│   │   ├── client_app.hpp/.cpp
│   │   ├── cli_parser.hpp/.cpp
│   │   ├── copy_mode.hpp/.cpp
│   │   ├── input_parser.hpp/.cpp
│   │   ├── keybindings.hpp/.cpp
│   │   ├── reconnect_buffer.hpp/.cpp
│   │   ├── renderer.hpp/.cpp
│   │   ├── session_chooser.hpp/.cpp
│   │   └── tty_raw.hpp/.cpp
│   ├── config/                 # toml++ configuration parsers
│   │   ├── config.hpp
│   │   ├── config_parser.hpp/.cpp
│   │   └── config_types.hpp
│   ├── pki/                    # OpenSSL 3.5+ Ed25519 X.509v3 PKI engine
│   │   ├── cert_generator.hpp/.cpp
│   │   ├── crypto_utils.hpp/.cpp
│   │   ├── identity_validator.hpp/.cpp
│   │   ├── pki.hpp
│   │   ├── pki_manager.hpp/.cpp
│   │   └── raii.hpp
│   ├── platform/               # OS daemonization, systemd & launchd generators
│   │   └── platform.hpp/.cpp
│   ├── protocol/               # Zero-codegen binary wire codec & cell models
│   │   ├── cell.hpp/.cpp
│   │   ├── codec.hpp
│   │   ├── compression.hpp/.cpp
│   │   ├── frame.hpp
│   │   ├── messages.hpp/.cpp
│   │   └── protocol.hpp
│   ├── pty/                    # Cross-platform PTY management & terminfo
│   │   ├── pty.hpp
│   │   ├── pty_process.hpp/.cpp
│   │   └── terminfo.hpp/.cpp
│   ├── reactor/                # Reactor implementation
│   │   └── reactor.cpp
│   ├── server/                 # Server daemon engine, sessions, windows, layouts
│   │   ├── client_state.hpp/.cpp
│   │   ├── command_dsl.hpp/.cpp
│   │   ├── layout.hpp/.cpp
│   │   ├── pane.hpp/.cpp
│   │   ├── server_engine.hpp/.cpp
│   │   ├── session.hpp/.cpp
│   │   ├── session_manager.hpp/.cpp
│   │   └── window.hpp/.cpp
│   ├── transport/              # Transport abstractions (OpenSSL QUIC / TCP / AF_UNIX)
│   │   ├── happy_eyeballs.hpp/.cpp
│   │   ├── quic_transport.hpp/.cpp
│   │   ├── tls_tcp_transport.hpp/.cpp
│   │   ├── tls_utils.hpp/.cpp
│   │   ├── transport.hpp
│   │   └── unix_transport.hpp/.cpp
│   ├── vte/                    # libvterm C++23 wrapper & compressed scrollback
│   │   ├── dirty_tracker.hpp/.cpp
│   │   ├── scrollback_buffer.hpp/.cpp
│   │   ├── vte.hpp
│   │   └── vte_wrapper.hpp/.cpp
│   └── main.cpp                # Unified entrypoint (subcommands: client, daemon, pki)
└── tests/                      # Doctest unit & end-to-end test suites
    ├── test_client.cpp
    ├── test_e2e_cli.cpp
    ├── test_e2e_stack.cpp
    ├── test_pki_config.cpp
    ├── test_protocol.cpp
    ├── test_pty_vte.cpp
    ├── test_reactor.cpp
    ├── test_server.cpp
    └── test_transport.cpp
```

### 7.3 Phased Roadmap

1. **v1.0 (Core Cross-Platform Release — Current Target):**
   - Single-threaded Reactor (`epoll` / `kqueue` + Darwin TTY `poll` fallback).
   - Zero-codegen little-endian wire codec + `libzstd` RLE snapshots/diffs.
   - Cross-platform PTY subsystem (`openpty`, `TIOCSWINSZ`, terminfo fallbacks).
   - `libvterm` C++23 RAII VTE engine, Truecolor RGB, alternate screen, mouse, OSC 52.
   - Chunked zstd-compressed scrollback ring buffer.
   - OpenSSL 3.5+ Ed25519 X.509v3 PKI (`tmux2 pki`) & `0600` profiles.
   - Dual transport: OpenSSL QUIC (UDP/8443) + TLS 1.3 TCP fallback + local `0600` `AF_UNIX` auto-spawn mode (`_NSGetExecutablePath` / `/proc/self/exe`).
   - Rootless Mode A daemon (`--daemonize`, `systemd --user` & macOS `LaunchAgent`).
   - Full TUI client: raw TTY mode, `Prefix+s` Session Chooser, `Prefix+[` Copy Mode, mouse hit-testing, `DECSET 2026` atomic rendering.
   - Seamless reconnect, `resume_token` sequence replay & `client_seq` deduplication.
   - Multi-client `window-size = latest` and `attach -d`.

2. **v2.0+ (Enterprise Multi-User & Extended Features — Deferred):**
   - Mode B privilege-separated multi-user root supervisor daemon (`tmux2d-listener` / `tmux2d-tls` / `tmux2d-user`) with PAM authentication and cgroup isolation.
   - Optional Trust-On-First-Use (TOFU) key pinning without Root CAs.
   - Certificate-based fine-grained RBAC extensions (read-only spectator certs, session whitelist restrictions).
   - Remote file transfer & buffer piping RPCs.
