# Component-Level Testing Plan for Parallel Tx Set Downloading

This document describes the component-level testing strategy for the parallel tx set downloading SCP change. Component-level testing exercises a single node's SCP state machine with scripted sequences of SCP envelopes and tx set arrival events, and checks invariants over the node's local state.

See also: `claude-protocol-testing.md` for the multi-node (protocol-level) testing plan that builds on this work.

## Context and rationale

The original proposal in `parallel-download-docs/testing.md` split testing into component-level (ad-hoc manual tests) and protocol-level (MBT). In discussion we concluded that:

- Most of the invariants we care about for this feature are local to a single node's state (see concrete code paths in `parallel-download-docs/claude-learnings.md`).
- An invariant-checking approach (generate scenarios, check invariants, do not predict outcomes) is dramatically cheaper than building a predictive oracle and captures the safety / liveness / performance concerns we actually have.
- That invariant-checking approach is *more natural* at the component level than the ad-hoc approach originally proposed, because the "model" is just the node's SCP state machine. The generator produces envelope sequences and tx set arrival timings; the invariants are properties of the resulting state transitions.
- Component-level infrastructure is an order of magnitude cheaper than protocol-level, because it avoids multi-node determinism (message ordering, cross-node timer coordination, leader progression). This makes it the right starting point.

Component-level testing does not cover emergent cross-node properties (safety, liveness of the network as a whole). Those are deferred to the protocol-level plan.

## Goals

- Programmatically generate envelope sequences and tx set arrival timings that exercise the parallel downloading state transitions in a single node.
- Catch local invariant violations: forbidden state transitions, forbidden validation results, skip-value construction bugs, reprocessing bugs, orphaned pending-state entries.
- Build up in-code assertions that continue to catch bugs in every future test run (component, protocol-level, and production).

## Non-goals

- Test multi-node safety, liveness, or consensus emergent behavior. That's protocol-level.
- Replace or remove existing ad-hoc SCP component tests. This plan is strictly additive: existing tests coexist with new work and may be enhanced but not removed.
- Build a predictive oracle for a single node's outcome.

## Approach

A test harness drives a single node's `HerderSCPDriver` / `BallotProtocol` / `NominationProtocol` with:

1. A sequence of synthetic SCP envelopes from simulated peers (controllable statement types, signatures, values).
2. A scripted schedule of tx set arrivals (simulating `HerderSCPDriver::onTxSetReceived` at specified points in the sequence).
3. A virtual clock for timer-based behavior (skip timer expiry, nomination leader timeout).

Invariants are checked in three forms:

1. **In-code assertions** — implemented with `releaseAssert` in implementation code. Running a scenario without crashing is the pass condition. These compound: they also run in protocol-level tests and in production. We use `releaseAssert` exclusively; debug-only `assert` is not used. If an assertion is too expensive to evaluate in a release build, gate it behind `#ifdef BUILD_TESTS` rather than demoting it.
2. **Metric-based invariants** — assertions over single-node metric values at the end of a scenario.
3. **Progress and terminal-state invariants** — the harness inspects the node's terminal state and classifies it, and asserts progress for scenarios produced by constructed cooperative generators.

## Invariant catalog

This is a starting list. Expect to extend it as we explore the state space.

### In-code assertions

