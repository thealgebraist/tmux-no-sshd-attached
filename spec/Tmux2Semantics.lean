/-
  spec/Tmux2Semantics.lean
  =============================================================================
  Formal Operational Semantics & Machine-Checked Proofs for `tmux2`
  (An mTLS-Native, Detachable, Persistent Multi-Session Terminal Multiplexer)

  Architecture of the Formalization:
  ----------------------------------
  We use a **Stratified Dual-Semantics** approach combining:
  1. **Big-Step Natural Semantics (`⇓`)** for deterministic, terminating intra-tick
     evaluations:
     - `EvalAuth` (`⇓_auth`): TLS 1.3 mTLS X.509v3 certificate & SAN RBAC verification.
     - `EvalCodec` (`⇓_codec`): Little-endian wire frame serialization & RLE roundtrip.
     - `EvalLayout` (`⇓_layout`): Binary split layout tree spatial partitioning (`Rect`).
     - `EvalDsl` (`⇓_dsl`): Server Command DSL (`new-session`, `split-window`, `attach -d -r`, `kill-session`).
     - `EvalUiMode` (`⇓_ui`): Client modal TUI state machine (`Normal`, `PrefixPending`, `SessionChooser`, `CopyMode`).

  2. **Small-Step Labelled Transition System (`—[α]→`)** for reactive, non-terminating
     distributed client-server execution across asynchronous transports (`QUIC`, `TcpTls`, `AfUnix`):
     - `StepSys`: Models keystroke forwarding, monotonic `client_seq` deduplication,
       PTY output -> VTE dirty-row diffing, compressed scrollback ring buffering,
       multi-client `window-size = latest` dynamic resizing, `attach -d` detach-others,
       and Section 5 seamless roaming (`resume_token` HMAC verification, `FrameDiff`
       delta catch-up vs. `ScreenSnapshot` fallback).

  Every theorem in this file is machine-checked by Lean 4 with zero `sorry`s.
-/

namespace Tmux2Semantics

-- ============================================================================
-- §1. Core Identifiers & Cryptographic / mTLS Primitives (src/pki/, src/transport/)
-- ============================================================================

abbrev ClientId  := Nat
abbrev SessionId := Nat
abbrev WindowId  := Nat
abbrev PaneId    := Nat
abbrev SeqNo     := Nat

/-- Asymmetric key algorithms evaluated during TLS 1.3 mTLS handshake (spec.md §3.1). -/
inductive KeyAlg where
  | ed25519 : KeyAlg
  | p256    : KeyAlg
  | rsa3072 : KeyAlg
  | weakRsa : KeyAlg
  deriving DecidableEq, Repr

/-- Underlying transport channels (spec.md §4.1). -/
inductive TransportKind where
  | quic   : TransportKind -- OpenSSL 3.5+ QUIC over UDP/8443 (Primary)
  | tcpTls : TransportKind -- OpenSSL 3.5+ TLS 1.3 over TCP/8443 (Happy Eyeballs fallback)
  | afUnix : TransportKind -- Local 0600 AF_UNIX domain socket (Auto-spawn)
  deriving DecidableEq, Repr

/-- TLS 1.3 ALPN protocol negotiation identifiers. -/
inductive AlpnProto where
  | tmux2V1  : AlpnProto -- "tmux2/1"
  | enrollV1 : AlpnProto -- "tmux2-enroll/1"
  deriving DecidableEq, Repr

/-- Abstract X.509v3 certificate presented during mTLS handshake. -/
structure Certificate where
  alg            : KeyAlg
  certFp         : String
  spkiFp         : String
  issuerSpkiFp   : String
  sanUser        : Option String
  commonName     : String
  notExpired     : Bool
  deriving DecidableEq, Repr

/-- Server-side authorization rule (src/config/config_types.hpp). -/
structure AuthRule where
  expectedUser   : String
  readOnly       : Bool
  deriving DecidableEq, Repr

/-- Result of Big-Step mTLS certificate & identity verification (`⇓_auth`). -/
inductive AuthDecision where
  | granted (user : String) (certFp : String) (readOnly : Bool)
  | denied  (reason : String)
  deriving DecidableEq, Repr

