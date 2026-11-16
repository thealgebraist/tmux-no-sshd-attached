# Simplifying `tmux2` Bootstrapping: 8-Iteration Research & Design (`bootstrap.md`)

This document presents an 8-iteration architectural and cryptographic research analysis on how to collapse the multi-step `tmux2` mTLS bootstrapping workflow (e.g., connecting a MacBook to a Linux server at `159.195.249.30`) from **5 manual commands across 2 machines** down to **1 command**, both **with** an initial SSH channel and in **pure zero-`sshd` environments**.

---

## Iteration 1: Deconstructing Current Bootstrap Friction & Security Flaws

### Current 5-Step Workflow
Today, enrolling a new MacBook client with a Linux server requires 5 manual steps across two terminals:

```bash
# [Server] 1. Initialize CA, server cert, and client cert
./build/tmux2 pki init --host 159.195.249.30 --listen 0.0.0.0:8443

# [Server] 2. Export client bundle to a temporary file on disk
./build/tmux2 pki export-client macbook --server 159.195.249.30:8443 > ~/macbook.tmux2-profile

# [Server] 3. Start daemon with explicit config path
./build/tmux2d --config ~/.config/tmux2/pki/server.toml --daemonize

# [MacBook] 4. Copy plaintext profile bundle over SCP
scp agent1@159.195.249.30:~/macbook.tmux2-profile ./macbook.tmux2-profile

# [MacBook] 5. Import profile into ~/.config/tmux2/
./build/tmux2 profile import ./macbook.tmux2-profile
```

### Five Root Causes of Friction
1. **Manual IP/Hostname Discovery**: The operator must manually look up `159.195.249.30` and pass it twice (`--host 159.195.249.30` and `--server 159.195.249.30:8443`).
2. **Split Server PKI Commands**: `pki init` and `pki export-client` are separate steps even though `init_pki()` in `src/pki/pki_manager.cpp` already generates `ca.crt`, `client.crt`, and `client.key` in memory.
3. **Explicit Daemon Config Path**: `tmux2d` requires `--config ~/.config/tmux2/pki/server.toml` unless copied to `~/.config/tmux2/server.toml`, and does not auto-initialize PKI on first launch.
4. **Temporary Cleartext Key Files on Disk**: `~/macbook.tmux2-profile` and `./macbook.tmux2-profile` remain on disk on both machines containing an unencrypted `-----BEGIN PRIVATE KEY-----` block unless manually shredded.
5. **Server-Side Client Private Key Generation**: `init_pki()` generates `client.key` on the *server* and transmits the private key over the network, violating the principle that a client's private key should never leave the client device.

---

## Iteration 2: Immediate Ergonomic Wins (`stdin` Pipe & Auto-Host Detection)

Before introducing any new wire messages, three small CLI enhancements in `src/app/pki_commands.cpp` and `src/app/daemon_runner.cpp` immediately eliminate temporary files and redundant flags:

1. **Support `stdin` in `tmux2 profile import`**:
   - `pki::import_client_profile(std::string_view profile_toml_content, cfg_dir)` *already exists* in `src/pki/pki_manager.cpp:181`.
   - Updating `run_profile_import()` so `tmux2 profile import -` (or `tmux2 profile import` when `!isatty(STDIN_FILENO)`) reads from `std::cin` eliminates the `scp` step and all temporary `.tmux2-profile` files on disk.
2. **Auto-Detect `--host` and Default `--server`**:
   - If `--host` is omitted in `tmux2 pki init`, inspect non-loopback global `AF_INET`/`AF_INET6` interfaces via `getifaddrs(3)` plus `gethostname(2)` and include all discovered addresses in the server certificate's X.509 Subject Alternative Names (`SAN` IP + DNS).
3. **Auto-Initialize PKI & Default Config in `tmux2d`**:
   - When `tmux2d` starts and neither `~/.config/tmux2/server.toml` nor `~/.config/tmux2/pki/server.toml` exists, `tmux2d` automatically runs `init_pki()` using the detected host IPs and starts listening on `0.0.0.0:8443`.

With just Iteration 2, the 5-step workflow shrinks to a single pipeline from the MacBook:

```bash
ssh agent1@159.195.249.30 "tmux2 pki init --host 159.195.249.30 && tmux2d --daemonize && tmux2 pki export-client macbook --server 159.195.249.30:8443" \
  | ./build/tmux2 profile import -
```

---

