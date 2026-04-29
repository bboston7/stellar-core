# Parallel-Download Testing Work — Handoff

Detailed handoff for the agent picking up this work. Everything you need to be productive without re-deriving context.

## Project context

- **Repo**: `/Users/brettboston/Documents/core/stellar-core/parallel-download`
- **Branch**: `component-tests`
- **Feature under test**: parallel transaction set downloading SCP change
- **Background docs**: `parallel-download-docs/` — `README.md`, `theory.md`, `implementation-plan.md`, `claude-learnings.md`, plus the SCP IETF draft and the older `testing.md` design exploration.

The feature lets SCP nominate and ballot on tx set hashes before the tx set itself arrives, falling back to a skip value if the tx set never arrives or arrives invalid. The full protocol change is described in `parallel-download-docs/theory.md` and `parallel-download-docs/implementation-plan.md`.

## Top-level testing plans

Two long-form plan documents live in `testing/`:

- **`testing/claude-component-testing.md`** — the canonical catalog. Contains: invariant catalog (in-code assertions, metric-based invariants, progress/terminal-state invariants), scenario generator design (input space, bias targets, constructed scenario classes), determinism notes, what-this-gives-up, and the 8-phase implementation plan. **Read this first.**
- **`testing/claude-protocol-testing.md`** — protocol-level (multi-node) plan. References component-level work as a prerequisite. Covers the Herder-level-and-beyond track. Skipping ahead to its "Additional steps required" section gives the concrete deliverables.

The 8 phases (per claude-component-testing.md):

| Phase | Title | Status |
|---|---|---|
| 1 | In-code assertions | **Done** |
| 2 | SCP-level harness extensions | **Done** |
| 3 | SCP-level scenario generator | **Mostly done** — three closeout items pending; see "Phase 3 closeout" below |
| 4 | SCP-level metric, progress, terminal-state invariants | **Not started** |
| 5 | Herder-level harness extensions | Not started |
| 6 | Herder-level scenario generator | Not started |
| 7 | Herder-level metric, progress, terminal-state invariants | Not started |
| 8 | Scale and tune | Not started |

The user has a concrete list of work to land before declaring Phase 3 complete; see the **"Phase 3 closeout"** section below for details. Until that's done, do not start Phase 4.

## What's been implemented

### Phase 1 — in-code assertions (already merged on branch)

`releaseAssert` calls added to flag invariant violations of the parallel-download protocol. Tests pass cleanly. Per the user's confirmation: tests pass; don't run tests as the agent — the user runs them.

In `src/scp/BallotProtocol.cpp`:
- Refined `setConfirmPrepared` commit-gate assertion + upgraded `dbgAssert(!mCommit)` at line ~1198 to `releaseAssert`. Added `releaseAssert(validationLevel != kInvalidValue && validationLevel != kAwaitingDownload)` in the same branch.
- New `setConfirmCommit` boundary assertion at line ~1694 (immediately before `valueExternalized`): re-validates `mCommit->getBallot().value` and asserts not `kInvalidValue` and not `kAwaitingDownload`.
- 11 `dbgAssert` → `releaseAssert` upgrades inside `checkInvariants` (~lines 711–751).
- 2 `dbgAssert` → `releaseAssert` upgrades in `bumpToBallot` at lines 525, 530–531.

In `src/herder/HerderSCPDriver.cpp`:
- `validateValueAgainstLocalState`: `releaseAssert(res != kAwaitingDownload || isCurrentLedger)` postcondition at function exit (covers both the "kAwaitingDownload only for LCL+1" and "validatePastOrFutureValue never returns kAwaitingDownload" invariants).
- `extractValidValue`: `releaseAssert(res != nullptr)` inside the `>= kAwaitingDownload` branch (postcondition).
- `makeSkipLedgerValueFromValue`: `releaseAssert(originalValue.ext.v() == STELLAR_VALUE_SIGNED)` precondition.
- `onTxSetReceived` entry: `releaseAssert(txSet != nullptr)` and `releaseAssert(txSet->getContentsHash() == txSetHash)`.