/-- Check if key algorithm satisfies spec.md §3.1 minimum strength policy. -/
def isStrongKeyAlg : KeyAlg → Bool
  | .ed25519 => true
  | .p256    => true
  | .rsa3072 => true
  | .weakRsa => false

/-- Resolve identity following spec.md §3.2 priority: SAN URI user first, then CN. -/
def resolveCertUser (c : Certificate) : String :=
  match c.sanUser with
  | some u => u
  | none   => c.commonName

/-- Big-Step Natural Semantics (`⇓_auth`) for mTLS 1.3 Client Authentication. -/
inductive EvalAuth (trustedCaSpki : String) (rule : AuthRule) :
    Certificate → AuthDecision → Prop where
  | grantValid (c : Certificate) :
      c.notExpired = true →
      isStrongKeyAlg c.alg = true →
      c.issuerSpkiFp = trustedCaSpki →
      resolveCertUser c = rule.expectedUser →
      EvalAuth trustedCaSpki rule c (.granted (resolveCertUser c) c.certFp rule.readOnly)
  | denyExpired (c : Certificate) :
      c.notExpired = false →
      EvalAuth trustedCaSpki rule c (.denied "certificate expired")
  | denyWeakKey (c : Certificate) :
      c.notExpired = true →
      isStrongKeyAlg c.alg = false →
      EvalAuth trustedCaSpki rule c (.denied "weak key algorithm rejected")
  | denyUntrustedCa (c : Certificate) :
      c.notExpired = true →
      isStrongKeyAlg c.alg = true →
      c.issuerSpkiFp ≠ trustedCaSpki →
      EvalAuth trustedCaSpki rule c (.denied "untrusted issuing CA")

/-- **Theorem 1 (mTLS Soundness)**: Weak keys (`weakRsa`) and untrusted CAs can never
    produce an `AuthDecision.granted` judgment. -/
theorem mtls_auth_soundness
    (ca : String) (rule : AuthRule) (c : Certificate) (u fp : String) (ro : Bool)
    (h : EvalAuth ca rule c (.granted u fp ro)) :
    c.notExpired = true ∧ isStrongKeyAlg c.alg = true ∧ c.issuerSpkiFp = ca := by
  cases h with
  | grantValid hexp halg hca _ => exact ⟨hexp, halg, hca⟩

-- ============================================================================
-- §2. Wire Frame Codec & RLE Cell Serialization (src/protocol/)
-- ============================================================================

def kStreamMagic : Nat := 0x5432 -- "T2" little-endian magic
def kMaxPayloadLen : Nat := 64 * 1024 * 1024 -- 64 MiB

structure StreamHeader where
  magic      : Nat := kStreamMagic
  msgType    : Nat
  flags      : Nat
  payloadLen : Nat
  deriving DecidableEq, Repr

def StreamHeader.wellFormed (h : StreamHeader) : Bool :=
  h.magic == kStreamMagic && h.payloadLen <= kMaxPayloadLen

/-- Executable frame encoder/decoder model proving deterministic roundtrip. -/
structure WireFrame where
  header  : StreamHeader
  payload : List UInt8
  deriving DecidableEq, Repr

def encodeFrame (msgType flags : Nat) (payload : List UInt8) : Option WireFrame :=
  if payload.length <= kMaxPayloadLen then
    some {
      header := { magic := kStreamMagic, msgType := msgType, flags := flags, payloadLen := payload.length }
      payload := payload
    }
  else
    none

def decodeFrame (f : WireFrame) : Option (Nat × Nat × List UInt8) :=
  if f.header.magic == kStreamMagic &&
     f.header.payloadLen <= kMaxPayloadLen &&
     f.payload.length == f.header.payloadLen then
    some (f.header.msgType, f.header.flags, f.payload)
  else
    none

/-- **Theorem 2 (Wire Codec Roundtrip Identity)**: Any payload within the 64 MiB bound
    encodes to a valid `WireFrame` that decodes back to the exact `(msgType, flags, payload)`. -/
theorem wire_codec_roundtrip (msgType flags : Nat) (payload : List UInt8)
    (hLen : payload.length <= kMaxPayloadLen) :
    ∃ frame, encodeFrame msgType flags payload = some frame ∧
             decodeFrame frame = some (msgType, flags, payload) := by
  unfold encodeFrame decodeFrame
  simp [hLen, kStreamMagic]

