# Protocol-Level Testing Plan for Parallel Tx Set Downloading

This document proposes a testing strategy for the protocol-level (multi-node) correctness of the parallel tx set downloading SCP change. It is a refinement of the "Protocol MBT" idea from `parallel-download-docs/testing.md`.

**Prerequisite reading**: `claude-component-testing.md`. This document builds on the component-level plan; it deliberately focuses on the delta required to extend that work to multiple nodes.

> **Status (2026-07-01)**: not started (this is Phases 5–8 territory and beyond). The feature has since **merged into upstream `master`** with renames — "skip value" is now **"empty-tx-set value"**, `kAwaitingDownload` is now **`kStructurallyValidValue`** — and this document has been updated to the merged terminology. See `testing/handoff.md` for the full rename map and the current state of the component-level work this plan builds on. One concrete assist from the merge: master already exposes `{"scp", "empty-tx-set", "externalized"}` / `{"scp", "empty-tx-set", "value-replaced"}` counters (HerderSCPDriver.cpp:83–86), which the aggregate metrics below can consume directly.

## Relationship to component-level testing

Component-level testing (see `claude-component-testing.md`) exercises a single node's SCP state machine with scripted envelope and tx set arrival sequences, checks single-node invariants via in-code assertions and per-node metrics, and uses a biased-random scenario generator over envelope sequences and timing.

Protocol-level testing reuses that foundation and adds what only multi-node execution can cover:

1. **Cross-node invariants** — properties that require observing multiple nodes simultaneously (safety, cross-node liveness, empty-tx-set ancestry consistency).
2. **Multi-node scenario generation** — scenarios parameterized over per-node tx set arrival timing, not just one node's timing.
3. **Multi-node determinism** — reproducibility across message delivery order, timer firing across nodes, and nomination leader progression.

The in-code assertions from the component plan continue to run inside every node in the simulation, so protocol-level testing strictly strengthens coverage of those invariants.

## Context and rationale

The original proposal in `testing.md` described "Protocol MBT" as either (a) a set of high-level invariants over inter-node interactions, or (b) a model object that predicts the correct outcome ("externalize an empty tx set, confirm original value, etc.") given a scenario.

Option (b) — an oracle — is effectively a second implementation of parallel downloading plus enough of SCP's ballot machinery to know when `setConfirmPrepared` fires on each node. Earlier work has already shown that re-implementing SCP (even with AI tooling) is hard and error-prone. The marginal value over IVy model updates (already planned per `implementation-plan.md`) is unclear.

Option (a) — an invariant checker — is substantially cheaper. It sacrifices the ability to catch "valid-but-unexpected" outcomes, but preserves the ability to catch safety violations, liveness stalls, and performance regressions. This plan pursues option (a), consistent with the approach in the component-level plan.

## Goals

- Programmatically generate multi-node scenarios that vary per-node tx set arrival timing, network topology, and malicious-node profiles.
- Detect cross-node safety violations, network-level liveness stalls, and unexpected aggregate performance regressions.
- Complement, not replace, existing component-level tests and IVy verification.

## Non-goals

- Build an executable oracle that predicts specific externalized outcomes for arbitrary scenarios.
- Re-verify SCP itself. The IVy model updates cover the protocol-level safety and liveness questions for the core protocol change.
- Replicate single-node invariants already covered by component-level in-code assertions.

## Approach

Generate multi-node scenarios programmatically, execute them against the simulation framework, and check invariants during and after execution. Invariants live in two new places (on top of what the component plan covers):

1. **Harness checks** for cross-node invariants requiring observation of multiple nodes.
2. **Aggregate / cross-node metrics** for network-level statistical properties.

Single-node in-code assertions continue to fire inside every simulated node and catch violations wherever they occur.

## Cross-node invariant catalog

A starting list. Expect to refine as we go.

### Harness checks

Implemented in the test harness outside the simulated nodes. The harness has visibility into every node's state and emitted messages.

- **Safety**: no two honest nodes ever externalize contradictory values for the same slot.
- **Validity of non-empty externalization**: if the network externalizes a non-empty-tx-set value for slot `N`, at least one honest node obtained and validated the pre-image of that value before externalization.
- **Timing-conditional liveness**: if at any point during slot `N` a quorum of honest nodes held a valid tx set before their respective download timeouts expired, the network must externalize that tx set (i.e. must not externalize an empty-tx-set value).
- **Empty-tx-set ancestry consistency**: if the network externalizes an empty-tx-set value for slot `N`, then for slot `N+1`, `previousLedgerHash` / `previousLedgerVersion` on all honest-node ballots are consistent with slot `N` being treated as empty.
- **Quorum convergence on `combineCandidates`**: after a bounded number of ballot rounds in which a quorum has at least one valid tx set, the network converges to externalizing a valid value or an empty-tx-set value — not perpetual churn.
- **No honest-node divergence under good timing**: under scenarios where every honest node has the tx set before any download timeout fires, every honest node externalizes the same non-empty-tx-set value.

### Aggregate / cross-node metrics

Evaluated over a run or a suite of runs, using the metrics already defined in `implementation-plan.md`.

