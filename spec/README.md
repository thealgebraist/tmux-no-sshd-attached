# `tmux2` Formal Operational Semantics & Mechanized Proofs (`spec/`)

This directory contains the formal semantics and machine-checked verification of **`tmux2`** (`tmux-no-sshd-attached`), using a **Stratified Dual-Semantics** architecture that combines **Big-Step Natural Semantics ($\Downarrow$)** with a **Small-Step Labelled Transition System ($\xrightarrow{\alpha}$)**.

---

## 1. Files in `spec/`

| File | Toolchain | Purpose |
| :--- | :--- | :--- |
| [Tmux2Semantics.lean](./Tmux2Semantics.lean) | **Lean 4 (`v4.36.0`)** | Machine-checked formalization of all 7 `tmux2` subsystems (mTLS PKI Auth, Wire Codec, Binary Split Layout Tree, VTE Grid & Compressed Scrollback, Client Modal TUI, Multi-Client `window-size = latest` & `attach -d`, and Section 5 Roaming/Deduplication LTS) with **15 machine-checked theorems** and **0 `sorry`s**. |
| [tmux2.ott](./tmux2.ott) | **Ott (`0.34`)** | Classic operational semantics specification in **Ott** metalanguage defining metavariables, grammars, Big-Step judgments (`AuthCert`, `EvalLayout`, `EvalUiMode`), and Small-Step LTS transitions (`StepSys`). |
| [README.md](./README.md) | Markdown / Math | Architectural guide explaining why Big-Step $\Downarrow$ and Small-Step $\xrightarrow{\alpha}$ are stratified by layer, inference rule reference, and theorem index. |

---

## 2. Why Stratified Big-Step ($\Downarrow$) + Small-Step ($\xrightarrow{\alpha}$) Fits `tmux2` Best

A network-native terminal multiplexer consists of **two fundamentally different computational regimes**:

1. **Intra-Tick Deterministic Evaluations $\implies$ Big-Step Natural Semantics ($\Downarrow$)**:
   Within a single non-blocking Reactor callback (`ServerEngine::handle_message` or `ClientApp::process_input_event`), sub-computations are **terminating, deterministic, and atomic**:
   - **mTLS 1.3 Certificate & Identity Verification ($\Downarrow_{\text{auth}}$)**:
     $$\frac{\neg\text{expired}(c) \quad \text{strongAlg}(c.\text{alg}) \quad c.\text{issuerSpki} = \text{caSpki} \quad \text{resolveUser}(c) = r.\text{user}}{\text{caSpki}, r \vdash c \Downarrow_{\text{auth}} \mathsf{Granted}(\text{resolveUser}(c), c.\text{certFp}, r.\text{readOnly})}$$
   - **Wire Frame Codec Serialization & Deserialization ($\Downarrow_{\text{codec}}$)**:
     $$\frac{|\text{payload}| \le 64\text{ MiB} \quad \text{hdr} = \langle \texttt{0x5432}, \text{type}, \text{flags}, |\text{payload}| \rangle}{\langle \text{type}, \text{flags}, \text{payload} \rangle \Downarrow_{\text{encode}} \langle \text{hdr}, \text{payload} \rangle \Downarrow_{\text{decode}} \langle \text{type}, \text{flags}, \text{payload} \rangle}$$
   - **Binary Split Layout Tree Spatial Partitioning ($\Downarrow_{\text{layout}}$)**:
     $$\frac{w_1 = \min(w_L, W) \quad s = [W > w_1] \quad w_2 = W - w_1 - s \quad \langle L_1, (X, Y, w_1, H) \rangle \Downarrow_{\text{layout}} M_1 \quad \langle L_2, (X+w_1+s, Y, w_2, H) \rangle \Downarrow_{\text{layout}} M_2}{\langle \mathsf{HSplit}(w_L, L_1, L_2), (X, Y, W, H) \rangle \Downarrow_{\text{layout}} M_1 \cup M_2}$$
   - **Client Modal TUI Dispatch ($\Downarrow_{\text{ui}}$)**:
     $$\langle \mathsf{Normal}, \texttt{Ctrl-b} \rangle \Downarrow_{\text{ui}} \langle \mathsf{PrefixPending}, \mathsf{None} \rangle, \qquad \langle \mathsf{PrefixPending}, \texttt{'s'} \rangle \Downarrow_{\text{ui}} \langle \mathsf{SessionChooser}, \mathsf{OpenSessionList} \rangle$$
     $$\langle \mathsf{PrefixPending}, \texttt{'['} \rangle \Downarrow_{\text{ui}} \langle \mathsf{CopyMode}, \mathsf{FetchScrollback} \rangle, \qquad \langle \mathsf{CopyMode}, \texttt{'y'} \rangle \Downarrow_{\text{ui}} \langle \mathsf{Normal}, \mathsf{EmitOsc52Yank} \rangle$$