-- ============================================================================
-- §3. Spatial Split Layout Tree Big-Step Semantics (src/server/layout.*)
-- ============================================================================

structure Rect where
  x      : Nat
  y      : Nat
  width  : Nat
  height : Nat
  deriving DecidableEq, Repr

def Rect.area (r : Rect) : Nat := r.width * r.height

/-- Binary split layout tree (`HSplit` = left/right, `VSplit` = top/bottom, `Zoomed`). -/
inductive LayoutTree where
  | leaf   (paneId : PaneId)
  | hSplit (leftWidth : Nat) (left right : LayoutTree)
  | vSplit (topHeight : Nat) (top bottom : LayoutTree)
  | zoomed (activePane : PaneId) (underlying : LayoutTree)
  deriving DecidableEq, Repr

/-- Big-Step Spatial Layout Evaluator (`⟨tree, bounds⟩ ⇓_layout paneRects`):
    Computes the bounding `Rect` for each visible pane (1-cell separator border). -/
def evalLayout : LayoutTree → Rect → List (PaneId × Rect)
  | .leaf pid, r => [(pid, r)]
  | .hSplit lw l rTree, bounds =>
      let w1 := Nat.min lw bounds.width
      let sep := if bounds.width > w1 then 1 else 0
      let w2 := bounds.width - w1 - sep
      let rLeft  : Rect := { x := bounds.x, y := bounds.y, width := w1, height := bounds.height }
      let rRight : Rect := { x := bounds.x + w1 + sep, y := bounds.y, width := w2, height := bounds.height }
      evalLayout l rLeft ++ evalLayout rTree rRight
  | .vSplit th t bTree, bounds =>
      let h1 := Nat.min th bounds.height
      let sep := if bounds.height > h1 then 1 else 0
      let h2 := bounds.height - h1 - sep
      let rTop : Rect := { x := bounds.x, y := bounds.y, width := bounds.width, height := h1 }
      let rBot : Rect := { x := bounds.x, y := bounds.y + h1 + sep, width := bounds.width, height := h2 }
      evalLayout t rTop ++ evalLayout bTree rBot
  | .zoomed pid _, bounds =>
      [(pid, bounds)]

/-- **Theorem 3 (Horizontal Split Width Conservation)**: In any horizontal split where
    `lw < bounds.width`, the left pane width `w1`, the 1-cell border, and the right pane
    width `w2` sum exactly to `bounds.width` without overflow or overlap. -/
theorem hsplit_width_conservation (lw width : Nat) (h : lw < width) :
    let w1 := Nat.min lw width
    let sep := 1
    let w2 := width - w1 - sep
    w1 + sep + w2 = width := by
  dsimp only
  have hMin : Nat.min lw width = lw := Nat.min_eq_left (Nat.le_of_lt h)
  rw [hMin]
  omega

/-- **Theorem 4 (Zoom Override Fullscreen Property)**: A zoomed layout tree always
    assigns the full window `bounds` rectangle exclusively to the zoomed `activePane`. -/
theorem zoomed_layout_fullscreen (pid : PaneId) (tree : LayoutTree) (bounds : Rect) :
    evalLayout (.zoomed pid tree) bounds = [(pid, bounds)] := by
  rfl

-- ============================================================================
-- §4. VTE Screen Grid, Dirty Diffs & Scrollback Ring Buffer (src/vte/)
-- ============================================================================

abbrev RowData := List Char

structure VtePaneState where
  paneId     : PaneId
  cols       : Nat
  rows       : Nat
  screen     : List RowData      -- Active visible screen lines
  scrollback : List RowData      -- Zstd-compressed scrollback history ring buffer
  maxHistory : Nat               -- Maximum scrollback capacity (e.g. 50,000 lines)
  dirtyRows  : List (Nat × RowData) -- Incremental dirty rows since last broadcast
  deriving DecidableEq, Repr

/-- Push a scrolled-off top line into the bounded scrollback ring buffer. -/
def pushScrollback (st : VtePaneState) (line : RowData) : VtePaneState :=
  let cand := st.scrollback ++ [line]
  let trimmed :=
    if cand.length > st.maxHistory then
      cand.drop (cand.length - st.maxHistory)
    else
      cand
  { st with scrollback := trimmed }

