// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Tracker.h"

#include "OverlayMetrics.h"
#include "crypto/BLAKE2.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/meter.h"
#include "overlay/OverlayManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/Math.h"
#include <Tracy.hpp>

namespace stellar
{

std::chrono::milliseconds const Tracker::MS_TO_WAIT_FOR_FETCH_REPLY{1500};
int const Tracker::MAX_REBUILD_FETCH_LIST = 10;

namespace
{
medida::Meter&
nextPeerDontHaveMeter(OverlayMetrics& om, ItemFetcherKind kind)
{
    return kind == ItemFetcherKind::TxSet
               ? om.mItemFetcherTxSetNextPeerDontHave
               : om.mItemFetcherQSetNextPeerDontHave;
}
medida::Meter&
nextPeerTimeoutMeter(OverlayMetrics& om, ItemFetcherKind kind)
{
    return kind == ItemFetcherKind::TxSet ? om.mItemFetcherTxSetNextPeerTimeout
                                          : om.mItemFetcherQSetNextPeerTimeout;
}
}

Tracker::Tracker(Application& app, Hash const& hash, AskPeer& askPeer,
                 ItemFetcherKind kind)
    : mAskPeer(askPeer)
    , mApp(app)
    , mNumListRebuild(0)
    , mTimer(app)
    , mItemHash(hash)
    , mKind(kind)
    , mTryNextPeer(
          app.getOverlayManager().getOverlayMetrics().mItemFetcherNextPeer)
    , mNextPeerDontHave(nextPeerDontHaveMeter(
          app.getOverlayManager().getOverlayMetrics(), kind))
    , mNextPeerTimeout(nextPeerTimeoutMeter(
          app.getOverlayManager().getOverlayMetrics(), kind))
    , mFetchTime("fetch-" + hexAbbrev(hash), LogSlowExecution::Mode::MANUAL)
{
    releaseAssert(mAskPeer);

    // The claim grace only applies to tx set fetches: the qset fetcher never
    // receives HAS_TX_SET claims, so a grace there would only add latency.
    auto const grace = mApp.getConfig().EXPERIMENTAL_TX_SET_FETCH_CLAIM_GRACE;
    if (mKind == ItemFetcherKind::TxSet && grace.count() > 0)
    {
        mGraceEnabled = true;
        mGraceStart = mApp.getClock().now();
        mGraceDeadline = mGraceStart + grace;
    }
}

Tracker::~Tracker()
{
    cancel();
}

SCPEnvelope
Tracker::pop()
{
    auto env = mWaitingEnvelopes.back().second;
    mWaitingEnvelopes.pop_back();
    return env;
}

// returns false if no one cares about this guy anymore
bool
Tracker::clearEnvelopesOutsideRange(std::optional<uint64> minSlot,
                                    std::optional<uint64> maxSlot,
                                    uint64 slotToKeep)
{
    ZoneScoped;
    for (auto iter = mWaitingEnvelopes.begin();
         iter != mWaitingEnvelopes.end();)
    {
        auto const index = iter->second.statement.slotIndex;
        bool const outsideRange =
            (minSlot && index < *minSlot) || (maxSlot && index > *maxSlot);
        if (outsideRange && index != slotToKeep)
        {
            iter = mWaitingEnvelopes.erase(iter);
        }
        else
        {
            iter++;
        }
    }
    if (!mWaitingEnvelopes.empty())
    {
        return true;
    }

    mTimer.cancel();
    mLastAskedPeer = nullptr;

    return false;
}

void
Tracker::doesntHave(Peer::pointer peer)
{
    if (mLastAskedPeer == peer)
    {
        CLOG_TRACE(Overlay, "Does not have {}", hexAbbrev(mItemHash));
        // Any claim this peer made was wrong. Remember when the miss
        // happened so the peer can be re-asked once
        // EXPERIMENTAL_TX_SET_FETCH_REASK_DELAY (if set) elapses.
        mClaimingPeers.erase(peer);
        mDontHaveSince[peer] = mApp.getClock().now();
        tryNextPeer(NextPeerCause::DontHave);
    }
}

void
Tracker::peerClaims(Peer::pointer peer)
{
    ZoneScoped;
    mClaimingPeers.insert(peer);
    if (!mLastAskedPeer)
    {
        // No ask is outstanding (e.g. idling in a rebuild backoff or the claim
        // grace window); act on the claim immediately instead of waiting out
        // the timer.
        mTimer.cancel();
        tryNextPeer(NextPeerCause::Other);
    }
}

void
Tracker::seedClaim(Peer::pointer peer)
{
    // Insert-only: the caller (ItemFetcher::fetch) drives the first
    // tryNextPeer after seeding all buffered claims.
    mClaimingPeers.insert(peer);
}

void
Tracker::tryNextPeer(NextPeerCause cause)
{
    ZoneScoped;
    // will be called by some timer or when we get a
    // response saying they don't have it
    CLOG_TRACE(Overlay, "tryNextPeer {} last: {}", hexAbbrev(mItemHash),
               (mLastAskedPeer ? mLastAskedPeer->toString() : "<none>"));

    if (mLastAskedPeer)
    {
        mTryNextPeer.Mark();
        // Attribute this abandonment to its cause for the metric split. The
        // aggregate above always fires; only DONT_HAVE/timeout get a sub-meter
        // (the first ask and claim-triggered asks have no outstanding peer).
        if (cause == NextPeerCause::DontHave)
        {
            mNextPeerDontHave.Mark();
        }
        else if (cause == NextPeerCause::Timeout)
        {
            mNextPeerTimeout.Mark();
        }
        mLastAskedPeer.reset();
    }

    auto const reaskDelay =
        mApp.getConfig().EXPERIMENTAL_TX_SET_FETCH_REASK_DELAY;
    auto const now = mApp.getClock().now();

    // A peer that answered DONT_HAVE becomes re-askable once the configured
    // cooldown elapses, as it may have completed its own fetch since.
    auto cooldownExpired = [&](Peer::pointer const& p) {
        if (reaskDelay.count() <= 0)
        {
            return false;
        }
        auto dh = mDontHaveSince.find(p);
        return dh != mDontHaveSince.end() && now - dh->second >= reaskDelay;
    };

    // canAskPeer is best effort and send happens asynchronously; in the worst
    // case, we'll place something in the queue that will subsequently be
    // discarded due to a peer drop.
    auto canAskPeer = [&](Peer::pointer const& p, bool peerHas) {
        if (!p->isAuthenticatedAtomic())
        {
            return false;
        }
        auto it = mPeersAsked.find(p);
        return it == mPeersAsked.end() || (peerHas && !it->second) ||
               cooldownExpired(p);
    };

    // Helper function to populate "candidates" with a set of peers, which we're
    // going to randomly select a candidate from to ask for the item.
    //
    // We want to bias the candidates set towards peers that are close to us in
    // terms of network latency, so we repeatedly lower a "nearness threshold"
    // in units of 500ms (1/3 of the MS_TO_WAIT_FOR_FETCH_REPLY) until we have a
    // "closest peers" bucket that we have at least one peer for, and keep all
    // the peers in that bucket, and then (later) randomly select from it.
    //
    // if the map of peers passed in is for peers that claim to have the data we
    // need, `peersHave` is also set to true. in this case, the candidate list
    // will also be populated with peers that we asked before but that since
    // then received the data that we need
    std::vector<Peer::pointer> candidates;
    int64 curBest = INT64_MAX;

    auto procPeers = [&](std::map<NodeID, Peer::pointer> const& peerMap,
                         bool peersHave) {
        for (auto& mp : peerMap)
        {
            auto& p = mp.second;
            if (canAskPeer(p, peersHave))
            {
                int64 GROUPSIZE_MS = (MS_TO_WAIT_FOR_FETCH_REPLY.count() / 3);
                int64 plat = p->getPing().count() / GROUPSIZE_MS;
                if (plat < curBest)
                {
                    candidates.clear();
                    curBest = plat;
                    candidates.emplace_back(p);
                }
                else if (curBest == plat)
                {
                    candidates.emplace_back(p);
                }
            }
        }
    };

    // Peers that explicitly announced (via HAS_TX_SET) that they have the
    // item are the most reliable targets.
    std::map<NodeID, Peer::pointer> claimingPeers;
    for (auto const& p : mClaimingPeers)
    {
        if (canAskPeer(p, true))
        {
            claimingPeers.emplace(p->getPeerID(), p);
        }
    }

    // Claim grace: with no peer known to hold the tx set yet, prefer waiting a
    // bounded time for a HAS_TX_SET claim over blind-asking a relayer that
    // likely does not have it yet under parallel downloading. An arriving
    // claim preempts the wait via peerClaims(); once the grace expires we fall
    // through to the relayer/random tiers as usual.
    if (claimingPeers.empty() && mGraceEnabled && now < mGraceDeadline)
    {
        auto const wait = std::chrono::duration_cast<std::chrono::milliseconds>(
            mGraceDeadline - now);
        mTimer.expires_from_now(wait);
        mTimer.async_wait([this]() { this->tryNextPeer(NextPeerCause::Other); },
                          VirtualTimer::onFailureNoop);
        return;
    }

    // With HAS_TX_SET announcements enabled, a peer that relayed an envelope
    // merely knows OF the item: envelopes are relayed before tx sets are
    // fetched when parallel tx set downloading is on, so relaying no longer
    // implies possession — explicit claims do. Without announcements, keep
    // the historical assumption that relaying implies possession.
    bool const relayersImplyHave = !mApp.getConfig().EXPERIMENTAL_HAS_TX_SET;

    // build the set of peers we didn't ask yet that have (or know of) this
    // envelope
    std::map<NodeID, Peer::pointer> newPeersWithEnvelope;
    for (auto const& e : mWaitingEnvelopes)
    {
        auto const& s = mApp.getOverlayManager().getPeersKnows(e.first);
        for (auto pit = s.begin(); pit != s.end(); ++pit)
        {
            auto& p = *pit;
            if (canAskPeer(p, relayersImplyHave))
            {
                newPeersWithEnvelope.emplace(p->getPeerID(), *pit);
            }
        }
    }

    bool claimTierSelected = false;
    bool selectedPeersHave = false;
    if (!claimingPeers.empty())
    {
        claimTierSelected = true;
        selectedPeersHave = true;
        procPeers(claimingPeers, true);
    }
    else if (!newPeersWithEnvelope.empty())
    {
        selectedPeersHave = relayersImplyHave;
        procPeers(newPeersWithEnvelope, relayersImplyHave);
    }
    else
    {
        auto& inPeers = mApp.getOverlayManager().getInboundAuthenticatedPeers();
        auto& outPeers =
            mApp.getOverlayManager().getOutboundAuthenticatedPeers();
        procPeers(inPeers, false);
        procPeers(outPeers, false);
    }

    // pick a random element from the candidate list
    if (!candidates.empty())
    {
        mLastAskedPeer = rand_element(candidates);
    }

    std::chrono::milliseconds nextTry;
    if (!mLastAskedPeer)
    {
        // If previously-asked peers are merely cooling down after a
        // DONT_HAVE, wait for the soonest cooldown to expire rather than
        // rebuilding the whole fetch list with backoff. Drop entries for
        // disconnected peers, which can never be re-asked.
        std::optional<VirtualClock::time_point> soonestReask;
        if (reaskDelay.count() > 0)
        {
            for (auto it = mDontHaveSince.begin(); it != mDontHaveSince.end();)
            {
                if (!it->first->isAuthenticatedAtomic())
                {
                    it = mDontHaveSince.erase(it);
                    continue;
                }
                auto const expiry = it->second + reaskDelay;
                if (!soonestReask || expiry < *soonestReask)
                {
                    soonestReask = expiry;
                }
                ++it;
            }
        }

        if (soonestReask)
        {
            nextTry = std::max(
                std::chrono::milliseconds(1),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    *soonestReask - now));
            CLOG_TRACE(Overlay, "tryNextPeer {} waiting {} ms to re-ask",
                       hexAbbrev(mItemHash), nextTry.count());
        }
        else
        {
            // we have asked all our peers, reset the list and try again
            // after a pause
            mNumListRebuild++;
            mPeersAsked.clear();
            mDontHaveSince.clear();

            CLOG_TRACE(Overlay, "tryNextPeer {} restarting fetch #{}",
                       hexAbbrev(mItemHash), mNumListRebuild);

            nextTry = MS_TO_WAIT_FOR_FETCH_REPLY *
                      std::min(MAX_REBUILD_FETCH_LIST, mNumListRebuild);
        }
    }
    else
    {
        auto& om = mApp.getOverlayManager().getOverlayMetrics();
        auto prevAsked = mPeersAsked.find(mLastAskedPeer);
        if (claimTierSelected)
        {
            om.mItemFetcherClaimAsk.Mark();
        }
        if (prevAsked != mPeersAsked.end() &&
            !(selectedPeersHave && !prevAsked->second))
        {
            // This peer was previously asked and only became askable again
            // because its DONT_HAVE cooldown expired (the other re-ask path —
            // asked without a claim, then claimed — is excluded above).
            om.mItemFetcherCooldownReask.Mark();
        }
        // Record the claim-grace outcome once, at the first ask: did waiting
        // for a claim land us on a claimer, or did we fall back to a blind
        // ask? mGraceEnabled is only set for TxSet fetches.
        if (mGraceEnabled && !mGraceResolved)
        {
            mGraceResolved = true;
            om.mItemFetcherClaimGraceWait.Update(now - mGraceStart);
            if (claimTierSelected)
            {
                om.mItemFetcherClaimGraceSatisfied.Mark();
            }
            else
            {
                om.mItemFetcherClaimGraceExpired.Mark();
            }
        }
        mDontHaveSince.erase(mLastAskedPeer);
        mPeersAsked[mLastAskedPeer] = selectedPeersHave;
        CLOG_TRACE(Overlay, "Asking for {} to {}", hexAbbrev(mItemHash),
                   mLastAskedPeer->toString());
        mAskPeer(mLastAskedPeer, mItemHash);
        nextTry = MS_TO_WAIT_FOR_FETCH_REPLY;
    }

