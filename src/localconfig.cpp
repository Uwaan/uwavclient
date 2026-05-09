#include "localconfig.h"
#include "u_sharedfunctions.h"
#include <ctime>
#include <string>
#include <QDebug>
#include <filesystem>
#include <fstream>


// For linux build
#include <unistd.h>
#include <limits.h>

// For windows build
//#include <windows.h>
//#include <Lmcons.h>

#ifndef U_APPNAME
#define U_APPNAME "uwavclient"
#endif

#ifndef U_SAVE_PREV_SERVERS
#define U_SAVE_PREV_SERVERS 5
#endif

LocalConfig::LocalConfig()
{
    // Linux
    if (std::getenv("HOME") != NULL)
        localPath = std::getenv("HOME");
    else
        localPath = ".";
    localPath += "/.local/share/";
    localPath += U_APPNAME;
    localPath += "/client.conf";

    // Windows
    //localPath = std::getenv("APPDATA");
    //localPath += "\\Roaming\\";
    //localPath += U_APPNAME;
    //localPath += "\\client.conf";
}

LocalConfig::~LocalConfig()
{
    //delete config;
}

void LocalConfig::Save(Ui::UwaVClient *ui, bool skipHistory)
{
    // Things to save:
    // - UID
    // - current input device, output device
    // - current model name, current compute device
    // - List of last connected servers
    std::string oldPath = localPath + ".old";
    bool skipOverwrite = false;
    std::error_code err;

    std::filesystem::file_status fStat = std::filesystem::file_status{};

    if (!std::filesystem::exists(localPath))
    {
        // Attempt to create

        // Linux
        std::string path;
        if (std::getenv("HOME") != NULL)
            path = std::getenv("HOME");
        else
            path = ".";
        path += "/.local/share/";
        path += U_APPNAME;

        // Windows
        //std::string path = std::getenv("APPDATA");
        //path += "\\Roaming\\";
        //path += U_APPNAME;

        if (!std::filesystem::exists(path))
        {
            std::filesystem::create_directories(path, err);

            if (err.value() != 0)
            {
                // Couldn't create directories for some reason;
                // fail
                qCritical("CRITICAL: Unable to save settings! %s", err.message().c_str());
                return;
            }
            else
            {
                qInfo("Created directory %s", path.c_str());
            }
            skipOverwrite = true;
        }
        else
        {
            // We have a path for this application in the .local folder,
            // but there is no config file there currently. Just skip
            // parsing through the old one, and create a new one
            skipOverwrite = true;
        }

        // Do a test write just to make sure we have permission to
        // create, remove files here
        if (!std::ofstream(localPath.c_str()).put('#'))
        {
            // Failure to create file; we can't store here
            // perror may have more info
            std::perror("Error creating file");
            qCritical("CRITICAL: Unable to save settings! (Cannot create file at %s)", localPath.c_str());
            return;
        }
        // File will be writeable
        std::filesystem::remove(localPath, err);
        if (err.value() != 0)
        {
            qCritical("CRITICAL: Unable to save settings! (Temp remove failed), %s", err.message().c_str());
            return;
        }
    }

    if (std::filesystem::exists(oldPath))
    {
        // A previous operation must not have cleaned up properly.
        // Move will fail if we don't clean it up now.
        std::filesystem::remove(oldPath, err);
        if (err.value() != 0)
        {
            qCritical("CRITICAL: Unable to save settings! (Could not remove stale copy), %s", err.message().c_str());
            return;
        }
    }

    // All right, finally: try moving the current settings to a ".old" file
    // so that we can write new settings
    if (!skipOverwrite)
    {
        std::filesystem::rename(localPath.c_str(), oldPath.c_str(), err);
        if (err.value() != 0)
        {
            // Failure to rename file. Could be that we have insufficient
            // permissions to write here?
            qCritical("CRITICAL: Unable to save settings! (Could not shadow copy), %s", err.message().c_str());
            return;
        }
    }

    std::ifstream oldConfigStream(oldPath);
    std::ofstream newConfigStream(localPath);
    std::string nextLine;
    std::pair<std::string, std::string> keyval;
    bool setClientID = false;
    bool setInDevice = false;
    bool setOutDevice = false;
    bool setServerHistory = false;
    bool setInferDevice = false;
    bool setModel = false;

    if (newConfigStream.is_open())
    {
        if (!skipOverwrite)
        {
            while (std::getline(oldConfigStream, nextLine))
            {
                // Seek through file & replace lines where they exist
                keyval = ParseLine(nextLine);
                if (keyval.first == "uid")
                {
                    setClientID = true;
                    if (keyval.second != config.clientID)
                        newConfigStream << "uid=" << config.clientID << "\n";
                    else
                        newConfigStream << nextLine << "\n";
                }
                else if (keyval.first == "inputDevice")
                {
                    setInDevice = true;
                    if (keyval.second != config.inputDevice)
                        newConfigStream << "inputDevice=" << config.inputDevice << "\n";
                    else
                        newConfigStream << nextLine << "\n";
                }
                else if (keyval.first == "outputDevice")
                {
                    setOutDevice = true;
                    if (keyval.second != config.outputDevice)
                        newConfigStream << "outputDevice=" << config.outputDevice << "\n";
                    else
                        newConfigStream << nextLine << "\n";
                }
                else if (keyval.first == "previousServers")
                {
                    setServerHistory = true;

                    if (ui != nullptr && !skipHistory)
                    {
                        int numEntries = ui->comboServerSelect->count();
                        if (numEntries > U_SAVE_PREV_SERVERS) numEntries = U_SAVE_PREV_SERVERS;

                        newConfigStream << "previousServers=";

                        for (int i = numEntries - 1; i >= 0; i--)
                        {
                            newConfigStream << ui->comboServerSelect->itemText(i).toStdString() << ";";
                        }

                        if (numEntries == 0)
                        {
                            newConfigStream << "none\n";
                        }
                        else
                        {
                            newConfigStream << "\n";
                        }
                    }
                    else
                    {
                        newConfigStream << nextLine << "\n";
                    }
                }
                else if (keyval.first == "inferDevice")
                {
                    setInferDevice = true;

                    if (ui != nullptr)
                    {
                        if (!ui->comboInferDevice->currentText().isEmpty())
                            config.inferDevice = ui->comboInferDevice->currentText().toStdString();
                    }

                    if (keyval.second != config.inferDevice)
                        newConfigStream << "inferDevice=" << config.inferDevice << "\n";
                    else
                        newConfigStream << nextLine << "\n";
                }
                else if (keyval.first == "modelName")
                {
                    setModel = true;

                    if (ui != nullptr)
                    {
                        if (!ui->comboModel->currentText().isEmpty())
                            config.modelName = ui->comboModel->currentText().toStdString();
                    }

                    if (keyval.second != config.modelName)
                        newConfigStream << "modelName=" << config.modelName << "\n";
                    else
                        newConfigStream << nextLine << "\n";
                }
                else
                {
                    // This line was not one of the things that need to be written -
                    // pass through
                    newConfigStream << nextLine << "\n";
                }
            }
        }

        // Finished reading input stream
        // Write any items that weren't overwritten
        if (!setClientID)
        {
            newConfigStream << "uid=" << config.clientID << "\n";
        }
        if (!setInDevice)
        {
            newConfigStream << "inputDevice=" << config.inputDevice << "\n";
        }
        if (!setOutDevice)
        {
            newConfigStream << "outputDevice=" << config.outputDevice << "\n";
        }
        if (!setInferDevice)
        {
            newConfigStream << "inferDevice=" << config.inferDevice << "\n";
        }
        if (!setModel)
        {
            newConfigStream << "modelName=" << config.modelName << "\n";
        }
        if (!setServerHistory)
        {
            int i = 0;

            newConfigStream << "previousServers=";
            if (ui != nullptr && !skipHistory)
            {
                int numEntries = ui->comboServerSelect->count();
                if (numEntries > U_SAVE_PREV_SERVERS) numEntries = U_SAVE_PREV_SERVERS;

                newConfigStream << "previousServers=";

                for (int i = numEntries - 1; i >= 0; i--)
                {
                    newConfigStream << ui->comboServerSelect->itemText(i).toStdString() << ";";
                }

                if (numEntries == 0)
                {
                    newConfigStream << "none\n";
                }
                else
                {
                    newConfigStream << "\n";
                }
            }
            else if (!skipHistory)
            {
                newConfigStream << "previousServers=none\n";
            }

            if (i == 0)
            {
                // Didn't find any items in combo box
                newConfigStream << "none\n";
            }
            else
            {
                newConfigStream << "\n";
            }
        }

        // All items have been written
        // Not bothering with any possible error code right now
        // because other paths deal with it
        std::filesystem::remove(oldPath.c_str());
    }
    else
    {
        qWarning("Unable to open settings file for saving.");
    }
}

