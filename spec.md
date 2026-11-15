# `tmux2` Technical Specification
**An mTLS-Native, Detachable, Persistent Multi-Session Terminal Multiplexer**

- **Status:** Draft Specification (v1.0)
- **Target Architecture:** Direct Client-to-Daemon over TLS 1.3 (mTLS) via QUIC and TCP
- **Core Goal:** Eliminate the `ssh -> remote PTY -> tmux attach` workflow by replacing local-only `AF_UNIX` + `SCM_RIGHTS` IPC with a network-native, mTLS-authenticated multiplexing protocol supporting multiple sessions, windows, and panes.

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
| **`tmux2` (Proposed)** | **QUIC + TCP with TLS 1.3 mTLS** | **Yes (Full `tmux` hierarchy)** | **Yes (Direct mTLS to daemon)** | **Yes (Pure TUI/CLI client)** | **Yes (Connection migration + sequence resume)** |

---

## 2. System Architecture

`tmux2` splits the traditional terminal multiplexer into a **Network-Native State Server (`tmux2d`)** and a **Local Rendering & Input Client (`tmux2`)**.

```
+-------------------------------------------------------------------------+
| Local Workstation (Any Terminal: Kitty, Ghostty, Alacritty, iTerm2...)  |
|                                                                         |
|  +-------------------------------------------------------------------+  |
|  | `tmux2` Client Process                                            |  |
|  |  - Puts local TTY into raw mode                                   |  |
|  |  - Parses local key/mouse/paste/resize events                     |  |
|  |  - Renders server frame diffs & status UI to local stdout         |  |
|  |  - Client-side reconnect & outbound input ring buffer             |  |
|  +-------------------------------------------------------------------+  |
+------------------------------------+------------------------------------+
                                     |
                                     |  TLS 1.3 with Mutual TLS (mTLS)
                                     |  Primary: QUIC (UDP/8443)
                                     |  Fallback: Multiplexed TCP (TCP/8443)
                                     v
+-------------------------------------------------------------------------+
| Remote Server (`tmux2d`)                                                |
|                                                                         |
|  +-------------------------------------------------------------------+  |
|  | Privilege-Separated mTLS Gatekeeper (System Mode) or User Listener|  |
|  |  - Verifies client X.509 cert against trusted CA / pinned certs   |  |
|  |  - Maps cert identity (SAN/CN/Fingerprint) -> UNIX UID            |  |
|  +-------------------------------------------------------------------+  |
|                                     | (Local IPC or In-Process)         |
|  +-------------------------------------------------------------------+  |
|  | Per-User `tmux2d` Session Engine                                  |  |
|  |                                                                   |  |
|  |  +-- Session "work" ------------------+  +-- Session "deploy" -+  |  |
|  |  |  +-- Window 0: "editor" ---------+ |  |  +-- Window 0 ----+ |  |  |
|  |  |  | Pane 0 (PTY + VTE Grid/Hist)  | |  |  | Pane 0 (PTY)   | |  |  |
|  |  |  | Pane 1 (PTY + VTE Grid/Hist)  | |  |  +----------------+ |  |  |
|  |  |  +-------------------------------+ |  +---------------------+  |  |
|  |  |  +-- Window 1: "logs" -----------+ |                           |  |
|  |  +------------------------------------+                           |  |
|  +-------------------------------------------------------------------+  |
+-------------------------------------------------------------------------+
```

### 2.1 Object Hierarchy
`tmux2d` preserves `tmux`'s 3-tier hierarchy per user:
1. **Session (`SessionId`, `name`):** Named persistent workspace (e.g., `dev`, `infra`, `ml-train`). Holds an ordered map of Windows, active window pointer, last window pointer, session environment variables, options, and a set of currently attached `ClientAttachment` handles.
2. **Window (`WindowId`, `index`, `name`):** Holds a binary/n-ary split **Layout Tree** (horizontal split, vertical split, tiled, zoomed pane) and window dimensions determined by policy (`smallest`, `largest`, `latest`, or `manual`).
3. **Pane (`PaneId`):**
   - Owns a single server-side PTY master/slave pair (`openpty(3)`) and child process group (`pid`).
   - Owns a server-side **Virtual Terminal Emulator (VTE) State Machine** (screen cell matrix, alternate screen matrix, cursor state, character sets, terminal modes, and a configurable compressed scrollback ring buffer, e.g., 50,000 lines).

