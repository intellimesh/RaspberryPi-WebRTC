#include "signaling/kinesis_service.h"
#include "common/logging.h"
#include <iostream>
#include <nlohmann/json.hpp>

#define DEFAULT_REGION "us-west-2"
#define KVS_LOG_Level LOG_LEVEL_DEBUG

// Helper to convert STATUS to string if needed, or just use hex
#define STATUS_CHECK(x)                                                                            \
    do {                                                                                           \
        STATUS ret = (x);                                                                          \
        if (ret != STATUS_SUCCESS) {                                                               \
            ERROR_PRINT("KVS Call failed 0x%08x line %d", ret, __LINE__);                          \
        }                                                                                          \
    } while (0)

using namespace nlohmann;

std::shared_ptr<KinesisService> KinesisService::Create(Args args,
                                                       std::shared_ptr<Conductor> conductor) {
    return std::make_shared<KinesisService>(args, conductor);
}

KinesisService::KinesisService(Args args, std::shared_ptr<Conductor> conductor)
    : SignalingService(conductor), channel_name_(args.kvs_channel), region_(args.aws_region) {

    if (region_.empty()) {
        region_ = DEFAULT_REGION;
    }
}

KinesisService::~KinesisService() {
    Disconnect();
}

void KinesisService::Disconnect() {
    if (signaling_client_handle_ != NULL) {
        freeSignalingClient(&signaling_client_handle_);
        signaling_client_handle_ = NULL;
    }
    DEBUG_PRINT("KinesisService disconnected");
}

void KinesisService::Connect() {
    STATUS retStatus = STATUS_SUCCESS;

    channel_info_.version = CHANNEL_INFO_CURRENT_VERSION;
    channel_info_.pChannelName = (PCHAR)channel_name_.c_str();
    channel_info_.pRegion = (PCHAR)region_.c_str();
    channel_info_.pKmsKeyId = NULL;
    channel_info_.channelType = SIGNALING_CHANNEL_TYPE_SINGLE_MASTER;
    channel_info_.channelRoleType = SIGNALING_CHANNEL_ROLE_TYPE_MASTER;
    channel_info_.pCachingPolicy = NULL;
    channel_info_.cachingPeriod = 0;
    channel_info_.asyncIceServerConfig = TRUE;
    channel_info_.retry = TRUE;
    channel_info_.reconnect = TRUE;
    channel_info_.pCertPath = NULL;
    channel_info_.messageTtl = 0;

    MEMSET(&client_info_, 0, SIZEOF(SignalingClientInfo));
    client_info_.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
    client_info_.loggingLevel = KVS_LOG_Level;
    client_info_.pChannelInfo = &channel_info_;
    client_info_.exclusiveHandle = TRUE;

    MEMSET(&client_callbacks_, 0, SIZEOF(SignalingClientCallbacks));
    client_callbacks_.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
    client_callbacks_.customData = (UINT64)this;
    client_callbacks_.messageReceivedFn = 
        [](UINT64 customData, PSignalingMessage pMessage) -> STATUS {
            if (pMessage->messageType == SIGNALING_MESSAGE_TYPE_SDP_OFFER) {
                return KinesisService::OnOffer(customData, pMessage);
            } else if (pMessage->messageType == SIGNALING_MESSAGE_TYPE_SDP_ANSWER) {
                return KinesisService::OnAnswer(customData, pMessage);
            } else if (pMessage->messageType == SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE) {
                return KinesisService::OnIceCandidate(customData, pMessage);
            }
            return STATUS_SUCCESS;
        };
    client_callbacks_.stateChangeFn = KinesisService::OnConnectionStateChange;

    INFO_PRINT("Creating Kinesis Signaling Client for channel %s in %s", channel_name_.c_str(), region_.c_str());

    retStatus = createSignalingClientSync(&client_info_, &channel_info_,
                                          &client_callbacks_, &signaling_client_handle_);
    if (retStatus != STATUS_SUCCESS) {
        ERROR_PRINT("Failed to create Kinesis Signaling Client: 0x%08x", retStatus);
        return;
    }

    INFO_PRINT("Connecting Kinesis Signaling Client...");
    retStatus = signalingClientConnectSync(signaling_client_handle_);
    if (retStatus != STATUS_SUCCESS) {
         ERROR_PRINT("Failed to connect Kinesis Signaling Client: 0x%08x", retStatus);
         Disconnect();
    } else {
        INFO_PRINT("Kinesis Signaling Client init initiated.");
    }
}

STATUS KinesisService::OnConnectionStateChange(UINT64 custom_data, SIGNALING_CLIENT_STATE state) {
    INFO_PRINT("Kinesis Connection State Changed: %d", state);
    return STATUS_SUCCESS;
}

STATUS KinesisService::OnOffer(UINT64 custom_data, PSignalingMessage message) {
    KinesisService *service = reinterpret_cast<KinesisService *>(custom_data);
    std::string sdp(message->payload, message->payloadLen);
    std::string peer_id(message->peerClientId, message->peerClientIdLen);
    
    DEBUG_PRINT("Received Offer from %s", peer_id.c_str());
    service->HandleOffer(sdp, peer_id);
    return STATUS_SUCCESS;
}

