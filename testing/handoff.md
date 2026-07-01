# Parallel-Download Testing Work — Handoff

Detailed handoff for the agent picking up this work. Everything you need to be productive without re-deriving context.

**Updated 2026-07-01** for the port of the harness onto upstream `master`. The feature under test has **merged into master** (PR #5209 "Enable downloading of transaction sets in parallel with early SCP stages", plus CAP-0083 base `6bd15f5f0`), but it was renamed and partially redesigned during review. The harness has been ported to the merged API. All line numbers in this document were re-verified against the ported tree; the rename map and design deltas below are the essential reading.

## Project context

- **Repo**: `/Users/brettboston/Documents/core/stellar-core/parallel-download`
- **Branch**: `component-tests-port` (based on `upstream/master` @ `d6f254679`, v27.1.0). The pre-port work lives on `component-tests-rebased` (harness squashed into `59e6b49ca`, sitting atop a pre-merge copy of the feature); treat that branch as a historical reference only.
- **Feature under test**: parallel transaction-set downloading — SCP nominates and ballots on tx-set hashes before the tx set itself arrives, falling back to an **empty-tx-set value** (pre-merge name: "skip value") if the tx set never arrives or arrives invalid.
- **Background docs**: `parallel-download-docs/` — `README.md`, `theory.md`, `implementation-plan.md`, `claude-learnings.md`, the SCP IETF draft, and the older `testing.md` design exploration. Note these predate the merge and use pre-merge terminology; apply the rename map below when reading them.
- **Port plan**: `~/.claude/plans/ethereal-hopping-patterson.md` records the port's evaluation, decisions, and assertion-disposition table.

## The rename map (pre-merge → merged)

The feature was renamed during review. The old docs, the old branch, and any memory of this project use the left column; current `master` uses the right.

| Pre-merge (old branch / docs)          | Merged (`master` / this branch)                                  |
|----------------------------------------|------------------------------------------------------------------|
| `kAwaitingDownload`                     | `kStructurallyValidValue`                                        |
| `kValidForPrepare`                      | also folded into `kStructurallyValidValue`                       |
| "skip value", `STELLAR_VALUE_SKIP`      | "empty-tx-set value", `STELLAR_VALUE_EMPTY_TX_SET`               |
| `maybeReplaceValueWithSkip`             | `maybeReplaceValueWithEmptyTxSet` (BallotProtocol.cpp:377)       |
| `makeSkipLedgerValueFromValue`          | `makeEmptyTxSetValueFromValue`                                   |
| `isSkipLedgerValue`                     | `isEmptyTxSetValue`                                              |
| `"SKIP:"` test-value prefix (5 bytes)   | `"EMPTY:"` test-value prefix (6 bytes)                           |
| `throwIfValueInvalidForCommit(v, caller)` | `throwIfValueInvalidForConfirmCommit(v)` (BallotProtocol.cpp:1441) |
| exc. `"SCP forced commit on locally-invalid value (slot=N, caller=...)"` | `"SCP confirm-commit on locally-invalid value (slot=N)"` (BallotProtocol.cpp:1466) |
| `skipify(scp, v)` test helper           | `toEmptyTxSet(scp, v)` (SCPTests.cpp:832)                        |
| `mDownloadWaitTimes`, `startDownload`, `clearDownload`, `onTxSetReceived`, `mPendingTxSetWrappers` | **unchanged** |

The merged `SCPDriver::ValidationLevel` enum (SCPDriver.h): `kInvalidValue = 0`, `kMaybeValidNotCurrentValue = 1`, `kStructurallyValidValue = 2`, `kFullyValidatedValue = 3`. **`kStructurallyValidValue` covers two distinct situations** — tx set still downloading, and tx set downloaded-but-invalid (recoverable via empty-tx-set) — which the pre-merge design expressed as separate levels. Production disambiguates at runtime via `getTxSetDownloadWaitTime()` (see `setConfirmPrepared`, BallotProtocol.cpp:1170).

## Design deltas in the merged feature (things the old harness assumed differently)

1. **Ingress rejection.** On master, `processEnvelope` rejects envelopes carrying locally-invalid values outright, and on the ungated path a peer CONFIRM referencing a `kStructurallyValidValue` value returns `SCP::EnvelopeState::INVALID` (see the dormant master test at SCPTests.cpp:4268). Consequence: the "forced commit on locally-invalid value" exception is now defense-in-depth rather than an expected artifact — see "Phase 3 closeout" item 2.
2. **The production gate moved.** Pre-merge docs said the CONFIRM/EXTERNALIZE gate lived in `HerderImpl::recvSCPEnvelope`. On master it is `HerderSCPDriver::isEnvelopeReady` (HerderSCPDriver.cpp:217–256): CONFIRM/EXTERNALIZE with missing tx sets are never "ready", so `PendingEnvelopes::recvSCPEnvelope` holds them at `ENVELOPE_STATUS_FETCHING` (PendingEnvelopes.cpp:416–440) and `HerderImpl::recvSCPEnvelope` only processes `READY` envelopes (HerderImpl.cpp:919/932). Parallel download applies only to NOMINATE/PREPARE, only for LCL+1, only when tracking and in sync.
3. **`setConfirmPrepared` handles `kStructurallyValidValue` via control flow, not an assert** (BallotProtocol.cpp:1170): it stalls `mCommit`, records how long balloting was blocked on the tx set, and lets the empty-tx-set replacement machinery take over. The old harness's blanket "never commit a kAwaitingDownload value" assertion has no direct equivalent and was dropped in the port.
4. **Metrics exist on master** — `{"scp", "empty-tx-set", "externalized"}` and `{"scp", "empty-tx-set", "value-replaced"}` counters (HerderSCPDriver.cpp:83–86), plus the blocked-on-txset timer recorded in `setConfirmPrepared`. Phase 4 (metric invariants) can hook these directly instead of inventing new ones.
5. **Compile guards.** The empty-tx-set API (`makeEmptyTxSetValueFromValue` declaration, `maybeReplaceValueWithEmptyTxSet` body) is `#ifdef CAP_0083` — a **mutable build flag** in this working copy (see Gotchas). The `kStructurallyValidValue` level, download-wait plumbing, and the gate are unguarded (always compiled). Master's own seven parallel-download TEST_CASEs (SCPTests.cpp:4213–4667) are guarded by `#ifdef CAP_0087`, which is defined **nowhere in the tree** — they are dormant. The ported tests deliberately use `CAP_0083`/unguarded so they actually run (see "Open issues" item 4).

## What's on `component-tests-port` (status after the port)

### Phase 1 — in-code assertions (partially landed; remainder deferred)

Disposition of the old Phase 1 set against master:

- **Already on master** (no action): `validateValueAgainstLocalState` postcondition — master asserts non-LCL+1 values are never `kStructurallyValidValue`/`kFullyValidatedValue` (HerderSCPDriver.cpp:512); `makeEmptyTxSetValueFromValue` precondition `releaseAssert(ext.v() == STELLAR_VALUE_SIGNED)` (HerderSCPDriver.cpp:~696).
- **Added in the port** (net-new): `extractValidValue` postcondition `releaseAssert(res != nullptr)` (HerderSCPDriver.cpp:654); `onTxSetReceived` entry asserts `txSet != nullptr` and `getContentsHash() == txSetHash` (HerderSCPDriver.cpp:1724–1725).
- **Deferred to a separate follow-up change** (explicit user decision, 2026-07-01): the `dbgAssert` → `releaseAssert` upgrades — `bumpToBallot` ×2 (~550/555), `checkInvariants` ×11 (743–774), `setConfirmPrepared` `dbgAssert(!mCommit)` (1163). These change release-build behavior for **all** SCP, so they should be reviewed on their own risk profile, not bundled with test infrastructure.
- **Dropped** (no longer map to the merged design): the `setConfirmPrepared` blanket reject (see design delta 3) and the `setConfirmCommit` belt-and-suspenders re-validation (redundant — `throwIfValueInvalidForConfirmCommit` is called at the top of `setConfirmCommit`, BallotProtocol.cpp:1681, its only call site).

The user's rule stands: **always `releaseAssert`, never debug-only `assert`/`dbgAssert`; gate behind `#ifdef BUILD_TESTS` if too expensive.**

### Phase 2 — SCP-level harness extensions (ported)

All in `src/scp/test/SCPTests.cpp`. Master's `TestSCP` already carried forward the download mock (`mDownloadWaitTimes`, `startDownload`:446, `clearDownload`:452, mock `validateValue` returning `kStructurallyValidValue` for in-download values, `makeEmptyTxSetValueFromValue` with the `"EMPTY:"` prefix, `isEmptyTxSetValue`), so the port sat on an already-compatible foundation.

- `QuorumFixture` (708) + `makeQuorumFixture(nodeCount, threshold)` (728) + `canonical3Node`/`canonical4Node`.
- `PeerProfile` (767) + `makePeerProfile` — thin forwarding to the existing `makeNominate`/`makePrepare`/`makeConfirm`/`makeExternalize`.
- `corruptEnvelope` (820) — still mostly symbolic (mock `signEnvelope` is a no-op).
- `toEmptyTxSet(scp, v)` (832, `#ifdef CAP_0083`) — the renamed `skipify`.
- **Herder-gate emulation** in `TestSCP`: `mBufferedEnvelopes` (295), `gatedReferencedValue` (302), `receiveEnvelopeGated` (398), and the `clearDownload` drain-and-replay (452). **Design change from the old branch — read this**: the gate is a **separate entry point** (`receiveEnvelopeGated`), not a modification of `receiveEnvelope`. Master's SCP-level unit tests deliver envelopes ungated and assert direct rejection (`EnvelopeState::INVALID`) for CONFIRMs referencing structurally-valid values; folding the gate into the shared `receiveEnvelope` would break them. The old "gate always on, no flag" intent survives where it matters: `runScenario` and all harness scenarios deliver exclusively through `receiveEnvelopeGated`.
- Hand-authored TEST_CASEs: **"Herder-gate emulation: CONFIRM/EXTERNALIZE buffered while awaiting tx-set download"** (4685, unguarded) — buffer two CONFIRMs, `clearDownload`, assert drain → externalize; **"Mixed peer profiles smoke test"** (4745, `CAP_0083`) — cooperative + sends-invalid + sends-empty-tx-set + sends-malformed in one scenario.

### Phase 3 — SCP-level scenario generator (ported; closeout items pending)

All in `SCPTests.cpp`'s anonymous namespace:

- `ScenarioEvent` (845) with the four kinds (`ReceiveEnvelope`, `TxSetArrives`, `FireBallotTimer`, `AdvanceTimerOffset`); `Scenario` alias; `runScenario` (903) with per-step `INFO` + `CLOG_DEBUG`, delivering via `receiveEnvelopeGated`.
- Generators: `generateCooperative` (941), `generateCooperativeButSlow` (969, `CAP_0083` — peers echo the empty-tx-set value), `generateCooperativeWithDownload` (995), `generateBiasedRandom` (1195), `thresholdFor` (1205).
- Perturbation operators (5 of 8 catalog bias targets, unchanged from the old branch): `shiftTxSetArrival`, `duplicateEnvelope`, `replaceWithYValue`, `corruptOneEnvelope`, `injectVBlockingConfirms`, composed by `applyRandomPerturbations` (~1020–1190).
- Logging: `formatPeerName` (1230), `formatValue` (1246 — unwraps the `"EMPTY:"` prefix, rendering `empty(xValue)`), `formatBallot`, `formatScenarioEvent`, `scenarioToString` (1350).
- TEST_CASEs: **"Generator: cooperative scenarios"** (4799), **"Generator: cooperative-but-slow scenarios"** (4819, `CAP_0083`), **"Generator: biased-random scenarios"** (4843; 50 scenarios per fixture × `GENERATE(3, 4, 5)`).
- The biased-random catch now matches master's message: `"SCP confirm-commit on locally-invalid value"`. Given design delta 1 (ingress rejection), this exception is **not expected to fire** via the current perturbations; the catch documents the contract as defense-in-depth.

### Build & test verification status

- Full `make -j $(nproc)` build on the ported branch: **clean** (2026-07-01, `CAP_0083` ON). All 5 ported TEST_CASEs registered in the binary (`./src/stellar-core test "[parallel-download]" --list-tests`).
- **Tests have not been run yet** — per the workflow, the user runs them. Suggested invocations:
  - `./src/stellar-core test "[parallel-download]"` — all five.
  - `./src/stellar-core test "[gen]" --rng-seed <n>` — generator tests, reproducible.
- First-run semantic risks to watch: cooperative-but-slow relies on `bumpState` replacing the timed-out download with an empty-tx-set value via `maybeReplaceValueWithEmptyTxSet`; the fuzzer's pass condition is "no in-code assertion fires" across seeds.

## Coverage overlap with master's existing tests

Master already ships seven targeted deterministic parallel-download tests — all **dormant** under `#ifdef CAP_0087` (SCPTests.cpp:4213–4667): "nomination times out structurally-valid value into empty tx set" (4214), "ballot protocol self-emits CONFIRM after federated accept-commit…" (4268), "drop tx set on download timeout" (4343), "Proper handling of non-current ledger value" (4478), "setConfirmPrepared stalls on kStructurallyValidValue value" (4504), "incoming PREPARE with structurally valid prepared value is accepted" (4597), "incoming PREPARE with non-tx-set-invalid value is dropped" (4632).

What the ported suite adds beyond those:

| Ported test | Master's nearest coverage | Net-new contribution |
|---|---|---|
| Herder-gate emulation | 4268 asserts ungated CONFIRM is *rejected* | The production hold-and-replay path (buffer → drain → externalize) |
| Generator: cooperative | assorted single-shot externalize tests | Same outcome via fixture/scenario machinery, across 3/4/5-node sizes |
| Generator: cooperative-but-slow | 4343 covers one 3-node case | Multi-size coverage + exercises the generator itself |
| Generator: biased-random | none | The fuzzer + perturbation catalog — the harness's core unique value |
| Mixed peer profiles | none | Five-profile catalog mixing (smoke) |

Because the ported tests use `CAP_0083`/unguarded, they are currently the **only active** parallel-download coverage in this build.

## Phase 3 closeout — pending before Phase 3 is "done"

1. **Diversify generated SCP statement shapes.** Unchanged from before: generators emit a single PREPARE shape (`ballot=(1,v) prepared=(1,v) nC=1 nH=1`) and a single CONFIRM shape. Generate multiple shapes per statement type (`h` set without `c`, `prepared'` set, distinct counters, …).
2. **Re-design the forced-commit catch check.** The old plan ("count peers voting-to-commit on the invalid value; accept the exception only if v-blocking") assumed the exception fires as a routine artifact. On master it likely **cannot** fire via the current perturbations — `replaceWithYValue` envelopes are rejected at ingress before they can drive `setAcceptCommit`. First verify empirically (run the fuzzer wide; log if the catch ever triggers), then either (a) delete the catch and let any such exception fail the test loudly, or (b) keep it with the justification check if some path still reaches it. Don't port the old justification-check design blindly.
3. **Add the three remaining catalog bias targets**: tx-set arrives after slot purge / ledger advance; previously-silent peer tips quorum at the moment of tx-set arrival; empty-tx-set value from a peer combined with a valid tx set received locally (`combineCandidates` path — `TestSCP::combineCandidates` is nomination-only and may need extending).

## Key file locations (re-verified on `component-tests-port`)

### Test infrastructure — `src/scp/test/SCPTests.cpp` (~4900 lines)
- `TestSCP` (38–~520): mock driver. Download mock: `validateValue` (75), `getTxSetDownloadWaitTime` (119) / `getTxSetDownloadTimeout` (130), `makeEmptyTxSetValueFromValue` (`CAP_0083`, 137), `isEmptyTxSetValue` (154), `startDownload` (446), `clearDownload` + drain (452). Gate: `mBufferedEnvelopes` (295), `gatedReferencedValue` (302), `receiveEnvelopeGated` (398). Test constants: `TX_SET_TIMEOUT{5000}` / `UNDER_TX_SET_TIMEOUT{1000}` / `OVER_TX_SET_TIMEOUT{6000}` (30–36).
- Anonymous namespace (~700–1360): `setupValues()` + `xValue`/`yValue`/`zValue`/`zzValue`/`kValue` globals; envelope builders + verify helpers; master's validation-override helpers (`xValueStructurallyValidValidationOverride` etc., ~672–699); then the ported Phase 2 + 3 helpers (708–1360, itemized above).
- TEST_CASEs: master's dormant `CAP_0087` block (4213–4667); ported cases (4685–end).

### Production code
- `src/scp/BallotProtocol.cpp` — `maybeReplaceValueWithEmptyTxSet` (377, body `CAP_0083`); `bumpToBallot` (541, dbgAsserts ~550/555); `checkInvariants` (735, dbgAsserts 743–774); `setConfirmPrepared` (1140; `dbgAssert(!mCommit)` 1163; `kStructurallyValidValue` handling 1170); `throwIfValueInvalidForConfirmCommit` (1441, message 1466, sole call site 1681); `setAcceptCommit` (1472); `setConfirmCommit` (1673).
- `src/herder/HerderSCPDriver.cpp` — empty-tx-set metrics (83–86); `isEnvelopeReady` gate (217–256); `validateValueAgainstLocalState` (404, postcondition 512); `extractValidValue` (637, ported assert 654); `makeEmptyTxSetValueFromValue` (691); `onTxSetReceived` (1721, ported asserts 1724–1725).
- `src/herder/PendingEnvelopes.cpp` — `recvSCPEnvelope` routing through `isEnvelopeReady` (416 → READY 431 / FETCHING 440).
- `src/herder/HerderImpl.cpp` — envelope status handling (919/932).
- `src/scp/SCPDriver.h` — `ValidationLevel` enum (~155); `makeEmptyTxSetValueFromValue` decl (`CAP_0083`, ~186).

### Plan files
- `~/.claude/plans/` — one file per plan-mode session. The port plan is `ethereal-hopping-patterson.md`.

## Important design decisions and rationale

### TestSCP is a mock, not the real driver
Unchanged in spirit. `mDownloadWaitTimes` is the source of truth for "is this value awaiting download?" — the mock `validateValue` maps membership to `kStructurallyValidValue`. `signEnvelope` is a no-op (signature corruption is symbolic). `makeEmptyTxSetValueFromValue` prepends `"EMPTY:"`; real production builds a `STELLAR_VALUE_EMPTY_TX_SET` XDR value. `getValueString` still won't work on test values (raw hashes, not `StellarValue`s). Phase 5 (Herder-level harness) exercises the real driver.

### The gate is a dedicated entry point, always used by the harness
The pre-port decision was "always emulate the herder gate, no flag." That intent survives, but the mechanism changed: master's own SCP unit tests assert *ungated* rejection behavior, so the gate lives in `receiveEnvelopeGated` and `receiveEnvelope` stays pristine. Every harness path (`runScenario`, the hand-authored cases) uses the gated method; do the same in new tests unless you are specifically testing ungated ingress rejection.

### Imperative scenario style, not declarative
Unchanged. Event lists via `ScenarioEvent::receive(...)` etc.; no DSL.

### Shrinking dropped; seeded RNG
Unchanged. `getGlobalRandomEngine()` from `util/Math.h`; Catch2 `--rng-seed` gives reproducibility.

### Guard choice for ported tests
Empty-tx-set-dependent tests (`Mixed peer profiles`, `Generator: cooperative-but-slow`) are `#ifdef CAP_0083`, matching the production API they call. Everything else is unguarded because the mechanisms it exercises (structurally-valid gating, download-wait plumbing) are unconditional in production. Deliberately **not** `CAP_0087` (see Open issues item 4).

## Conventions and patterns

- **Catch2 `GENERATE(3, 4, 5)`** re-runs the whole test body per fixture size.
- **`thresholdFor`**: 3→2, 4→3, 5→4; default `(2n+2)/3`.
- **`setupValues()` regenerates values per call** (seeded from `getGlobalRandomEngine()`); the biased-random loop calls it per iteration because perturbations are value-keyed.
- **Build**: `make -j $(nproc)` from the repo root — never run `./configure` (see Gotchas on the mutable flag). After adding a *new* file: `git add <file> && ./make-mks`. This port added no new source files.
- **Do not run tests as the agent** — the user runs them. Failing seed + iteration number reproduce via `--rng-seed <n>`.

## Gotchas

### CAP_0083 is a mutable build flag
This working copy is reconfigured between `--enable-next-protocol-version-unsafe-for-production` ON and OFF depending on what's being tested. When OFF, `CAP_0083` is undefined: the `CAP_0083`-guarded tests vanish from the binary and empty-tx-set values can never validate. **Verify before relying on it**: `grep "CAP_0083" src/Makefile` — a commented-out `-DCAP_0083` means OFF. It was ON as of 2026-07-01.

### Ungated vs gated delivery — pick deliberately
`receiveEnvelope` (ungated): a CONFIRM referencing a value in `mDownloadWaitTimes` is **rejected** (`EnvelopeState::INVALID`) — master's ingress behavior without a gate. `receiveEnvelopeGated`: the same envelope is **buffered** and replayed on `clearDownload` — production end-to-end behavior. A test asserting the wrong one will fail confusingly.

### `bumpTimerOffset` doesn't fire callbacks
Unchanged: it jumps virtual time by 5 hours without firing; fire manually via `auto cb = scp.getBallotProtocolTimer().mCallback; cb();`.

### `mValidateValueOverride` replaces default behavior entirely
To keep the awaiting-download default while adding overrides, replicate the `mDownloadWaitTimes` check — canonical pattern in the biased-random TEST_CASE (returns `kInvalidValue` for `yValue`, `kStructurallyValidValue` for in-download values, else `kFullyValidatedValue`).

### `attemptAcceptCommit` / `attemptConfirmCommit` run in the same `advanceSlot` pass
Still true. This is why the gate matters for testing the production path: without it, a CONFIRM quorum could drive accept-commit and confirm-commit in one pass. On master the ungated path defuses this differently (ingress rejection), so the gate's purpose in the harness is to reproduce production's *hold-and-replay*, not to prevent a crash.

### Quorum / v-blocking math at small sizes
Unchanged: 3-node t2 — any 2 v-blocking; 4-node t3 — any 2 of 4 v-blocking, v0+2 = quorum; 5-node t4 — any 2 v-blocking, v0+3 = quorum.

### Empty-tx-set values at TestSCP level
`"EMPTY:"`-prefixed bytes; `isEmptyTxSetValue` checks the prefix; the mock `validateValue` returns `kFullyValidatedValue` for them (not in `mDownloadWaitTimes`); they are not subject to the gate. `formatValue` renders them `empty(xValue)`.

### IDE diagnostics noise
clangd lacks include paths for these files (spurious "file not found" / "unknown type" / "no member" errors) and cSpell flags project jargon. `make` output is the sole source of truth for "does this build?". Build logs may also show harmless `xcrun_db` cache errors and a deprecated `-bind_at_load` linker warning on macOS.

## Open issues / TODOs

Items 1–3 of "Phase 3 closeout" above are the immediate priorities. Longer-horizon:

1. **Phase 4 not started** — metric / progress / terminal-state invariants. Master already exposes the `{"scp", "empty-tx-set", *}` counters and the blocked-on-txset timer; hook those rather than inventing parallel metrics.
2. **Phase 1 Herder-level assertions still only indirectly tested** — Phase 5 gives them targeted coverage.
3. **Scenario count tuning** — 50 × 3 fixtures per run; revisit once Phase 4 invariants land.
4. **CAP_0087 decision** — master's own parallel-download tests are dormant because `CAP_0087` is defined nowhere. Either wire `CAP_0087` into the build (and consider moving the ported tests under it for consistency) or fold that block's guard into `CAP_0083`. Worth raising with the feature authors before upstreaming.
5. **`dbgAssert` → `releaseAssert` follow-up** — the deferred upgrade set (see Phase 1 disposition) as its own reviewed change.
6. **`testing/` docs are untracked on this branch** — `git add testing/` when committing the port.

## Workflow with the user

- The user puts the agent in/out of plan mode explicitly. **Do not enter plan mode unprompted.**
- Each plan-mode session writes a fresh file under `~/.claude/plans/`; older plans accumulate as a session log.
- The user runs tests; the agent builds (`make -j $(nproc)`).
- Failing output → reproduce with `--rng-seed <n>` from the failing run, iteration number in the `INFO` context.
- Conversational drafts first, then formal plan mode, is the usual rhythm.

## Quick orientation: what to do first as the next agent

1. Read the **rename map** and **design deltas** above — they invalidate most pre-merge intuition, including anything recalled from earlier sessions on this project.
2. Confirm build state: `grep "CAP_0083" src/Makefile` (flag ON?), then `make -j $(nproc)`.
3. Ask whether the ported tests have been run yet. If not, that's the first gate: the user runs `./src/stellar-core test "[parallel-download]"` and you triage any failures (semantic risks listed under "Build & test verification status").
4. Skim the ported code: gate emulation (SCPTests.cpp 295–470), harness + generator (708–1360), TEST_CASEs (4685–end).
5. Then pick up "Phase 3 closeout" — item 2 (catch re-design) first requires only running the fuzzer and observing; items 1 and 3 are code work.