## Iteration 3: One-Command Client-Driven Bootstrap (`tmux2 bootstrap user@host`)

If the user can run an `ssh` pipeline from their MacBook once to enroll, `tmux2` itself can orchestrate that entire pipeline natively in a **single command**:

```bash
# Run once on MacBook: bootstraps PKI, starts remote tmux2d, imports profile, and attaches!
tmux2 bootstrap agent1@159.195.249.30
```

### How `tmux2 bootstrap <user@host>` Works
1. Parses `<user>@<host>[:<port>]` (extracting `159.195.249.30` automatically, so the user never types `--host` or `--server`).
2. Spawns a single one-shot `ssh <user>@<host> "tmux2 pki bootstrap-server --host 159.195.249.30 --client-name macbook"` subprocess.
3. On the server, `tmux2 pki bootstrap-server`:
   - Idempotently initializes `~/.config/tmux2/pki/` if not present (or ensures `159.195.249.30` is in `server.crt` SANs).
   - Starts `tmux2d --daemonize` if no daemon is listening on port `8443` (verifying via `kill(pid, 0)` / socket bind check).
   - Outputs the client profile TOML to `stdout`.
4. On the MacBook, `tmux2 bootstrap` reads the TOML stream from the subprocess pipe directly into memory, calls `pki::import_client_profile()`, writes `0600` credentials to `~/.config/tmux2/pki/159.195.249.30/`, and **immediately connects over QUIC / TCP TLS 1.3 mTLS**.

---

## Iteration 4: Cryptographic Hardening — Client-Side Key Generation (`Zero-Private-Key-Transit`)

### Critique of Iterations 1–3
In Iterations 1–3, the server generates `client.key` and sends the private key back to the client. If an attacker compromises the server later or snoops stdout logs, they obtain the client's private key.

### Solution: Public-Key Enrollment (`tmux2 pki sign-client`)
We invert key generation so **`client.key` is generated on the MacBook and never leaves the MacBook**:

```
MacBook (Client)                                     Linux Server (tmux2d)
----------------                                     ---------------------
1. Generate Ed25519 keypair locally:
   ~/.config/tmux2/pki/<host>/client.key (0600)
   Extract 32-byte Ed25519 SPKI Public Key PEM
                |
                |--- pubkey_pem + host + client_user --->|
                |                                        2. Ensure CA & server.crt exist
                |                                           Ensure tmux2d is running
                |                                           Sign X.509v3 client.crt over
                |                                           client's pubkey_pem using ca.key
                |<-- ca.crt + client.crt + endpoint -----|
3. Save ca.crt & client.crt
   Update ~/.config/tmux2/config.toml
   Connect via QUIC / TCP TLS 1.3 mTLS!
```

- Our OpenSSL 3.5+ `generate_client_cert()` in `src/pki/cert_generator.cpp` only needs a minor overload accepting `EVP_PKEY* client_public_key` instead of calling `generate_ed25519_key()`.
- Now **only public certificates (`ca.crt`, `client.crt`, `pubkey.pem`) ever cross the wire**.

---

## Iteration 5: Pure Zero-`sshd` Environments (`tmux2 invite` & `tmux2 join`)

### What If `sshd` Is Not Installed at All?
The project's namesake is **`tmux-no-sshd-attached`**. On a minimal container, cloud VM, or appliance where `sshd` is completely absent (and the operator only has a web serial console, cloud-init output, or local terminal on the server), `ssh` and `scp` cannot be used even once.

Copying a 40-line `.tmux2-profile` TOML file across a web VNC/serial console is error-prone. Why is `.tmux2-profile` ~1,200 bytes? Because it carries three full X.509/PKCS#8 PEM blocks.

### The `tmux2 invite` / `tmux2 join` Pattern
Instead of copying certificates out-of-band, the server prints a **single 1-line enrollment URI** (and optionally a UTF-8 ANSI QR code):

```bash
# [On Server Console / Cloud-Init Log]
$ tmux2 invite
Enrollment URI (valid for 10m, single-use):

  tmux2 join tmux2://sha256:4f8a9b2c...e10d@159.195.249.30:8443#k9XmP2vL8qR4tW1zY6nB3cF5hJ0mK7pQ
```

And on the MacBook:

```bash
# [On MacBook]
$ tmux2 join "tmux2://sha256:4f8a9b2c...e10d@159.195.249.30:8443#k9XmP2vL8qR4tW1zY6nB3cF5hJ0mK7pQ"
```

