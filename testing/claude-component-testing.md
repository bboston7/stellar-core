# Component-Level Testing Plan for Parallel Tx Set Downloading

This document describes the component-level testing strategy for the parallel tx set downloading SCP change. Component-level testing exercises a single node's SCP state machine with scripted sequences of SCP envelopes and tx set arrival events, and checks invariants over the node's local state.

See also: `claude-protocol-testing.md` for the multi-node (protocol-level) testing plan that builds on this work.

> **Status (2026-07-01)**: the feature **merged into upstream `master`** (PR #5209 + CAP-0083 base) with renames — "skip value" became **"empty-tx-set value"**, and `kAwaitingDownload`/`kValidForPrepare` folded into **`kStructurallyValidValue`**. Phases 2–3 of this plan are implemented and have been ported onto `master` (branch `component-tests-port`); Phase 1 landed partially (see the in-code assertion catalog below for per-item disposition). This document has been updated to merged terminology and current line numbers. **`testing/handoff.md` carries the authoritative rename map, design deltas, and work status** — read it first.

## Context and rationale

The original proposal in `parallel-download-docs/testing.md` split testing into component-level (ad-hoc manual tests) and protocol-level (MBT). In discussion we concluded that:

- Most of the invariants we care about for this feature are local to a single node's state (see concrete code paths in `parallel-download-docs/claude-learnings.md`).
- An invariant-checking approach (generate scenarios, check invariants, do not predict outcomes) is dramatically cheaper than building a predictive oracle and captures the safety / liveness / performance concerns we actually have.
- That invariant-checking approach is *more natural* at the component level than the ad-hoc approach originally proposed, because the "model" is just the node's SCP state machine. The generator produces envelope sequences and tx set arrival timings; the invariants are properties of the resulting state transitions.
- Component-level infrastructure is an order of magnitude cheaper than protocol-level, because it avoids multi-node determinism (message ordering, cross-node timer coordination, leader progression). This makes it the right starting point.

Component-level testing does not cover emergent cross-node properties (safety, liveness of the network as a whole). Those are deferred to the protocol-level plan.

## Goals

- Programmatically generate envelope sequences and tx set arrival timings that exercise the parallel downloading state transitions in a single node.
- Catch local invariant violations: forbidden state transitions, forbidden validation results, empty-tx-set-value construction bugs, reprocessing bugs, orphaned pending-state entries.
- Build up in-code assertions that continue to catch bugs in every future test run (component, protocol-level, and production).

## Non-goals

- Test multi-node safety, liveness, or consensus emergent behavior. That's protocol-level.
- Replace or remove existing ad-hoc SCP component tests. This plan is strictly additive: existing tests coexist with new work and may be enhanced but not removed.
- Build a predictive oracle for a single node's outcome.

## Approach

A test harness drives a single node's `HerderSCPDriver` / `BallotProtocol` / `NominationProtocol` with:

1. A sequence of synthetic SCP envelopes from simulated peers (controllable statement types, signatures, values).
2. A scripted schedule of tx set arrivals (simulating `HerderSCPDriver::onTxSetReceived` at specified points in the sequence).
3. A virtual clock for timer-based behavior (download timeout expiry, nomination leader timeout).

Invariants are checked in three forms:

1. **In-code assertions** — implemented with `releaseAssert` in implementation code. Running a scenario without crashing is the pass condition. These compound: they also run in protocol-level tests and in production. We use `releaseAssert` exclusively; debug-only `assert` is not used. If an assertion is too expensive to evaluate in a release build, gate it behind `#ifdef BUILD_TESTS` rather than demoting it.
2. **Metric-based invariants** — assertions over single-node metric values at the end of a scenario.
3. **Progress and terminal-state invariants** — the harness inspects the node's terminal state and classifies it, and asserts progress for scenarios produced by constructed cooperative generators.

## Invariant catalog

This is a starting list. Expect to extend it as we explore the state space.

### In-code assertions

Disposition against merged `master` is marked per item: **[on master]** the merged code already enforces it; **[added]** landed with the port (branch `component-tests-port`); **[deferred]** intentionally split into a follow-up change; **[dropped]** no longer maps to the merged design; **[open]** not yet implemented anywhere.

- **[dropped]** `BallotProtocol::setConfirmPrepared` commit-gate assertion ("never assign `mCommit` for a `kAwaitingDownload`/`kInvalidValue` value"). The merged design handles `kStructurallyValidValue` via control flow instead (BallotProtocol.cpp:1170): it stalls `mCommit`, records the blocked-on-txset time, and defers to the empty-tx-set replacement machinery — a blanket assert would reject a state the design deliberately supports. The residual assertable piece is the pre-existing `dbgAssert(!mCommit)` at 1163 → see the dbgAssert sweep below.
- **[on master]** `HerderSCPDriver::validateValueAgainstLocalState` (HerderSCPDriver.cpp:404): `kStructurallyValidValue` only for `LCL+1`. Master asserts the non-LCL+1 path never returns `kStructurallyValidValue`/`kFullyValidatedValue` (HerderSCPDriver.cpp:512), which also covers the old "validatePastOrFutureValue never returns it" item.
- **[open]** Once a node has received and successfully validated a tx set for the current slot, subsequent calls to `validateValue` for that value must never return `kStructurallyValidValue`.
- **[open]** Any `STELLAR_VALUE_EMPTY_TX_SET` value received from the network must carry a valid inner `lcValueSignature` covering `(networkID, ENVELOPE_TYPE_SCPVALUE, originalTxSetHash, closeTime)`.
- **[on master]** Empty-tx-set values reaching validation must have `previousLedgerHash` / `previousLedgerVersion` matching the node's LCL (enforced in `HerderSCPDriver::validateValue` / `validateValueAgainstLocalState`, ~319/427).
- **[on master]** `BallotProtocol::maybeReplaceValueWithEmptyTxSet` (BallotProtocol.cpp:377): replacement preconditions are asserted in the merged body (PREPARE phase only, protocol allows empty-tx-set values, input is not already an empty-tx-set value).
- **[added]** `HerderSCPDriver::onTxSetReceived` (HerderSCPDriver.cpp:1721): entry asserts `txSet != nullptr` and `txSet->getContentsHash() == txSetHash` (1724–1725). Previously the hash was only checked downstream in `SCPHerderValueWrapper::setTxSet` after a successful `weak_ptr::lock()`; a miskeyed registry would silently write the wrong tx set or skip cleanup.
- **[on master]** `HerderSCPDriver::makeEmptyTxSetValueFromValue` (HerderSCPDriver.cpp:691): `releaseAssert(ext.v() == STELLAR_VALUE_SIGNED)` precondition — readable, and rules out building an empty-of-an-empty.
- **[added]** `HerderSCPDriver::extractValidValue` (HerderSCPDriver.cpp:637): when `validateValueAgainstLocalState` returned `>= kStructurallyValidValue`, the returned `ValueWrapperPtr` must be non-null (654). Documents the postcondition now that nomination depends on structurally-valid values being treated as acceptable.
- **[dropped]** `BallotProtocol::setConfirmCommit` belt-and-suspenders re-validation before `valueExternalized`. Redundant on master: `throwIfValueInvalidForConfirmCommit` (BallotProtocol.cpp:1441) runs at the top of `setConfirmCommit` (1681, its only call site) and rejects anything that isn't `kFullyValidatedValue`/`kMaybeValidNotCurrentValue` — strictly stronger than the proposed assert. The upstream gate is `HerderSCPDriver::isEnvelopeReady` (217–256) holding CONFIRM/EXTERNALIZE at `ENVELOPE_STATUS_FETCHING`.
- **[on master]** `HerderImpl::processExternalized`: `releaseAssert(externalizedSet != nullptr)` — the application-layer backstop, unchanged.

**Existing `dbgAssert` sweep [deferred]**: per the "always `releaseAssert`" rule in the Approach section, the existing `dbgAssert` calls in ballot-state invariant checks — `BallotProtocol::checkInvariants` (735, ×11 at 743–774), `bumpToBallot` (×2 at ~550/555), and `setConfirmPrepared`'s `dbgAssert(!mCommit)` (1163) — should be upgraded to `releaseAssert`. Deliberately **split out of the port** (decision 2026-07-01): the upgrades change release-build behavior for all of SCP and deserve their own review.

### Metric-based invariants (single-node)

Evaluated over a single scenario execution.

- `time spent blocked on tx set download` is zero when the tx set arrives before ballot `setConfirmPrepared` fires. (Master records this in `setConfirmPrepared`'s blocked-on-txset timer.)
- Empty-tx-set count for the node is zero when the node obtains a valid tx set before its own download timeout expires. (Master exposes `{"scp", "empty-tx-set", "externalized"}` and `{"scp", "empty-tx-set", "value-replaced"}` counters, HerderSCPDriver.cpp:83–86 — hook these.)
- Count of `kStructurallyValidValue` observations per slot is finite and bounded (no infinite reprocessing loop).
- Count of `validateValue` / `extractValidValue` calls per envelope is bounded — the `isNewerStatement` relaxation allowing reprocessing should fire at most a bounded number of times.
- `mPendingTxSetWrappers` / `mPendingTxSetEnvelopeWrappers` entries for slot `N` are all resolved or cleaned up by the time slot `N` is purged (via `eraseBelow`). This is a temporal property rather than a point invariant, so it lives here rather than as an in-code assertion: the harness verifies at scenario end that every entry observed at any point during the scenario was resolved by a corresponding tx set arrival, slot purge, or `weak_ptr` expiry.

### Progress and terminal-state invariants (single-node)

These complement the scenario-agnostic metric invariants above. The deadlock check applies to any scenario; the progress invariants apply only to scenarios produced by constructed cooperative generators (see "Constructed scenario classes" below).

- **Deadlock / terminal state (scenario-agnostic)**: at the end of any scenario, the node's state must be classifiable as one of: (a) externalized a value for the current slot, (b) waiting on a specific unfulfilled input (e.g. `kStructurallyValidValue` plus a tx set that was not scheduled to arrive, or insufficient quorum messages for the current ballot phase), or (c) download timeout not yet expired. Any other terminal state — the node has what it needs but did not advance — is a bug.
- **Progress under fully cooperative scenarios**: for scenarios produced by the "fully cooperative" generator class, the node externalizes a non-empty-tx-set value within `K` ballot rounds. `K` is a small constant calibrated empirically (likely 2–3).
- **Progress under cooperative-but-slow scenarios**: for scenarios produced by the "cooperative-but-slow" generator class, the node externalizes an empty-tx-set value within `K` ballot rounds.

**Scope and caveat**: component-level progress is a sanity check, not a liveness proof. Peer messages here are synthesized, so "progress" means "the state machine advances when the test author constructed inputs that should be sufficient." The deeper liveness question — whether the actual protocol converges under real multi-node interaction — is addressed in `claude-protocol-testing.md`.

## Scenario generator

### Input space

Primary axes:

1. **Envelope sequence**: ordering, statement types (`SCP_ST_NOMINATE`, `SCP_ST_PREPARE`, `SCP_ST_CONFIRM`, `SCP_ST_EXTERNALIZE`), and values. Does the node see `NOMINATE` before or after `PREPARE` from a peer? Does it see conflicting values from different peers?
2. **Tx set arrival timing**: when, relative to envelope arrival and the node's ballot state, the tx set arrives at the node.
3. **Download-timeout calibration**: the `getTxSetDownloadTimeout()` value and its interaction with the scenario clock (the SCP-level harness models this as `TX_SET_TIMEOUT` with per-value wait times in `mDownloadWaitTimes`).

Secondary axes (fixed or drawn from a small set):

- Tx set validity: valid / invalid / never-arrives.
- Simulated peer profile per peer: cooperative, withholds tx set, sends invalid tx set, sends malformed envelope, sends empty-tx-set value.
- Peer identity and quorum configuration of the node under test (determines when the node reaches quorum internally).

### Bias targets

Random uniform generation hits happy paths. Bias generator toward (implementation status per the Phase 3 generator marked inline):

- **[done: `shiftTxSetArrival`]** Tx set arrives exactly at, just before, and just after download-timeout expiry (approximated by varying arrival position relative to envelopes).
- **[partial: via `shiftTxSetArrival`]** Tx set arrives at each SCP state-machine transition boundary: `vote nominate → accept nominate`, `accept nominate → vote prepare`, `vote prepare → accept prepare`, `accept prepare → confirm prepared` (the `setConfirmPrepared` gate). Ordering shifts cover some boundaries; not boundary-targeted yet.
- **[done: `duplicateEnvelope`]** Duplicate envelope delivery: the same envelope arrives both before and after the tx set arrives. Exercises the `isNewerStatement` reprocessing path called out in `implementation-plan.md`.
- **[TODO]** Envelope from a previously-silent peer tips the node into quorum at the exact moment the tx set arrives.
- **[TODO]** Tx set arrives after the slot has been purged / ledger advanced.
- **[done: `replaceWithYValue` + `mValidateValueOverride`, and `corruptOneEnvelope`]** Invalid tx set / malformed envelope arrives at the boundaries listed above.
- **[TODO]** Empty-tx-set value received from peer combined with valid tx set received locally (tests `combineCandidates` behavior on a single node; `TestSCP::combineCandidates` is nomination-only and may need extending).
- **[done: `injectVBlockingConfirms`]** **`setAcceptCommit` with `kStructurallyValidValue` + CONFIRM/EXTERNALIZE gating**: PREPARE envelopes carrying a quorum vote-to-commit (via `nC`/`nH` fields) drive the node to `setAcceptCommit` for a value whose local `validateValue` is still `kStructurallyValidValue`. Node transitions to `SCP_PHASE_CONFIRM` and emits `SCP_ST_CONFIRM` for a locally-unvalidated value — safe by design. CONFIRM/EXTERNALIZE envelopes for the same value arrive from peers; they must remain in `ENVELOPE_STATUS_FETCHING` and *not* trigger `setConfirmCommit` until the tx set arrives at this node. Once the tx set arrives, those envelopes become `READY`, `setConfirmCommit` fires, and the node externalizes with the tx set available. This exercises the production gate end-to-end — `HerderSCPDriver::isEnvelopeReady` (HerderSCPDriver.cpp:217–256), routed via `PendingEnvelopes::recvSCPEnvelope` — the mechanism that guarantees externalize cannot happen without the tx set. The SCP-level harness emulates it with `TestSCP::receiveEnvelopeGated` + buffered replay on `clearDownload`.

### Constructed scenario classes

Alongside the biased-random generator, we maintain a small set of "constructed" generators whose scenarios are built by rule to satisfy a specific precondition. These exist to support the progress invariants and to give the biased-random generator known happy-path anchor points.

- **Fully cooperative**: every peer sends a complete sequence of compatible envelopes (nominate → prepare → confirm) in an order that lets the node form a quorum at each step; a valid tx set arrives before the download timeout.
- **Cooperative-but-slow**: peers cooperate as above, but the tx set either never arrives or arrives only after the download timeout expires on a quorum; the node replaces the value with an empty-tx-set value and externalizes that.

The biased-random generator may perturb these starting points — shifting timing, injecting failures, or adding adversarial peers — to explore the boundary around known-good behavior.

### Determinism

Component-level determinism is much simpler than protocol-level because there are no inter-node ordering problems:

- Single node means no multi-node message delivery order to control.
- Virtual clock drives all timer behavior deterministically.
- Envelope and tx set arrival events are scripted; they are not produced by a network simulator.

One subtlety: the node's internal scheduling (asio post ordering, herder queue processing order) must be deterministic. We believe the existing single-node test harness is deterministic enough for our purposes and do not plan to audit this up front. If scenario reproducibility issues surface during generator work, we revisit then.

## What this approach gives up

- **Cross-node emergent behavior.** Safety, liveness, and consensus properties of a multi-node network are not exercised. Protocol-level testing covers this.
- **Interactions with real tx set dissemination.** The harness synthesizes tx set arrival events; it does not test the real overlay / fetch path. Existing overlay tests cover that separately.
- **Valid-but-unexpected local outcomes.** Without an oracle, a single node doing something "allowed but surprising" will not trip an invariant.

## Implementation phases

Component-level testing splits into two parallel tracks: an **SCP-level track** that uses the existing `TestSCP` mock-driver harness in `src/scp/test/SCPTests.cpp`, and a **Herder-level track** that uses a real single-node `HerderSCPDriver` / `PendingEnvelopes` built on `createTestApplication`. The SCP track exercises SCP state-machine invariants (most of the catalog); the Herder track exercises the `HerderSCPDriver`-specific invariants from the in-code list (e.g. `onTxSetReceived` hash check, `validateValueAgainstLocalState` postcondition, `makeEmptyTxSetValueFromValue` precondition) that the mock driver can't reach. Both tracks share the same invariant catalog and overall approach — only the harness, the input-injection mechanism, and the observation hooks differ. The SCP track lands first because the existing infrastructure is closer to what we need; Herder-level work follows once the SCP track has shaken out the design.

Each phase is intentionally scoped small. Some can be split further if any individual phase grows larger than expected when we get to it (e.g. peeling individual peer profiles out of the harness phase).

### SCP-level track

1. **In-code assertions.** *(Status: partially landed — see the per-item disposition in the invariant catalog; the `dbgAssert` sweep is deferred to a dedicated follow-up change.)* Add to implementation. Enable in existing test suites. Immediate value.
2. **SCP-level harness extensions.** *(Status: done; ported to `master` on `component-tests-port`.)* Extend `TestSCP` with: (a) reusable peer-profile strategies — cooperative, withholds tx set, sends invalid, sends malformed envelope, sends empty-tx-set — pluggable so multiple profiles can be mixed in one scenario; (b) configurable canonical quorum-set fixtures, starting with 3-node and 4-node, with a builder shape that makes it easy to add more later. Keep the existing imperative scenario style. The existing `bumpTimerOffset()` plus manual callback-firing pattern is sufficient for any clock-related needs; no fine-grained `advanceClockBy(ms)` is needed at this phase. Validate against a handful of hand-authored scenarios using the new abstractions, including the `setAcceptCommit` + `kStructurallyValidValue` + CONFIRM/EXTERNALIZE-gating bias target.
3. **SCP-level scenario generator.** *(Status: mostly done; ported to `master`. Three closeout items pending — see `handoff.md` "Phase 3 closeout".)* Biased-random envelope sequences and tx-set arrival timing against the `TestSCP` harness, plus constructed cooperative / cooperative-but-slow generators, parameterized over multiple quorum sizes (3-, 4-, 5-node). Use the test suite's existing seeded random engine (`getGlobalRandomEngine()` from `util/Math.h`) — no new RNG infrastructure. Initial coverage targets 5 of the 8 catalog bias targets: download-timeout timing, duplicate envelope delivery, sends-invalid peer, sends-malformed envelope, and `setAcceptCommit` + CONFIRM gating. Three deferred targets — tx-set arrives after slot purge, previously-silent peer tips quorum at tx-set arrival, empty-tx-set value combined with valid tx set (`combineCandidates` path) — are TODO for a follow-up extension of this phase or rolled into Phase 4.
4. **SCP-level metric, progress, and terminal-state invariants.** *(Status: not started. Master already exposes the `{"scp", "empty-tx-set", *}` counters and the blocked-on-txset timer — hook those.)* Implement the SCP-observable invariants — empty-tx-set count, blocked-time metric, `kStructurallyValidValue` observation bound, deadlock / terminal-state check, and the progress invariants for the cooperative scenario classes. Run against generated scenarios.

### Herder-level track

5. **Herder-level harness extensions.** *(Status: not started.)* Build a lightweight single-node Herder harness on top of `createTestApplication` (single `ApplicationImpl` with real `HerderSCPDriver` / `PendingEnvelopes`, no overlay, no multi-node `Simulation`). Add helpers to: script tx-set arrival timing through real `onTxSetReceived`; trigger the `kStructurallyValidValue` validation result through real `PendingEnvelopes` (start a fetch, defer arrival); exercise the `HerderSCPDriver`-specific Phase 1 assertions with edge-case inputs. Build for Herder-level needs first; sharing abstractions with the SCP-level track is not a design goal for this phase, and any shared interface can be refactored out later if it would help.
6. **Herder-level scenario generator.** Same biased-random + constructed-class concepts as the SCP-level generator, retargeted to the Herder-level harness.
7. **Herder-level metric, progress, and terminal-state invariants.** Implement the `HerderSCPDriver`-observable invariants (e.g. `mPendingTxSetWrappers` lifecycle), plus any cross-cutting catalog items that aren't exercisable at the SCP level.

### Wrap-up

8. **Scale and tune.** Run extended suites overnight / pre-release across both harnesses. Expand bias targets based on bugs found. Begin protocol-level work (see `claude-protocol-testing.md`).

## Open questions

- What is the right interface for "simulated peer" profiles? Ideally pluggable strategies (cooperative, withholds tx set, sends invalid, sends empty-tx-set) designed so they can be reused when we move to protocol-level testing. (Phase 2 answered this at the SCP level: `PeerProfile` + value-keyed harness configuration, not subclassing; revisit for protocol-level reuse.)
