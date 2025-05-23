#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantManager.h"
#include <map>
#include <vector>

namespace medida
{
class MetricsRegistry;
class Counter;
}

namespace stellar
{

class InvariantManagerImpl : public InvariantManager
{
    std::map<std::string, std::shared_ptr<Invariant>> mInvariants;
    std::vector<std::shared_ptr<Invariant>> mEnabled;
    medida::Counter& mInvariantFailureCount;

    struct InvariantFailureInformation
    {
        uint32_t lastFailedOnLedger;
        std::string lastFailedWithMessage;
    };
    std::map<std::string, InvariantFailureInformation> mFailureInformation;

  public:
    InvariantManagerImpl(medida::MetricsRegistry& registry);

    virtual Json::Value getJsonInfo() override;

    virtual std::vector<std::string> getEnabledInvariants() const override;
    bool isBucketApplyInvariantEnabled() const override;

    virtual void
    checkOnOperationApply(Operation const& operation,
                          OperationResult const& opres,
                          LedgerTxnDelta const& ltxDelta,
                          std::vector<ContractEvent> const& events) override;

    virtual void checkOnBucketApply(
        std::shared_ptr<LiveBucket const> bucket, uint32_t ledger,
        uint32_t level, bool isCurr,
        std::unordered_set<LedgerKey> const& shadowedKeys) override;

    virtual void checkAfterAssumeState(uint32_t newestLedger) override;

    virtual void
    registerInvariant(std::shared_ptr<Invariant> invariant) override;

    virtual void enableInvariant(std::string const& name) override;

#ifdef BUILD_TESTS
    void snapshotForFuzzer() override;
    void resetForFuzzer() override;
#endif // BUILD_TESTS

  private:
    void onInvariantFailure(std::shared_ptr<Invariant> invariant,
                            std::string const& message, uint32_t ledger);

    virtual void handleInvariantFailure(std::shared_ptr<Invariant> invariant,
                                        std::string const& message) const;
};
}