---

## Iteration 6: Cryptographic Protocol for In-Band `tmux2 join` Over Port 8443

How can `tmux2 join` exchange certificates in-band over UDP/TCP port `8443` **without weakening `tmux2d`'s mandatory TLS 1.3 mTLS posture** or exposing the server to MITM or brute-force attacks?

### Anatomy of the Enrollment URI
```
tmux2://sha256:<CA_SPKI_SHA256_HEX>@<HOST>:<PORT>#<BASE64URL_256BIT_TOKEN>
```
1. `<CA_SPKI_SHA256_HEX>` (64 hex chars = 256 bits): SHA-256 fingerprint of the server CA's SubjectPublicKeyInfo (`spki_fingerprint_sha256`, already computed in `src/pki/crypto_utils.cpp`!).
2. `<HOST>:<PORT>`: Target `tmux2d` address (`159.195.249.30:8443`).
3. `<BASE64URL_256BIT_TOKEN>` (43 chars = 256 bits of entropy from `RAND_bytes()`): Single-use enrollment secret $T$ with a short TTL (default 10 minutes).

### 4-Step Channel-Bound Handshake (`ALPN: "tmux2-enroll/1"`)

```mermaid
sequenceDiagram
    participant C as MacBook (tmux2 join)
    participant S as Linux Server (tmux2d :8443)

    Note over S: tmux2 invite generates 256-bit token T,<br/>stores SHA256(T) with 10m TTL (max_uses=1)
    Note over C: 1. Generate permanent Ed25519 keypair (pk_C, sk_C)<br/>and ephemeral self-signed TLS client cert
    C->>S: 2. TLS 1.3 Handshake (ALPN: "tmux2-enroll/1")
    Note over C: Verify Server Cert chain & match<br/>Root CA SPKI == <CA_SPKI_SHA256_HEX> (Stops MITM!)
    Note over C,S: Both sides derive 32-byte RFC 8446 §7.5 TLS Exporter:<br/>E = SSL_export_keying_material("EXPORTER-tmux2-enroll", 32)
    C->>S: 3. MsgEnrollReq { client_name, pk_C_spki,<br/>hmac = HMAC-SHA256(T, E || pk_C_spki || client_name) }
    Note over S: Constant-time verify hmac against active token T;<br/>Atomically burn token T (single-use);<br/>Sign X.509v3 client.crt for pk_C_spki
    S->>C: 4. MsgEnrollResp { ca_cert_pem, client_cert_pem }
    Note over C: Save ca.crt, client.crt, client.key (0600);<br/>Reconnect on ALPN "tmux2/1" with full mTLS!
```

### Why This Protocol Is Cryptographically Sound
- **Zero MITM Risk**: The client pins `<CA_SPKI_SHA256_HEX>` from the URI during the TLS 1.3 handshake before sending a single enrollment byte.
- **Zero TLS Exporter Replay / Relay Risk**: Binding `HMAC-SHA256(T, ...)` to `SSL_export_keying_material(ssl, ..., "EXPORTER-tmux2-enroll", ...)` (RFC 8446 §7.5 / RFC 9266) cryptographically ties the proof to the exact TLS 1.3 session and the client's public key $pk_C$.
- **Zero Private Key Exposure**: Neither $T$ nor $sk_C$ is ever transmitted in plaintext or ciphertext; $T$ is used only as an HMAC key, and $sk_C$ never leaves the MacBook.
- **Strict Separation from Production mTLS**: Enrollment only runs when at least one non-expired invite token exists, and uses a dedicated ALPN (`tmux2-enroll/1`) that can only process a single `MsgEnrollReq` frame before closing.

---

## Iteration 7: Zero-Command Cloud VM Provisioning via `~/.ssh/authorized_keys` Ed25519 Bridge

What if a user spins up a fresh cloud VM (AWS, Hetzner, DigitalOcean, GCP) that already has their `~/.ssh/authorized_keys` (`ssh-ed25519 AAAAC3NzaC1lZDI1NTE5...`) injected by cloud-init, and `tmux2d` is started by systemd on boot?

### Key Insight: `ssh-ed25519` and X.509v3 Ed25519 Share the Exact Same 32-Byte Public Key
An OpenSSH `ssh-ed25519` public key in `~/.ssh/authorized_keys` is simply a 19-byte header (`00 00 00 0b "ssh-ed25519" 00 00 00 20`) followed by the **raw 32-byte Ed25519 public key** (`EVP_PKEY_new_raw_public_key_ex(nullptr, "ED25519", nullptr, raw32, 32)` in OpenSSL 3.5!).

