#pragma once

// TODO: Remove
// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <mutex>
#include <variant>

#include "transactions/TransactionFrameBase.h"
#include "util/RandomEvictionCache.h"

namespace stellar
{

// TODO: Docs throughout
class TransactionPool
{
  public:
    // TODO: Rename?
    using TxMap = std::unordered_map<Hash, TransactionFrameBasePtr>;

    explicit TransactionPool(Application const& app);

    void updateSnapshots(Application const& app);

    // TODO: It would be nice to get rid of `upperBoundCloseTimeOffset`...
    void addTransaction(TransactionFrameBasePtr tx,
                        uint64_t upperBoundCloseTimeOffset);

    std::unique_ptr<TxMap> clearPool();

    TransactionFrameBasePtr getTx(Hash const& hash) const;

  private:
    std::recursive_mutex mutable mMutex;

    AppConnector& mApp;

    std::unique_ptr<ImmutableValidationSnapshot const> mValidationSnapshot;
    std::unique_ptr<LedgerSnapshot const> mLedgerSnapshot;

    std::unique_ptr<TxMap> mTransactions;

    // TODO: Should this be something more robust?
    RandomEvictionCache<Hash, std::monostate> mSeenTransactions;
};

} // namespace stellar