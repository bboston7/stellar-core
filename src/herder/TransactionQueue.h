#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/TxQueueLimiter.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerTxn.h"
#include "transactions/TransactionFrame.h"
#include "util/HashOfHash.h"
#include "util/Timer.h"
#include "util/XDROperators.h"
#include "xdr/Stellar-transaction.h"

#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include <chrono>
#include <deque>
#include <memory>
#include <vector>

namespace medida
{
class Counter;
class Timer;
}

namespace stellar
{

class Application;

/**
 * TransactionQueue keeps received transactions that are valid and have not yet
 * been included in a transaction set.
 *
 * An accountID is in mAccountStates if and only if it is the fee-source or
 * sequence-number-source for at least one transaction in the TransactionQueue.
 * This invariant is maintained by releaseFeeMaybeEraseAccountState.
 *
 * Transactions received from the HTTP "tx" endpoint and the overlay network
 * should be added by calling tryAdd. If that succeeds, the transaction may be
 * removed later in three ways:
 * - removeApplied() should be called after transactions are applied. It removes
 *   the specified transactions, but leaves transactions with subsequent
 *   sequence numbers in the TransactionQueue. It also resets the age for the
 *   sequence-number-source of each specified transaction.
 * - ban() should be called after transactions become invalid for any reason.
 *   Banned transactions cannot be added to the TransactionQueue again for a
 *   banDepth ledgers.
 * - shift() should be called after each ledger close, after removeApplied. It
 *   increases the age for every account that is the sequence-number-source for
 *   at least one transaction. If the age becomes greater than or equal to
 *   pendingDepth, all transactions for that source account are banned. It also
 *   unbans any transactions that have been banned for more than banDepth
 *   ledgers.
 */
class TransactionQueue
{
  public:
    static uint64_t const FEE_MULTIPLIER;

    enum class AddResultCode
    {
        ADD_STATUS_PENDING = 0,
        ADD_STATUS_DUPLICATE,
        ADD_STATUS_ERROR,
        ADD_STATUS_TRY_AGAIN_LATER,
        ADD_STATUS_FILTERED,
        ADD_STATUS_COUNT,
        ADD_STATUS_UNKNOWN // TODO: rename?
    };

    struct AddResult
    {
        TransactionQueue::AddResultCode code;
        MutableTxResultPtr txResult;

        // AddResult with no txResult
        explicit AddResult(TransactionQueue::AddResultCode addCode);

        // AddResult from existing transaction result
        explicit AddResult(TransactionQueue::AddResultCode addCode,
                           MutableTxResultPtr payload);

        // AddResult with error txResult with the specified txErrorCode
        explicit AddResult(TransactionQueue::AddResultCode addCode,
                           TransactionFrameBasePtr tx,
                           TransactionResultCode txErrorCode);
    };

    /**
     * AccountState stores the following information:
     * - mTotalFees: the sum of feeBid() over every transaction for which this
     *   account is the fee-source (this may include transactions that are not
     *   in mTransactions)
     * - mAge: the number of ledgers that have closed since the last ledger in
     *   which a transaction in mTransactions was included. This is always 0 if
     *   mTransactions is empty
     * - mTransactions: the list of transactions for which this account is the
     *   sequence-number-source, ordered by sequence number
     */

    struct TimestampedTx
    {
        TransactionFrameBasePtr mTx;
        bool mBroadcasted;
        VirtualClock::time_point mInsertionTime;
        bool mSubmittedFromSelf;
    };
    using Transactions = std::vector<TransactionFrameBasePtr>;
    struct AccountState
    {
        int64_t mTotalFees{0};
        uint32_t mAge{0};
        std::optional<TimestampedTx> mTransaction;
    };

    // TODO: Get rid of `Application` in constructor? Might be able to get away
    // with an AppConnector here.
    explicit TransactionQueue(Application& app,
                              SearchableSnapshotConstPtr bucketSnapshot,
                              uint32 pendingDepth, uint32 banDepth,
                              uint32 poolLedgerMultiplier, bool isSoroban);
    virtual ~TransactionQueue();