### 2.2 Server-Side State vs. Client-Side Rendering
Unlike raw byte-stream forwarders, `tmux2d` parses all child PTY output into its in-memory VTE grid on the server:
- **Instant Reattachment:** If a pane outputs 10 GB of logs while detached, reattaching sends only the current visible screen matrix (~10–50 KB compressed) plus scrollback metadata, completing in `< 20 ms`.
- **Terminal Independence:** The wire protocol transmits structured **Cell Grid Snapshots & Row/Run Diffs** rather than raw `terminfo` strings tied to a single `$TERM`. Two clients attached to the same session from different terminal emulators each render optimally for their local terminal capabilities.
- **Single-Connection Session Switching:** Switching between Session `A` and Session `B` (`Prefix + s` or `:switch-client -t B`) happens in-band over the existing mTLS connection by swapping the client's subscribed `SessionId` on the server and transmitting a fresh `ScreenSnapshot` for Session `B`.

---

## 3. Security, Authentication, and Privilege Model (mTLS)

### 3.1 TLS 1.3 Mutual Authentication Requirements
All network communication uses **TLS 1.3** (RFC 8446) with mandatory client certificate authentication (`CertificateRequest` in server handshake):
- **Cipher Suites:** `TLS_AES_256_GCM_SHA384`, `TLS_CHACHA20_POLY1305_SHA256`, `TLS_AES_128_GCM_SHA256`.
- **Key Types:** Ed25519 (recommended default) or ECDSA P-256 (`secp256r1`). RSA keys `< 3072 bits` are rejected.
- **ALPN Identifier:** `tmux2/1`.
- **0-RTT Policy:** TLS 1.3 0-RTT early data is **disabled** for control/input commands to prevent replay attacks; fast resumption uses TLS 1.3 session tickets (`PSK` with `psk_dhe_ke` forward secrecy) combined with `tmux2`'s application-layer sequence numbers.

### 3.2 Certificate Verification & Identity Mapping
1. **Server Verification (by Client):**
   - The client validates the server certificate against a configured Root CA (`ca_cert` in client config) and verifies the hostname/IP against the server certificate's X.509 Subject Alternative Name (`SAN`).
   - **Trust-On-First-Use (TOFU) / Pinning Option:** For single-user self-signed setups, the client can alternatively pin the server certificate's SPKI SHA-256 fingerprint.
2. **Client Verification (by Server):**
   - The server verifies the client's certificate chain against its configured `client_ca_cert` bundle and checks expiry plus optional CRL / pinned fingerprint revocation list.
   - **Principal Mapping:** The server extracts the authenticated identity from the client certificate in priority order:
     1. X.509 `SAN` URI matching `spiffe://<trust-domain>/user/<username>` or `tmux2://user/<username>`
     2. X.509 `SAN` rfc822Name / Common Name (`CN`)
     3. Explicit SHA-256 certificate fingerprint mapped in `/etc/tmux2/authorized_clients.toml` or `~/.config/tmux2/authorized_clients.toml`.
   - **Role-Based Access Control (Optional Cert Extensions):** Client certificates may carry custom extensions or SAN attributes restricting them to specific sessions (e.g., `role=readonly` for pair-programming spectators or `allowed_sessions=["pair-debug"]`).

### 3.3 Daemon Deployment Modes

#### Mode A: Rootless Per-User Daemon (Single-User / Dev VM)
- Runs directly under the target user account (e.g., via `systemd --user` with `loginctl enable-linger $USER`).
- Listens on an unprivileged port (default `8443`).
- Reads `~/.config/tmux2/server.toml` and `~/.config/tmux2/pki/`.
- Requires zero root privileges to install or run.

