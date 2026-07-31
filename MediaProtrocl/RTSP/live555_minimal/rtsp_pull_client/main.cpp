#include <BasicUsageEnvironment.hh>
#include <GroupsockHelper.hh>
#include <MediaSession.hh>
#include <MediaSink.hh>
#include <RTSPClient.hh>
#include <liveMedia.hh>

#include <winsock2.h>

#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {
constexpr unsigned kReceiveBufferSize = 200000;

class WinsockRuntime {
public:
    WinsockRuntime() : ok_(false) {
        WSADATA data;
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~WinsockRuntime() {
        if (ok_) {
            WSACleanup();
        }
    }

    bool ok() const { return ok_; }

private:
    bool ok_;
};

class StreamClientState {
public:
    StreamClientState() : iter(nullptr), session(nullptr), subsession(nullptr), sink(nullptr), streamTimerTask(nullptr), duration(0.0) {}

    ~StreamClientState() {
        delete iter;
        if (session != nullptr) {
            UsageEnvironment& env = session->envir();
            env.taskScheduler().unscheduleDelayedTask(streamTimerTask);
            Medium::close(session);
        }
    }

    MediaSubsessionIterator* iter;
    MediaSession* session;
    MediaSubsession* subsession;
    MediaSink* sink;
    TaskToken streamTimerTask;
    double duration;
};

class OurRTSPClient : public RTSPClient {
public:
    static OurRTSPClient* createNew(UsageEnvironment& env, const char* rtspURL, bool useTcp) {
        return new OurRTSPClient(env, rtspURL, useTcp);
    }

    bool useTcp() const { return useTcp_; }
    StreamClientState& state() { return state_; }

protected:
    OurRTSPClient(UsageEnvironment& env, const char* rtspURL, bool useTcp)
        : RTSPClient(env, rtspURL, 1, "live555_minimal_pull_client", 0, -1), useTcp_(useTcp) {}

    ~OurRTSPClient() override = default;

private:
    bool useTcp_;
    StreamClientState state_;
};

class DebugSink : public MediaSink {
public:
    static DebugSink* createNew(UsageEnvironment& env, MediaSubsession& subsession) {
        return new DebugSink(env, subsession);
    }

private:
    DebugSink(UsageEnvironment& env, MediaSubsession& subsession)
        : MediaSink(env), subsession_(subsession), buffer_(new unsigned char[kReceiveBufferSize]), frameCount_(0) {}

    ~DebugSink() override { delete[] buffer_; }

    Boolean continuePlaying() override {
        if (fSource == nullptr) {
            return False;
        }

        fSource->getNextFrame(buffer_, kReceiveBufferSize, afterGettingFrame, this, onSourceClosure, this);
        return True;
    }

    static void afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
                                  timeval presentationTime, unsigned durationInMicroseconds) {
        static_cast<DebugSink*>(clientData)->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
    }

    void afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes, timeval presentationTime, unsigned durationInMicroseconds) {
        ++frameCount_;
        std::ostringstream line;
        line << "[RTP payload] " << subsession_.mediumName() << "/" << subsession_.codecName()
             << " pt=" << static_cast<unsigned>(subsession_.rtpPayloadFormat())
             << " clock=" << subsession_.rtpTimestampFrequency()
             << " frame=" << frameCount_
             << " size=" << frameSize;

        if (numTruncatedBytes > 0) {
            line << " truncated=" << numTruncatedBytes;
        }

        line << " pts=" << presentationTime.tv_sec << "." << std::setw(6) << std::setfill('0') << presentationTime.tv_usec
             << " duration_us=" << durationInMicroseconds;

        if (frameSize > 0) {
            line << std::setfill(' ') << " first_byte=0x" << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned>(buffer_[0]) << std::dec;
            const std::string codec = subsession_.codecName() == nullptr ? "" : subsession_.codecName();
            if (codec == "H264") {
                line << " h264_nal_type=" << static_cast<unsigned>(buffer_[0] & 0x1F);
            } else if (codec == "H265") {
                line << " h265_nal_type=" << static_cast<unsigned>((buffer_[0] >> 1) & 0x3F);
            }
        }

        std::cout << line.str() << "\n";
        continuePlaying();
    }

    MediaSubsession& subsession_;
    unsigned char* buffer_;
    unsigned frameCount_;
};