- If `tmux2d` is configured with `authorize_ssh_ed25519_keys = "~/.ssh/authorized_keys"`:
  - During `MsgEnrollReq`, instead of a one-time token $T$, the client can sign `E || pk_C_spki` using the user's local `~/.ssh/id_ed25519` (or via `SSH_AUTH_SOCK` SSH agent signing!).
  - `tmux2d` verifies the signature against the 32-byte Ed25519 public keys in `~/.ssh/authorized_keys` and issues the X.509v3 `client.crt` automatically!
- This allows a user to connect to a newly provisioned cloud server **without running any bootstrap command on the server and without `sshd` even running**.

---

## Iteration 8: Trade-Off Synthesis & Concrete Implementation Roadmap

### Comparison Matrix

| Metric | Current 5-Step Flow | Tier 1: `tmux2 bootstrap user@host` (SSH Pipe + CSR) | Tier 2: `tmux2 invite` + `tmux2 join <uri>` (In-Band Port 8443) | Tier 3: `authorized_keys` Ed25519 Bridge |
| :--- | :--- | :--- | :--- | :--- |
| **Commands Typed by User** | **5** (across 2 machines) | **1** (on MacBook) | **2** (`tmux2 invite` on server, `tmux2 join` on MacBook) | **1** (on MacBook) |
| **Requires `sshd` on Server?** | Yes (for `scp`) | Yes (once, during bootstrap) | **No (`0%` SSH dependency)** | **No (`0%` SSH dependency)** |
| **Client Private Key Stays on MacBook?** | No (generated on server) | **Yes** | **Yes** | **Yes** |
| **Leaves Temp Files on Disk?** | Yes (`*.tmux2-profile`) | **No** (in-memory pipe) | **No** (in-memory stream) | **No** |
| **Estimated C++23 Implementation Size** | `0 LOC` (Done) | **`~140 LOC`** | **`~260 LOC`** | **`~180 LOC`** |

---

### Recommended Phased Implementation Plan

#### Phase 1: One-Command SSH Bootstrap + Zero-Temp-File PKI (`~140 LOC`)
1. **`src/pki/cert_generator.{hpp,cpp}`**:
   - Add `generate_client_cert_for_pubkey(const ClientCertOptions& opts, EVP_PKEY* client_pubkey, const EVP_PKEY* ca_key, const X509* ca_cert)` so the server can sign a client-generated Ed25519 public key without ever holding `client.key`.