#### Mode B: Privilege-Separated Multi-User System Daemon (`/usr/sbin/tmux2d`)
Designed to replace `sshd` on multi-user hosts without running complex parsing logic as `root`:
1. **`tmux2d-listener` (Supervisor):** Binds port `8443` (or `443`), spawns unprivileged `tmux2d-tls` worker processes running in a `chroot` + `seccomp` sandbox as user `_tmux2_tls`.
2. **`tmux2d-tls` (Handshake & Proxy Worker):** Terminates the TLS 1.3 mTLS handshake, validates the client certificate, resolves the target UNIX username, and requests the supervisor to connect to that user's socket `/run/tmux2/<uid>/server.sock`.
3. **`tmux2d-user` (Per-UID Session Server):** If not already running for `<uid>`, the supervisor spawns `tmux2d-user` after dropping privileges (`setgroups`, `setgid`, `setuid`, PAM session initialization, cgroup placement). The authenticated stream is spliced to `tmux2d-user`.
4. **Isolation Guarantee:** Compromise of the TLS parser does not yield root or access to any user's sessions; each user's panes and sessions live in their own UID process tree.

### 3.4 Built-In PKI Tooling (`tmux2 pki`)
To make mTLS as ergonomic as `ssh-keygen` + `ssh-copy-id`:
- `tmux2 pki init --host dev.example.com`: Generates an Ed25519 Root CA (`ca.key`, `ca.crt`), server certificate (`server.key`, `server.crt`), and default client certificate (`client.key`, `client.crt`).
- `tmux2 pki export-client <name> --server dev.example.com:8443 > dev.tmux2-profile`: Outputs a single portable TOML profile containing the server address, CA cert, client cert, and client private key (encrypted with an optional passphrase or sealed for OS keychain).
- `tmux2 profile import dev.tmux2-profile`: Installs the profile on the client machine so `tmux2 -H dev` works immediately.

---

## 4. Transport & Wire Protocol Specification

### 4.1 Dual-Transport Abstraction (QUIC Primary, TCP Fallback)
`tmux2` defines a transport-agnostic stream abstraction over two TLS 1.3 carriers:

1. **QUIC Transport (Default, UDP/8443):**
   - Uses native QUIC bidirectional and unidirectional streams:
     - **Stream 0 (Bidirectional Control & Input Stream):** Session RPCs (list, create, attach, switch, detach, kill), keyboard/mouse input events, window resize events, heartbeats.
     - **Stream 2 (Server -> Client Render Stream):** High-priority screen snapshots and incremental frame diffs for the currently attached session/window.
     - **Dynamic Streams (Bidirectional):** On-demand bulk operations (e.g., fetching historical scrollback pages, searching scrollback on the server, OSC 52 clipboard sync, or remote file/buffer transfer) so bulk transfers never cause head-of-line blocking on typing or screen rendering.
   - **QUIC Connection Migration:** If the client's source IP/port changes (Wi-Fi to cellular), QUIC migrates the connection seamlessly without a new TLS handshake.

2. **TCP + TLS 1.3 Transport (Fallback, TCP/8443):**
   - When UDP is blocked, the client connects over TCP with TLS 1.3 and runs a lightweight multiplexed framing header over the TLS byte stream:
     ```
     +-------------------+-------------------+-------------------+-----------------------+
     | Stream ID (u16)   | Msg Type (u8)     | Flags (u8)        | Payload Length (u32)  |
     +-------------------+-------------------+-------------------+-----------------------+
     | Payload Bytes (Protobuf / MsgPack / bincode, optionally zstd-compressed)          |
     +-----------------------------------------------------------------------------------+
     ```
   - `Flags`: `0x01 = ZSTD_COMPRESSED`, `0x02 = FIN_STREAM`, `0x04 = PRIORITY_URGENT`.

### 4.2 Connection Lifecycle & Session Handshake

