#ifndef UWAVCLIENT_H
#define UWAVCLIENT_H

#include "sio/sio_socket.h"
#include "sio/sio_client.h"
#include "u_audioprovider.h"
#include "localconfig.h"
#include <QMainWindow>

//Q_DECLARE_METATYPE(std::string)
Q_DECLARE_METATYPE(sio::client::close_reason)
Q_DECLARE_METATYPE(sio::message::list)

QT_BEGIN_NAMESPACE
namespace Ui {
class UwaVClient;
}
QT_END_NAMESPACE

class UwaVClient : public QMainWindow
{
    Q_OBJECT
public:
    UwaVClient(QWidget *parent = nullptr);
    ~UwaVClient();

    void AppendMessage(QtMsgType, const QString &);

    void LoadSettings();
    void RegisterAudioProvider(u_AudioProvider *a);
    bool ValidateStartStopState();
    bool ValidateConnectString(QString &uri);

    void ui_ServerDisconnect();
    void AttemptConnectionTo(QString dest);

    sio::client *GetSioHandler();
    void signalSioEvent(sio::event &event);

public slots:
    void on_InputLevelChanged(float level);
    void on_OutputLevelChanged(float level);
    void onInputBufferWrite();

private slots:
    void onSioConnectFail();
    void onSioConnectRetry();
    void onSioConnectSuccess();
    void onSioConnectClose(sio::client::close_reason const &closeReason);
    void onSioSocketOpen(std::string const &nSpace);
    void onSioSocketClose(std::string const &nSpace);
    void onSioErrorMsg(sio::message::ptr const &message);
    void onSioMessage(std::string const &name, sio::message::list const &messages);

    void EmitSamples(int sampleRate, int numSamples);
    void onSamplesReceived(const sio::message::ptr &msg);
    void on_DeviceListsReady();
    void on_InputDeviceAdded(int index, std::string name);
    void on_OutputDeviceAdded(int index, std::string name);
    void on_InputDeviceRemoved(int index);
    void on_OutputDeviceRemoved(int index);
    void on_AudioStreamSuccess();
    void on_AudioStreamEnd();
    void on_AudioStreamConnecting();

    void on_buttonStart_clicked();
    void on_buttonStop_clicked();
    void on_buttonConnect_clicked();

    void on_comboAudioSrcLocal_currentIndexChanged(int index);
    void on_comboAudioSinkLocal_currentIndexChanged(int index);
    void on_comboServerSelect_currentTextChanged(const QString &arg1);
    void on_comboServerSelect_editTextChanged(const QString &arg1);
    void on_comboServerSelect_currentIndexChanged(int index);

    void on_sliderInputGain_sliderMoved(int position);
    void on_sliderOutputGain_sliderMoved(int position);
    void on_sliderPitchShift_sliderMoved(int position);
    void on_sliderChunkSize_sliderMoved(int position);
    void on_sliderExtraSize_sliderMoved(int position);

    void on_sliderInputGain_sliderPressed();
    void on_sliderOutputGain_sliderPressed();
    void on_sliderPitchShift_sliderPressed();
    void on_sliderChunkSize_sliderPressed();
    void on_sliderExtraSize_sliderPressed();

    void on_sliderInputGain_sliderReleased();
    void on_sliderOutputGain_sliderReleased();
    void on_sliderChunkSize_sliderReleased();
    void on_sliderExtraSize_sliderReleased();
    void on_sliderPitchShift_sliderReleased();

    void on_sliderCrossfadeSize_sliderMoved(int position);

    void on_sliderFormantShift_sliderMoved(int position);

    void on_sliderMatchRMS_sliderMoved(int position);

    void on_sliderCrossfadeSize_sliderPressed();

    void on_sliderFormantShift_sliderPressed();

    void on_sliderMatchRMS_sliderPressed();

    void on_sliderCrossfadeSize_sliderReleased();

    void on_sliderFormantShift_sliderReleased();

    void on_sliderMatchRMS_sliderReleased();

    void on_checkUseFP32_toggled(bool checked);

    void on_checkUseJIT_toggled(bool checked);

    void on_checkNoiseReduce_toggled(bool checked);

    void on_checkUseVocode_toggled(bool checked);

    void on_sliderGateRMS_sliderPressed();

    void on_sliderGateRMS_sliderMoved(int position);

    void on_sliderGateRMS_sliderReleased();

    void on_buttonServerExit_clicked();

    void on_comboInferDevice_currentIndexChanged(int index);

    void on_comboModel_currentIndexChanged(int index);

signals:
    void signalSioConnectFail();
    void signalSioConnectRetry();
    void signalSioConnectSuccess();
    void signalSioConnectClose(sio::client::close_reason const &closeReason);
    void signalSioSocketOpen(std::string const &nSpace);
    void signalSioSocketClose(std::string const &nSpace);
    void signalSioErrorMsg(sio::message::ptr const &message);
    void signalSioMessage(std::string const &name, sio::message::list const &messages);

private:
    Ui::UwaVClient *ui;
    QtMessageHandler msgHandler = nullptr;
    u_AudioProvider *audio = nullptr;
    sio::client sioClient;
    LocalConfig *savedConfig = nullptr;

    int primaryInputDeviceNum = -1;
    QString primaryInputDeviceDesc;
    QString primaryInputDeviceName;

    int primaryOutputDeviceNum = -1;
    QString primaryOutputDeviceDesc;
    QString primaryOutputDeviceName;

    bool connectRetryState = false;
    bool serverStarting = false;
    bool serverStarted = false;

    int chunkSize;
    int numInputSamplesPerChunk = 0;
    int numOutputSamplesPerChunk = 0;

    //float timeBetweenSliderUpdates = 0.4f;  // Seconds
    std::chrono::duration<double, std::ratio<1, 1>> timeBetweenSliderUpdates;
    std::chrono::steady_clock::time_point lastSliderUpdateTime;

    void ui_SetLayoutState(QLayout *layout, bool enable);
    void ui_LocalStop();
    void ui_RemoteStart();
    void ui_RemoteStop();
    void ui_ServerConnect();

    void EmitInteger(const std::string &msg, const std::string &key, int val);
    void EmitDouble(const std::string &msg, const std::string &key, double val);
    void EmitBoolean(const std::string &msg, const std::string &key, bool val);
};
#endif // UWAVCLIENT_H
