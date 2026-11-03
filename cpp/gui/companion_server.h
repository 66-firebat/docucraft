#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class CompanionServer {
public:
    static constexpr int kPort = 53421;

    CompanionServer();
    ~CompanionServer();

    CompanionServer(const CompanionServer&) = delete;
    CompanionServer& operator=(const CompanionServer&) = delete;

    bool start();
    void stop();
    void enablePairing(int seconds = 120);
    bool ensureFirewall(bool promptElevation = true);
    static bool installFirewallRules();

    bool running() const;
    bool pairingEnabled() const;
    bool firewallReady() const;
    int activeJobs() const;
    std::string deviceName() const;
    std::string deviceId() const;
    std::string status() const;
    std::string firewallStatus() const;
    std::vector<std::string> recentJobs() const;

    struct JobView {
        std::string id, fileName, batchId, status, stage, message;
        int batchIndex = 0, batchCount = 0, progress = 0;
        bool cancelled = false;
    };
    std::vector<JobView> jobViews() const;
    void cancelJob(const std::string& jobId);
    void cancelBatch(const std::string& batchId);

    struct PhoneJob {
        std::wstring cli;
        std::string input, output;
        bool comic = false;
        bool pdfRaster = false;
        int pageWidth = 1920, pageHeight = 2560;
    };
    using PcSettingsCommand = std::function<std::wstring(const PhoneJob&)>;
    void setPcSettingsCommand(PcSettingsCommand command);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