    // TODO: Need to be careful about interleaving herder. At the very least,
    // should condense Herder's use of functions into "before application" and
    // "after application" atomic functions. I also need to think about whether
    // it's safe for txs to be added during application. My first guess is that
    // it's fine, but we really need to be careful here. For example, perhaps a
    // tx that comes in after pre-apply steps but before post-apply steps would
    // be on the wrong side of a "shift" call?

    static std::vector<AssetPair>
    findAllAssetPairsInvolvedInPaymentLoops(TransactionFrameBasePtr tx);

    AddResult tryAdd(TransactionFrameBasePtr tx, bool submittedFromSelf);
    // Ban transactions that are no longer valid or have insufficient fee;
    // transaction per account limit applies here, so `txs` should have no
    // duplicate source accounts
    void ban(Transactions const& txs);

    void shutdown();

    // TODO: Docs
    // TODO: Rename both function and arg names.
    // TODO: Might be able to remove `newValidationSnapshot` and use the
    // internal `AppConnector` to construct it instead. But IDK if that's doable
    // in the background (though I think this is only called from the
    // foreground?)
    void update(
        Transactions const& applied, LedgerHeader const& lcl,
        SearchableSnapshotConstPtr newBucketSnapshot,
        std::function<TxFrameList(TxFrameList const&)> const& filterInvalidTxs);

    bool isBanned(Hash const& hash) const;
    TransactionFrameBaseConstPtr getTx(Hash const& hash) const;
    TxFrameList getTransactions(LedgerHeader const& lcl) const;
    bool sourceAccountPending(AccountID const& accountID) const;

    virtual size_t getMaxQueueSizeOps() const = 0;

    // TODO: Add an `atomically` function that grabs lock and calls the passed
    // in function. This may be useful for chaining operations together in
    // HerderImpl.

#ifdef BUILD_TESTS
    AccountState
    getAccountTransactionQueueInfo(AccountID const& accountID) const;
    size_t countBanned(int index) const;
#endif

  protected:
    /**
     * The AccountState for every account. As noted above, an AccountID is in
     * AccountStates iff at least one of the following is true for the
     * corresponding AccountState
     * - AccountState.mTotalFees > 0
     * - !AccountState.mTransactions.empty()
     */
    using AccountStates = UnorderedMap<AccountID, AccountState>;

    /**
     * Banned transactions are stored in deque of depth banDepth, so it is easy
     * to unban all transactions that were banned for long enough.
     */
    using BannedTransactions = std::deque<UnorderedSet<Hash>>;

    uint32 const mPendingDepth;

    AccountStates mAccountStates;
    BannedTransactions mBannedTransactions;

    // counters
    struct QueueMetrics
    {
        QueueMetrics(std::vector<medida::Counter*> sizeByAge,
                     medida::Counter& bannedTransactionsCounter,
                     medida::Timer& transactionsDelay,
                     medida::Timer& transactionsSelfDelay)
            : mSizeByAge(std::move(sizeByAge))
            , mBannedTransactionsCounter(bannedTransactionsCounter)
            , mTransactionsDelay(transactionsDelay)
            , mTransactionsSelfDelay(transactionsSelfDelay)
        {
        }
        std::vector<medida::Counter*> mSizeByAge;
        medida::Counter& mBannedTransactionsCounter;
        medida::Timer& mTransactionsDelay;
        medida::Timer& mTransactionsSelfDelay;
    };

    std::unique_ptr<QueueMetrics> mQueueMetrics;

    UnorderedSet<OperationType> mFilteredTypes;

    bool mShutdown{false};
    bool mWaiting{false};
    // TODO: Is this thing safe to use in a multi-threaded context? vv It takes
    // an application in its constructor and appears to store a VirtualClock
    // reference. Is that OK?
    VirtualTimer mBroadcastTimer;

    virtual std::pair<Resource, std::optional<Resource>>
    getMaxResourcesToFloodThisPeriod() const = 0;
    virtual bool broadcastSome() = 0;
    virtual int getFloodPeriod() const = 0;
    virtual bool allowTxBroadcast(TimestampedTx const& tx) = 0;

    void broadcast(bool fromCallback);
    // broadcasts a single transaction
    enum class BroadcastStatus
    {
        BROADCAST_STATUS_ALREADY,
        BROADCAST_STATUS_SUCCESS,
        BROADCAST_STATUS_SKIPPED
    };
    BroadcastStatus broadcastTx(TimestampedTx& tx);