```
Client (`tmux2`)                                         Server (`tmux2d`)
   |                                                            |
   |--- 1. TLS 1.3 Handshake (ALPN: "tmux2/1", mTLS Certs) ---->|
   |                                                            | (Verify Client Cert
   |                                                            |  & Map to UID)
   |--- 2. ClientHello { protocol_ver, client_instance_id,      |
   |       term_caps, resume_token?, last_srv_seq, target } --->|
   |                                                            |
   |<-- 3. ServerHello { server_version, assigned_client_id,    |
   |       resume_token, last_client_seq_acked, session_list } -|
   |                                                            |
   |    [If target == Attach(session_name, create_if_missing)]  |
   |<-- 4. AttachAck { session_id, window_id, layout_tree } ----|
   |<-- 5. ScreenSnapshot { panes: [GridState], status_bar } ---|
   |                                                            |
   |=== 6. Interactive Loop (InputEvents <-> FrameDiffs) =======|
```

### 4.3 Core Protocol Messages

#### A. Control & Multi-Session Management Messages
Because the protocol separates *connection* from *session attachment*, a client can perform one-shot CLI operations or switch sessions interactively without reconnecting:

| Message | Direction | Description |
| :--- | :--- | :--- |
| `ClientHello` | C -> S | Negotiates protocol version, reports terminal capabilities (`truecolor`, `kitty_kbd`, `sgr_mouse`, `osc52`), and optionally requests fast resume via `resume_token`. |
| `ServerHello` | S -> C | Confirms authentication, returns `resume_token`, acks the last processed `client_seq`, and provides server metadata. |
| `ListSessionsReq` / `Resp` | C <-> S | Returns all sessions on the server: `[{ session_id, name, created_at, window_count, attached_clients, preview_summary }]`. |
| `NewSessionReq` / `Resp` | C <-> S | Creates a new session (`name`, initial `window_name`, `cwd`, `command`, `env`, `detached: bool`). |
| `AttachSessionReq` / `Resp` | C <-> S | Binds this client connection to `session_id` (or `session_name`), optionally with `read_only: bool` or `size_policy`. |
| `SwitchSessionReq` / `Resp` | C <-> S | Atomically detaches the client from its current session and attaches it to another session on the same server over the existing mTLS connection. |
| `DetachReq` / `DetachNotice` | C <-> S | Cleanly detaches the client (leaving the session running on the server) or notifies the client that another client ran `detach-client`. |
| `KillSessionReq` / `Resp` | C <-> S | Terminates a session and all its windows/panes. |
| `CommandRpcReq` / `Resp` | C <-> S | Executes any `tmux2` command DSL expression remotely (e.g., `split-window -h`, `new-window -n logs`, `send-keys -t build:0.1 "make test" Enter`, `capture-pane`). |

#### B. Interactive Input & Terminal Event Messages (Client -> Server)
Every input message carries a monotonically increasing `client_seq: u64`:
- `KeyEvent { client_seq, target_pane, key_code, modifiers, raw_bytes }`: Supports both raw VT bytes and unambiguous Kitty Keyboard Protocol key events.
- `MouseEvent { client_seq, pane_id, kind, button, col, row, modifiers }`: Server-routed mouse clicks, drags, and scroll-wheel events (automatically translated into pane-local coordinates by either client or server).
- `PasteEvent { client_seq, pane_id, bracketed: bool, data: bytes }`: Atomic paste payload.
- `ClientResize { cols: u16, rows: u16, pixel_width: u32, pixel_height: u32 }`: Triggers server-side layout recalculation and `ioctl(TIOCSWINSZ)` on affected pane PTYs.

#### C. Rendering, Synchronized Updates & Scrollback Messages (Server -> Client)
Every state update carries a monotonically increasing `server_seq: u64` and `acked_client_seq: u64`:
- `LayoutUpdate { server_seq, session_id, window_id, layout_tree, active_pane_id, zoomed_pane_id }`: Describes exact pane geometries (`x, y, width, height`) and border styles.
- `ScreenSnapshot { server_seq, pane_id, cols, rows, cursor, modes, compressed_cells }`: Full visible matrix for a pane (sent on initial attach, session switch, or after a long disconnect).
- `FrameDiff { server_seq, acked_client_seq, pane_diffs: Vec<PaneRowDiff>, status_line }`:
  - Coalesced at an adaptive frame rate (up to 120 Hz for local/low-latency links, dynamically paced by RTT and QUIC/TCP congestion window on slow links).
  - Client wraps each rendered frame in `DECSET 2026` (**Synchronized Output** `\x1b[?2026h ... \x1b[?2026l`) so the local terminal paints atomically with zero tearing.