2. **Inter-Tick Distributed & Reactive Execution $\implies$ Small-Step Labelled Transition System ($\xrightarrow{\alpha}$)**:
   Across time, `tmux2d` and multiple `tmux2` clients form a **non-terminating reactive system** subject to asynchronous PTY output, concurrent multi-client attachments, network drops, and roaming reconnections:
   - **Fresh vs. Duplicate Keystroke Processing (`Step_Key_Forward_Fresh` / `Step_Key_Drop_Duplicate`)**:
     $$\frac{\text{seq} > \Sigma.\text{lastAckedCliSeq}}{\langle \Sigma, \sigma \rangle \xrightarrow{\mathsf{DeliverKey}(\text{seq}, k)} \langle \Sigma[\text{lastAckedCliSeq} \mapsto \text{seq}, \text{ptyLog} \mathrel{+}= [k]], \sigma \rangle}$$
     $$\frac{\text{seq} \le \Sigma.\text{lastAckedCliSeq}}{\langle \Sigma, \sigma \rangle \xrightarrow{\mathsf{DeliverKey}(\text{seq}, k)} \langle \Sigma, \sigma \rangle}$$
   - **Network Disconnection Hold (`Step_Transport_Disconnect`)**:
     $$\langle \Sigma, \sigma[\text{mode} = \mathsf{Normal}] \rangle \xrightarrow{\mathsf{NetworkDisconnect}} \langle \Sigma, \sigma[\text{mode} \mapsto \mathsf{Reconnecting}] \rangle$$
   - **Section 5 Reconnect via Delta Replay vs. Snapshot Fallback (`Step_Reconnect_Delta_CatchUp` / `Step_Reconnect_Snapshot_Fallback`)**:
     $$\frac{\sigma.\text{mode} = \mathsf{Reconnecting} \quad \sigma.\text{certFp} = \Sigma.\text{certFp} \quad \sigma.\text{resumeToken} = \Sigma.\text{resumeToken} \quad \text{applyRowDiffs}(\sigma.\text{grid}, \Delta) = \Sigma.\text{grid}}{\langle \Sigma, \sigma \rangle \xrightarrow{\mathsf{ReconnectDelta}(\Delta)} \langle \Sigma, \sigma[\text{mode} \mapsto \mathsf{Normal}, \text{grid} \mapsto \text{applyRowDiffs}(\sigma.\text{grid}, \Delta), \text{lastSeenSrvSeq} \mapsto \Sigma.\text{srvSeq}] \rangle}$$

---

## 3. Machine-Checked Theorems in `Tmux2Semantics.lean`

All **15 theorems** in [Tmux2Semantics.lean](./Tmux2Semantics.lean) are verified by Lean 4 with zero `sorry` axioms:

| # | Theorem Name | Subsystem | Verified Property |
| :--- | :--- | :--- | :--- |
| **1** | `mtls_auth_soundness` | PKI & mTLS (§3) | Weak keys (`weakRsa`) and untrusted CAs can never derive `AuthDecision.granted`. |
| **2** | `wire_codec_roundtrip` | Wire Codec (§4.2) | Any frame payload $\le 64\text{ MiB}$ encodes and decodes losslessly (`decode(encode(m)) = some m`). |
| **3** | `hsplit_width_conservation` | Layout Tree (§2.1) | Horizontal split child widths plus the 1-cell border sum strictly to parent width (`w1 + 1 + w2 = width`). |
| **4** | `zoomed_layout_fullscreen` | Layout Tree (§2.1) | A zoomed pane overrides the split tree and receives the full window bounding rectangle. |
| **5** | `scrollback_capacity_invariant` | VTE & Scrollback (§2.1) | Pushing scrolled-off lines into `ScrollbackBuffer` never exceeds `maxHistory` capacity. |
| **6** | `single_row_diff_convergence` | Dirty Tracker (§2.3) | Applying an emitted dirty-row diff to the client grid produces the exact server VTE row state. |
| **7** | `prefix_s_opens_session_chooser` | Client TUI (§6.2) | `Ctrl-b` then `s` enters `sessionChooser` without leaking keystrokes to the remote PTY. |
| **8** | `copy_mode_yank_returns_normal` | Copy Mode (§6.2) | `Ctrl-b [` enters `copyMode`, fetches scrollback, and `y` emits `OSC 52` yank and returns to `normal`. |
| **9** | `attach_detach_others_singleton` | Multi-Client (§2.3) | Attaching with `detachOthers = true` (`attach -d`) guarantees `attachedClients = [c]`. |
| **10** | `read_only_client_never_resizes` | Multi-Client (§5.2) | Spectator (`readOnly = true`) interactions never resize the session or child PTYs. |
| **11** | `interactive_client_resizes_latest` | Multi-Client (§5.2) | Active client (`readOnly = false`) interaction dynamically resizes the session (`window-size = latest`). |
| **12** | `duplicate_key_dropped_idempotently` | Section 5 (§5.2) | Replaying an already acknowledged `(seq, ch)` is strictly idempotent (`processClientKey srv1 seq ch = srv1`). |
| **13** | `client_seq_monotonic` | Section 5 (§5.2) | Server `lastAckedCliSeq` is monotonically non-decreasing across all input deliveries. |
| **14** | `disconnect_preserves_screen_and_keys` | Section 5 (§5.2) | Transport disconnection preserves the client's visible `grid` and `pendingKeys` buffer. |
| **15** | `reconnect_delta_snapshot_equivalence` | Section 5 (§5.2) | Catching up via `FrameDiff` ring-buffer delta replay (`reconnectDelta`) is observationally equivalent to full `ScreenSnapshot` fallback (`reconnectSnapshot`). |

---

## 4. How to Run the Formal Verification

```bash
# Verify all 15 machine-checked theorems in Lean 4 (zero errors, zero sorrys):
~/.elan/bin/lean spec/Tmux2Semantics.lean
```