// Attempt to load any saved preferences from disk (user's home
// folder). Will auto-select audio devices if they exist, and
// if 'ui' is not null.
void LocalConfig::Load(Ui::UwaVClient *ui)
{
    std::ifstream fs(localPath);
    std::string nextLine;
    std::pair<std::string, std::string> keyval;
    size_t index;

    if (fs.is_open())
    {
        while (std::getline(fs, nextLine))
        {
            keyval = ParseLine(nextLine);
            if (keyval.first == "uid")
            {
                config.clientID = keyval.second;
            }
            else if (keyval.first == "inputDevice" && keyval.second != "none")
            {
                config.inputDevice = keyval.second;
                if (ui != nullptr)
                {
                    index = ui->comboAudioSrcLocal->findText(keyval.second.c_str());
                    if (index != -1)
                    {
                        ui->comboAudioSrcLocal->setCurrentIndex(index);
                    }
                    else if (keyval.second == "server")
                    {
                        // This is placeholder text, set when not connected
                        // to any server (and thus no server device list)
                        index = ui->comboAudioSrcLocal->findText("Input Audio on Server");
                        if (index != -1)
                        {
                            ui->comboAudioSrcLocal->setCurrentIndex(index);
                        }
                    }
                }
            }
            else if (keyval.first == "outputDevice" && keyval.second != "none")
            {
                config.outputDevice = keyval.second;
                if (ui != nullptr)
                {
                    index = ui->comboAudioSinkLocal->findText(keyval.second.c_str());
                    if (index != -1)
                    {
                        ui->comboAudioSinkLocal->setCurrentIndex(index);
                    }
                    else if (keyval.second == "server")
                    {
                        // Same as inputDevice - check placeholder
                        index = ui->comboAudioSinkLocal->findText("Output Audio on Server");
                        if (index != -1)
                        {
                            ui->comboAudioSinkLocal->setCurrentIndex(index);
                        }
                    }
                }
            }
            else if (keyval.first == "previousServers" && keyval.second != "none")
            {
                if (ui != nullptr)
                {
                    // We don't want loading of the current text
                    // to initiate a connection.
                    ui->comboServerSelect->blockSignals(true);
                    ui->comboServerSelect->clear();

                    // Cheap hack just in case - makes sure last entry gets counted
                    if (keyval.second.back() != ';')
                    {
                        keyval.second = keyval.second.append(";");
                    }

                    for (int i = 0; i < U_SAVE_PREV_SERVERS; i++)
                    {
                        index = keyval.second.find(';');
                        if (index != std::string::npos) // .find() returns npos if no match
                        {
                            ui->comboServerSelect->addItem(keyval.second.substr(0, index).c_str());
                            if (keyval.second.length() > index + 1)
                            {
                                keyval.second = keyval.second.substr(index + 1);
                            }
                            else
                            {
                                // There is nothing after this ';'
                                break;
                            }
                        }
                        else break;
                    }

                    if (ui->comboServerSelect->count() > 0)
                    {
                        ui->comboServerSelect->setCurrentIndex(0);
                    }
                    ui->comboServerSelect->blockSignals(false);
                }
            }
            else if (keyval.first == "inferDevice" && keyval.second != "none")
            {
                config.inferDevice = keyval.second;
                if (ui != nullptr)
                {
                    // Client has almost certainly not connected to get a list of
                    // available infer devices yet.
                    ui->comboInferDevice->blockSignals(true);
                    ui->comboInferDevice->setCurrentText(QString::fromStdString(config.inferDevice));
                    ui->comboInferDevice->blockSignals(false);
                }
            }
            else if (keyval.first == "modelName" && keyval.second != "none")
            {
                config.modelName = keyval.second;
                if (ui != nullptr)
                {
                    // Client has almost certainly not connected to get a list of
                    // available infer devices yet.
                    ui->comboModel->blockSignals(true);
                    ui->comboModel->setCurrentText(QString::fromStdString(config.modelName));
                    ui->comboModel->blockSignals(false);
                }
            }
        }
    }
    else
    {
        qWarning("Unable to load saved settings. Attempting to set defaults...");
        config.clientID = GenerateLocalUID();
        config.inputDevice = "none";
        config.outputDevice = "none";

        if (ui != nullptr)
        {
            Save(ui);
        }
        else
        {
            Save(ui, true);
        }
    }
}