- `ScrollbackSliceReq` / `Resp`: When the user enters Copy Mode (`Prefix + [`) and scrolls up, the client fetches compressed pages of immutable scrollback history by line range `[start_line, end_line]` and caches them locally.
- `PassthroughEvent { pane_id, kind: Osc52Clipboard | Bell | DesktopNotification | Hyperlink }`: Forwards clipboard writes (`OSC 52`), bells, and notifications directly to the local terminal without corrupting pane grid state.

---

## 5. Persistence, Roaming & Reconnection Mechanics

### 5.1 Decoupling Client Connection Drop from Session Lifecycle
On `tmux2d`:
- **Sessions are immortal until explicitly killed or all child panes exit.**
- Each connected client has a `ClientSessionState` holding:
  - `client_instance_id` (128-bit UUID)
  - `resume_token` (256-bit HMAC-authenticated token bound to the client certificate fingerprint, valid for configurable TTL, default 24 hours)
  - Ring buffer of recent `FrameDiff` packets (e.g., last 256 frames / 1 MB) and last `acked_client_seq`.

### 5.2 Fast Reconnect & Catch-Up Algorithm
When a network interruption occurs (Wi-Fi drop, VPN toggle, laptop sleep/wake):
1. **Client Side:**
   - The client keeps the user's local terminal screen intact and renders a non-intrusive status indicator in the top-right corner (`[reconnecting to dev.example.com... 2s]`).
   - Outbound keystrokes typed during a brief drop (optional/configurable) are buffered in a local outbound queue with their `client_seq` numbers.
   - Exponential backoff with jitter (`100ms`, `250ms`, `500ms`, `1s`, max `5s`) plus immediate retry on OS network interface change (`netlink` / `NWPathMonitor`).
2. **Handshake & Resumption:**
   - Upon reconnecting over mTLS, the client sends `ClientHello` with its `resume_token`, `last_server_seq`, and `attached_session_id`.
   - `tmux2d` verifies that the new connection's client certificate matches the `resume_token` owner.
   - **Delta Resume:** If `last_server_seq` is still within the server's ring buffer, `tmux2d` replays only the missed `FrameDiff`s.
   - **Snapshot Fallback:** If the client was offline longer than the ring buffer window, `tmux2d` immediately sends a fresh `LayoutUpdate` + `ScreenSnapshot` for the current window.
   - **Input Deduplication:** The client replays any unacknowledged `KeyEvent`s (`client_seq > last_client_seq_acked`), ensuring neither dropped nor duplicated characters.

---

## 6. User Experience, CLI & Configuration Specification

### 6.1 Ergonomic CLI (No SSH Required)
`tmux2` mirrors familiar `tmux` flags while adding native remote host targeting (`-H / --host` or `host:session` shorthand):

```bash
# 1. One-time PKI setup on server and profile import on client
tmux2 pki init --host dev.example.com
tmux2 profile import ./dev.tmux2-profile

# 2. Start or attach to default session on remote server (or create if none exists)
tmux2 -H dev new -A -s main

# 3. Shorthand syntax: attach to session "work" on profile "dev"
tmux2 attach dev:work

# 4. List all running sessions across a remote server without attaching
tmux2 -H dev ls
# Output:
# SESSION   WINDOWS   PANES   ATTACHED   CREATED          STATUS
# main      4         7       1 (roam)   2 days ago       active
# training  2         3       0          5 hours ago      detached (gpu_train.py running)
# debug     1         2       0          12 mins ago      detached

# 5. Spawn a background job in a new detached session remotely (no interactive shell needed!)
tmux2 -H dev new-session -d -s build -c /src/app "cargo build --release"

# 6. Send keys or capture output from a remote session scriptably
tmux2 -H dev capture-pane -t build:0.0 -p -S -100
```

