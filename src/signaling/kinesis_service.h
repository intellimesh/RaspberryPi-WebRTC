#ifndef KINESIS_SERVICE_H_
#define KINESIS_SERVICE_H_

#include "signaling/signaling_service.h"
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>

#include "args.h"

class KinesisService : public SignalingService {
  public:
    static std::shared_ptr<KinesisService> Create(Args args, std::shared_ptr<Conductor> conductor);

    KinesisService(Args args, std::shared_ptr<Conductor> conductor);
    ~KinesisService();

  protected:
    void Connect() override;
    void Disconnect() override;
    void RefreshPeerMap() override;

  private:
    std::string channel_name_;
    std::string region_;
    PSignalingClientHandle signaling_client_handle_ = NULL;
    SignalingClientInfo client_info_;
    SignalingClientCallbacks client_callbacks_;
    ChannelInfo channel_info_;

    static STATUS OnOffer(UINT64 custom_data, PSignalingMessage message);
    static STATUS OnAnswer(UINT64 custom_data, PSignalingMessage message);
    static STATUS OnIceCandidate(UINT64 custom_data, PSignalingMessage message);
    static STATUS OnConnectionStateChange(UINT64 custom_data,
                                          SIGNALING_CLIENT_STATE state);

    void HandleOffer(const std::string &sdp, const std::string &peer_id);
    void HandleAnswer(const std::string &sdp, const std::string &peer_id);
    void HandleIceCandidate(const std::string &candidate, const std::string &sdp_mid,
                            int sdp_mline_index, const std::string &peer_id);
    void AnswerLocalSdp(const std::string &peer_id, const std::string &sdp);
    void AnswerLocalIce(const std::string &peer_id, const std::string &mid, int index, const std::string &candidate);

    std::unordered_map<std::string, std::string> client_id_to_peer_id_;
    std::unordered_map<std::string, std::string> peer_id_to_client_id_;
};

#endif
