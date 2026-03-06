// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "TxSetFrame.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "xdr/Stellar-internal.h"
#include <optional>
#include <string>

namespace stellar
{

/**
 * Helper class that describes a single ledger-to-close -- a set of transactions
 * and auxiliary values -- as decided by the Herder (and ultimately: SCP). This
 * does not include the effects of _performing_ any transactions, merely the
 * values that the network has agreed _to apply_ to the current ledger,
 * atomically, in order to produce the next ledger.
 */
class LedgerCloseData
{
  public:
    LedgerCloseData(
        uint32_t ledgerSeq, TxSetResult txSet, StellarValue const& v,
        std::optional<Hash> const& expectedLedgerHash = std::nullopt);

#ifdef BUILD_TESTS
    LedgerCloseData(uint32_t ledgerSeq, TxSetResult txSet,
                    StellarValue const& v,
                    std::optional<Hash> const& expectedLedgerHash,
                    std::optional<TransactionResultSet> const& expectedResults);
#endif // BUILD_TESTS

    uint32_t
    getLedgerSeq() const
    {
        return mLedgerSeq;
    }
    TxSetResult
    getTxSet() const
    {
        return mTxSet;
    }
    StellarValue const&
    getValue() const
    {
        return mValue;
    }
    std::optional<Hash> const&
    getExpectedHash() const
    {
        return mExpectedLedgerHash;
    }
#ifdef BUILD_TESTS
    std::optional<TransactionResultSet> const&
    getExpectedResults() const
    {
        return mExpectedResults;
    }
#endif // BUILD_TESTS

    using GetPrevHeaderFn = std::function<
        std::optional<LedgerHeaderHistoryEntry>(SkipLedgerTxSet const&)>;

    // I think we want this to take a function from SkipLedgerTxSet to
    // optional<prevHeader>. If it can't get the prev header, then this should
    // return nullopt, which will tell writeDebugTxSet to do nothing.
    // TODO: Docs explaining the above ^^
    std::optional<StoredDebugTransactionSet>
    toXDR(GetPrevHeaderFn const& getPrevHeader) const
    {
        TxSetXDRFrameConstPtr txSet;
        if (std::holds_alternative<TxSetXDRFrameConstPtr>(mTxSet))
        {
            txSet = std::get<TxSetXDRFrameConstPtr>(mTxSet);
        }
        else
        {
            // Try to convert the SkipLedgerTxSet to a TxSetXDRFrameConstPtr. If
            // we can't get the previous header, then we can't do the
            // conversion, and we should return nullopt to indicate that
            // TODO: Maybe this conversion logic should be pulled into
            // `tTxSetXdrFrameConstPtr` instead? We might want something similar
            // in apply.
            auto const skipLedgerTxSet = std::get<SkipLedgerTxSet>(mTxSet);
            std::optional<LedgerHeaderHistoryEntry> const maybePrevHeader =
                getPrevHeader(skipLedgerTxSet);
            if (!maybePrevHeader.has_value())
            {
                return std::nullopt;
            }
            txSet = skipLedgerTxSet.toTxSetXdrFrameConstPtr(*maybePrevHeader);
        }
        releaseAssert(txSet);

        StoredDebugTransactionSet sts;
        txSet->storeXDR(sts.txSet);
        sts.scpValue = mValue;
        sts.ledgerSeq = mLedgerSeq;
        return sts;
    }

    static LedgerCloseData
    toLedgerCloseData(StoredDebugTransactionSet const& sts)
    {
        if (sts.txSet.v() == 0)
        {
            return LedgerCloseData(
                sts.ledgerSeq, TxSetXDRFrame::makeFromWire(sts.txSet.txSet()),
                sts.scpValue);
        }
        else
        {
            return LedgerCloseData(
                sts.ledgerSeq,
                TxSetXDRFrame::makeFromWire(sts.txSet.generalizedTxSet()),
                sts.scpValue);
        }
    }

  private:
    uint32_t mLedgerSeq;
    TxSetResult mTxSet;
    StellarValue mValue;
    std::optional<Hash> mExpectedLedgerHash = std::nullopt;
#ifdef BUILD_TESTS
    std::optional<TransactionResultSet> mExpectedResults = std::nullopt;
#endif // BUILD_TESTS
};

std::string stellarValueToString(Config const& c, StellarValue const& sv);

#define emptyUpgradeSteps (xdr::xvector<UpgradeType, 6>(0))
}