In `src/herder/HerderImpl.cpp`: existing `releaseAssert(externalizedSet != nullptr)` at line 343 documented as the application-layer backstop. No code change.

The user's rule: **always `releaseAssert`, never debug-only `assert` or `dbgAssert`. If a check is too expensive, gate it behind `#ifdef BUILD_TESTS` rather than demoting it.**

### Phase 2 — SCP-level harness extensions

All in `src/scp/test/SCPTests.cpp`'s anonymous namespace.

- `QuorumFixture` struct + `makeQuorumFixture(int nodeCount, int threshold)` configurable builder + `canonical3Node()` / `canonical4Node()` convenience helpers (both `[[maybe_unused]]` because not every test uses both sizes).
- `PeerProfile` POD struct (secret key + qSetHash) with thin `nominate` / `prepare` / `confirm` / `externalize` methods that forward to the existing file-scope `makeNominate`/`makePrepare`/`makeConfirm`/`makeExternalize` helpers.
- `makePeerProfile(fixture, index)` factory.
- `corruptEnvelope(env)` — zeroes the signature. Mostly symbolic at the SCP-mock level since `TestSCP::signEnvelope` is a no-op.
- `skipify(scp, v)` — wraps the value via `TestSCP::makeSkipLedgerValueFromValue` (which prepends `"SKIP:"` to the bytes).

Two hand-authored TEST_CASEs were added at the end of the file. **Test 1 was reworked during the Herder-gate-emulation change** (see below) — its current form is a gate-correctness test:

- **"Herder-gate emulation: CONFIRM/EXTERNALIZE buffered while kAwaitingDownload"** (was originally "PREPARE-driven setAcceptCommit while kAwaitingDownload") — uses 4-node threshold-3, sends CONFIRMs while xValue is kAwaitingDownload, asserts they're buffered, calls `clearDownload`, asserts externalize completes.
- **"Mixed peer profiles smoke test"** — 4-node fixture, mixes cooperative + sends-invalid + sends-skip + sends-malformed peer behaviors. Pass condition is "no in-code assertion fires."

### Phase 3 — SCP-level scenario generator + follow-ups

All in `src/scp/test/SCPTests.cpp`'s anonymous namespace.

**Core types and runner:**

- `ScenarioEvent` struct with four kinds: `ReceiveEnvelope`, `TxSetArrives`, `FireBallotTimer`, `AdvanceTimerOffset`. Factory methods: `receive(env)`, `txSetArrives(value)`, `fireBallotTimer()`, `advanceTimerOffset()`.
- `using Scenario = std::vector<ScenarioEvent>;`
- `runScenario(TestSCP& scp, Scenario const& scenario, QuorumFixture const& fixture)` — walks events imperatively. **Each step emits per-step `INFO` and `CLOG_DEBUG` for debug logging** (see "Logging" section below). Note: takes the fixture (not just scp + scenario) for peer-name lookup in the log output.

**Generators:**

- `generateCooperative(fixture)` — deterministic; every peer sends one PREPARE-with-prepared-and-nC-and-nH plus one CONFIRM. v0 should externalize xValue.
- `generateCooperativeButSlow(fixture, scp)` — deterministic; tx set never arrives (caller seeds `startDownload(xValue, 6000ms)` so first `bumpState` replaces with skip via `maybeReplaceValueWithSkip`). Peers echo the skip value.
- `generateCooperativeWithDownload(fixture)` — base for biased-random; peers send PREPAREs first, then `TxSetArrives`, then CONFIRMs.
- `generateBiasedRandom(fixture, rng)` — starts from `generateCooperativeWithDownload` and applies 1–3 random perturbations from `applyRandomPerturbations`.

**Perturbation operators (5 of 8 catalog bias targets):**

| Operator | Catalog target |
|---|---|
| `shiftTxSetArrival` | tx-set-arrives at/just-before/just-after skip-timer expiry |
| `duplicateEnvelope` | duplicate envelope delivery |
| `replaceWithYValue` | sends-invalid peer (test configures `mValidateValueOverride` to mark yValue as `kInvalidValue`) |
| `corruptOneEnvelope` | sends-malformed envelope |
| `injectVBlockingConfirms` | setAcceptCommit + CONFIRM gating |