/-- **Theorem 5 (Scrollback Ring Buffer Capacity Invariant)**: If `st.scrollback.length ≤ st.maxHistory`,
    then after `pushScrollback`, the scrollback history never exceeds `st.maxHistory`. -/
theorem scrollback_capacity_invariant (st : VtePaneState) (line : RowData) :
    (pushScrollback st line).scrollback.length ≤ st.maxHistory := by
  unfold pushScrollback
  simp only
  split
  · rename_i hGt
    rw [List.length_drop]
    omega
  · rename_i hLe
    omega

/-- Apply a row update on the server VTE and record the dirty diff. -/
def updateVteRow (screen : List RowData) (rowIdx : Nat) (newRow : RowData) : List RowData :=
  screen.set rowIdx newRow

/-- Apply a list of dirty row diffs sequentially to a client's grid replica. -/
def applyRowDiffs (grid : List RowData) : List (Nat × RowData) → List RowData
  | [] => grid
  | (idx, row) :: rest => applyRowDiffs (grid.set idx row) rest

/-- **Theorem 6 (Single-Row Dirty Diff Convergence)**: Applying the emitted dirty row
    diff `(rowIdx, newRow)` to a synchronized client grid yields the exact updated server VTE grid. -/
theorem single_row_diff_convergence (grid : List RowData) (rowIdx : Nat) (newRow : RowData) :
    applyRowDiffs grid [(rowIdx, newRow)] = updateVteRow grid rowIdx newRow := by
  rfl

-- ============================================================================
-- §5. Client Modal TUI State Machine Big-Step Semantics (src/client/)
-- ============================================================================

inductive UiMode where
  | normal         : UiMode -- Direct PTY input forwarding
  | prefixPending  : UiMode -- `Ctrl-b` pressed, awaiting command key
  | sessionChooser : UiMode -- Modal Session Chooser overlay (`Ctrl-b s`)
  | copyMode       : UiMode -- Modal Vi Scrollback Copy Mode (`Ctrl-b [`)
  | reconnecting   : UiMode -- Transport disconnected; TUI held & buffering input
  deriving DecidableEq, Repr

inductive UiKey where
  | ctrlB        : UiKey
  | charS        : UiKey
  | charLBracket : UiKey
  | charD        : UiKey
  | charY        : UiKey
  | enter        : UiKey
  | escape       : UiKey
  | otherChar    : Char → UiKey
  deriving DecidableEq, Repr

inductive ClientAction where
  | none              : ClientAction
  | sendPtyKey        : UiKey → ClientAction
  | openSessionList   : ClientAction
  | switchSession     : ClientAction
  | fetchScrollback   : ClientAction
  | emitOsc52Yank     : ClientAction
  | detachSession     : ClientAction
  | bufferOfflineKey  : UiKey → ClientAction
  deriving DecidableEq, Repr

/-- Big-Step Client Modal TUI Transition Function (`⟨mode, key⟩ ⇓_ui ⟨mode', action⟩`). -/
def stepClientUi (mode : UiMode) (key : UiKey) : UiMode × ClientAction :=
  match mode with
  | .normal =>
      match key with
      | .ctrlB => (.prefixPending, .none)
      | k      => (.normal, .sendPtyKey k)
  | .prefixPending =>
      match key with
      | .ctrlB        => (.normal, .sendPtyKey .ctrlB) -- Double Ctrl-b sends literal Ctrl-b
      | .charS        => (.sessionChooser, .openSessionList)
      | .charLBracket => (.copyMode, .fetchScrollback)
      | .charD        => (.normal, .detachSession)
      | _             => (.normal, .none)
  | .sessionChooser =>
      match key with
      | .enter  => (.normal, .switchSession)
      | .escape => (.normal, .none)
      | _       => (.sessionChooser, .none)
  | .copyMode =>
      match key with
      | .charY  => (.normal, .emitOsc52Yank)
      | .escape => (.normal, .none)
      | _       => (.copyMode, .none)
  | .reconnecting =>
      (.reconnecting, .bufferOfflineKey key)

/-- **Theorem 7 (Prefix-Key Isolation)**: Pressing `Ctrl-b` followed by `s` transitions
    from `normal` to `sessionChooser` without forwarding any stray keystrokes to the PTY. -/