std::pair<std::string, std::string> LocalConfig::ParseLine(std::string &cfgLine)
{
    std::pair<std::string, std::string> kv = {"", ""};

    size_t commentPos = cfgLine.find('#');
    if (commentPos != std::string::npos)   // .find() returns npos if no match
    {
        cfgLine.erase(commentPos);
    }

    if (!cfgLine.empty())
    {
        // Remove leading, trailing whitespace
        removeLeading(cfgLine);
        removeTrailing(cfgLine);

        size_t eqPos = cfgLine.find('=');
        if (eqPos != std::string::npos)
        {
            // Looks like there is a something = something
            kv.first = cfgLine.substr(0, eqPos);
            removeTrailing(kv.first);
            kv.second = cfgLine.substr(eqPos + 1);
            removeLeading(kv.second);

            // Quick validation
            if (kv.first != "uid"
                && kv.first != "previousServers"
                && kv.first != "inputDevice"
                && kv.first != "outputDevice"
                && kv.first != "inferDevice"
                && kv.first != "modelName")
            {
                qWarning("UNKNOWN KEY (%s) IN CONFIG!", kv.first.c_str());
                return {"", ""};
            }
        }
    }

    return kv;
}

std::string LocalConfig::GetSavedInputDevice()
{
    return config.inputDevice;
}
std::string LocalConfig::GetSavedOutputDevice()
{
    return config.outputDevice;
}
std::string LocalConfig::GetSavedInferDevice()
{
    return config.inferDevice;
}
std::string LocalConfig::GetSavedModelName()
{
    return config.modelName;
}