    mTimer.expires_from_now(nextTry);
    // The reply timer firing means no answer arrived in time; attribute the
    // next abandonment to a timeout. A DONT_HAVE arriving first cancels this
    // wait via expires_from_now on the re-ask.
    mTimer.async_wait([this]() { this->tryNextPeer(NextPeerCause::Timeout); },
                      VirtualTimer::onFailureNoop);
}

static std::function<bool(std::pair<Hash, SCPEnvelope> const&)>
matchEnvelope(SCPEnvelope const& env)
{
    return [&env](std::pair<Hash, SCPEnvelope> const& x) {
        return x.second == env;
    };
}

void
Tracker::listen(SCPEnvelope const& env)
{
    ZoneScoped;
    mLastSeenSlotIndex = std::max(env.statement.slotIndex, mLastSeenSlotIndex);

    // don't track the same envelope twice
    auto matcher = matchEnvelope(env);
    auto it = std::find_if(mWaitingEnvelopes.begin(), mWaitingEnvelopes.end(),
                           matcher);
    if (it != mWaitingEnvelopes.end())
    {
        return;
    }

    StellarMessage m;
    m.type(SCP_MESSAGE);
    m.envelope() = env;

    // NB: hash here is BLAKE2 of StellarMessage because that is
    // what the floodmap is keyed by, and we're storing its keys
    // in mWaitingEnvelopes, not the mItemHash that is the SHA256
    // of the item being tracked.
    mWaitingEnvelopes.push_back(std::make_pair(xdrBlake2(m), env));
}

void
Tracker::discard(SCPEnvelope const& env)
{
    ZoneScoped;
    auto matcher = matchEnvelope(env);
    mWaitingEnvelopes.erase(std::remove_if(std::begin(mWaitingEnvelopes),
                                           std::end(mWaitingEnvelopes),
                                           matcher),
                            std::end(mWaitingEnvelopes));
}

void
Tracker::cancel()
{
    mTimer.cancel();
    mLastSeenSlotIndex = 0;
    mClaimingPeers.clear();
    mDontHaveSince.clear();
}

std::chrono::milliseconds
Tracker::getDuration()
{
    return mFetchTime.checkElapsedTime();
}
}