- `BallotProtocol::setConfirmPrepared` (BallotProtocol.cpp ~1146): on the `setConfirmPrepared` commit-gate path, `mCommit` must not be assigned for a value whose local `validateValue` is `kAwaitingDownload` or `kInvalidValue`. Note: `setAcceptCommit` (BallotProtocol.cpp ~1478) is a separate path that intentionally *does* assign `mCommit` for `kAwaitingDownload` values via federated accept, which is safe because the network-level accept chain guarantees the value was validated upstream. The assertion is therefore scoped to the `setConfirmPrepared` path. The existing `dbgAssert(!mCommit)` at ~line 1198 (in the branch that first sets `mCommit`) is the natural place and should be upgraded to `releaseAssert`.
- `HerderSCPDriver::validateValueAgainstLocalState` (HerderSCPDriver.cpp ~348): `kAwaitingDownload` is returned only when the slot is `LCL+1` and a fetch is actively in progress.
- `HerderSCPDriver::validatePastOrFutureValue` (HerderSCPDriver.cpp ~230): must never return `kAwaitingDownload`, since "awaiting download" is only meaningful for `LCL+1`.
- Once a node has received and successfully validated a tx set for the current slot, subsequent calls to `validateValue` for that value must never return `kAwaitingDownload`.
- Any `STELLAR_VALUE_SKIP` value received from the network must carry a valid inner `lcValueSignature` covering `(networkID, ENVELOPE_TYPE_SCPVALUE, originalTxSetHash, closeTime)`.
- Skip values reaching `validateValueAgainstLocalState` must have `previousLedgerHash` / `previousLedgerVersion` matching the node's LCL. (This is already enforced; an assertion documents the invariant.)
- `BallotProtocol::maybeReplaceValueWithSkip` (BallotProtocol.cpp ~358): invariants around when replacement is allowed (e.g. only when the download timer has expired and the value is in `kAwaitingDownload`).
- `HerderSCPDriver::onTxSetReceived` (HerderSCPDriver.cpp ~1628): on entry, assert `txSet->getContentsHash() == txSetHash`. Currently the hash is only checked downstream in `SCPHerderValueWrapper::setTxSet` after a successful `weak_ptr::lock()`; a miskeyed registry would silently write the wrong tx set or skip cleanup.
- `HerderSCPDriver::makeSkipLedgerValueFromValue` (HerderSCPDriver.cpp ~598): assert `originalValue.ext.v() == STELLAR_VALUE_SIGNED` on entry. The function already accesses `originalValue.ext.lcValueSignature()` unconditionally (which throws on non-SIGNED), but an explicit assertion makes the precondition readable and rules out building a skip-of-a-skip.
- `HerderSCPDriver::extractValidValue` (HerderSCPDriver.cpp ~548): when `validateValueAgainstLocalState` returned `>= kAwaitingDownload`, the returned `ValueWrapperPtr` must be non-null. Documents the postcondition now that nomination depends on `kAwaitingDownload` values being treated as acceptable (see TODO(9) in `parallel-download-docs/claude-learnings.md`).
- `BallotProtocol::setConfirmCommit` (BallotProtocol.cpp ~1694): immediately before the call to `mSlot.getSCPDriver().valueExternalized(...)`, assert that `validateValue(mCommit->getBallot().value)` is neither `kInvalidValue` nor `kAwaitingDownload`. This is structurally guaranteed by two upstream mechanisms: `throwIfValueInvalidForCommit` at ~line 1682 (catches `kInvalidValue`), and the envelope-processing gate in `HerderImpl::recvSCPEnvelope` at HerderImpl.cpp ~934 (holds CONFIRM/EXTERNALIZE envelopes at `ENVELOPE_STATUS_FETCHING` until the tx set arrives, so `setConfirmCommit` cannot fire on a `kAwaitingDownload` value). An explicit assertion at the SCP → driver handoff catches any regression in either mechanism before the driver is notified.
- `HerderImpl::processExternalized` (HerderImpl.cpp ~343): `releaseAssert(externalizedSet != nullptr)` already exists. Documents the application-layer invariant "externalize always has a tx set (real or synthesized-from-skip)." Redundant with the `setConfirmCommit` assertion above given the upstream gating, but kept as a downstream backstop.

**Existing `dbgAssert` sweep**: per the "always `releaseAssert`" rule in the Approach section, the existing `dbgAssert` calls in ballot-state invariant checks — especially `BallotProtocol::checkInvariants` (BallotProtocol.cpp ~711) and scattered asserts in ballot-state transitions (e.g. ~525, ~730, ~1198) — should be upgraded to `releaseAssert` as part of this phase. These encode ballot-state consistency invariants that are cheap to evaluate.

### Metric-based invariants (single-node)

Evaluated over a single scenario execution.

- `time spent blocked on tx set download` is zero when the tx set arrives before ballot `setConfirmPrepared` fires.
- Skip count for the node is zero when the node obtains a valid tx set before its own skip timer expires.
- Count of `kAwaitingDownload` observations per slot is finite and bounded (no infinite reprocessing loop).
- Count of `validateValue` / `extractValidValue` calls per envelope is bounded — the `isNewerStatement` relaxation allowing reprocessing should fire at most a bounded number of times.
- `mPendingTxSetWrappers` / `mPendingTxSetEnvelopeWrappers` entries for slot `N` are all resolved or cleaned up by the time slot `N` is purged (via `eraseBelow`). This is a temporal property rather than a point invariant, so it lives here rather than as an in-code assertion: the harness verifies at scenario end that every entry observed at any point during the scenario was resolved by a corresponding tx set arrival, slot purge, or `weak_ptr` expiry.

### Progress and terminal-state invariants (single-node)

These complement the scenario-agnostic metric invariants above. The deadlock check applies to any scenario; the progress invariants apply only to scenarios produced by constructed cooperative generators (see "Constructed scenario classes" below).

