#ifndef LOCALCONFIG_H
#define LOCALCONFIG_H
#include <string>
#include "ui_uwavclient.h"

struct u_ConfigData {
    std::string clientID = "";
    std::string inputDevice = "";
    std::string outputDevice = "";
};

class LocalConfig
{
public:
    LocalConfig();
    ~LocalConfig();

    void Save(Ui::UwaVClient *ui, bool skipHistory = false);
    void Load(Ui::UwaVClient *ui);

    std::string GetSavedInputDevice();
    std::string GetSavedOutputDevice();
    void SetSavedInputDevice(std::string name);
    void SetSavedOutputDevice(std::string name);
    std::string GetClientID();

private:
    std::string localPath;
    struct u_ConfigData config;

    std::pair<std::string, std::string> ParseLine(std::string &cfgLine);
    std::string GenerateLocalUID();
};

#endif // LOCALCONFIG_H
