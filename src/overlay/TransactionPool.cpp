#include "overlay/TransactionPool.h"

#include "main/AppConnector.h"
#include "main/Application.h"
#include "overlay/OverlayManager.h"
#include "transactions/MutableTransactionResult.h"

// TODO: Instrument and test

namespace stellar
{

// TODO: Document thread must be main to call constructor
TransactionPool::TransactionPool(Application const& app)
    : mApp(app.getAppConnector())
    , mTransactions(
          std::make_unique<std::unordered_map<Hash, TransactionFrameBasePtr>>())
    , mSeenTransactions(100'000) // TODO: Magic number
{
    updateSnapshots(app);
}

void
TransactionPool::updateSnapshots(Application const& app)
{
    // TODO: Document thread must be main
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> guard(mMutex);
    mValidationSnapshot =
        std::make_unique<ImmutableValidationSnapshot>(app.getAppConnector());
    mLedgerSnapshot = std::make_unique<LedgerSnapshot>(app);
}

void
TransactionPool::addTransaction(TransactionFrameBasePtr tx,
                                uint64_t upperBoundCloseTimeOffset)
{
    // TODO: Remove this assert? If not, document thread must be background.
    releaseAssert(!threadIsMain());

    std::lock_guard<std::recursive_mutex> guard(mMutex);

    // NOTE: Using `maybeGet` here instead of `exists` because we want to update
    // the last access time of the tx if it's already in the cache.
    if (mSeenTransactions.maybeGet(tx->getFullHash()))
    {
        // Already seen. Ignore
        return;
    }

    // TODO: Structure checks?
    // TODO: Add tx
    if (tx->checkValid(*mValidationSnapshot, *mLedgerSnapshot, 0, 0,
                       upperBoundCloseTimeOffset)
            ->isSuccess())
    {
        releaseAssert(mTransactions);
        mTransactions->try_emplace(tx->getFullHash(), tx);
        mSeenTransactions.put(tx->getFullHash(), std::monostate{});
        // TODO: Rebroadcast. Note that this will send a demand. Will need to
        // add a function to get the tx from the hash.

        // TODO: Flow control around broadcasting.
        // TODO: Enqueue for broadcast instead of straight up broadcasting.
        // TODO: Add metrics
        mApp.postOnMainThread(
            [this, tx]() {
                // TODO: Is this safe? I think tx is immutable, so it should be
                // OK if there are other threads simultaneously interacting with
                // `tx`.
                mApp.getOverlayManager().broadcastMessage(
                    tx->toStellarMessage(), tx->getFullHash());
            },
            "broadcast transaction");
    }
}

std::unique_ptr<TransactionPool::TxMap>
TransactionPool::clearPool()
{
    std::lock_guard<std::recursive_mutex> guard(mMutex);
    std::unique_ptr<TransactionPool::TxMap> ret(std::move(mTransactions));
    mTransactions = std::make_unique<TxMap>();
    return ret;
}

TransactionFrameBasePtr
TransactionPool::getTx(Hash const& hash) const
{
    std::lock_guard<std::recursive_mutex> guard(mMutex);
    auto it = mTransactions->find(hash);
    if (it == mTransactions->end())
    {
        return nullptr;
    }
    return it->second;
}

} // namespace stellar