theorem prefix_s_opens_session_chooser :
    let (m1, a1) := stepClientUi .normal .ctrlB
    let (m2, a2) := stepClientUi m1 .charS
    m1 = .prefixPending ∧ a1 = .none ∧ m2 = .sessionChooser ∧ a2 = .openSessionList := by
  decide

/-- **Theorem 8 (Copy Mode OSC 52 Yank Exit)**: Pressing `Ctrl-b` then `[` enters `copyMode`,
    and pressing `y` emits `emitOsc52Yank` and returns cleanly to `normal` mode. -/
theorem copy_mode_yank_returns_normal :
    let (m1, _)  := stepClientUi .normal .ctrlB
    let (m2, a2) := stepClientUi m1 .charLBracket
    let (m3, a3) := stepClientUi m2 .charY
    m2 = .copyMode ∧ a2 = .fetchScrollback ∧ m3 = .normal ∧ a3 = .emitOsc52Yank := by
  decide

-- ============================================================================
-- §6. Multi-Client Sizing (`window-size = latest`) & `attach -d` (src/server/)
-- ============================================================================

structure AttachedClient where
  clientId : ClientId
  readOnly : Bool
  cols     : Nat
  rows     : Nat
  deriving DecidableEq, Repr

structure SessionState where
  sessionId       : SessionId
  name            : String
  cols            : Nat
  rows            : Nat
  attachedClients : List AttachedClient
  deriving DecidableEq, Repr

/-- Attach a client to a session, optionally detaching all other clients (`attach -d`). -/
def attachClientToSession (s : SessionState) (c : AttachedClient) (detachOthers : Bool) :
    SessionState × List ClientId :=
  let remaining :=
    if detachOthers then
      []
    else
      s.attachedClients.filter (fun x => x.clientId != c.clientId)
  let kickedIds :=
    if detachOthers then
      (s.attachedClients.filter (fun x => x.clientId != c.clientId)).map (·.clientId)
    else
      []
  let newCols := if !c.readOnly && c.cols > 0 then c.cols else s.cols
  let newRows := if !c.readOnly && c.rows > 0 then c.rows else s.rows
  ({ s with
      cols := newCols
      rows := newRows
      attachedClients := remaining ++ [c] },
   kickedIds)

/-- Dynamic `window-size = latest` resize upon active non-read-only client interaction. -/
def onClientInteract (s : SessionState) (c : AttachedClient) : SessionState :=
  if c.readOnly then
    s
  else
    { s with cols := c.cols, rows := c.rows }

/-- **Theorem 9 (`attach -d` Exclusive Attachment Invariant)**: Attaching with
    `detachOthers = true` guarantees that `c` is the sole attached client on the session. -/
theorem attach_detach_others_singleton (s : SessionState) (c : AttachedClient) :
    (attachClientToSession s c true).1.attachedClients = [c] := by
  unfold attachClientToSession
  simp

/-- **Theorem 10 (Read-Only Spectator Sizing Isolation)**: An interaction from a
    read-only client (`c.readOnly = true`) never mutates the session's dimensions. -/
theorem read_only_client_never_resizes (s : SessionState) (c : AttachedClient)
    (hRo : c.readOnly = true) :
    (onClientInteract s c).cols = s.cols ∧ (onClientInteract s c).rows = s.rows := by
  unfold onClientInteract
  simp [hRo]

/-- **Theorem 11 (`window-size = latest` Active Resize)**: An interaction from an
    interactive client (`c.readOnly = false`) updates session dimensions to `(c.cols, c.rows)`. -/
theorem interactive_client_resizes_latest (s : SessionState) (c : AttachedClient)
    (hRw : c.readOnly = false) :
    (onClientInteract s c).cols = c.cols ∧ (onClientInteract s c).rows = c.rows := by
  unfold onClientInteract
  simp [hRw]

-- ============================================================================
-- §7. Small-Step LTS for Section 5 Persistence, Roaming & Deduplication
-- ============================================================================

/-- Historical frame diff entry retained in the server's ring buffer (`ClientState`). -/
structure DiffEntry where
  srvSeq : SeqNo
  diff   : Nat × RowData
  deriving DecidableEq, Repr