2. **`src/app/pki_commands.cpp`**:
   - Support `tmux2 profile import -` (read profile TOML from `std::cin`).
   - Auto-detect host IP addresses via `getifaddrs(3)` when `--host` is omitted in `tmux2 pki init`.
   - Add `tmux2 pki bootstrap-server --host <h> --pubkey <pem>` (idempotently initializes CA/server certs, signs the client's public key, ensures `tmux2d --daemonize` is running, and writes the profile TOML to `stdout`).
3. **`src/client/cli_parser.cpp` & `src/app/client_runner.cpp`**:
   - Add `tmux2 bootstrap <user@host>`: generates `client.key` locally on the MacBook, invokes `ssh <user>@<host> tmux2 pki bootstrap-server ...`, imports the returned `ca.crt` + `client.crt` into `~/.config/tmux2/`, and immediately attaches to the remote session.

#### Phase 2: Native Zero-`sshd` Enrollment (`tmux2 invite` / `tmux2 join`, `~260 LOC`)
1. **`src/protocol/messages.{hpp,cpp}`**:
   - Add `MsgEnrollReq` (`0x0F`: `client_name`, `client_pubkey_pem`, `hmac_proof`) and `MsgEnrollResp` (`0x1F`: `success`, `error_msg`, `ca_cert_pem`, `client_cert_pem`).
2. **`src/app/pki_commands.cpp` & `src/app/daemon_runner.cpp`**:
   - Add `tmux2 invite [--ttl 10m]`: generates a 256-bit single-use token in `~/.config/tmux2/pki/invites.toml` (`0600`) and prints `tmux2 join tmux2://sha256:<ca_spki>@<ip>:8443#<token>`.
   - Add `tmux2 join <uri>`: performs the TLS 1.3 handshake with CA SPKI pinning, computes RFC 8446 §7.5 `SSL_export_keying_material("EXPORTER-tmux2-enroll")`, exchanges `MsgEnrollReq`/`MsgEnrollResp`, writes `~/.config/tmux2/pki/<host>/`, and seamlessly transitions into an active `tmux2` session.

---

## Iteration 9: Using Cloudflare Services for Safe Certificate Distribution & Zero-Trust Bootstrapping

Can Cloudflare's edge, PKI, and Zero Trust services make `tmux2` certificate distribution and bootstrapping safer and simpler? **Yes — across four distinct architectural patterns**, depending on whether you want Cloudflare to act only as an **identity-gated PKI enrollment authority** (keeping direct peer-to-peer UDP QUIC/TCP on port `8443`) or also as a **Zero-Trust network transport proxy**.

```mermaid
flowchart TD
    subgraph PatternA["Pattern A: Identity-Gated PKI Broker (Direct QUIC/TCP Kept!)"]
        M1["MacBook (tmux2 login)"] -->|"1. OIDC / GitHub / Google SSO"| CFA["Cloudflare Access + Worker CA"]
        CFA -->|"2. Signs Client CSR (Ed25519/P-256)\nReturns client.crt + ca.crt + server endpoint"| M1
        M1 ==>|"3. Direct UDP/8443 QUIC mTLS 1.3\n(Zero proxy latency!)"| S1["Linux Server (tmux2d :8443)\nTrusts Worker/CF Root CA"]
    end

    subgraph PatternB["Pattern B: DNS / HTTPS Record Trust Bootstrapping"]
        M2["MacBook"] -->|"1. DoH DNSSEC / HTTPS RR Query\n_8443._udp.dev.example.com"| CFD["Cloudflare Authoritative DNS"]
        CFD -->|"2. Returns Server IP + CA SPKI SHA-256 Pin"| M2
    end
```

### 1. Cloudflare Access + Cloudflare Worker as an SSO-Gated Certificate Authority (Best Fit for Direct QUIC mTLS!)
- **Why it fits `tmux2` best**: Cloudflare's regular HTTP proxy does not forward raw `UDP/8443` custom-ALPN (`tmux2/1`) QUIC traffic on standard plans (unless using Cloudflare Spectrum Enterprise or Cloudflare WARP). However, you **do not need** Cloudflare in the data path to solve certificate distribution!
- **How it works**:
  1. **Control Plane (Cloudflare Access + Worker)**: Deploy a tiny Cloudflare Worker (`enroll.example.com`) behind **Cloudflare Access** (requiring GitHub, Google Workspace, Okta, or Passkey/WebAuthn login).
  2. **Root CA Trust**: Your `tmux2d` server(s) are configured once with `client_ca_cert` trusting your organization's Root CA (stored as an encrypted Cloudflare Worker Secret or backed by **Cloudflare Zero Trust Custom Trust Store / Client Certificates API**).
  3. **Client Enrollment (`tmux2 login enroll.example.com`)**:
     - The MacBook generates its Ed25519 private key (`client.key`, `0600`) locally — **the private key never leaves the MacBook**.
     - `tmux2` invokes `cloudflared access login` (or opens a browser tab for Passkey/SSO authentication) and POSTs its 32-byte Ed25519 public key CSR to `https://enroll.example.com/sign`.
     - The Cloudflare Worker verifies the signed `Cf-Access-Jwt-Assertion` header (extracting the authenticated user's email/identity, e.g. `alice@example.com`), signs a **short-lived X.509v3 client certificate** (e.g., 24-hour or 7-day TTL with `SAN URI: tmux2://user/alice`), and returns `{ca_cert, client_cert, hosts}` as a `.tmux2-profile`.
  4. **Data Plane (Direct Peer-to-Peer QUIC)**:
     - The MacBook connects **directly** to `159.195.249.30:8443` over native UDP QUIC / TCP TLS 1.3 (`ALPN: "tmux2/1"`) with zero proxy latency!
- **Security Wins**:
  - **Zero SSH / Zero Manual Tokens**: Authentication uses your existing identity provider (GitHub/Google/Okta + hardware FIDO2 passkeys).
  - **Short-Lived Certificates Eliminate Revocation Headaches**: If the Worker issues 24-hour client certificates, stolen laptops expire automatically without needing CRL/OCSP distribution to every `tmux2d` daemon.
  - **Fleet-Wide Multi-Server Access**: A single `tmux2 login` grants mTLS access to *all* your `tmux2d` servers that trust that Root CA.

### 2. Cloudflare Zero Trust Managed Client Certificates API (`POST /accounts/{id}/mtls_certificates`)
- Cloudflare Zero Trust provides a built-in managed PKI (**Zero Trust $\rightarrow$ Access $\rightarrow$ Service Auth $\rightarrow$ Mutual TLS** / Client Certificates API):
  - Cloudflare hosts a dedicated Root CA per account and exposes an API (`POST /zones/{zone_id}/client_certificates`) that accepts a PKCS#10 **Certificate Signing Request (CSR)** and returns a signed X.509v3 client certificate.
  - You can download the Cloudflare-managed Root CA certificate (`ca.crt`) onto your `tmux2d` Linux server (`client_ca_cert = "cloudflare-zt-ca.crt"`) so `tmux2d` natively trusts client certificates issued by Cloudflare's PKI API!
  - Note: Cloudflare's managed Client Certificates API issues ECDSA P-256 / RSA certificates (and our `tmux2` PKI & TLS engine in `src/pki/crypto_utils.cpp` and `src/transport/tls_utils.cpp` **already supports both Ed25519 and ECDSA P-256**!).

### 3. Cloudflare Authoritative DNS (`TLSA` / `TXT` Records over DNS-over-HTTPS) for Zero-MITM Server Pinning
- When bootstrapping to a new `tmux2d` server, how does the client verify the server's `ca.crt` without copying `ca.crt` over SSH first?
- If your domain is on **Cloudflare DNS** (with DNSSEC enabled):
  - When `tmux2d` initializes its PKI on `159.195.249.30`, it can publish its CA SPKI SHA-256 fingerprint to a DNS `TXT` or `TLSA` (DANE RFC 6698) record via a scoped Cloudflare DNS API token:
    `_8443._udp.dev.example.com. IN TXT "v=tmux2-1; ca-spki-sha256=4f8a9b2c...; ip=159.195.249.30:8443"`
  - Your MacBook queries `https://cloudflare-dns.com/dns-query` (DNS-over-HTTPS) for `_8443._udp.dev.example.com`, retrieves the authenticated `<ca-spki-sha256>` pin and IP address automatically, and verifies the server's TLS 1.3 handshake against that DNS-pinned SPKI hash!

### 4. Cloudflare WARP / Cloudflare Tunnel (`cloudflared`) for NAT-Piercing Inbound-Port-Zero Servers
- What if your Linux server is behind CGNAT, a corporate firewall, or home router where you **cannot open inbound port `8443`**?
  - **Option 4A — Cloudflare WARP-to-WARP / Private Network Routing (Supports UDP QUIC + TCP!)**:
    - Running `cloudflared` on the Linux server and the Cloudflare WARP client on your MacBook creates a WireGuard/MASQUE private overlay network (`100.96.x.x` or private CIDR).
    - `tmux2` on your MacBook connects directly to `tmux2d` over **native UDP QUIC (`8443`)** across the WARP virtual IP, while still enforcing `tmux2`'s end-to-end Ed25519 mTLS 1.3 on top!
  - **Option 4B — Cloudflare Tunnel (`cloudflared access tcp`)**:
    - Exposes `localhost:8443` on the server through Cloudflare's edge with zero open inbound firewall ports (`--tcp-only` fallback mode in `tmux2`).

### Summary Matrix of Cloudflare Integrations for `tmux2`

| Cloudflare Service | Role in `tmux2` | Preserves Direct UDP/8443 QUIC? | Key Security Benefit |
| :--- | :--- | :--- | :--- |
| **1. Cloudflare Access + Worker CA** | SSO/Passkey-gated CSR signing & `.tmux2-profile` distribution | **Yes (100% direct data plane)** | Hardware-key SSO auth + short-lived (24h) certs + client private key never leaves MacBook |
| **2. Cloudflare Zero Trust Client Cert API** | Managed Root CA & CSR signing API (`P-256`) | **Yes (100% direct data plane)** | Managed HSM-backed CA + API revocation |
| **3. Cloudflare DNS (`TLSA`/`TXT` + DoH)** | Publishes server IP & `<ca-spki-sha256>` pin | **Yes (100% direct data plane)** | Eliminates TOFU/MITM when discovering server CA |
| **4. Cloudflare WARP Private Routing** | NAT traversal when port `8443` cannot be opened | **Yes (UDP + TCP over MASQUE/WireGuard)** | Zero open inbound ports on the Linux server |

