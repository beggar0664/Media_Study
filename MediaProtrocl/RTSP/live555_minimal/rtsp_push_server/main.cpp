#include <BasicUsageEnvironment.hh>
#include <GroupsockHelper.hh>
#include <H264VideoFileServerMediaSubsession.hh>
#include <liveMedia.hh>

#include <winsock2.h>

#include <iostream>
#include <string>

namespace {
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

void printUsage(const char* app) {
    std::cout << "Usage:\n"
              << "  " << app << " <h264-annexb-file> [stream-name] [port]\n\n"
              << "Example:\n"
              << "  " << app << " E:\\media\\test.h264 test 8554\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    WinsockRuntime winsock;
    if (!winsock.ok()) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    const char* fileName = argv[1];
    const char* streamName = argc >= 3 ? argv[2] : "test";
    unsigned short portNumber = argc >= 4 ? static_cast<unsigned short>(std::stoi(argv[3])) : 8554;

    TaskScheduler* scheduler = BasicTaskScheduler::createNew();
    UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

    RTSPServer* rtspServer = RTSPServer::createNew(*env, Port(portNumber));
    if (rtspServer == nullptr) {
        *env << "Failed to create RTSP server: " << env->getResultMsg() << "\n";
        return 1;
    }

    ServerMediaSession* sms = ServerMediaSession::createNew(
        *env,
        streamName,
        streamName,
        "live555 minimal H264 RTSP server");
    sms->addSubsession(H264VideoFileServerMediaSubsession::createNew(*env, fileName, False));
    rtspServer->addServerMediaSession(sms);

    char* url = rtspServer->rtspURL(sms);
    std::cout << "RTSP server started\n"
              << "  input : " << fileName << "\n"
              << "  url   : " << url << "\n"
              << "Press Ctrl+C to stop.\n";
    delete[] url;

    env->taskScheduler().doEventLoop();
    return 0;
}

