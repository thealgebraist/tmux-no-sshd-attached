# `tmux2` (`tmux-no-sshd-attached`)

**An mTLS-Native, Detachable, Persistent Multi-Session Terminal Multiplexer in C++23**

`tmux2` eliminates the legacy `ssh -> remote PTY -> tmux attach` workflow by replacing local-only `AF_UNIX` + `SCM_RIGHTS` file-descriptor passing with a network-native **TLS 1.3 Mutual TLS (mTLS)** multiplexing protocol over **OpenSSL 3.5+ QUIC (`UDP/8443`)** and **TCP TLS 1.3 (`TCP/8443`)**, while retaining zero-config local **`AF_UNIX` auto-spawn** ergonomics on both **Linux** and **macOS**.

See [spec.md](./spec.md) for the complete technical architecture and wire protocol specification.

---

## Key Features

- **Direct mTLS 1.3 Connectivity (No `sshd` Required)**: Connect directly from your workstation terminal to `tmux2d` using Ed25519 X.509v3 client and server certificates (`ALPN: "tmux2/1"`).
- **QUIC Primary + TCP TLS 1.3 Fallback**: Concurrent Happy Eyeballs connection racing between OpenSSL 3.5+ QUIC (`UDP/8443`) and TCP TLS 1.3 (`TCP/8443`), plus local `0600` `AF_UNIX` domain socket auto-spawn.
- **Full `tmux` Hierarchy**: Named Sessions $\rightarrow$ Windows $\rightarrow$ Horizontal/Vertical Split Panes with dynamic `window-size = latest` multi-client sizing and `attach -d` detach-others support.
- **Server-Side VTE & Compressed Scrollback**: Powered by `libvterm` + `libzstd`-compressed ring buffers (`ScrollbackBuffer`) and incremental dirty-row cell diffing (`MsgFrameDiff` / `MsgScreenSnapshot`).
- **Seamless Roaming & Reconnect**: Certificate-bound HMAC `resume_token`, server `FrameDiff` ring-buffer delta catch-up (with automatic `ScreenSnapshot` fallback), and `client_seq` keystroke deduplication.
- **Daily-Driver TUI UX**: `Ctrl-b` prefix keybindings, interactive Session Chooser overlay (`Ctrl-b s`), Vi Copy Mode (`Ctrl-b [`) with remote scrollback paging and `OSC 52` clipboard yanking, SGR mouse click-to-focus & split-border drag resize, and `DECSET 2026` synchronized atomic rendering.
- **Built-in Ed25519 PKI**: One-command CA, server, and client certificate provisioning (`tmux2 pki init`, `tmux2 pki export-client`, `tmux2 profile import`).

---

## Testing from a MacBook (macOS) to a Linux Server

Below is the step-by-step workflow to test `tmux2` directly from a MacBook client to this Linux server (`159.195.249.30`) over QUIC / TCP TLS 1.3 mTLS without SSH.

### Step 1: On the Linux Server (`159.195.249.30`)

1. **Build `tmux2` / `tmux2d`**:
   ```bash
   cd /home/agent1/tmux2
   PATH="$HOME/.local/bin:$PATH" cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   PATH="$HOME/.local/bin:$PATH" cmake --build build -j$(nproc)
   ```