- **Deadlock / terminal state (scenario-agnostic)**: at the end of any scenario, the node's state must be classifiable as one of: (a) externalized a value for the current slot, (b) waiting on a specific unfulfilled input (e.g. `kAwaitingDownload` plus a tx set that was not scheduled to arrive, or insufficient quorum messages for the current ballot phase), or (c) skip timer not yet expired. Any other terminal state — the node has what it needs but did not advance — is a bug.
- **Progress under fully cooperative scenarios**: for scenarios produced by the "fully cooperative" generator class, the node externalizes a non-skip value within `K` ballot rounds. `K` is a small constant calibrated empirically (likely 2–3).
- **Progress under cooperative-but-slow scenarios**: for scenarios produced by the "cooperative-but-slow" generator class, the node externalizes a skip value within `K` ballot rounds.

**Scope and caveat**: component-level progress is a sanity check, not a liveness proof. Peer messages here are synthesized, so "progress" means "the state machine advances when the test author constructed inputs that should be sufficient." The deeper liveness question — whether the actual protocol converges under real multi-node interaction — is addressed in `claude-protocol-testing.md`.

## Scenario generator

### Input space

Primary axes:

1. **Envelope sequence**: ordering, statement types (`SCP_ST_NOMINATE`, `SCP_ST_PREPARE`, `SCP_ST_CONFIRM`, `SCP_ST_EXTERNALIZE`), and values. Does the node see `NOMINATE` before or after `PREPARE` from a peer? Does it see conflicting values from different peers?
2. **Tx set arrival timing**: when, relative to envelope arrival and the node's ballot state, the tx set arrives at the node.
3. **Skip timer calibration**: `TX_SET_DOWNLOAD_TIMEOUT` value and its interaction with the scenario clock.

Secondary axes (fixed or drawn from a small set):

- Tx set validity: valid / invalid / never-arrives.
- Simulated peer profile per peer: cooperative, withholds tx set, sends invalid tx set, sends malformed envelope, sends skip value.
- Peer identity and quorum configuration of the node under test (determines when the node reaches quorum internally).

### Bias targets

Random uniform generation hits happy paths. Bias generator toward:

- Tx set arrives exactly at, just before, and just after skip timer expiry.
- Tx set arrives at each SCP state-machine transition boundary: `vote nominate → accept nominate`, `accept nominate → vote prepare`, `vote prepare → accept prepare`, `accept prepare → confirm prepared` (the `setConfirmPrepared` gate).
- Duplicate envelope delivery: the same envelope arrives both before and after the tx set arrives. Exercises the `isNewerStatement` reprocessing path called out in `implementation-plan.md`.
- Envelope from a previously-silent peer tips the node into quorum at the exact moment the tx set arrives.
- Tx set arrives after the slot has been purged / ledger advanced.
- Invalid tx set arrives at each boundary listed above.
- Skip value received from peer combined with valid tx set received locally (tests `combineCandidates` behavior on a single node).
- **`setAcceptCommit` with `kAwaitingDownload` + CONFIRM/EXTERNALIZE gating**: PREPARE envelopes carrying a quorum vote-to-commit (via `nC`/`nH` fields) drive the node to `setAcceptCommit` for a value whose local `validateValue` is still `kAwaitingDownload`. Node transitions to `SCP_PHASE_CONFIRM` and emits `SCP_ST_CONFIRM` for a locally-unvalidated value — safe by design. CONFIRM/EXTERNALIZE envelopes for the same value arrive from peers; they must remain in `ENVELOPE_STATUS_FETCHING` and *not* trigger `setConfirmCommit` until the tx set arrives at this node. Once the tx set arrives, those envelopes become `READY`, `setConfirmCommit` fires, and the node externalizes with the tx set available. This scenario exercises the `recvSCPEnvelope` gating (HerderImpl.cpp ~934) end-to-end — the mechanism that guarantees externalize cannot happen without the tx set.

### Constructed scenario classes

Alongside the biased-random generator, we maintain a small set of "constructed" generators whose scenarios are built by rule to satisfy a specific precondition. These exist to support the progress invariants and to give the biased-random generator known happy-path anchor points.

- **Fully cooperative**: every peer sends a complete sequence of compatible envelopes (nominate → prepare → confirm) in an order that lets the node form a quorum at each step; a valid tx set arrives before the skip timer.
- **Cooperative-but-slow**: peers cooperate as above, but the tx set either never arrives or arrives only after the skip timer expires on a quorum.

The biased-random generator may perturb these starting points — shifting timing, injecting failures, or adding adversarial peers — to explore the boundary around known-good behavior.

### Determinism

Component-level determinism is much simpler than protocol-level because there are no inter-node ordering problems:

- Single node means no multi-node message delivery order to control.
- Virtual clock drives all timer behavior deterministically.
- Envelope and tx set arrival events are scripted; they are not produced by a network simulator.

One subtlety: the node's internal scheduling (asio post ordering, herder queue processing order) must be deterministic. We believe the existing single-node test harness is deterministic enough for our purposes and do not plan to audit this up front. If scenario reproducibility issues surface during generator or shrinking work, we revisit then.

