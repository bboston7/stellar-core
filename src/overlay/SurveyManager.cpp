// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SurveyManager.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "overlay/OverlayManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

namespace stellar
{

uint32_t const SurveyManager::SURVEY_THROTTLE_TIMEOUT_MULT(3);

SurveyManager::SurveyManager(Application& app)
    : mApp(app)
    , mSurveyThrottleTimer(std::make_unique<VirtualTimer>(mApp))
    , NUM_LEDGERS_BEFORE_IGNORE(12) // ~60 seconds
    , MAX_REQUEST_LIMIT_PER_LEDGER(10)
    , mMessageLimiter(app, NUM_LEDGERS_BEFORE_IGNORE,
                      MAX_REQUEST_LIMIT_PER_LEDGER)
    , SURVEY_THROTTLE_TIMEOUT_SEC(
          mApp.getConfig().getExpectedLedgerCloseTime() *
          SURVEY_THROTTLE_TIMEOUT_MULT)
{
}

bool
SurveyManager::startSurvey(SurveyMessageCommandType type,
                           std::chrono::seconds surveyDuration)
{
    if (mRunningSurveyType)
    {
        return false;
    }

    // results are only cleared when we start the NEXT survey so we can query
    // the results  after the survey closes
    mResults.clear();
    mBadResponseNodes.clear();

    // queued peers are only cleared when we start the NEXT survey so we know
    // which peers were in our backlog before we stopped
    mPeersToSurvey.clear();
    mPeersToSurveyQueue = std::queue<NodeID>();

    mRunningSurveyType = std::make_optional<SurveyMessageCommandType>(type);

    mCurve25519SecretKey = curve25519RandomSecret();
    mCurve25519PublicKey = curve25519DerivePublic(mCurve25519SecretKey);

    updateSurveyExpiration(surveyDuration);
    // starts timer
    topOffRequests(type);

    return true;
}

void
SurveyManager::stopSurvey()
{
    // do nothing if survey isn't running
    if (!mRunningSurveyType)
    {
        return;
    }

    mRunningSurveyType.reset();
    mSurveyThrottleTimer->cancel();

    clearCurve25519Keys(mCurve25519PublicKey, mCurve25519SecretKey);

    CLOG_INFO(Overlay, "SurveyResults {}", getJsonResults().toStyledString());
}

void
SurveyManager::addNodeToRunningSurveyBacklog(
    SurveyMessageCommandType type, std::chrono::seconds surveyDuration,
    NodeID const& nodeToSurvey)
{
    if (!mRunningSurveyType || *mRunningSurveyType != type)
    {
        throw std::runtime_error("addNodeToRunningSurveyBacklog failed");
    }

    addPeerToBacklog(nodeToSurvey);
    updateSurveyExpiration(surveyDuration);
}

void
SurveyManager::relayOrProcessResponse(StellarMessage const& msg,
                                      Peer::pointer peer)
{
    releaseAssert(msg.type() == SURVEY_RESPONSE);
    auto const& signedResponse = msg.signedSurveyResponseMessage();
    auto const& response = signedResponse.response;

    auto onSuccessValidation = [&]() -> bool {
        return dropPeerIfSigInvalid(response.surveyedPeerID,
                                    signedResponse.responseSignature,
                                    xdr::xdr_to_opaque(response), peer);
    };

    // This essentially just validates that the response corresponds to a seen
    // request and that it isn't too old
    if (!mMessageLimiter.recordAndValidateResponse(response,
                                                   onSuccessValidation))
    {
        return;
    }

    // mMessageLimiter filters out duplicates, so here we are guaranteed
    // to record the message for the first time
    mApp.getOverlayManager().recvFloodedMsg(msg, peer);

    if (response.surveyorPeerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        // only process if survey is still running and we haven't seen the
        // response
        if (mRunningSurveyType && *mRunningSurveyType == response.commandType)
        {
            try
            {
                xdr::opaque_vec<> opaqueDecrypted = curve25519Decrypt(
                    mCurve25519SecretKey, mCurve25519PublicKey,
                    response.encryptedBody);

                SurveyResponseBody body;
                xdr::xdr_from_opaque(opaqueDecrypted, body);

                processTopologyResponse(response.surveyedPeerID, body);
            }
            catch (std::exception const& e)
            {
                CLOG_ERROR(Overlay, "processing survey response failed: {}",
                           e.what());

                mBadResponseNodes.emplace(response.surveyedPeerID);
                return;
            }
        }
    }
    else
    {
        // messageLimiter guarantees we only flood the response if we've seen
        // the request
        broadcast(msg);
    }
}

// I don't see anything slowing down requests, other than that maybe they would
// get dropped if multiple surveys are running or the request script spammed
// requests. Maybe there are timeouts in broadcast logic I haven't found that
// lead to dropping requests?
void
SurveyManager::relayOrProcessRequest(StellarMessage const& msg,
                                     Peer::pointer peer)
{
    releaseAssert(msg.type() == SURVEY_REQUEST);
    SignedSurveyRequestMessage const& signedRequest =
        msg.signedSurveyRequestMessage();

    SurveyRequestMessage const& request = signedRequest.request;

    auto surveyorIsSelf =
        request.surveyorPeerID == mApp.getConfig().NODE_SEED.getPublicKey();
    if (!surveyorIsSelf)
    {
        releaseAssert(peer);

        // perform all validation checks before signature validation so we don't
        // waste time verifying signatures
        auto const& surveyorKeys = mApp.getConfig().SURVEYOR_KEYS;

        if (surveyorKeys.empty())
        {
            auto const& quorumMap =
                mApp.getHerder().getCurrentlyTrackedQuorum();
            if (quorumMap.count(request.surveyorPeerID) == 0)
            {
                return;
            }
        }
        else
        {
            if (surveyorKeys.count(request.surveyorPeerID) == 0)
            {
                return;
            }
        }
    }

    auto onSuccessValidation = [&]() -> bool {
        auto res = dropPeerIfSigInvalid(request.surveyorPeerID,
                                        signedRequest.requestSignature,
                                        xdr::xdr_to_opaque(request), peer);
        if (!res && surveyorIsSelf)
        {
            CLOG_ERROR(Overlay, "Unexpected invalid survey request: {} ",
                       REPORT_INTERNAL_BUG);
        }
        return res;
    };

    // This returns true iff the request is valid, we haven't seen it before,
    // and the various survey limits haven't been hit
    if (!mMessageLimiter.addAndValidateRequest(request, onSuccessValidation))
    {
        return;
    }

    if (peer)
    {
        // Mark the messages as seen from peer so it doesn't flood back to them
        mApp.getOverlayManager().recvFloodedMsg(msg, peer);
    }

    if (request.surveyedPeerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        // The survey request is for us!
        processTopologyRequest(request);
    }
    else
    {
        // The request is not for us. Broadcast onwards.
        std::cout << "Broadcasting request!" << std::endl;
        broadcast(msg);
    }
}

StellarMessage
SurveyManager::makeSurveyRequest(NodeID const& nodeToSurvey) const
{
    StellarMessage newMsg;
    newMsg.type(SURVEY_REQUEST);

    auto& signedRequest = newMsg.signedSurveyRequestMessage();

    auto& request = signedRequest.request;
    request.ledgerNum = mApp.getHerder().trackingConsensusLedgerIndex();
    request.surveyorPeerID = mApp.getConfig().NODE_SEED.getPublicKey();

    request.surveyedPeerID = nodeToSurvey;
    request.encryptionKey = mCurve25519PublicKey;
    request.commandType = SURVEY_TOPOLOGY;

    auto sigBody = xdr::xdr_to_opaque(request);
    signedRequest.requestSignature = mApp.getConfig().NODE_SEED.sign(sigBody);

    return newMsg;
}

void
SurveyManager::sendTopologyRequest(NodeID const& nodeToSurvey)
{
    // Record the request in message limiter and broadcast
    relayOrProcessRequest(makeSurveyRequest(nodeToSurvey), nullptr);
}

void
SurveyManager::processTopologyResponse(NodeID const& surveyedPeerID,
                                       SurveyResponseBody const& body)
{
    auto& peerResults =
        mResults["topology"][KeyUtils::toStrKey(surveyedPeerID)];
    auto populatePeerResults = [&](auto const& topologyBody) {
        auto& inboundResults = peerResults["inboundPeers"];
        auto& outboundResults = peerResults["outboundPeers"];

        peerResults["numTotalInboundPeers"] =
            topologyBody.totalInboundPeerCount;
        peerResults["numTotalOutboundPeers"] =
            topologyBody.totalOutboundPeerCount;

        recordResults(inboundResults, topologyBody.inboundPeers);
        recordResults(outboundResults, topologyBody.outboundPeers);
    };

    bool extendedSurveyType = body.type() == SURVEY_TOPOLOGY_RESPONSE_V1;
    if (extendedSurveyType)
    {
        auto const& topologyBody = body.topologyResponseBodyV1();
        populatePeerResults(topologyBody);
        peerResults["maxInboundPeerCount"] = topologyBody.maxInboundPeerCount;
        peerResults["maxOutboundPeerCount"] = topologyBody.maxOutboundPeerCount;
    }
    else
    {
        auto const& topologyBody = body.topologyResponseBodyV0();
        populatePeerResults(topologyBody);
    }
}

void
SurveyManager::processTopologyRequest(SurveyRequestMessage const& request) const
{
    CLOG_TRACE(Overlay, "Responding to Topology request from {}",
               mApp.getConfig().toShortString(request.surveyorPeerID));

    StellarMessage newMsg;
    newMsg.type(SURVEY_RESPONSE);

    auto& signedResponse = newMsg.signedSurveyResponseMessage();
    auto& response = signedResponse.response;

    // NOTE: This uses the ledgerNum *from the request*, so it really has 60
    // seconds from sending a request to receiving a response. Maybe this should
    // hold the node's ledger number instead, effectively doubling the
    // permissable round trip time?
    response.ledgerNum = request.ledgerNum;
    response.surveyorPeerID = request.surveyorPeerID;
    response.surveyedPeerID = mApp.getConfig().NODE_SEED.getPublicKey();
    response.commandType = SURVEY_TOPOLOGY;

    SurveyResponseBody body;
    body.type(SURVEY_TOPOLOGY_RESPONSE_V1);

    auto& topologyBody = body.topologyResponseBodyV1();

    auto const& randomInboundPeers =
        mApp.getOverlayManager().getRandomInboundAuthenticatedPeers();
    auto const& randomOutboundPeers =
        mApp.getOverlayManager().getRandomOutboundAuthenticatedPeers();

    populatePeerStats(randomInboundPeers, topologyBody.inboundPeers,
                      mApp.getClock().now());

    populatePeerStats(randomOutboundPeers, topologyBody.outboundPeers,
                      mApp.getClock().now());

    topologyBody.totalInboundPeerCount =
        static_cast<uint32_t>(randomInboundPeers.size());
    topologyBody.totalOutboundPeerCount =
        static_cast<uint32_t>(randomOutboundPeers.size());
    topologyBody.maxInboundPeerCount =
        mApp.getConfig().MAX_ADDITIONAL_PEER_CONNECTIONS;
    topologyBody.maxOutboundPeerCount =
        mApp.getConfig().TARGET_PEER_CONNECTIONS;

    try
    {
        response.encryptedBody = curve25519Encrypt<EncryptedBody::max_size()>(
            request.encryptionKey, xdr::xdr_to_opaque(body));
    }
    catch (std::exception const& e)
    {
        CLOG_ERROR(Overlay, "curve25519Encrypt failed: {}", e.what());
        return;
    }

    auto sigBody = xdr::xdr_to_opaque(response);
    signedResponse.responseSignature = mApp.getConfig().NODE_SEED.sign(sigBody);

    broadcast(newMsg);
}

void
SurveyManager::broadcast(StellarMessage const& msg) const
{
    mApp.getOverlayManager().broadcastMessage(msg, false);
}

void
SurveyManager::populatePeerStats(std::vector<Peer::pointer> const& peers,
                                 PeerStatList& results,
                                 VirtualClock::time_point now) const
{
    size_t resultSize = std::min<size_t>(peers.size(), results.max_size());
    results.reserve(resultSize);

    for (size_t i = 0; i < resultSize; ++i)
    {
        auto& peer = peers[i];
        auto& peerMetrics = peer->getPeerMetrics();

        PeerStats stats;
        stats.id = peer->getPeerID();
        stats.versionStr = peer->getRemoteVersion();
        stats.messagesRead = peerMetrics.mMessageRead;
        stats.messagesWritten = peerMetrics.mMessageWrite;
        stats.bytesRead = peerMetrics.mByteRead;
        stats.bytesWritten = peerMetrics.mByteWrite;

        stats.uniqueFloodBytesRecv = peerMetrics.mUniqueFloodBytesRecv;
        stats.duplicateFloodBytesRecv = peerMetrics.mDuplicateFloodBytesRecv;
        stats.uniqueFetchBytesRecv = peerMetrics.mUniqueFetchBytesRecv;
        stats.duplicateFetchBytesRecv = peerMetrics.mDuplicateFetchBytesRecv;

        stats.uniqueFloodMessageRecv = peerMetrics.mUniqueFloodMessageRecv;
        stats.duplicateFloodMessageRecv =
            peerMetrics.mDuplicateFloodMessageRecv;
        stats.uniqueFetchMessageRecv = peerMetrics.mUniqueFetchMessageRecv;
        stats.duplicateFetchMessageRecv =
            peerMetrics.mDuplicateFetchMessageRecv;

        stats.secondsConnected =
            std::chrono::duration_cast<std::chrono::seconds>(
                now - peerMetrics.mConnectedTime)
                .count();

        results.emplace_back(stats);
    }
}

void
SurveyManager::recordResults(Json::Value& jsonResultList,
                             PeerStatList const& peerList) const
{
    for (auto const& peer : peerList)
    {
        Json::Value peerInfo;
        peerInfo["nodeId"] = KeyUtils::toStrKey(peer.id);
        peerInfo["version"] = peer.versionStr;
        peerInfo["messagesRead"] = static_cast<Json::UInt64>(peer.messagesRead);
        peerInfo["messagesWritten"] =
            static_cast<Json::UInt64>(peer.messagesWritten);
        peerInfo["bytesRead"] = static_cast<Json::UInt64>(peer.bytesRead);
        peerInfo["bytesWritten"] = static_cast<Json::UInt64>(peer.bytesWritten);
        peerInfo["secondsConnected"] =
            static_cast<Json::UInt64>(peer.secondsConnected);

        peerInfo["uniqueFloodBytesRecv"] =
            static_cast<Json::UInt64>(peer.uniqueFloodBytesRecv);
        peerInfo["duplicateFloodBytesRecv"] =
            static_cast<Json::UInt64>(peer.duplicateFloodBytesRecv);
        peerInfo["uniqueFetchBytesRecv"] =
            static_cast<Json::UInt64>(peer.uniqueFetchBytesRecv);
        peerInfo["duplicateFetchBytesRecv"] =
            static_cast<Json::UInt64>(peer.duplicateFetchBytesRecv);

        peerInfo["uniqueFloodMessageRecv"] =
            static_cast<Json::UInt64>(peer.uniqueFloodMessageRecv);
        peerInfo["duplicateFloodMessageRecv"] =
            static_cast<Json::UInt64>(peer.duplicateFloodMessageRecv);
        peerInfo["uniqueFetchMessageRecv"] =
            static_cast<Json::UInt64>(peer.uniqueFetchMessageRecv);
        peerInfo["duplicateFetchMessageRecv"] =
            static_cast<Json::UInt64>(peer.duplicateFetchMessageRecv);

        jsonResultList.append(peerInfo);
    }
}

void
SurveyManager::clearOldLedgers(uint32_t lastClosedledgerSeq)
{
    mMessageLimiter.clearOldLedgers(lastClosedledgerSeq);
}

Json::Value const&
SurveyManager::getJsonResults()
{
    mResults["surveyInProgress"] = mRunningSurveyType.has_value();

    auto& jsonBacklog = mResults["backlog"];
    jsonBacklog.clear();

    for (auto const& peer : mPeersToSurvey)
    {
        jsonBacklog.append(KeyUtils::toStrKey(peer));
    }

    auto& badResponseNodes = mResults["badResponseNodes"];
    badResponseNodes.clear();

    for (auto const& peer : mBadResponseNodes)
    {
        badResponseNodes.append(KeyUtils::toStrKey(peer));
    }

    return mResults;
}

std::string
SurveyManager::getMsgSummary(StellarMessage const& msg)
{
    std::string summary;
    SurveyMessageCommandType commandType;
    switch (msg.type())
    {
    case SURVEY_REQUEST:
        summary = "SURVEY_REQUEST:";
        commandType = msg.signedSurveyRequestMessage().request.commandType;
        break;
    case SURVEY_RESPONSE:
        summary = "SURVEY_RESPONSE:";
        commandType = msg.signedSurveyResponseMessage().response.commandType;
        break;
    default:
        throw std::runtime_error(
            "invalid call of SurveyManager::getMsgSummary");
    }
    return summary + commandTypeName(commandType);
}

void
SurveyManager::topOffRequests(SurveyMessageCommandType type)
{
    // Only stop the survey if all pending requests have been processed
    // NOTE: I don't think this is thread safe. mSurveyExpirationTime or
    // mPeersToSurvey could be modified by another thread while reading here.
    // Could this be terminating the survey?
    if (mApp.getClock().now() > mSurveyExpirationTime && mPeersToSurvey.empty())
    {
        stopSurvey();
        return;
    }

    // we only send up to MAX_REQUEST_LIMIT_PER_LEDGER requests and wait
    // mSurveyThrottleTimeoutSec between topoffs as to reduce the
    // chance of having more than MAX_REQUEST_LIMIT_PER_LEDGER (which is the
    // rate limit) on any node relaying requests on the network (NB: can still
    // happen if some connections get congested)

    uint32_t requestsSentInSchedule = 0;
    while (mRunningSurveyType &&
           requestsSentInSchedule < MAX_REQUEST_LIMIT_PER_LEDGER &&
           !mPeersToSurvey.empty())
    {
        if (mPeersToSurveyQueue.empty())
        {
            throw std::runtime_error("mPeersToSurveyQueue unexpectedly empty");
        }
        auto key = mPeersToSurveyQueue.front();
        mPeersToSurvey.erase(key);
        mPeersToSurveyQueue.pop();

        sendTopologyRequest(key);

        ++requestsSentInSchedule;
    }

    std::weak_ptr<SurveyManager> weak = shared_from_this();
    auto handler = [weak, type]() {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }

        self->topOffRequests(type);
    };

