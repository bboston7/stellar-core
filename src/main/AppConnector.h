#pragma once

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
    SorobanNetworkConfig const& getSorobanNetworkConfigReadOnly() const;
    // TODO: Docs. Mention that the difference between this and `getSorobanNetowrkConfig` is that:
    // 1. This makes a copy, which is safe to use in other threads (TODO: Really
    // double check this. I don't see any references or pointers in
    // SorobanNetworkConfig, but there is a `mutable` field, which needs to be
    // investigated).
    // 2. This returns nullopt when the network config is not set, while
    // `getSorobanNetworkConfig` will throw an assertion error in that case.
    // TODO: Should this be a universal reference (&&)? I pass it to a std::move
    // "call" in the ImmutableValidationSnapshot constructor.
    // TODO: Is this even necessary anymore after getting rid of the tx pool?
    std::optional<SorobanNetworkConfig> maybeGetSorobanNetworkConfigReadOnly() const;
    medida::MetricsRegistry& getMetrics() const;
    SorobanMetrics& getSorobanMetrics() const;
    void checkOnOperationApply(Operation const& operation,
                               OperationResult const& opres,
                               LedgerTxnDelta const& ltxDelta);
    Hash const& getNetworkID() const;

    // Thread-safe methods
    void postOnMainThread(
        std::function<void()>&& f, std::string&& message,
        Scheduler::ActionType type = Scheduler::ActionType::NORMAL_ACTION);
    void postOnOverlayThread(std::function<void()>&& f,
                             std::string const& message);
    VirtualClock::time_point now() const;
    // TODO: Is this thread safe? Is `now` really thread safe? Seems like they
    // have the same issue potentially of `mVirtualNow` changing during access.
    VirtualClock::system_time_point system_now() const;
    Config const& getConfig() const;
    // TODO: Am I using this anywhere after getting rid of tx pool?
    std::shared_ptr<Config const> getConfigPtr() const;
    bool overlayShuttingDown() const;
    OverlayMetrics& getOverlayMetrics();
    // This method is always exclusively called from one thread
    bool
    checkScheduledAndCache(std::shared_ptr<CapacityTrackedMessage> msgTracker);
};
}