### Shrinking

On invariant failure, minimize the scenario to the smallest envelope sequence and timing that still reproduces the violation. Delta-debugging / binary-search over sequence length and timing parameters is sufficient initially. No need for an integrated shrinking framework.

## What this approach gives up

- **Cross-node emergent behavior.** Safety, liveness, and consensus properties of a multi-node network are not exercised. Protocol-level testing covers this.
- **Interactions with real tx set dissemination.** The harness synthesizes tx set arrival events; it does not test the real overlay / fetch path. Existing overlay tests cover that separately.
- **Valid-but-unexpected local outcomes.** Without an oracle, a single node doing something "allowed but surprising" will not trip an invariant.

## Implementation phases

Component-level testing splits into two parallel tracks: an **SCP-level track** that uses the existing `TestSCP` mock-driver harness in `src/scp/test/SCPTests.cpp`, and a **Herder-level track** that uses a real single-node `HerderSCPDriver` / `PendingEnvelopes` built on `createTestApplication`. The SCP track exercises SCP state-machine invariants (most of the catalog); the Herder track exercises the `HerderSCPDriver`-specific invariants from the in-code list (e.g. `onTxSetReceived` hash check, `validateValueAgainstLocalState` postcondition, `makeSkipLedgerValueFromValue` precondition) that the mock driver can't reach. Both tracks share the same invariant catalog and overall approach — only the harness, the input-injection mechanism, and the observation hooks differ. The SCP track lands first because the existing infrastructure is closer to what we need; Herder-level work follows once the SCP track has shaken out the design.

Each phase is intentionally scoped small. Some can be split further if any individual phase grows larger than expected when we get to it (e.g. peeling shrinking out of the generator phase, or peeling individual peer profiles out of the harness phase).

### SCP-level track

1. **In-code assertions.** Add to implementation. Enable in existing test suites. Immediate value.
2. **SCP-level harness extensions.** Extend `TestSCP` with: (a) reusable peer-profile strategies — cooperative, withholds tx set, sends invalid, sends malformed envelope, sends skip — pluggable so multiple profiles can be mixed in one scenario; (b) configurable canonical quorum-set fixtures, starting with 3-node and 4-node, with a builder shape that makes it easy to add more later; (c) finer-grained virtual-clock control to replace the existing `bumpTimerOffset` 5-hour jump with a precise `advanceClockBy(ms)` helper. Keep the existing imperative scenario style. Validate against a handful of hand-authored scenarios using the new abstractions, including the `setAcceptCommit` + `kAwaitingDownload` + CONFIRM/EXTERNALIZE-gating bias target.
3. **SCP-level scenario generator.** Biased-random envelope sequences and tx set arrival timing against the `TestSCP` harness, plus constructed cooperative / cooperative-but-slow generators. Includes shrinking with deterministic RNG seeding so failures replay exactly. Bias targets per the catalog above.
4. **SCP-level metric, progress, and terminal-state invariants.** Implement the SCP-observable invariants — skip count, blocked-time metric, `kAwaitingDownload` observation bound, deadlock / terminal-state check, and the progress invariants for the cooperative scenario classes. Run against generated scenarios.

### Herder-level track

5. **Herder-level harness extensions.** Build a lightweight single-node Herder harness on top of `createTestApplication` (single `ApplicationImpl` with real `HerderSCPDriver` / `PendingEnvelopes`, no overlay, no multi-node `Simulation`). Add helpers to: script tx-set arrival timing through real `onTxSetReceived`; trigger the `kAwaitingDownload` validation result through real `PendingEnvelopes` (start a fetch, defer arrival); exercise the `HerderSCPDriver`-specific Phase 1 assertions with edge-case inputs. Build for Herder-level needs first; sharing abstractions with the SCP-level track is not a design goal for this phase, and any shared interface can be refactored out later if it would help.
6. **Herder-level scenario generator.** Same biased-random + constructed-class concepts as the SCP-level generator, retargeted to the Herder-level harness. Includes shrinking.
7. **Herder-level metric, progress, and terminal-state invariants.** Implement the `HerderSCPDriver`-observable invariants (e.g. `mPendingTxSetWrappers` lifecycle), plus any cross-cutting catalog items that aren't exercisable at the SCP level.

### Wrap-up

8. **Scale and tune.** Run extended suites overnight / pre-release across both harnesses. Expand bias targets based on bugs found. Begin protocol-level work (see `claude-protocol-testing.md`).

## Open questions

- What is the right interface for "simulated peer" profiles? Ideally pluggable strategies (cooperative, withholds tx set, sends invalid, sends skip) designed so they can be reused when we move to protocol-level testing.
