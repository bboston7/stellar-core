#pragma once

#include "bucket/BucketUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/metrics_registry.h"

namespace stellar
{
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
class ZstdCompressor;
class ZstdDecompressor;

// Helper class to isolate access to Application; all function helpers must
// either be called from main or be thread-sade
class AppConnector
{
    Application& mApp;
    // Copy config for threads to use, and avoid warnings from thread sanitizer
    // about accessing mApp
    Config const mConfig;

    // TODO: Move this somewhere else
    SearchableSnapshotConstPtr mOverlayThreadSnapshot;

  public:
    AppConnector(Application& app);

    // Methods that can only be called from main thread
    Herder& getHerder();
    LedgerManager& getLedgerManager();
    OverlayManager& getOverlayManager();
    BanManager& getBanManager();
    bool shouldYield() const;
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
    Config const& getConfig() const;
    bool overlayShuttingDown() const;
    OverlayMetrics& getOverlayMetrics();
    // This method is always exclusively called from one thread
    bool
    checkScheduledAndCache(std::shared_ptr<CapacityTrackedMessage> msgTracker);
    SorobanNetworkConfig const& getLastClosedSorobanNetworkConfig() const;
    SorobanNetworkConfig const& getSorobanNetworkConfigForApply() const;
    bool threadIsType(Application::ThreadType type) const;

    medida::MetricsRegistry& getMetrics() const;
    SearchableHotArchiveSnapshotConstPtr
    copySearchableHotArchiveBucketListSnapshot();

    SearchableSnapshotConstPtr getOverlayThreadSnapshot();

    // Not thread-safe, must acquire lock first
    ZstdCompressor const& getZstdCompressor() const;
    ZstdDecompressor const& getZstdDecompressor() const;
    std::mutex& getZstdCompressorMutex() const;
    std::mutex& getZstdDecompressorMutex() const;
};
}