**Deferred targets (TODO):** tx-set arrives after slot purge / ledger advanced; previously-silent peer tips quorum at the moment of tx-set arrival; skip value combined with valid tx set (`combineCandidates` path).

**Tests:**

- "Generator: cooperative scenarios" — parameterized over `GENERATE(3, 4, 5)` fixture sizes; deterministic single scenario per fixture; assert externalize == xValue.
- "Generator: cooperative-but-slow scenarios" — same parameterization; assert externalize is a skip value.
- "Generator: biased-random scenarios" — same parameterization, **50 scenarios per fixture** (150 total). Configures `mValidateValueOverride` per-iteration so the override interacts correctly with the gate.

**Logging (Phase 3 follow-up):**

- `formatPeerName(NodeID, fixture)` — reverse-maps NodeID to `v<i>` (or `?`).
- `formatValue(Value)` — symbolic name for the well-known test globals (`xValue`/`yValue`/`zValue`/`zzValue`/`kValue`); recursively unwraps the `SKIP:`-byte prefix so `skipify(scp, xValue)` renders as `skip(xValue)`. Falls back to `hexAbbrev` for unrecognized values. Both `formatBallot` and the `TxSetArrives` arm of `formatScenarioEvent` route through it.
- `formatBallot(SCPBallot)` — `(counter, formatValue(value))`.
- `formatScenarioEvent(ev, fixture)` — one-line summary covering all four event kinds and all four SCP statement types.
- `scenarioToString(s, fixture)` — multi-line `Scenario (N events):\n  [<i>] ...`.

In the biased-random TEST_CASE, before `runScenario`:
```cpp
INFO("Iteration " << i << " (fixture size " << fixtureSize << ")");
auto scenarioStr = scenarioToString(scenario, fixture);
INFO("Scenario:\n" << scenarioStr);
CLOG_DEBUG(SCP, "Iteration {} (fixture size {}) scenario:\n{}", i,
           fixtureSize, scenarioStr);
```

Inside `runScenario`, before each event:
```cpp
auto stepStr = formatScenarioEvent(ev, fixture);
INFO("Step " << stepIdx << ": " << stepStr);
CLOG_DEBUG(SCP, "Step {}: {}", stepIdx, stepStr);
```

Catch2 `INFO` is captured and printed only on `REQUIRE` failures within scope. `CLOG_DEBUG` is silent at the default log level; running with debug logging enabled flushes lines for crash investigation since `releaseAssert` aborts the test binary mid-flight.

**Herder-gate emulation in TestSCP (Phase 3 follow-up):**

The biased-random generator was producing false positives — `injectVBlockingConfirms` and `duplicateEnvelope` could put CONFIRMs referencing a `kAwaitingDownload` value into the scenario before `TxSetArrives`. SCP would federated-accept then federated-confirm in the same `advanceSlot` pass, hitting the Phase 1 assertion at the `setConfirmCommit → valueExternalized` boundary. In production this can't happen because `HerderImpl::recvSCPEnvelope` (line 934) gates CONFIRM/EXTERNALIZE envelopes at `ENVELOPE_STATUS_FETCHING` until the tx set arrives.

`TestSCP` now mirrors this gate. Always on — no flag — because production never bypasses the gate.

- New member: `std::map<Value, std::vector<SCPEnvelope>> mBufferedEnvelopes;`
- New static helper: `gatedReferencedValue(envelope)` returns the ballot value for CONFIRM/EXTERNALIZE statements, nullptr for NOMINATE/PREPARE.
- `receiveEnvelope`: if envelope is CONFIRM/EXTERNALIZE and references a value in `mDownloadWaitTimes`, push to `mBufferedEnvelopes[refValue]` and return without forwarding to SCP.
- `clearDownload(v)`: after erasing from `mDownloadWaitTimes`, look up `mBufferedEnvelopes[v]`, move out, erase the map entry, then re-feed the buffered envelopes through `receiveEnvelope` (the recursive call is safe — `v` is no longer in `mDownloadWaitTimes`).