2. **Initialize the Ed25519 X.509v3 PKI and export a client profile for your MacBook**:
   *(Replace `159.195.249.30` with your server's public IP or DNS hostname)*
   ```bash
   ./build/tmux2 pki init --host 159.195.249.30 --listen 0.0.0.0:8443
   ./build/tmux2 pki export-client macbook --server 159.195.249.30:8443 > ~/macbook.tmux2-profile
   ```
   - Generates `~/.config/tmux2/pki/{ca.crt,ca.key,server.crt,server.key,server.toml}` with `0600` permissions on private keys and bundles the CA cert, client cert, client private key, and server endpoint into `~/macbook.tmux2-profile`.

3. **Start the `tmux2d` daemon on UDP/TCP port `8443`**:
   *(Ensure UDP and TCP port `8443` are open in your firewall)*
   ```bash
   # Run in foreground to inspect logs, or pass --daemonize to run in the background
   ./build/tmux2d --config ~/.config/tmux2/pki/server.toml
   ```

---

### Step 2: On Your MacBook (macOS)

1. **Install prerequisites via Homebrew & build `tmux2`**:
   `CMakeLists.txt` automatically detects Homebrew `openssl@3` (`brew --prefix openssl@3`) and uses CMake `FetchContent` to build `libvterm`, `zstd`, `toml++`, and `doctest` if not installed locally:
   ```bash
   brew install cmake openssl@3

   git clone https://github.com/thealgebraist/tmux-no-sshd-attached.git
   cd tmux-no-sshd-attached
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j$(sysctl -n hw.ncpu)

   # Optional: run the 9/9 unit & E2E test suites locally on macOS (tests kqueue + Darwin PTYs)
   ctest --test-dir build --output-on-failure
   ```

2. **Copy `macbook.tmux2-profile` from the server once and import it**:
   ```bash
   scp agent1@159.195.249.30:~/macbook.tmux2-profile ./macbook.tmux2-profile
   ./build/tmux2 profile import ./macbook.tmux2-profile
   ```
   - Installs client certificates into `~/.config/tmux2/pki/macbook/` (`0600`) and configures `[hosts.macbook]` (`default_host = "macbook"`) in `~/.config/tmux2/config.toml`.

---

### Step 3: Connect Directly from Your MacBook (No `ssh`!)

Once imported, `tmux2` connects directly over **OpenSSL 3.5 QUIC (`UDP/8443`)** / **TCP TLS 1.3 (`TCP/8443`)**:

```bash
# 1. Create and attach to a persistent remote session named "dev"
./build/tmux2 new -s dev

# 2. List remote sessions or re-attach explicitly with -H macbook
./build/tmux2 -H macbook ls
./build/tmux2 -H macbook attach -t dev

# 3. Attach and detach any other currently attached clients (-d)
./build/tmux2 -H macbook attach -d -t dev
```

*(To use `tmux2` locally on your MacBook via `AF_UNIX` auto-spawn without hitting the remote `default_host`, remove `default_host = "macbook"` from `~/.config/tmux2/config.toml` and pass `-H macbook` when targeting the remote server.)*

---

### Step 4: Interactive Keybindings & Features to Try

| Keybinding / Action | Description |
| :--- | :--- |
| `Ctrl-b "` / `Ctrl-b %` | Split active pane horizontally / vertically |
| `Ctrl-b o` or **Mouse Click** | Cycle or click to focus panes (drag borders with the mouse to resize) |
| `Ctrl-b z` | Toggle active pane zoom |
| `Ctrl-b c` / `Ctrl-b n` / `Ctrl-b p` | Create new window / switch to next or previous window |
| `Ctrl-b s` | Open interactive **Session Chooser overlay** (`j`/`k` to select, `Enter` to switch sessions in-band) |
| `Ctrl-b [` | Enter **Vi Copy Mode** (`h`/`j`/`k`/`l`/`Ctrl-u`/`g`/`G`, `/` search, `v` select, `y` yank to local clipboard via **OSC 52**) |
| `Ctrl-b :` | Open command prompt (`new-window`, `split-window -h`, `kill-pane`, `attach-session -r`, etc.) |
| `Ctrl-b d` | Detach from session while remote processes continue running |
| **Toggle Wi-Fi off/on** | Observe the `[Reconnecting...]` banner buffer keystrokes (`client_seq`) and replay missed `FrameDiff` deltas on reconnect |

---

## Local Build & Test Suite

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

To force building all dependencies (`doctest`, `toml++`, `zstd`, `libvterm`) from source via `FetchContent`:

```bash
cmake -S . -B build-fetch -DTMUX2_USE_SYSTEM_DEPS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-fetch -j
ctest --test-dir build-fetch --output-on-failure
```
