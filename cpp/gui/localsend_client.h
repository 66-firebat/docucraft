#pragma once

#include <string>
#include <vector>

struct LocalSendPeer {
    std::string alias;
    std::string ip;
    int port = 53317;
    std::string fingerprint;
    bool useTls = true;
};

std::vector<LocalSendPeer> scanLocalSendPeers();
bool probeLocalSendPeer(const std::string& ip, int port, LocalSendPeer& peer, std::string& error);
bool sendLocalSendFile(const LocalSendPeer& peer, const std::string& filePath, std::string& error);

bool loadBoundLocalSendPeer(LocalSendPeer& peer);
bool saveBoundLocalSendPeer(const LocalSendPeer& peer);