    // schedule next top off
    // This is interesting, the timer is set to run this function every 15
    // seconds (every 3 ledgers)! That means this sends up to 10 messages every
    // 3 ledgers, not every ledger! But the script requests them every ledger.
    // It looks like `sendTopologyRequest` in this function correctly sets the
    // ledger number to this ledger, so this shouldn't be a problem in and of
    // itself, but there are a few other places this could cause issues:
    // 1. Does mSurveyExpirationTime run out prematurely? What happens then? I
    //    think it's OK because the queue is allowed to empty, but if there is
    //    every a gap where the queue is empty and the timer has expired then a
    //    whole wave of requests will be missed.
    // 2. Does the queue ever get so full that stellar-core stops accepting new
    //    queue entries over the http API? Or starts pruning the queue?
    mSurveyThrottleTimer->expires_from_now(SURVEY_THROTTLE_TIMEOUT_SEC);
    mSurveyThrottleTimer->async_wait(handler, &VirtualTimer::onFailureNoop);
}

void
SurveyManager::updateSurveyExpiration(std::chrono::seconds surveyDuration)
{
    mSurveyExpirationTime = mApp.getClock().now() + surveyDuration;
}

void
SurveyManager::addPeerToBacklog(NodeID const& nodeToSurvey)
{
    // filter conditions-
    // 1. already queued
    // 2. node would survey itself
    // This ensures that mPeersToSurveyQueue doesn't contain any duplicates.
    if (mPeersToSurvey.count(nodeToSurvey) != 0 ||
        nodeToSurvey == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        return;
    }

    mBadResponseNodes.erase(nodeToSurvey);

    // we clear the results because it's possible to send and receive
    // multiple requests and responses for a surveyor-surveyed node pair. We
    // expect the user to save any previous results before sending the
    // duplicate requests, so we can just overwrite the previous result
    mResults["topology"][KeyUtils::toStrKey(nodeToSurvey)].clear();

    mPeersToSurvey.emplace(nodeToSurvey);
    mPeersToSurveyQueue.emplace(nodeToSurvey);
}

bool
SurveyManager::dropPeerIfSigInvalid(PublicKey const& key,
                                    Signature const& signature,
                                    ByteSlice const& bin, Peer::pointer peer)
{
    bool success = PubKeyUtils::verifySig(key, signature, bin);

    if (!success && peer)
    {
        // we drop the connection to keep a bad peer from pegging the CPU with
        // signature verification
        peer->sendErrorAndDrop(ERR_MISC, "Survey has invalid signature",
                               Peer::DropMode::IGNORE_WRITE_QUEUE);
    }
    return success;
}

std::string
SurveyManager::commandTypeName(SurveyMessageCommandType type)
{
    return xdr::xdr_traits<SurveyMessageCommandType>::enum_name(type);
}
}