The Phase 1 assertion at `setConfirmCommit → valueExternalized` remains the safety net behind the gate.

**Forced-commit-on-invalid-value catch (Phase 3 follow-up):**

The biased-random TEST_CASE in `src/scp/test/SCPTests.cpp` (around line 4975) wraps `runScenario` in a try/catch:

```cpp
try
{
    runScenario(scp, scenario, fixture);
}
catch (std::runtime_error const& e)
{
    if (std::string(e.what()).find(
            "SCP forced commit on locally-invalid value") ==
        std::string::npos)
    {
        throw;
    }
}
```

`replaceWithYValue` can land on two distinct PREPAREs (or two CONFIRMs), producing a v-blocking set whose ballots all carry yValue with `nC=1, nH=1`. v0 federated-accepts commit on yValue, and `BallotProtocol::throwIfValueInvalidForCommit` (BallotProtocol.cpp ~line 1469) throws a `std::runtime_error` with message `"SCP forced commit on locally-invalid value (slot=N, caller=...)"` (commit `22548a7c9 Detect and offer useful error on transition to invalid state`). Two `caller=` values exist: `setAcceptCommit` and `setConfirmCommit`; the prefix match handles both.

Limitation acknowledged in the closeout list below: the catch is **permissive** — it accepts the exception unconditionally as long as the message matches. A regression that fired the same exception when the scenario didn't actually justify it would still pass. Tightening this is closeout item 2.

The deterministic generator tests (`generateCooperative`, `generateCooperativeButSlow`) intentionally do **not** wrap `runScenario` — neither involves yValue, so neither should ever throw.

The pattern (raw try/catch + `std::string(e.what()).find(...)`) matches the established style at SCPTests.cpp ~line 4675 (TEST_CASE `"setAcceptCommit throws when quorum forces invalid value"`) and `src/ledger/test/LedgerCloseMetaStreamTests.cpp:160`.

## Phase 3 closeout — pending work before Phase 3 is "done"

The user wants three concrete items finished before moving on to Phase 4. None blocks the others; they can be picked up in any order.

1. **Diversify generated SCP statement shapes.** Today the generators only emit a single PREPARE shape (`ballot=(1, v) prepared=(1, v) nC=1 nH=1`) and a single CONFIRM shape (`ballot=(1, v) nPrepared=1 nCommit=1 nH=1`). The state space is broader: PREPAREs can have `h` set but not `c` (i.e. confirmed-prepared without yet voting-to-commit), `prepared'` set, distinct ballot counters, etc. CONFIRM has narrower variation but should still be audited. Generate multiple shapes per statement type so biased-random scenarios exercise the full grammar.

2. **Tighten the runtime_error catch (item from "Forced-commit-on-invalid-value catch" above).** The current catch unconditionally accepts the matched message. Add a justification check inside the handler: walk the *executed* portion of the scenario, count the distinct peers who voted-to-commit (PREPARE with `nC≥1, nH≥nC` referencing the same ballot, or CONFIRM with `nCommit≥1`) on a value that the override marks `kInvalidValue`. If that count is ≥ v-blocking-size for the fixture (`fixture.size() - threshold + 1`), the exception is justified — pass. Otherwise the exception fired without cause — re-throw. This converts the catch from "tolerate the known artifact" into "verify the protocol's intentional signal fires only when warranted."

3. **Add the three remaining catalog bias targets.** From `claude-component-testing.md`'s Phase 3 entry, deferred earlier:
   - tx-set arrives after the slot has been purged / ledger advanced.
   - Previously-silent peer tips the node into quorum at the moment the tx-set arrives.
   - Skip value received from a peer combined with a valid tx-set received locally (`combineCandidates` path — currently nomination-only on `TestSCP`, which may need extending).

   Each becomes a new perturbation operator (or perhaps a constructed-class generator for the third, since `combineCandidates` is more naturally driven by composing candidates than by perturbing a base scenario). Update the operator-vs-catalog-target table in the "Phase 3 — SCP-level scenario generator + follow-ups" section above when each lands.

