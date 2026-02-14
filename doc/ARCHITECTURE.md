# System Architecture

This document provides a high-level overview of how the various modules in `src/` interact to form the complete application.

## High-Level Component Diagram

```mermaid
graph TD
    classDef hardware fill:#f9f,stroke:#333,stroke-width:2px;
    classDef core fill:#bbf,stroke:#333,stroke-width:2px;
    classDef module fill:#dfd,stroke:#333,stroke-width:2px;
    classDef external fill:#ddd,stroke:#333,stroke-width:2px;

    Camera([Camera Hardware]):::hardware
    Microphone([Microphone]):::hardware

    subgraph "Application Scope (src/)"
        Main[Main Entry Point]:::core

        subgraph "Media Sources (src/capturer)"
            VideoCapturer[Video Capturer]:::module
            PaCapturer[Audio Capturer]:::module
        end

        subgraph "RTC Core (src/rtc)"
            Conductor[Conductor]:::core
            RtcPeer[RtcPeer Connection]:::module
            RtcChannel[Data Channel]:::module
        end

        subgraph "Signaling (src/signaling)"
            MqttService[MQTT Service]:::module
            HttpService[HTTP Service]:::module
            WebSocketService[WebSocket Service]:::module
        end

        subgraph "Recording (src/recorder)"
            RecorderManager[Recorder Manager]:::module
            VideoRecorder[Video Recorder]:::module
            AudioRecorder[Audio Recorder]:::module
        end

        subgraph "IPC (src/ipc)"
            UnixSocket[Unix Socket Server]:::module
        end
    end

    ExternalApp[Local External App]:::external
    RemoteClient[Remote Web/Mobile Client]:::external
    MQTTBroker[MQTT Broker]:::external

    %% Initialization
    Main --> Conductor
    Main --> MqttService
    Main --> HttpService
    Main --> RecorderManager

    %% Media Flow
    Camera --> VideoCapturer
    Microphone --> PaCapturer

    VideoCapturer -- Raw/Encoded Frames --> Conductor
    VideoCapturer -- Raw/Encoded Frames --> RecorderManager
    PaCapturer -- Raw Audio --> Conductor
    PaCapturer -- Raw Audio --> RecorderManager

    %% RTC Flow
    Conductor --> RtcPeer
    RtcPeer --> RemoteClient
    RtcPeer --> RtcChannel

    %% Signaling Flow
    MqttService -- SDP/ICE --> Conductor
    HttpService -- SDP/ICE --> Conductor
    WebSocketService -- SDP/ICE --> Conductor

    MqttService <--> MQTTBroker
    MQTTBroker <--> RemoteClient

    %% Recording Flow
    RecorderManager --> VideoRecorder
    RecorderManager --> AudioRecorder
    VideoRecorder --> Disk[(Local Mp4 Files)]

    %% IPC Flow
    ExternalApp <--> UnixSocket
    UnixSocket <--> Conductor
    Conductor <--> RtcChannel
```

## Module Responsibilities

1.  **`src/capturer` (Producer)**:
    *   Interfaces with V4L2 or Libcamera to get video frames.
    *   Interfaces with PulseAudio to get audio samples.
    *   Acts as the *Source of Truth* for media data.

2.  **`src/rtc` (Consumer 1 & Controller)**:
    *   **Conductor**: Central hub. It owns the capturers and creates peers.
    *   **RtcPeer**: Encapsulates a WebRTC connection. It takes frames from the capturer (via Conductor) and streams them to the network.
    *   **RtcChannel**: Handles non-media data (commands, IoT control).

3.  **`src/recorder` (Consumer 2)**:
    *   **Independently** subscribes to the `src/capturer`.
    *   Records the same frames to disk that `src/rtc` is streaming.
    *   Operates even if no peer is connected.

4.  **`src/signaling` (Negotiator)**:
    *   Handles the specific protocol (MQTT, HTTP, WebSocket) to exchange metadata (SDP, ICE) with clients.
    *   Tells the `Conductor` when to create a new `RtcPeer`.
    *   Doesn't touch media data itself.

5.  **`src/ipc` (Bridge)**:
    *   Allows local processes to piggyback on the WebRTC Data Channel.
    *   Routes messages: `Unix Socket <-> Conductor <-> RtcChannel`.