    TransactionQueue::AddResult
    canAdd(TransactionFrameBasePtr tx, AccountStates::iterator& stateIter,
           std::vector<std::pair<TransactionFrameBasePtr, bool>>& txsToEvict);

    void releaseFeeMaybeEraseAccountState(TransactionFrameBasePtr tx);

    void prepareDropTransaction(AccountState& as);
    void dropTransaction(AccountStates::iterator stateIter);

    bool isFiltered(TransactionFrameBasePtr tx) const;

    ValidationSnapshotPtr mValidationSnapshot;

    SearchableSnapshotConstPtr mBucketSnapshot;

    TxQueueLimiter mTxQueueLimiter;
    UnorderedMap<AssetPair, uint32_t, AssetPairHash> mArbitrageFloodDamping;

    UnorderedMap<Hash, TransactionFrameBasePtr> mKnownTxHashes;

    size_t mBroadcastSeed;

    // TODO: Lock all public functions
    mutable std::recursive_mutex mTxQueueMutex;

  private:
    AppConnector& mAppConn;

    void removeApplied(Transactions const& txs);

    /**
     * Increase age of each AccountState that has at least one transaction in
     * mTransactions. Also increments the age for each banned transaction, and
     * unbans transactions for which age equals banDepth.
     */
    void shift();

    void rebroadcast();

#ifdef BUILD_TESTS
  public:
    friend class TransactionQueueTest;

    size_t getQueueSizeOps() const;
    std::optional<int64_t> getInQueueSeqNum(AccountID const& account) const;
    std::function<void(TransactionFrameBasePtr&)> mTxBroadcastedEvent;
#endif
};

class SorobanTransactionQueue : public TransactionQueue
{
  public:
    SorobanTransactionQueue(Application& app,
                            SearchableSnapshotConstPtr bucketSnapshot,
                            uint32 pendingDepth, uint32 banDepth,
                            uint32 poolLedgerMultiplier);
    int
    getFloodPeriod() const override
    {
        std::lock_guard<std::recursive_mutex> guard(mTxQueueMutex);
        return mValidationSnapshot->getConfig().FLOOD_SOROBAN_TX_PERIOD_MS;
    }

    size_t getMaxQueueSizeOps() const override;
#ifdef BUILD_TESTS
    void
    clearBroadcastCarryover()
    {
        std::lock_guard<std::recursive_mutex> guard(mTxQueueMutex);
        mBroadcastOpCarryover.clear();
        mBroadcastOpCarryover.resize(1, Resource::makeEmptySoroban());
    }
#endif

  private:
    virtual std::pair<Resource, std::optional<Resource>>
    getMaxResourcesToFloodThisPeriod() const override;
    virtual bool broadcastSome() override;
    std::vector<Resource> mBroadcastOpCarryover;
    // No special flooding rules for Soroban
    virtual bool
    allowTxBroadcast(TimestampedTx const& tx) override
    {
        return true;
    }
};

class ClassicTransactionQueue : public TransactionQueue
{
  public:
    ClassicTransactionQueue(Application& app,
                            SearchableSnapshotConstPtr bucketSnapshot,
                            uint32 pendingDepth, uint32 banDepth,
                            uint32 poolLedgerMultiplier);

    int
    getFloodPeriod() const override
    {
        std::lock_guard<std::recursive_mutex> guard(mTxQueueMutex);
        return mValidationSnapshot->getConfig().FLOOD_TX_PERIOD_MS;
    }

    size_t getMaxQueueSizeOps() const override;

  private:
    medida::Counter& mArbTxSeenCounter;
    medida::Counter& mArbTxDroppedCounter;

    virtual std::pair<Resource, std::optional<Resource>>
    getMaxResourcesToFloodThisPeriod() const override;
    virtual bool broadcastSome() override;
    std::vector<Resource> mBroadcastOpCarryover;
    virtual bool allowTxBroadcast(TimestampedTx const& tx) override;
};

extern std::array<const char*,
                  static_cast<int>(
                      TransactionQueue::AddResultCode::ADD_STATUS_COUNT)>
    TX_STATUS_STRING;
}