- **Network empty-tx-set rate**: under "normal timing" scenarios, `empty_tx_set_ledgers / total_ledgers < 1/1000` across the network (consume the `{"scp", "empty-tx-set", "externalized"}` counter).
- **Aggregate happy-path latency improvement**: with feature enabled, the network-wide mean `time spent blocked on tx set download` is strictly less than with feature disabled, for matched scenarios.
- **Good-timing happy path**: under scenarios where the tx set arrives at every node before its first ballot timer, the network empty-tx-set count is zero. This is the closest we get to an oracle check without writing one.
- **Bounded cross-node blocked time**: per slot, the maximum across honest nodes of `time between vote-to-nominate and vote-to-commit` has a finite upper bound under scenarios where a valid tx set is eventually available on a quorum.

## Multi-node scenario generator

### Input space — additions over component-level

Where the component generator varies one node's envelope sequence and tx set arrival timing, the protocol generator varies timing **per node** and adds structural axes that only exist in multi-node execution:

- **Per-node tx set arrival timing**: each honest node has its own arrival schedule.
- **Network topology and quorum configuration**: which nodes peer with which; quorum set shape.
- **Leader rotation order**: nomination leader per round (or a fixed schedule).
- **Malicious-node profiles (reused from component-level)**: scaled up to multiple attackers. "One malicious withholds" vs. "two collude on conflicting empty-tx-set values" vs. "minority sends invalid tx sets."
- **Per-node download-timeout calibration**: heterogeneous timeout values to stress the dynamic-timeout mechanism.

### Bias targets

The component-level bias list still applies inside each node. In addition, bias toward multi-node boundaries:

- Tx set arrives for exactly `⌈2/3⌉` of the quorum but not the remainder (straddles the quorum threshold).
- Tx set arrives for a quorum but the remainder's download timeout fires first (tests the "fast quorum, slow minority" path).
- Download timeout on one node fires mid-`combineCandidates` on another.
- One node receives a valid tx set; another receives an invalid tx set with the same hash (tampering / partitioned validity).
- Nomination leader is a node whose tx set never arrives.
- A minority of honest nodes receive no tx set at all; the rest receive it promptly.

### Determinism — the hard problem

Reproducible multi-node scenarios are the main infrastructure challenge. The simulation framework's virtual clock provides a baseline, but we need deterministic control over:

- Inter-node message delivery ordering.
- Timer firing order across nodes within a virtual-time tick.
- Nomination leader round progression and tie-breaking.
- Per-node internal scheduling (asio post order, herder queue order).

An audit of what the existing `simulation/` subsystem already provides is a prerequisite before the generator is built. The component-level determinism work should front a significant portion of this, but cross-node coordination is genuinely new.

## What this approach gives up

- **Valid-but-unexpected network outcomes.** The invariant set does not catch cases where the network produces a valid-but-non-optimal result. Metric-based invariants under good-timing scenarios partially cover performance regressions.
- **Proof of correctness.** This is testing, not verification. It complements IVy-level verification; it does not replace it.
- **Subtle byzantine patterns.** The honest-vs-malicious binary and our specific malicious profiles may miss failure modes where a node is subtly broken but produces well-formed signed messages.

These tradeoffs match the component-level plan's tradeoffs and are acceptable given the goals.

## Additional steps required to add protocol-level tests

These are the concrete additions beyond what the component-level plan already delivers.

1. **Simulation determinism audit.** Characterize what guarantees the existing `simulation/` subsystem provides and what gaps need to close for reproducible multi-node scenarios. Specifically: message delivery ordering, per-node timer firing order within a tick, nomination-leader tie-breaking, and per-node internal scheduling determinism. This is the critical-path prerequisite.
2. **Harness cross-node observation.** Add hooks so the harness can observe every node's externalize events, SCP state transitions, `mPendingTxSetWrappers` state, and emitted messages without perturbing the simulation. Without this the cross-node invariants cannot be checked.
3. **Cross-node invariant checkers.** Implement the harness checks listed above, first against existing hand-authored multi-node simulation tests. Validates the invariants are correctly specified before the generator is built.
4. **Topology and quorum configuration fixtures.** Decide on a small set of canonical topologies (e.g. "tier1-like seven-node," "small-and-dense three-node," "large-and-sparse"). Parameterize the generator over them.
5. **Multi-node scenario generator.** Extend the component-level generator to emit per-node timing schedules, topology and quorum choices, and scaled-up malicious profiles. Reuse the simulated-peer strategy interface from the component plan.
6. **Aggregate-metric invariants.** Compute and assert over network-wide metric aggregates per scenario and per suite. Integrate with the performance-rollout plan from `implementation-plan.md`.
7. **Run-environment decision.** Overnight / pre-release suite vs. CI. Runtime budget probably forces the former; confirm early so reporting / dashboarding can be designed appropriately.

## Open questions

- How deterministic is the existing `simulation/` subsystem today across the axes listed above? What is the minimum set of changes needed?
- Which topology or topologies do we treat as canonical? Tier1-like seems natural given production; should we also include small-and-dense for fast iteration?
- How do we model malicious-node profiles concretely? Ideally as pluggable strategies at the SCP driver level, reusable between component-level and protocol-level tests.
- Can any cross-node invariants be expressed in IVy/TLA+ and piggyback on existing verification effort rather than being duplicated here?
- Do we run protocol-level MBT scenarios in CI or as an overnight / pre-release suite? Runtime budget probably forces the latter; confirm.
- What's the right interface between protocol-level scenarios and the supercluster missions listed in `implementation-plan.md`? Are they complementary, redundant, or sequential?
