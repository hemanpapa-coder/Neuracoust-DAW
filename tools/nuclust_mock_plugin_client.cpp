#include "nuclust/NuclustDspManager.h"

#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

std::string sendLine(const std::string& line) {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
#endif
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(48780);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return "Unable to connect to Neuracoust DSP Manager on 127.0.0.1:48780.\n";
    }
    std::string message = line;
    if (message.empty() || message.back() != '\n') {
        message.push_back('\n');
    }
    send(sock, message.c_str(), static_cast<int>(message.size()), 0);
    char buffer[4096] = {};
    const int count = recv(sock, buffer, sizeof(buffer) - 1, 0);
#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
    return count > 0 ? std::string(buffer, buffer + count) : std::string();
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "External DSP";
    std::string module = argc > 2 ? argv[2] : "neuracoust.test.external";
    std::string server = argc > 3 ? argv[3] : "";
    std::string request =
        "{\"command\":\"registerPlugin\","
        "\"instanceId\":\"mock-plugin-001\","
        "\"pluginId\":\"neuracoust.mock.plugin\","
        "\"pluginVersion\":\"260704.0000\","
        "\"requestedMode\":\"" + mode + "\","
        "\"requestedServerId\":\"" + server + "\","
        "\"serverModuleId\":\"" + module + "\","
        "\"channelCount\":2,"
        "\"sampleRate\":48000,"
        "\"blockSize\":128,"
        "\"latencyRequirement\":512,"
        "\"nativeFallbackAllowed\":true}";
    std::cout << sendLine(request);
    return 0;
}