### 6.2 Multi-Session Navigation Inside an Attached Client
Once attached to any session on `dev`:
- `Prefix + s`: Opens the interactive **Session & Window Chooser Overlay** (rendered locally with instant live preview of each session's active window). Selecting another session sends `SwitchSessionReq` and switches sessions in `< 10 ms` over the same mTLS connection.
- `Prefix + d`: Sends `DetachReq` and exits cleanly back to the local workstation shell in `< 5 ms`.
- `Prefix + $`: Rename current session.
- `Prefix + ( / )`: Cycle to previous / next session on the remote server.
- `Prefix + c` / `Prefix + %` / `Prefix + "`: Create window, vertical split, horizontal split (identical defaults to `tmux`).

### 6.3 Configuration Files (`~/.config/tmux2/config.toml`)

#### Client Configuration (`~/.config/tmux2/config.toml`)
```toml
default_host = "dev"
prefix_key = "C-b"
escape_time_ms = 0          # Zero escape delay because key events are structured!
mouse = true
clipboard = "osc52"

[hosts.dev]
address = "dev.example.com:8443"
transport = "auto"          # "quic" | "tcp" | "auto" (Happy Eyeballs QUIC -> TCP)
ca_cert = "~/.config/tmux2/pki/dev/ca.crt"
client_cert = "~/.config/tmux2/pki/dev/client.crt"
client_key = "~/.config/tmux2/pki/dev/client.key"
auto_reconnect = true
```

#### Server Configuration (`/etc/tmux2/server.toml` or `~/.config/tmux2/server.toml`)
```toml
listen_addrs = ["0.0.0.0:8443", "[::]:8443"]
local_socket = "/run/tmux2/1000/default.sock"
mode = "user"               # "user" or "system"
scrollback_lines = 50000
session_idle_timeout = "never"
resume_token_ttl = "24h"

[tls]
server_cert = "~/.config/tmux2/pki/server.crt"
server_key = "~/.config/tmux2/pki/server.key"
client_ca_cert = "~/.config/tmux2/pki/ca.crt"
require_client_cert = true
min_version = "1.3"

[[authorization.allow]]
cert_san_uri = "tmux2://user/alice"
unix_user = "alice"
allowed_sessions = ["*"]
```

---

## 7. Implementation Stack & Phased Roadmap

### 7.1 Recommended Technology Stack (Rust)
- **Language:** Rust (memory-safe TLS/network parsing, fearless concurrency across PTY and socket tasks).
- **Async Runtime:** `tokio`.
- **TLS 1.3 & mTLS Cryptography:** `rustls` + `aws-lc-rs` or `ring`, with `rcgen` for built-in `tmux2 pki` certificate generation and `x509-parser` for SAN/extension extraction.
- **QUIC Transport:** `quinn` (built on `rustls`, supports 0-RTT/1-RTT resumption and connection migration).
- **PTY Management:** `rustix-openpty` / `portable-pty` + `nix` for process groups, signals, and `TIOCSWINSZ`.
- **Server-Side VTE Parser & Grid:** `alacritty_terminal` or `vt100` / custom grid with zstd-compressed scrollback pages.
- **Local Terminal Client UI:** `crossterm` or `termion` with synchronized output (`DECSET 2026`) and Kitty keyboard protocol support.

### 7.2 Phased Delivery Plan
1. **Phase 1 (Core Daemon & mTLS Framing):**
   - `tmux2 pki` certificate generator.
   - `tmux2d` listening on TCP+TLS 1.3 (mTLS) and `tmux2` client with `ListSessions`, `NewSession`, `AttachSession`, `Detach`, and `SwitchSession`.
2. **Phase 2 (VTE Grid, Multi-Window/Pane Layout & Diff Renderer):**
   - Server-side VTE state machine per pane, binary split layout engine, status bar, and incremental `FrameDiff` rendering with `DECSET 2026`.
3. **Phase 3 (QUIC Transport, Roaming & Scrollback Paging):**
   - `quinn` QUIC transport with TCP fallback, `resume_token` sequence catch-up, and server-side scrollback paging + Copy Mode.
4. **Phase 4 (System Multi-User Privilege Separation):**
   - `tmux2d-listener` / `tmux2d-tls` / `tmux2d-user` split architecture with X.509 SAN-to-UID mapping and systemd socket activation.