## Key file locations

### Test infrastructure
- **`src/scp/test/SCPTests.cpp`** — everything for Phases 2 + 3. Roughly:
  - `class TestSCP` (~line 30–410) — mock `SCPDriver`, owns SCP state machine, has `mDownloadWaitTimes`, `mBufferedEnvelopes`, `mEnvs`, etc.
  - `class TestNominationSCP` (right after TestSCP).
  - Anonymous namespace (~line 414–end of namespace ~line 1085+):
    - `setupValues()` + global `xValue`/`yValue`/`zValue`/`zzValue`/`kValue`.
    - Envelope builders (`makeEnvelope`, `makeNominate`, `makePrepare`, `makeConfirm`, `makeExternalize`).
    - Verify helpers (`verifyPrepare` etc.).
    - **Phase 2 helpers**: `QuorumFixture`, `makeQuorumFixture`, `canonical3Node`, `canonical4Node`, `PeerProfile`, `makePeerProfile`, `corruptEnvelope`, `skipify`.
    - **Phase 3 generator**: `ScenarioEvent`, `Scenario`, forward-decl of `formatScenarioEvent`, `runScenario`, `generateCooperative`, `generateCooperativeButSlow`, `generateCooperativeWithDownload`, `findReceiveEnvelopeIndices`, perturbation operators, `applyRandomPerturbations`, `generateBiasedRandom`, `thresholdFor`.
    - **Phase 3 follow-up logging helpers**: `formatPeerName`, `formatBallot`, `formatScenarioEvent`, `scenarioToString`.
  - TEST_CASEs from ~line 1095 onwards. Phase 2 + 3 tests at the end of the file.

### Production code (Phase 1 assertion targets, also referenced by docs)
- `src/scp/BallotProtocol.cpp`/`.h` — assertions in `setConfirmPrepared` (~1146–1207), `setConfirmCommit` (~1674–1697), `bumpToBallot` (~515), `checkInvariants` (~711–751). Also `maybeReplaceValueWithSkip` (~356–403) and `throwIfValueInvalidForCommit` (~1443).
- `src/herder/HerderSCPDriver.cpp` — `validateValueAgainstLocalState` (~348), `validatePastOrFutureValue` (~230), `validateValue` (~507), `extractValidValue` (~548), `makeSkipLedgerValueFromValue` (~598), `onTxSetReceived` (~1628), `combineCandidates` (~826).
- `src/herder/HerderImpl.cpp` — `recvSCPEnvelope` gate at line 934, `processExternalized` `releaseAssert` at line 343.

### Plan files
- `~/.claude/plans/` — each plan-mode session creates a new file with a unique random name (e.g. `shimmering-forging-micali.md`, `cuddly-spinning-barto.md`). The system message at the start of plan mode supplies the path for the current session. Older plan files are kept for reference and not overwritten.

## Important design decisions and rationale

### TestSCP is a mock, not the real driver
`TestSCP` subclasses `SCPDriver` directly. It does **not** use real `HerderSCPDriver`/`PendingEnvelopes`. Implications:
- `mDownloadWaitTimes` map is the source of truth for "is this value kAwaitingDownload?". The mock `validateValue` checks this map.
- `signEnvelope` is a no-op. Signatures are not verified at the SCP-mock level. `corruptEnvelope` zeroing the signature is therefore mostly symbolic.
- `makeSkipLedgerValueFromValue` prepends `"SKIP:"` to the value bytes. Real production builds a `STELLAR_VALUE_SKIP` XDR struct. **`getValueString` won't work for test values** (they're SHA256 hashes, not real `StellarValue`s — they don't deserialize).

This decision keeps Phase 2/3 tests fast and decoupled from full Herder/Application setup. Phase 5 (Herder-level harness) will exercise the real driver.