void continueAfterOptions(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterDescribe(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterSetup(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterPlay(RTSPClient* rtspClient, int resultCode, char* resultString);

void setupNextSubsession(RTSPClient* rtspClient) {
    auto* client = static_cast<OurRTSPClient*>(rtspClient);
    UsageEnvironment& env = client->envir();
    StreamClientState& state = client->state();

    state.subsession = state.iter->next();
    if (state.subsession != nullptr) {
        if (!state.subsession->initiate()) {
            env << "Failed to initiate subsession " << state.subsession->mediumName() << "/" << state.subsession->codecName()
                << ": " << env.getResultMsg() << "\n";
            setupNextSubsession(rtspClient);
            return;
        }

        env << "SETUP " << state.subsession->mediumName() << "/" << state.subsession->codecName()
            << " pt=" << static_cast<unsigned>(state.subsession->rtpPayloadFormat())
            << " clock=" << state.subsession->rtpTimestampFrequency()
            << " transport=" << (client->useTcp() ? "RTP/RTSP/TCP" : "RTP/UDP") << "\n";
        client->sendSetupCommand(*state.subsession, continueAfterSetup, False, client->useTcp() ? True : False);
        return;
    }

    if (state.session->absStartTime() != nullptr) {
        client->sendPlayCommand(*state.session, continueAfterPlay, state.session->absStartTime(), state.session->absEndTime());
    } else {
        state.duration = state.session->playEndTime() - state.session->playStartTime();
        client->sendPlayCommand(*state.session, continueAfterPlay);
    }
}

void shutdownStream(RTSPClient* rtspClient, int exitCode = 0) {
    auto* client = static_cast<OurRTSPClient*>(rtspClient);
    UsageEnvironment& env = client->envir();
    StreamClientState& state = client->state();

    if (state.session != nullptr) {
        Boolean hasActiveSubsessions = False;
        MediaSubsessionIterator iter(*state.session);
        MediaSubsession* subsession;
        while ((subsession = iter.next()) != nullptr) {
            if (subsession->sink != nullptr) {
                Medium::close(subsession->sink);
                subsession->sink = nullptr;
                if (subsession->rtcpInstance() != nullptr) {
                    subsession->rtcpInstance()->setByeHandler(nullptr, nullptr);
                }
                hasActiveSubsessions = True;
            }
        }

        if (hasActiveSubsessions) {
            client->sendTeardownCommand(*state.session, nullptr);
        }
    }

    env << "RTSP client stopped, exitCode=" << exitCode << "\n";
    Medium::close(rtspClient);
}

void subsessionAfterPlaying(void* clientData) {
    auto* subsession = static_cast<MediaSubsession*>(clientData);
    Medium::close(subsession->sink);
    subsession->sink = nullptr;
}

void continueAfterOptions(RTSPClient* rtspClient, int resultCode, char* resultString) {
    UsageEnvironment& env = rtspClient->envir();
    std::unique_ptr<char[]> response(resultString);
    if (resultCode != 0) {
        env << "OPTIONS failed: " << (resultString == nullptr ? "" : resultString) << "\n";
    } else {
        env << "OPTIONS ok:\n" << (resultString == nullptr ? "" : resultString) << "\n";
    }
    rtspClient->sendDescribeCommand(continueAfterDescribe);
}

void continueAfterDescribe(RTSPClient* rtspClient, int resultCode, char* resultString) {
    auto* client = static_cast<OurRTSPClient*>(rtspClient);
    UsageEnvironment& env = client->envir();
    std::unique_ptr<char[]> sdpDescription(resultString);

    if (resultCode != 0) {
        env << "DESCRIBE failed: " << (resultString == nullptr ? "" : resultString) << "\n";
        shutdownStream(rtspClient, 1);
        return;
    }

    env << "DESCRIBE ok, SDP:\n" << resultString << "\n";
    StreamClientState& state = client->state();
    state.session = MediaSession::createNew(env, sdpDescription.get());
    if (state.session == nullptr) {
        env << "Failed to create MediaSession from SDP: " << env.getResultMsg() << "\n";
        shutdownStream(rtspClient, 1);
        return;
    }

    if (!state.session->hasSubsessions()) {
        env << "SDP contains no media subsessions\n";
        shutdownStream(rtspClient, 1);
        return;
    }

    state.iter = new MediaSubsessionIterator(*state.session);
    setupNextSubsession(rtspClient);
}

void continueAfterSetup(RTSPClient* rtspClient, int resultCode, char* resultString) {
    auto* client = static_cast<OurRTSPClient*>(rtspClient);
    UsageEnvironment& env = client->envir();
    StreamClientState& state = client->state();
    std::unique_ptr<char[]> response(resultString);

    if (resultCode != 0) {
        env << "SETUP failed: " << (resultString == nullptr ? "" : resultString) << "\n";
        setupNextSubsession(rtspClient);
        return;
    }

    env << "SETUP ok:\n" << resultString << "\n";
    state.subsession->sink = DebugSink::createNew(env, *state.subsession);
    state.subsession->sink->startPlaying(*state.subsession->readSource(), subsessionAfterPlaying, state.subsession);
    if (state.subsession->rtcpInstance() != nullptr) {
        state.subsession->rtcpInstance()->setByeHandler(subsessionAfterPlaying, state.subsession);
    }

    setupNextSubsession(rtspClient);
}

void continueAfterPlay(RTSPClient* rtspClient, int resultCode, char* resultString) {
    auto* client = static_cast<OurRTSPClient*>(rtspClient);
    UsageEnvironment& env = client->envir();
    std::unique_ptr<char[]> response(resultString);
    if (resultCode != 0) {
        env << "PLAY failed: " << (resultString == nullptr ? "" : resultString) << "\n";
        shutdownStream(rtspClient, 1);
        return;
    }

    env << "PLAY ok:\n" << resultString << "\n";
    env << "Receiving RTP payloads. Press Ctrl+C to stop.\n";
}

void printUsage(const char* app) {
    std::cout << "Usage:\n"
              << "  " << app << " <rtsp-url> [--tcp]\n\n"
              << "Example:\n"
              << "  " << app << " rtsp://127.0.0.1:8554/test\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    bool useTcp = false;
    if (argc >= 3 && std::string(argv[2]) == "--tcp") {
        useTcp = true;
    }

    WinsockRuntime winsock;
    if (!winsock.ok()) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    TaskScheduler* scheduler = BasicTaskScheduler::createNew();
    UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

    OurRTSPClient* client = OurRTSPClient::createNew(*env, argv[1], useTcp);
    if (client == nullptr) {
        *env << "Failed to create RTSP client: " << env->getResultMsg() << "\n";
        return 1;
    }

    *env << "Connecting to " << argv[1] << "\n";
    client->sendOptionsCommand(continueAfterOptions);
    env->taskScheduler().doEventLoop();
    return 0;
}