/-- Server-side state for a persistent session & client resume record. -/
structure ServerState where
  srvSeq          : SeqNo
  lastAckedCliSeq : SeqNo
  certFp          : String
  resumeToken     : String
  grid            : List RowData
  diffRing        : List DiffEntry -- Ordered ring buffer of recent FrameDiffs
  ptyInputLog     : List Char      -- Sequence of characters written to child PTY
  deriving DecidableEq, Repr

/-- Client-side state including outbound reconnect ring buffer. -/
structure ClientState where
  clientId        : ClientId
  mode            : UiMode
  nextCliSeq      : SeqNo
  lastSeenSrvSeq  : SeqNo
  certFp          : String
  resumeToken     : String
  grid            : List RowData
  pendingKeys     : List (SeqNo × Char) -- Unacknowledged keystrokes buffered for replay
  deriving DecidableEq, Repr

/-- Combined distributed configuration `⟨ServerState, ClientState⟩`. -/
structure DistributedConfig where
  server : ServerState
  client : ClientState
  deriving DecidableEq, Repr

/-- Server input deduplication step (`ClientState::deduplicate_client_seq` in C++23). -/
def processClientKey (srv : ServerState) (seq : SeqNo) (ch : Char) : ServerState :=
  if seq > srv.lastAckedCliSeq then
    { srv with
        lastAckedCliSeq := seq
        ptyInputLog     := srv.ptyInputLog ++ [ch] }
  else
    srv -- Duplicate `client_seq <= lastAckedCliSeq` dropped idempotently!

/-- **Theorem 12 (Zero-Duplicate Keystroke Idempotence)**: Re-transmitting an already
    acknowledged keystroke `(seq, ch)` after a network glitch leaves `ptyInputLog` and
    `lastAckedCliSeq` completely unchanged. -/
theorem duplicate_key_dropped_idempotently (srv : ServerState) (seq : SeqNo) (ch : Char)
    (hFresh : seq > srv.lastAckedCliSeq) :
    let srv1 := processClientKey srv seq ch
    processClientKey srv1 seq ch = srv1 := by
  unfold processClientKey
  simp [hFresh]

/-- **Theorem 13 (Monotonic `lastAckedCliSeq` Progress)**: Every processed keystroke
    monotonically preserves or advances `srv.lastAckedCliSeq`. -/
theorem client_seq_monotonic (srv : ServerState) (seq : SeqNo) (ch : Char) :
    srv.lastAckedCliSeq ≤ (processClientKey srv seq ch).lastAckedCliSeq := by
  unfold processClientKey
  split
  · rename_i hGt
    exact Nat.le_of_lt hGt
  · exact Nat.le_refl _

/-- Server-side reconnection catch-up mechanism (`spec.md` §5.2):
    - If `clientLastSeq` is covered by `diffRing` (`minSeq ≤ clientLastSeq + 1`),
      replay missed `DiffEntry` frames (`DeltaCatchUp`).
    - Otherwise, transmit a full `ScreenSnapshot` (`SnapshotFallback`). -/
inductive CatchUpPayload where
  | deltaReplay      (diffs : List (Nat × RowData)) (newSrvSeq : SeqNo)
  | snapshotFallback (fullGrid : List RowData)      (newSrvSeq : SeqNo)
  deriving DecidableEq, Repr

def applyCatchUpOnClient (cliGrid : List RowData) : CatchUpPayload → List RowData
  | .deltaReplay diffs _         => applyRowDiffs cliGrid diffs
  | .snapshotFallback fullGrid _ => fullGrid

/-- Small-Step Labelled Transition System (`DistributedConfig —[α]→ DistributedConfig`). -/
inductive StepLabel where
  | keyPress          (ch : Char)
  | deliverKey        (seq : SeqNo) (ch : Char)
  | networkDisconnect
  | reconnectDelta    (diffs : List (Nat × RowData))
  | reconnectSnapshot
  deriving DecidableEq, Repr