### Herder gate is always on in TestSCP (no flag)
The user explicitly rejected an opt-in flag: "We should always emulate the herder gate. If that requires modifying a test, so be it. It's unrealistic to not have the herder gate." Test 1 was reworked rather than disabling the gate.

### Imperative scenario style, not declarative
Earlier discussion considered a declarative DSL (`scenario.addEvent(t=50ms, "tx set arrives")`). Rejected. Keep imperative event lists. Phase 3 follows the existing manual-test style with structured event factories (`ScenarioEvent::receive(env)` etc.).

### Shrinking dropped from the plan
The user asked to drop shrinking entirely to keep things simple. Use the existing seeded `getGlobalRandomEngine()` from `util/Math.h` for reproducibility — `--rng-seed` controls determinism via Catch2.

### 5 of 8 bias targets in initial Phase 3
The deferred 3 targets are captured in `claude-component-testing.md`'s Phase 3 entry as TODO. Pickup: do them as a Phase 3 extension before Phase 4, or fold into Phase 4.

### Logging strategy
INFO + CLOG_DEBUG at two layers (iteration-level and per-step inside runScenario). INFO is captured by Catch2 for graceful `REQUIRE` failures. CLOG_DEBUG is silent by default but flushes per-line so the most recent log survives an in-process abort.

## Conventions and patterns

### Catch2 `GENERATE` for fixture parameterization
The Phase 3 tests use `GENERATE(3, 4, 5)` to re-run each TEST_CASE per fixture size. Catch2 re-runs the entire test body for each generated value.

### Threshold table for fixtures
Hardcoded for the canonical small sizes: 3-node → threshold 2, 4-node → threshold 3, 5-node → threshold 4 (standard 2/3 BFT). See `thresholdFor(int nodeCount)` in SCPTests.cpp. Adding a 7-node fixture is a one-line addition.

### `setupValues()` regenerates xValue/yValue/zValue per call
Uses `getGlobalRandomEngine()` for the seed. Inside the biased-random loop, `setupValues()` is called per iteration so different scenarios exercise different value bytes (some perturbations are value-keyed, e.g. `mValidateValueOverride` matching on yValue).

