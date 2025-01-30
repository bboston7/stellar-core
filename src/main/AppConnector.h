#pragma once

#include "bucket/BucketSnapshotManager.h"
#include "main/Config.h"
#include "medida/metrics_registry.h"

namespace stellar
{
class Application;
class OverlayManager;
class LedgerManager;
class Herder;
class BanManager;
struct OverlayMetrics;
class SorobanNetworkConfig;
class SorobanMetrics;
class SearchableHotArchiveBucketListSnapshot;
struct LedgerTxnDelta;
class CapacityTrackedMessage;

// Helper class to isolate access to Application; all function helpers must
// either be called from main or be thread-sade
class AppConnector
{
    Application& mApp;
    // Copy config for threads to use, and avoid warnings from thread sanitizer
    // about accessing mApp
    std::shared_ptr<const Config> const mConfig;

  public:
    AppConnector(Application& app);

    // Methods that can only be called from main thread
    Herder& getHerder() const;
    LedgerManager& getLedgerManager() const;
    OverlayManager& getOverlayManager() const;
    BanManager& getBanManager() const;
    bool shouldYield() const;
    SorobanMetrics& getSorobanMetrics() const;
    void checkOnOperationApply(Operation const& operation,
                               OperationResult const& opres,
                               LedgerTxnDelta const& ltxDelta);
    Hash const& getNetworkID() const; // TODO: Is this *really* not thread safe?

    // Thread-safe methods
    void postOnMainThread(
        std::function<void()>&& f, std::string&& message,
        Scheduler::ActionType type = Scheduler::ActionType::NORMAL_ACTION);
    void postOnOverlayThread(std::function<void()>&& f,
                             std::string const& message);
    void postOnTxQueueThread(std::function<void()>&& f,
                             std::string const& message);
    VirtualClock::time_point now() const;
    VirtualClock::system_time_point system_now() const;
    Config const& getConfig() const;
    std::shared_ptr<Config const> getConfigPtr() const;
    bool overlayShuttingDown() const;
    OverlayMetrics& getOverlayMetrics();
    bool ledgerIsSynced() const;
    // This method is always exclusively called from one thread
    bool
    checkScheduledAndCache(std::shared_ptr<CapacityTrackedMessage> msgTracker);
    SorobanNetworkConfig const& getSorobanNetworkConfigReadOnly() const;
    SorobanNetworkConfig const& getSorobanNetworkConfigForApply() const;
    // TODO: Docs. Mention that the difference between this and
    // `getSorobanNetowrkConfig` is that:
    // 1. This makes a copy, which is safe to use in other threads (TODO: Really
    // double check this. I don't see any references or pointers in
    // SorobanNetworkConfig, but there is a `mutable` field, which needs to be
    // investigated as it throws `const` functions into question).
    // 2. This returns nullopt when the network config is not set, while
    // `getSorobanNetworkConfig` will throw an assertion error in that case.
    std::optional<SorobanNetworkConfig>
    maybeGetSorobanNetworkConfigReadOnly() const;

    medida::MetricsRegistry& getMetrics() const;
    SearchableHotArchiveSnapshotConstPtr
    copySearchableHotArchiveBucketListSnapshot();
};
}