void LocalConfig::SetSavedInputDevice(std::string name)
{
    config.inputDevice = name;
}
void LocalConfig::SetSavedOutputDevice(std::string name)
{
    config.outputDevice = name;
}
void LocalConfig::SetSavedInferDevice(std::string name)
{
    config.inferDevice = name;
}
void LocalConfig::SetSavedModelName(std::string name)
{
    config.modelName = name;
}

std::string LocalConfig::GetClientID()
{
    // This SHOULD already be set (as long as Load() was
    // called at program start) but... just in case it
    // failed, or something
    if (config.clientID == "")
    {
        std::string newid = GenerateLocalUID();
        config.clientID = newid;
        Save(nullptr, true);
    }

    return config.clientID;
}

std::string LocalConfig::GenerateLocalUID()
{
    srand(time(0));
    int r = rand();
    std::string salted;

    // Only works on linux systems
    char uname[LOGIN_NAME_MAX];
    if (getlogin_r(uname, LOGIN_NAME_MAX) == 0)
    {
        // Got a username
        salted = uname;
    }
    else
    {
        salted = std::to_string(r);
        r = rand();
    }
    /* Windows? Taken from google / untested
    char uname[UNLEN + 1];
    DWORD l = UNLEN + 1;
    if (GetUserNameA(uname, &l))
    {
        // Got a username
        salted = uname;
    }
    */
    salted = salted.append(std::to_string(r));

    std::size_t hash = std::hash<std::string>{}(salted);

    return std::to_string(hash);
}