### Build / test workflow
- Build with `make -j9` from the repo root.
- **Do not run tests as the agent.** The user runs them.
- Verify after each major change: `make -j9` ⇒ clean build (modulo a single pre-existing `qSetHash0` unused-variable warning at SCPTests.cpp ~line 4395 in someone else's test).

### IDE diagnostics noise
The clangd / cSpell diagnostics emitted during edits are pre-existing IDE artifacts (clangd doesn't have proper include paths for this file; cSpell dictionary doesn't know words like "skipify", "Soroban", "ballotprotocol"). The actual compiler output from `make -j9` is the source of truth for "does this build?".

### sandbox warnings during `make -j9`
The build output may show `xcrun_db` cache file errors and `ld: warning: -bind_at_load is deprecated` — both are harmless macOS sandbox / linker noise, not real build failures.

## Gotchas

### `bumpTimerOffset` doesn't fire callbacks
`TestSCP::bumpTimerOffset()` jumps `mCurrentTimerOffset` by 5 hours. **Callbacks are NOT automatically fired.** Existing tests rely on this for "time has passed but timer hasn't fired" semantics, then manually invoke callbacks via `auto cb = scp.getBallotProtocolTimer().mCallback; cb();`. If you ever need a "fine clock + fire" helper, make sure it doesn't mix with `bumpTimerOffset` in the same test.

### `mValidateValueOverride` replaces default behavior
If you override `validateValue` via `mValidateValueOverride`, you fully replace the default mock behavior. To preserve the kAwaitingDownload-from-mDownloadWaitTimes default, your override must replicate it. See the biased-random test's lambda for the canonical pattern.

### `attemptAcceptCommit` and `attemptConfirmCommit` run synchronously
Both run in the same `advanceSlot` pass (BallotProtocol.cpp ~line 2074, 2076). At TestSCP level, this means once a CONFIRM envelope drives v0 to `setAcceptCommit`, `setConfirmCommit` can fire on the same envelope if quorum-of-accept is reached. This is what makes the Herder gate necessary at TestSCP level — without it, kAwaitingDownload values reach `setConfirmCommit` immediately.

### Quorum / v-blocking math at small sizes
- 3-node threshold-2: any 2 nodes is v-blocking.
- 4-node threshold-3: any 2 of 4 peers is v-blocking; v0 + 2 peers = quorum.
- 5-node threshold-4: any 2 of 4 peers is v-blocking; v0 + 3 peers = quorum.

The original Test 1 used 5-node-threshold-4 specifically so v0+v1+v2 = 3 < 4 = quorum size, preventing setConfirmCommit from firing alongside setAcceptCommit. After the Herder gate, this trick isn't needed and Test 1 uses 4-node threshold-3.

### Skip values at TestSCP level
- `TestSCP::makeSkipLedgerValueFromValue(v)` returns `"SKIP:" + v` byte-wise.
- `TestSCP::isSkipLedgerValue(v)` checks the prefix.
- The mock `validateValue` returns `kFullyValidatedValue` for skip values (they aren't in `mDownloadWaitTimes`).
- Skip-value envelopes are NOT subject to the new gate (they aren't kAwaitingDownload).

## Open issues / TODOs

Items 1–3 of the **"Phase 3 closeout"** section above are the immediate-priority TODOs and are not duplicated here. The list below is the longer-horizon work.

1. **Phase 4 not started.** Catalog of metric / progress / terminal-state invariants is in `claude-component-testing.md`. Initial deliverables: skip-count metric, blocked-time metric, kAwaitingDownload-observation bound, deadlock / terminal-state check, progress invariants for cooperative scenario classes. Do not begin until Phase 3 closeout is finished.
2. **Phase 1 Herder-level assertions are not directly tested.** They fire via existing Simulation tests when they fire at all. Phase 5 will give them targeted unit-style coverage.
3. **Scenario count tuning.** 50 biased-random scenarios per fixture × 3 fixtures = 150 total per test run. Healthy budget; tune up/down based on test runtime once Phase 4 invariants land.
4. **Catalog doc rough edges** — `testing/claude-component-testing.md` Phase 7 says it covers "any cross-cutting catalog items not exercisable at the SCP level," but no concrete walk-through of which items those are. Worth a deliberate audit when Phase 7 begins.

## Workflow with the user

- The user puts the agent in/out of plan mode explicitly. **Do not enter plan mode unprompted.**
- Each plan-mode session writes to a fresh file under `~/.claude/plans/` (filename supplied by the system message). Older plans aren't overwritten — they accumulate as a session log.
- The user runs tests; the agent only builds (`make -j9`).
- When the user provides a failing test output, the failing seed and iteration number are usually enough context to reproduce — re-running the test with `--rng-seed <n>` should reproduce.
- Conversational drafts are common. Often the user wants a plan in conversation first, iterates, then puts the agent in plan mode for the formal plan-file flow.

## Quick orientation: what to do first as the next agent

1. Read the **"Phase 3 closeout"** section above. Those three items are what the user wants done before Phase 4 starts.
2. Read `testing/claude-component-testing.md` (the catalog and phase plan) and `testing/claude-protocol-testing.md` (protocol-level plan).
3. Skim the new code in `src/scp/test/SCPTests.cpp`'s anonymous namespace from the Phase 2 helpers through the Phase 3 generator and logging helpers (~line 580 to end of namespace), plus the biased-random TEST_CASE around line 4920+ and the `formatValue` / try-catch additions.
4. Skim the Phase 1 assertions in `BallotProtocol.cpp` (lines ~1198, ~1694, `checkInvariants`, plus `throwIfValueInvalidForCommit` ~1444) and `HerderSCPDriver.cpp` (lines ~410, ~558, ~601, ~1628).
5. Scan `parallel-download-docs/theory.md` and `parallel-download-docs/claude-learnings.md` for the protocol design context.
6. Confirm the build is green: `make -j9` from the repo root.
7. Ask the user which of the three closeout items they want to start with.