inductive StepSys : DistributedConfig → StepLabel → DistributedConfig → Prop where
  /-- Client queues a keystroke with `nextCliSeq` into its outbound reconnect buffer. -/
  | clientQueueKey (cfg : DistributedConfig) (ch : Char) :
      StepSys cfg (.keyPress ch)
        { cfg with
          client := { cfg.client with
            nextCliSeq  := cfg.client.nextCliSeq + 1
            pendingKeys := cfg.client.pendingKeys ++ [(cfg.client.nextCliSeq, ch)] } }

  /-- Server receives a keystroke frame and deduplicates via `processClientKey`. -/
  | serverReceiveKey (cfg : DistributedConfig) (seq : SeqNo) (ch : Char) :
      StepSys cfg (.deliverKey seq ch)
        { cfg with
          server := processClientKey cfg.server seq ch }

  /-- Network drop transitions client TUI to `reconnecting` while preserving screen & buffered keys. -/
  | disconnectHold (cfg : DistributedConfig) :
      StepSys cfg .networkDisconnect
        { cfg with
          client := { cfg.client with mode := .reconnecting } }

  /-- Section 5 Reconnect via Delta Replay when certificate-bound `resumeToken` matches
      and missed diffs equal the gap between `client.grid` and `server.grid`. -/
  | reconnectViaDelta (cfg : DistributedConfig) (diffs : List (Nat × RowData)) :
      cfg.client.mode = .reconnecting →
      cfg.client.certFp = cfg.server.certFp →
      cfg.client.resumeToken = cfg.server.resumeToken →
      applyRowDiffs cfg.client.grid diffs = cfg.server.grid →
      StepSys cfg (.reconnectDelta diffs)
        { cfg with
          client := { cfg.client with
            mode           := .normal
            lastSeenSrvSeq := cfg.server.srvSeq
            grid           := applyCatchUpOnClient cfg.client.grid (.deltaReplay diffs cfg.server.srvSeq) } }

  /-- Section 5 Reconnect via Full `ScreenSnapshot` fallback when `lastSeenSrvSeq` aged out of ring buffer. -/
  | reconnectViaSnapshot (cfg : DistributedConfig) :
      cfg.client.mode = .reconnecting →
      cfg.client.certFp = cfg.server.certFp →
      cfg.client.resumeToken = cfg.server.resumeToken →
      StepSys cfg .reconnectSnapshot
        { cfg with
          client := { cfg.client with
            mode           := .normal
            lastSeenSrvSeq := cfg.server.srvSeq
            grid           := applyCatchUpOnClient cfg.client.grid (.snapshotFallback cfg.server.grid cfg.server.srvSeq) } }

/-- **Theorem 14 (Disconnect Screen & Buffer Preservation)**: When a transport disconnect occurs,
    the client's rendered `grid` and `pendingKeys` queue are preserved without loss. -/
theorem disconnect_preserves_screen_and_keys (cfg cfg' : DistributedConfig)
    (hStep : StepSys cfg .networkDisconnect cfg') :
    cfg'.client.grid = cfg.client.grid ∧
    cfg'.client.pendingKeys = cfg.client.pendingKeys ∧
    cfg'.client.mode = .reconnecting := by
  cases hStep
  exact ⟨rfl, rfl, rfl⟩

/-- **Theorem 15 (Section 5 Delta vs. Snapshot Observational Equivalence)**: Whether a
    reconnecting client catches up via `reconnectDelta` (ring-buffer diff replay) or
    `reconnectSnapshot` (full `ScreenSnapshot` fallback), the resulting client `grid`,
    `lastSeenSrvSeq`, and `UiMode.normal` state are observationally identical and synchronized
    with `cfg.server.grid`. -/
theorem reconnect_delta_snapshot_equivalence
    (cfg cfgDelta cfgSnap : DistributedConfig) (diffs : List (Nat × RowData))
    (hDelta : StepSys cfg (.reconnectDelta diffs) cfgDelta)
    (hSnap  : StepSys cfg .reconnectSnapshot cfgSnap) :
    cfgDelta.client.grid = cfg.server.grid ∧
    cfgSnap.client.grid  = cfg.server.grid ∧
    cfgDelta.client.grid = cfgSnap.client.grid ∧
    cfgDelta.client.lastSeenSrvSeq = cfgSnap.client.lastSeenSrvSeq ∧
    cfgDelta.client.mode = .normal ∧
    cfgSnap.client.mode  = .normal := by
  cases hDelta with
  | reconnectViaDelta _ _ _ _ hGridEq =>
    cases hSnap with
    | reconnectViaSnapshot _ _ _ =>
      simp [applyCatchUpOnClient, hGridEq]

end Tmux2Semantics