STATUS KinesisService::OnAnswer(UINT64 custom_data, PSignalingMessage message) {
    KinesisService *service = reinterpret_cast<KinesisService *>(custom_data);
    std::string sdp(message->payload, message->payloadLen);
    std::string peer_id(message->peerClientId, message->peerClientIdLen);
    
    DEBUG_PRINT("Received Answer from %s", peer_id.c_str());
    service->HandleAnswer(sdp, peer_id);
    return STATUS_SUCCESS;
}

STATUS KinesisService::OnIceCandidate(UINT64 custom_data, PSignalingMessage message) {
    KinesisService *service = reinterpret_cast<KinesisService *>(custom_data);
    std::string candidate_json(message->payload, message->payloadLen);
    std::string peer_id(message->peerClientId, message->peerClientIdLen);
    
    try {
        auto json = json::parse(candidate_json);
        std::string candidate = json["candidate"];
        std::string sdp_mid = json["sdpMid"];
        int sdp_mline_index = json["sdpMLineIndex"];
        
        DEBUG_PRINT("Received ICE from %s: %s", peer_id.c_str(), candidate.c_str());
        service->HandleIceCandidate(candidate, sdp_mid, sdp_mline_index, peer_id);
    } catch (const std::exception &e) {
        ERROR_PRINT("Failed to parse ICE candidate JSON: %s", e.what());
    }
    return STATUS_SUCCESS;
}

void KinesisService::HandleOffer(const std::string &sdp, const std::string &client_id) {
    auto peer = CreatePeer();
    if (!peer) {
        ERROR_PRINT("Failed to create peer for %s", client_id.c_str());
        return;
    }

    client_id_to_peer_id_[client_id] = peer->id();
    peer_id_to_client_id_[peer->id()] = client_id;

    peer->OnLocalSdp(
        [this, client_id](const std::string &id, const std::string &sdp, const std::string &type) {
            this->AnswerLocalSdp(client_id, sdp);
        });

    peer->OnLocalIce([this, client_id](const std::string &id, const std::string &mid, int index, const std::string &cand) {
        this->AnswerLocalIce(client_id, mid, index, cand);
    });

    peer->SetRemoteSdp(sdp, "offer");
}

void KinesisService::HandleAnswer(const std::string &sdp, const std::string &client_id) {
    if (client_id_to_peer_id_.count(client_id)) {
        auto peer_id = client_id_to_peer_id_[client_id];
        auto peer = GetPeer(peer_id);
        if (peer) {
            peer->SetRemoteSdp(sdp, "answer");
        }
    } else {
        ERROR_PRINT("Received Answer from unknown client: %s", client_id.c_str());
    }
}

void KinesisService::HandleIceCandidate(const std::string &candidate, const std::string &mid,
                                        int index, const std::string &client_id) {
    if (client_id_to_peer_id_.count(client_id)) {
        auto peer_id = client_id_to_peer_id_[client_id];
        auto peer = GetPeer(peer_id);
        if (peer) {
             peer->SetRemoteIce(mid, index, candidate);
        }
    } else {
        ERROR_PRINT("Received ICE from unknown client: %s", client_id.c_str());
    }
}

void KinesisService::AnswerLocalSdp(const std::string &client_id, const std::string &sdp) {
    SignalingMessage message;
    message.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    message.messageType = SIGNALING_MESSAGE_TYPE_SDP_ANSWER;
    message.payload = (PCHAR)sdp.c_str();
    message.payloadLen = sdp.length();
    message.peerClientId = (PCHAR)client_id.c_str();
    message.peerClientIdLen = client_id.length();
    
    DEBUG_PRINT("Sending SDP Answer to %s", client_id.c_str());
    STATUS ret = signalingClientSendMessageSync(signaling_client_handle_, &message);
    if (ret != STATUS_SUCCESS) {
        ERROR_PRINT("Failed to send SDP answer: 0x%08x", ret);
    }
}

void KinesisService::AnswerLocalIce(const std::string &client_id, const std::string &mid, int index, const std::string &candidate) {
    json j;
    j["candidate"] = candidate;
    j["sdpMid"] = mid;
    j["sdpMLineIndex"] = index;
    std::string payload = j.dump();

    SignalingMessage message;
    message.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    message.messageType = SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE;
    message.payload = (PCHAR)payload.c_str();
    message.payloadLen = payload.length();
    message.peerClientId = (PCHAR)client_id.c_str();
    message.peerClientIdLen = client_id.length();

    DEBUG_PRINT("Sending ICE Candidate to %s", client_id.c_str());
    STATUS ret = signalingClientSendMessageSync(signaling_client_handle_, &message);
    if (ret != STATUS_SUCCESS) {
        ERROR_PRINT("Failed to send ICE candidate: 0x%08x", ret);
    }
}

void KinesisService::RefreshPeerMap() {
    auto &map = GetPeerMap();
    auto pm_it = map.begin();
    while (pm_it != map.end()) {
        auto peer_id = pm_it->first;
        auto peer = GetPeer(peer_id);
        
        if (peer && !peer->isConnected()) {
            if (peer_id_to_client_id_.count(peer_id)) {
                auto client_id = peer_id_to_client_id_[peer_id];
                client_id_to_peer_id_.erase(client_id);
                peer_id_to_client_id_.erase(peer_id);
            }
            pm_it = map.erase(pm_it);
            DEBUG_PRINT("(%s) was erased by KinesisService.", peer_id.c_str());
        } else {
            ++pm_it;
        }
    }
}
