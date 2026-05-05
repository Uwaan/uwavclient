#include <chrono>
#include <QDebug>
#include "uwavclient.h"
#include "./ui_uwavclient.h"
#include "u_sharedfunctions.h"    // For removeLeading and removeTrailing functions
#include "localconfig.h"
#include "sio/sio_socket.h"
#include "sio/sio_client.h"

using namespace std::chrono_literals;

UwaVClient::UwaVClient(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::UwaVClient)
{
    ui->setupUi(this);
    ui->textDebug->clear();

    savedConfig = new LocalConfig();

    // Set some defaults with our sio object
    sioClient.set_reconnect_attempts(3);
    sioClient.set_logs_default();   // available: set_logs_quiet(), set_logs_verbose()

    // Signals
    qRegisterMetaType<std::string>();
    qRegisterMetaType<sio::client::close_reason>();
    qRegisterMetaType<sio::message::list>();

    QObject::connect(this, &UwaVClient::signalSioConnectFail, this, &UwaVClient::onSioConnectFail, Qt::QueuedConnection);
    QObject::connect(this, &UwaVClient::signalSioConnectRetry, this, &UwaVClient::onSioConnectRetry, Qt::QueuedConnection);
    QObject::connect(this, &UwaVClient::signalSioConnectSuccess, this, &UwaVClient::onSioConnectSuccess, Qt::QueuedConnection);
    QObject::connect(this, &UwaVClient::signalSioConnectClose, this, &UwaVClient::onSioConnectClose, Qt::QueuedConnection);
    QObject::connect(this, &UwaVClient::signalSioSocketOpen, this, &UwaVClient::onSioSocketOpen, Qt::QueuedConnection);
    QObject::connect(this, &UwaVClient::signalSioSocketClose, this, &UwaVClient::onSioSocketClose, Qt::QueuedConnection);
    QObject::connect(this, &UwaVClient::signalSioErrorMsg, this, &UwaVClient::onSioErrorMsg, Qt::QueuedConnection);
    QObject::connect(this, &UwaVClient::signalSioMessage, this, &UwaVClient::onSioMessage, Qt::QueuedConnection);

    timeBetweenSliderUpdates = std::chrono::duration(0.4s);
    lastSliderUpdateTime = std::chrono::steady_clock::now();
}

UwaVClient::~UwaVClient()
{
    delete savedConfig;
    delete ui;
}

sio::client *UwaVClient::GetSioHandler() { return &sioClient; }

// Helper rq here
std::string sioToString(const sio::message::ptr &msg)
{
    sio::message::flag flag;
    flag = msg->get_flag();

    if (flag == sio::message::flag_array)
    {
        return "(array)";
    }
    else if (flag == sio::message::flag_binary)
    {
        return "(binary data)";
    }
    else if (flag == sio::message::flag_boolean)
    {
        return (msg->get_bool() ? "true" : "false");
    }
    else if (flag == sio::message::flag_double)
    {
        return std::to_string(msg->get_double());
    }
    else if (flag == sio::message::flag_integer)
    {
        return std::to_string(msg->get_int());
    }
    else if (flag == sio::message::flag_null)
    {
        return "null";
    }
    else if (flag == sio::message::flag_object)
    {
        return "(Object)";
    }
    else if (flag == sio::message::flag_string)
    {
        return msg->get_string();
    }
    return "(Not found)";
}

/* Appends a message to the UI-facing text feedback box */
void UwaVClient::AppendMessage(QtMsgType messageType, const QString &msg)
{
    // Any special actions that need to be taken based on message type
    if (messageType == QtCriticalMsg)
    {

    }
    else if (messageType == QtFatalMsg)
    {
        // I think the documentation tells us to do a sys.exit("abort")
        // on receipt of this one
    }
    else if (messageType == QtWarningMsg)
    {

    }
    else if (messageType == QtInfoMsg)
    {

    }
    else if (messageType == QtDebugMsg)
    {

    }

    if (msg.left(7) != "Wayland")       // Shut up about wayland
    {
        ui->textDebug->append(msg);
        ui->textDebug->moveCursor(QTextCursor::End);

        // Fallback method
        //ui->textDebug->verticalScrollBar()->setValue(ui->textDebug->verticalScrollBar()->maximum());
    }
}

/* Tells us which u_AudioProvider we are using, and sets up
 * callbacks on that object. */
void UwaVClient::RegisterAudioProvider(u_AudioProvider *a)
{
    if (audio != nullptr)
    {
        qCritical("CRITICAL: Attempted to register a new audio provider. This is unsupported; ignoring.");
        return;
    }
    audio = a;

    // Subscribe to audio processing signals
    QObject::connect(audio->signaler, &u_EngineStatistics::currentInputRMSChangedTo, this, &UwaVClient::on_InputLevelChanged, Qt::QueuedConnection);
    QObject::connect(audio->signaler, &u_EngineStatistics::currentOutputRMSChangedTo, this, &UwaVClient::on_OutputLevelChanged, Qt::QueuedConnection);
    QObject::connect(audio->signaler, &u_EngineStatistics::deviceListsReady, this, &UwaVClient::on_DeviceListsReady, Qt::QueuedConnection);
    QObject::connect(audio->signaler, &u_EngineStatistics::inputDeviceAdded, this, &UwaVClient::on_InputDeviceAdded, Qt::QueuedConnection);
    QObject::connect(audio->signaler, &u_EngineStatistics::outputDeviceAdded, this, &UwaVClient::on_OutputDeviceAdded, Qt::QueuedConnection);
    QObject::connect(audio->signaler, &u_EngineStatistics::inputDeviceRemoved, this, &UwaVClient::on_InputDeviceRemoved, Qt::QueuedConnection);
    QObject::connect(audio->signaler, &u_EngineStatistics::outputDeviceRemoved, this, &UwaVClient::on_OutputDeviceRemoved, Qt::QueuedConnection);
    QObject::connect(audio->signaler, &u_EngineStatistics::streamStarted, this, &UwaVClient::on_AudioStreamSuccess, Qt::QueuedConnection);
    QObject::connect(audio->signaler, &u_EngineStatistics::streamStopped, this, &UwaVClient::on_AudioStreamEnd, Qt::QueuedConnection);
    QObject::connect(audio->signaler, &u_EngineStatistics::streamConnecting, this, &UwaVClient::on_AudioStreamConnecting, Qt::QueuedConnection);

    // Callback for when audio is being processed
    QObject::connect(
        audio->signaler, &u_EngineStatistics::signalInputBufferWrite,
        this, &UwaVClient::onInputBufferWrite,
        Qt::QueuedConnection);

    // Always perform initialization at startup
    audio->Init();
}

void UwaVClient::AttemptConnectionTo(QString dest)
{
    if (ui->comboAudioSrcLocal->currentIndex() == -1 ||
        ui->comboAudioSinkLocal->currentIndex() == -1)
    {
        qInfo("Please select valid input/output devices before connecting!");
        return;
    }

    // Update UI State
    ui->comboServerSelect->setEnabled(false);
    ui->buttonConnect->setEnabled(false);
    ui->labelConnectState->setText("Connecting..");

    if (ValidateConnectString(dest))
    {
        qInfo() << "Attempting connection to " << dest;
        sioClient.connect(dest.toStdString());
    }
    else
    {
        qWarning() << "Unable to initiate connection (invalid URI).";
        ui->comboServerSelect->setEnabled(true);
        ui->buttonConnect->setEnabled(true);
        ui->labelConnectState->setText("Disconnected");
    }
}

// Makes minor corrections needed by sio (http, port number)
// and returns true if url is valid, or false if malformed
bool UwaVClient::ValidateConnectString(QString &uri)
{
    while (uri.endsWith("/"))
    {
        uri.chop(1);
    }

    // TODO: Temp - until we get SSL transport working
    /*
    if (uri.startsWith("https:"))
    {
        uri.replace(0, 5, "http");
    }*/

    // Just discovered this:
    QUrl validateUrl(uri, QUrl::ParsingMode::TolerantMode);

    // Attempt some (minor) corrections
    if (validateUrl.scheme().isEmpty())
    {
        validateUrl.setScheme("http");
    }
    /*if (validateUrl.scheme() != "http")
    {
        // Filter unknown schemas (file://, ftp://, etc)
        validateUrl.setScheme("http");
    }*/
    if (validateUrl.port() == -1)
    {
        // Add default port number, if a port is not specified
        validateUrl.setPort(validateUrl.scheme() == "https" ? 443 : 80);
    }

    if (validateUrl.isValid())
    {
        uri = validateUrl.toString(QUrl::RemoveFragment | QUrl::RemoveQuery);
        return true;
    }
    return false;
}

// Check that selected input/output are valid, set button
// states
bool UwaVClient::ValidateStartStopState()
{
    // This function is called to check whether we are safe
    // to start conversion. Check for valid input/output endpoints.

    int in = ui->comboAudioSrcLocal->currentIndex();
    int out = ui->comboAudioSinkLocal->currentIndex();
    bool validInput = false;
    bool validOutput = false;

    if (audio)
    {
        // Quick sanity checks for now to get it running
        if (in == audio->GetActiveInput())
        {
            validInput = true;
        }
        else
        {
            qWarning("WARNING: Invalid input ID! (mismatch)");
        }

        if (out == audio->GetActiveOutput())
        {
            validOutput = true;
        }
        else
        {
            qWarning("WARNING: Invalid output ID! (mismatch)");
        }
    }

    // TODO: Run any other necessary checks that should occur
    // before starting conversion

    if (validInput && validOutput) return true;

    qWarning("WARN: Cannot start conversion; input/output validation failed.");
    return false;
}

/* Callback for audio system to notify main thread that the audio
 * source buffer has been written to. */
void UwaVClient::onInputBufferWrite()
{
    // Received notification that new data from the local microphone
    // is in the buffer.

    // This is triggered via queued signal so it is thread safe
    //if (audio->inBuffer()->getWriteAheadLength() > 0)
    //{
    //    EmitSamples(audio->inBuffer()->sampleRate, audio->inBuffer()->getWriteAheadLength());
    //}

    // It is more network traffic/overhead to send chunk-aligned
    // messages, but the minimum (20ms) @ 48KHz results in 960
    // samples which is 3.84kb of payload, 50 times a second.

    // During test runs, pipewire seems to like triggering its callback
    // 1024 samples at a time, so there is jitter, but it is very
    // predictable

    // Maximum chunk (500ms) @ 48 KHz is 96kb, 2 times a second

    if (serverStarted && numInputSamplesPerChunk != 0)
    {
        while(audio->inBuffer()->getWriteAheadLength() >= numInputSamplesPerChunk)
        {
            EmitSamples(audio->inBuffer()->sampleRate, numInputSamplesPerChunk);
        }
    }
    else
    {
        // It can take quite a while for the server to "warm up" when
        // conversion starts - while waiting for it to confirm that
        // it is ready, discard any input
        while(audio->inBuffer()->getWriteAheadLength() > numInputSamplesPerChunk)
        {
            audio->inBuffer()->discardSamples(numInputSamplesPerChunk);
        }

    }
}

/***********************************************************************************
 *                   UI HELPER FUNCTIONS
 **********************************************************************************/

// Recursive helper to enable/disable all items inside a particular
// layout
void UwaVClient::ui_SetLayoutState(QLayout *layout, bool enable)
{
    for (int i = 0; i < layout->count(); i++)
    {
        QLayoutItem *nextItem = layout->itemAt(i);

        // Need to access the "widget" in order to reach
        // an object that is enable/disable-able
        if (QWidget *w = nextItem->widget())
        {
            // Labels, sliders, combos, checkboxes
            if (qobject_cast<QLabel *>(w) ||
                qobject_cast<QSlider *>(w) ||
                qobject_cast<QComboBox *>(w) ||
                qobject_cast<QPushButton *>(w) ||
                qobject_cast<QCheckBox *>(w))
            {
                w->setEnabled(enable);
            }
        }
        else if (QLayout *nextLayout = nextItem->layout())
        {
            // This child is another layout; recurse
            // to get all of its elements as well
            ui_SetLayoutState(nextLayout, enable);
        }
    }
}

void UwaVClient::ui_LocalStop()
{
    ui->labelConnectState->setText("Stopping...");
    ui->buttonStop->setEnabled(false);
    ui->buttonStart->setEnabled(false);
}

void UwaVClient::ui_RemoteStart()
{
    ui->buttonStop->setEnabled(false);
    ui->buttonStart->setEnabled(false);

    ui->labelConnectState->setText("Streaming");

    ui->comboInferDevice->setEnabled(false);
    ui->comboModel->setEnabled(false);
    ui->buttonStop->setEnabled(true);
}

void UwaVClient::ui_RemoteStop()
{
    ui->buttonStop->setEnabled(false);
    ui->buttonStart->setEnabled(false);

    ui->labelConnectState->setText("Connected");
    ui->labelInferTime->setText("--");
    ui->labelInLevel->setText("--");
    ui->progressBarInLevel->setValue(0);
    ui->labelOutLevel->setText("--");
    ui->progressBarOutLevel->setValue(0);

    ui->comboInferDevice->setEnabled(true);
    ui->comboModel->setEnabled(true);
    ui->buttonStart->setEnabled(true);
}

void UwaVClient::ui_ServerConnect()
{
    ui->labelConnectState->setEnabled(true);
    ui->labelConnectState->setText("Connected");
    ui->buttonConnect->setText("Disconnect");
    ui->buttonConnect->setEnabled(true);

    ui_SetLayoutState(ui->serverMisc, true);
    ui_SetLayoutState(ui->modelParamSliders, true);
    ui_SetLayoutState(ui->audioStats, true);
    ui->volumeControls->setEnabled(true);
    ui->groupServer->setEnabled(true);

    ui->buttonStart->setEnabled(true);
    ui->buttonStop->setEnabled(false);
}

void UwaVClient::ui_ServerDisconnect()
{
    // Disable controls

    ui->buttonStart->setEnabled(false);
    ui->buttonStop->setEnabled(false);
    ui->buttonConnect->setEnabled(false);
    ui->comboServerSelect->setEnabled(false);

    ui_SetLayoutState(ui->serverMisc, false);
    ui_SetLayoutState(ui->modelParamSliders, false);
    ui_SetLayoutState(ui->audioStats, false);
    ui->volumeControls->setEnabled(false);
    ui->groupServer->setEnabled(false);

    // Some code paths will come back here with network/
    // audio not cleaned up yet (i.e., unexpected server
    // exit). Do cleanup just in case.

    // sio sync_close might not return immediately, so
    // run this only after disabling user input.
    if (sioClient.opened())
    {
        sioClient.socket()->off_all();
        sioClient.socket()->off_error();
        sioClient.sync_close();
    }

    // StopAudio will automatically check and do nothing
    // if audio is already stopped.
    if (audio->inStreamConnected || audio->outStreamConnected)
    {
        serverStarting = false;
        serverStarted = false;
        numInputSamplesPerChunk = 0;
        numOutputSamplesPerChunk = 0;
        qInfo ("Forcing stream disconnect: UI noticed it running on apparent disconnect");    // Tracing crash source
        audio->StopAudio();

    }

    // Clear user readouts in case they didn't
    // tie up cleanly
    ui->labelInLevel->setText("--");
    ui->progressBarInLevel->setValue(0);
    ui->labelOutLevel->setText("--");
    ui->progressBarOutLevel->setValue(0);

    ui->labelConnectState->setText("Disconnected");
    ui->labelConnectState->setEnabled(false);
    ui->labelHostname->setText("::Host::");
    ui->labelPing->setText("--");
    ui->labelInferTime->setText("--");
    ui->labelGpuUsage->setText("--");
    ui->labelGpuMemUsed->setText("--");
    ui->labelGpuMemTotal->setText("--");

    ui->buttonConnect->setText("Connect");
    ui->buttonConnect->setEnabled(true);
    ui->comboServerSelect->setEnabled(true);

    // TODO: recall of last connected servers, if applicable
}

/***********************************************************************************
 *                   SIO EVENTS
 ***********************************************************************************
 *
 * Note: These will be called by different threads. Can't do
 * thread-unsafe things here, probably need to marshal anything
 * that is doing more than queuing a signal.
 *
 *********************************************************************************/

void UwaVClient::onSioConnectFail()
{
    qWarning("Unable to connect to server.");
    if (connectRetryState)
    {
        ui->buttonConnect->setText("Connect");
        connectRetryState = false;
    }
    ui_ServerDisconnect();
}

void UwaVClient::onSioConnectRetry()
{
    qWarning("Retrying connection...");

    if (!ui->buttonConnect->isEnabled())
    {
        connectRetryState = true;
        ui->buttonConnect->setText("Cancel");
        ui->buttonConnect->setEnabled(true);
    }
}

void UwaVClient::onSioConnectSuccess()
{
    qInfo("Connection to server successful!");
    // TODO: If client is in "retry" state, and user clicks "disconnect"
    // but then server is started up before onSioConnectFail() is triggered,
    // it puts the client in a weird state where it will continue
    // retrying after disconnect.
    connectRetryState = false;
    // Holding off on this until we request and receive params
    //ui_ServerConnect();
}

void UwaVClient::onSioConnectClose(sio::client::close_reason const &closeReason)
{
    if (closeReason == sio::client::close_reason_normal)
    {
        qInfo("Connection to server closed successfully.");
    }
    else if (closeReason == sio::client::close_reason_drop)
    {
        qWarning("Connection to server dropped unexpectedly.");
    }

    if (audio->inStreamConnected || audio->outStreamConnected)
    {
        serverStarting = false;
        serverStarted = false;
        numInputSamplesPerChunk = 0;
        numOutputSamplesPerChunk = 0;
        audio->StopAudio();
        ui_RemoteStop();
    }

    ui_ServerDisconnect();
}

void UwaVClient::onSioSocketOpen(std::string const &nSpace)
{
    // "nSpace" is the websocket namespace. We're just using '/'
    //qInfo("Socket %s open.", nSpace.c_str());

    // Register callbacks
    sioClient.socket()->on_error(std::bind(&UwaVClient::signalSioErrorMsg, this, std::placeholders::_1));
    sioClient.socket()->on_any(std::bind(&UwaVClient::signalSioEvent, this, std::placeholders::_1));

    // First message should be sending our client's unique ID
    // so that the server knows which settings to load
    sio::message::ptr m = sio::object_message::create();
    m->get_map()["client_id"] = sio::string_message::create(savedConfig->GetClientID());
    sioClient.socket()->Emit("GET_PARAMS", sio::message::list(m));

    // Probably don't need to sleep, as messages should be
    // processed in order (and none of these rely on our
    // specific uid) but it feels like good practice to
    // give the server a second to respond to us.
    sleep(1);

    sioClient.socket()->Emit("GET_MODELS");
    sioClient.socket()->Emit("GET_DEVICES");
    sioClient.socket()->Emit("GET_STATUS");

    // Ping w timestamp, in milliseconds since system epoch
    // (NOT clock time - only measuring delta later)
    std::chrono::time_point ts = std::chrono::steady_clock::now();
    std::chrono::duration dur_ms = std::chrono::duration<double, std::milli>(ts.time_since_epoch());
    double dTime = dur_ms.count();

    EmitDouble("PING", "timestamp", dTime);
}

void UwaVClient::onSioSocketClose(std::string const &nSpace)
{
    //qInfo("Socket %s closing.", nSpace.c_str());
    // Unregister callbacks
    sioClient.socket()->off_all();
    sioClient.socket()->off_error();
}

void UwaVClient::onSioErrorMsg(sio::message::ptr const &message)
{
    if (message->get_flag() == sio::message::flag_string)
    {
        qWarning("SIO ERROR: %s", message->get_string().c_str());
    }
}

/* This is NOT where events are processed, that happens in
 * onSioMessage.
 * This function gets called *by the websocket thread* and
 * just needs to unpack the sio::event and generate a valid
 * signal that can be processed by the main thread later. */
void UwaVClient::signalSioEvent(sio::event &event)
{
    if (event.need_ack())
    {
        sio::message::list ml("PONG");

        event.put_ack_message(ml);
    }

    // TODO: How to check this for an empty message/fail state?
    sio::message::list l = event.get_messages();

    signalSioMessage(event.get_name(), l);
}

void UwaVClient::onSioMessage(std::string const &name, sio::message::list const &messages)
{
    int i = 0;

    //qInfo() << name.c_str() << ": (" << messages.size() << " items)";
    if (name != "AUDIO_OUT" && name != "SET_STATUS")
    {
        qInfo() << name.c_str() << ": (" << messages.size() << " items)";
    }

    if (name == "AUDIO_OUT")
    {
        // Received audio data to be output
        // Set this up as a signal earlier, but we SHOULD be on
        // the main thread right now, so just run it

        if(messages.at(0)->get_flag() != sio::message::flag_object)
        {
            qCritical("Unexpected input from AUDIO_OUT event.");
            return;
        }

        // ...This is a complicated payload.
        onSamplesReceived(messages.at(0));
    }
    else if (name == "SET_STATUS")
    {
        if (messages.at(0)->get_flag() != sio::message::flag_object)
        {
            // Invalid message?
            qCritical("Unexpected input from SET_STATUS event.");
            return;
        }

        std::map keyvals = messages.at(0)->get_map();
        for (const std::pair<std::string, sio::message::ptr> &kv : keyvals)
        {
            //qInfo() << kv.first.c_str() << ": " << sioToString(kv.second).c_str();
            /* cpu_percent :  double
             * gpu_load :  int
             * gpu_mem_total_mib :  int
             * gpu_mem_used_mib :  int
             * mem_total_mib :  int
             * mem_used_mib :  int
             * server_running :  bool
             */

            if (kv.first == "gpu_load")
            {
                ui->labelGpuUsage->setText(sioToString(kv.second).append("%").c_str());
            }
            else if (kv.first == "gpu_mem_used_mib")
            {
                ui->labelGpuMemUsed->setText(sioToString(kv.second).append(" MB /").c_str());
            }
            else if (kv.first == "gpu_mem_total_mib")
            {
                ui->labelGpuMemTotal->setText(sioToString(kv.second).append(" MB").c_str());
            }
            else if (kv.first == "host_name")
            {
                ui->labelHostname->setText(sioToString(kv.second).c_str());
            }

        }
    }
    else if (name == "SET_PARAMS")
    {
        if (messages.at(0)->get_flag() != sio::message::flag_object)
        {
            // Invalid message?
            qCritical("Unexpected input from SET_PARAMS event.");
            return;
        }

        qInfo("Received stored conversion parameters from server.");

        std::map keyvals = messages.at(0)->get_map();
        for (const std::pair<std::string, sio::message::ptr> &kv : keyvals)
        {
            /*
            chunk_size_s :  double
            chunk_pad_s :  double
            crossfade_s : double
            use_output_rms : double
            pitch_shift_semitones : int
            formant_shift : int
            input_gate_db : int
            use_noise_reduction : bool
            use_vocode_smoothing: bool
            use_fp32 : bool
            use_jit : bool
            inference_device :  string
            input_device :  string
            input_volume :  double
            output_device :  string
            output_volume :  double
            model_name :  string
            */
            if (kv.first == "model_name")
            {
                for(i = 0; i < ui->comboModel->count(); i++)
                {
                    if (QString::fromStdString(sioToString(kv.second)) == ui->comboModel->itemText(i))
                    {
                        ui->comboModel->setCurrentIndex(i);
                    }
                }
            }
            else if (kv.first == "input_device")
            {
                // Will be "client" or name of server input device
                std::string device = kv.second->get_string();

                if (device == "client")
                {
                    // TODO: Check whether client already has server device selected
                    // System default if so
                }
                else
                {
                    // TODO: Try to set combo box to correct position
                }
            }
            else if (kv.first == "output_device")
            {
                // Will be "client" or name of server output device
                std::string device = kv.second->get_string();

                if (device == "client")
                {
                    // TODO: Check whether client already has server device selected
                    // System default if so
                }
                else
                {
                    // TODO: Try to set combo box to correct position
                }
            }
            else if (kv.first == "input_volume")
            {
                lastSliderUpdateTime = std::chrono::steady_clock::now();
                ui->sliderInputGain->setValue(static_cast<int>(round(kv.second->get_double() * 100)));
                on_sliderInputGain_sliderMoved(ui->sliderInputGain->value());
            }
            else if (kv.first == "output_volume")
            {
                lastSliderUpdateTime = std::chrono::steady_clock::now();
                ui->sliderOutputGain->setValue(static_cast<int>(round(kv.second->get_double() * 100)));
                on_sliderOutputGain_sliderMoved(ui->sliderOutputGain->value());
            }
            else if (kv.first == "chunk_size_s")
            {
                // Set time - we want slider events to trigger, but not
                // send an update to the server
                lastSliderUpdateTime = std::chrono::steady_clock::now();
                ui->sliderChunkSize->setValue(static_cast<int>(round(kv.second->get_double() * 1000)));
                on_sliderChunkSize_sliderMoved(ui->sliderChunkSize->value());
            }
            else if (kv.first == "chunk_pad_s")
            {
                // Set time - we want slider events to trigger, but not
                // send an update to the server
                lastSliderUpdateTime = std::chrono::steady_clock::now();
                ui->sliderExtraSize->setValue(static_cast<int>((round(kv.second->get_double() * 1000))));
                on_sliderExtraSize_sliderMoved(ui->sliderExtraSize->value());
            }
            else if (kv.first == "crossfade_s")
            {
                // Set time - we want slider events to trigger, but not
                // send an update to the server
                lastSliderUpdateTime = std::chrono::steady_clock::now();
                ui->sliderCrossfadeSize->setValue(static_cast<int>((round(kv.second->get_double() * 1000))));
                on_sliderCrossfadeSize_sliderMoved(ui->sliderCrossfadeSize->value());
            }
            else if (kv.first == "pitch_shift_semitones")
            {
                // Set time - we want slider events to trigger, but not
                // send an update to the server
                lastSliderUpdateTime = std::chrono::steady_clock::now();
                ui->sliderPitchShift->setValue(kv.second->get_int());
                on_sliderPitchShift_sliderMoved(ui->sliderPitchShift->value());
            }
            else if (kv.first == "formant_shift")
            {
                // Set time - we want slider events to trigger, but not
                // send an update to the server
                lastSliderUpdateTime = std::chrono::steady_clock::now();
                ui->sliderFormantShift->setValue(kv.second->get_int());
                on_sliderFormantShift_sliderMoved(ui->sliderFormantShift->value());
            }
            else if (kv.first == "use_output_rms")
            {
                // Set time - we want slider events to trigger, but not
                // send an update to the server
                lastSliderUpdateTime = std::chrono::steady_clock::now();
                ui->sliderMatchRMS->setValue(static_cast<int>((1 - kv.second->get_double()) * 100));
                on_sliderMatchRMS_sliderMoved(ui->sliderMatchRMS->value());
            }
            else if (kv.first == "input_gate_db")
            {
                // Set time - we want slider events to trigger, but not
                // send an update to the server
                lastSliderUpdateTime = std::chrono::steady_clock::now();
                ui->sliderGateRMS->setValue(kv.second->get_int());
                on_sliderGateRMS_sliderMoved(ui->sliderGateRMS->value());
            }
            else if (kv.first == "use_noise_reduction")
            {
                ui->checkNoiseReduce->blockSignals(true);
                ui->checkNoiseReduce->setChecked(kv.second->get_bool());
                ui->checkNoiseReduce->blockSignals(false);
            }
            else if (kv.first == "use_vocode_smoothing")
            {
                ui->checkUseVocode->blockSignals(true);
                ui->checkUseVocode->setChecked(kv.second->get_bool());
                ui->checkUseVocode->blockSignals(false);
            }
            else if (kv.first == "use_fp32")
            {
                ui->checkUseFP32->blockSignals(true);
                ui->checkUseFP32->setChecked(kv.second->get_bool());
                ui->checkUseFP32->blockSignals(false);
            }
            else if (kv.first == "use_jit")
            {
                ui->checkUseJIT->blockSignals(true);
                ui->checkUseJIT->setChecked(kv.second->get_bool());
                ui->checkUseJIT->blockSignals(false);
            }

        }

        // NOW we can unlock the ui to mess with sliders and stuff
        ui_ServerConnect();

        // [key, val] list of currently stored conversion parameters
        // can be recalled as std::map
        /*
        std::map keyvals = messages.at(0)->get_map();
        for (const std::pair<std::string, sio::message::ptr> &kv : keyvals)
        {
            qInfo() << kv.first.c_str() << ": " << sioToString(kv.second).c_str();
        }*/
    }
    else if (name == "SERVER_STARTED")
    {
        /* "input_sample_rate": _settings["input_sr"],
         * "output_sample_rate": _settings["output_sr"],
         * "received_timestamp": (data or {}).get("timestamp"),
         * "server_timestamp": time.time(),
        */

        // TODO: Check input/output sample rate against internal values

        if(messages.at(0)->get_flag() != sio::message::flag_object)
        {
            qCritical("Unexpected input from SERVER_STARTED event.");
            return;
        }

        std::map keyvals = messages.at(0)->get_map();
        for (const std::pair<std::string, sio::message::ptr> &kv : keyvals)
        {
            //qInfo() << kv.first.c_str() << ": " << sioToString(kv.second).c_str();

            if (kv.first == "input_sample_rate")
            {
                qInfo("Input sample rate is %li", kv.second->get_int());
            }
            else if (kv.first == "output_sample_rate")
            {
                qInfo("Output sample rate is %li", kv.second->get_int());
            }
            else if (kv.first == "received_timestamp")
            {
                std::chrono::time_point ts = std::chrono::steady_clock::now();
                std::chrono::duration dur_ms = std::chrono::duration<double, std::milli>(ts.time_since_epoch());
                double newTime = dur_ms.count();
                double oldTime = 0;
                double delta = 0;

                // This presently crashes (assert) on get_double()
                qInfo() << "Got timestamp: " << sioToString(kv.second).c_str();
                /*
                oldTime = kv.second->get_double();
                // Compare timestamps to get round-trip latency
                if (oldTime != 0)
                {
                    delta = newTime - oldTime;
                    //qInfo("Ping event - %f - %f = %f", oldTime, newTime, delta);
                    int ms = round(delta);
                    std::string s = std::to_string(ms) + " ms";
                    //ui->labelPing->setText(s.c_str());
                    qInfo() << "Server started in " << s.c_str();
                }
                else
                {
                    ui->labelPing->setText("--");
                }*/
            }
        }
        serverStarting = false;
        serverStarted = true;

        // Set UI state
        ui_RemoteStart();
        ui->buttonStart->setEnabled(false);
        ui->buttonStop->setEnabled(true);

        // After a successful start, save our selected I/O devices
        // and server list for future recall
        savedConfig->Save(ui);
    }
    else if (name == "SERVER_STOPPED")
    {
        serverStarting = false;
        serverStarted = false;
        numInputSamplesPerChunk = 0;
        numOutputSamplesPerChunk = 0;
        audio->StopAudio();
        ui_RemoteStop();
        //ui_LocalStop();
    }
    else if (name == "SERVER_EXITING")
    {
        if (audio->inStreamConnected || audio->outStreamConnected)
        {
            serverStarting = false;
            serverStarted = false;
            numInputSamplesPerChunk = 0;
            numOutputSamplesPerChunk = 0;
            audio->StopAudio();
        }
        ui_RemoteStop();
        ui_ServerDisconnect();
    }
    else if (name == "LIST_MODELS")
    {
        if(messages.at(0)->get_flag() != sio::message::flag_object)
        {
            qCritical("Unexpected input from LIST_MODELS event.");
            return;
        }

        std::map keyvals = messages.at(0)->get_map();
        std::vector v = keyvals["models"]->get_vector();
        QString prevSelection = ui->comboModel->currentText();
        QString newItem = "";
        ui->comboModel->clear();
        for (sio::message::ptr msg : v)
        {
            // Parse list of models provided
            // TODO: check that inference is not running, etc -
            // dont want to change this while it's running
            newItem = QString::fromStdString(sioToString(msg));
            ui->comboModel->addItem(newItem);
            if (newItem == prevSelection)
            {
                ui->comboModel->setCurrentIndex(ui->comboModel->count()-1);
            }
        }
    }
    else if (name == "LIST_DEVICES")
    {
        if(messages.at(0)->get_flag() != sio::message::flag_object)
        {
            qCritical("Unexpected input from LIST_DEVICES event.");
            return;
        }

        std::map keyvals = messages.at(0)->get_map();
        std::vector v = keyvals["input"]->get_vector();
        for (sio::message::ptr msg : v)
        {
            if (msg->get_flag() == sio::message::flag_string)
            {
                qInfo() << "Server reported input device: " << sioToString(msg).c_str();
            }
        }
        v = keyvals["output"]->get_vector();
        for (sio::message::ptr msg : v)
        {
            if (msg->get_flag() == sio::message::flag_string)
            {
                qInfo() << "Server reported output device: " << sioToString(msg).c_str();
            }
        }
        /*
        for (const std::pair<std::string, sio::message::ptr> &kv : keyvals)
        {
            qInfo() << kv.first.c_str() << ": " << sioToString(kv.second).c_str();
        }*/
    }
    else if (name == "MODEL_LOAD_SUCCESS")
    {

    }
    else if (name == "MODEL_LOAD_FAIL")
    {

    }
    else if (name == "STREAM_OVERLOAD")
    {

    }
    else if (name == "MESSAGE")
    {
        if (messages.at(0)->get_flag() != sio::message::flag_object)
        {
            qCritical("Unexpected input from MESSAGE event.");
            return;
        }
        /* level: int (0=info, 1=warn, 2=error)
         * message: string
        */
        std::map keyvals = messages.at(0)->get_map();
        if (keyvals["message"] != NULL && keyvals["message"]->get_flag() != sio::message::flag_string)
        {
            qWarning("WARNING: Received message from server that is not a string!");
            return;
        }
        if (keyvals["level"] != NULL && keyvals["level"]->get_flag() != sio::message::flag_integer)
        {
            qWarning("WARNING: Received message from server without priority level!");
        }

        //std::vector v = keyvals["message"]->get_vector();

        std::string s = keyvals["message"]->get_string();
        qInfo("[SERVER] %s", s.c_str());
        //qInfo("MESSAGE RECEIVED!! Todo: repeat whatever you just did and figure out how to unwrap it");

    }
    else if (name == "PONG")
    {
        if(messages.at(0)->get_flag() != sio::message::flag_object)
        {
            qCritical("Unexpected input from PONG event.");
            return;
        }
        /*
         * received_timestamp :  double
         * server_timestamp :  float (as double)
         * */

        // (NOT system clock time - only measuring delta)
        std::chrono::time_point ts = std::chrono::steady_clock::now();
        std::chrono::duration dur_ms = std::chrono::duration<double, std::milli>(ts.time_since_epoch());
        double newTime = dur_ms.count();
        double oldTime = 0;
        double delta = 0;
        std::map keyvals = messages.at(0)->get_map();

        for (const std::pair<std::string, sio::message::ptr> &kv : keyvals)
        {
            if (kv.first == "received_timestamp")
            {
                oldTime = kv.second->get_double();
            }
        }

        // Compare timestamps to get round-trip latency
        if (oldTime != 0)
        {
            delta = newTime - oldTime;
            //qInfo("Ping event - %f - %f = %f", oldTime, newTime, delta);
            int ms = round(delta);
            std::string s = std::to_string(ms) + " ms";
            ui->labelPing->setText(s.c_str());
        }
        else
        {
            ui->labelPing->setText("--");
        }
    }
    else
    {
        qCritical("Received an odd duck.");
    }
}

void UwaVClient::EmitInteger(const std::string &msg, const std::string &key, int val)
{
    if (sioClient.opened())
    {
        sio::message::ptr m = sio::object_message::create();
        m->get_map()[key] = sio::int_message::create(val);
        sioClient.socket()->Emit(msg, sio::message::list(m));
    }
    else
    {
        qCritical("Cannot send message (%s) without open socket!", msg.c_str());
    }
}

void UwaVClient::EmitDouble(const std::string &msg, const std::string &key, double val)
{
    if (sioClient.opened())
    {
        sio::message::ptr m = sio::object_message::create();
        m->get_map()[key] = sio::double_message::create(val);
        sioClient.socket()->Emit(msg, sio::message::list(m));
    }
    else
    {
        qCritical("Cannot send message (%s) without open socket!", msg.c_str());
    }
}

void UwaVClient::EmitBoolean(const std::string &msg, const std::string &key, bool val)
{
    if (sioClient.opened())
    {
        sio::message::ptr m = sio::object_message::create();
        m->get_map()[key] = sio::bool_message::create(val);
        sioClient.socket()->Emit(msg, sio::message::list(m));
    }
    else
    {
        qCritical("Cannot send message (%s) without open socket!", msg.c_str());
    }
}

/* AUDIO SUBSYSTEM EVENTS */
void UwaVClient::EmitSamples(int sampleRate, int numSamples)
{
    // All right here's the main event. Attempt to read the
    // buffer yourself from the main thread.

    //audio->tempBuffer->readAudioData(chunkBuffer, numSamplesPerChunk);
    //std::shared_ptr<std::string> = std::make_shared<std::string>(
    //    reinterpret_cast<const char*>(chunkBuffer),
    //    numSamplesPerChunk
    //    );
    if (sioClient.opened())
    {
        sio::message::ptr m = sio::object_message::create();
        std::shared_ptr<const std::string> dat = audio->inBuffer()->readRawData(numSamples);

        // milliseconds since system epoch, not unix epoch/current time -
        // for measuring delta only
        std::chrono::time_point ts = std::chrono::steady_clock::now();
        std::chrono::duration dur_ms = std::chrono::duration<double, std::milli>(ts.time_since_epoch());
        double dTime = dur_ms.count();

        m->get_map()["timestamp"] = sio::double_message::create(dTime);
        m->get_map()["samples"] = sio::binary_message::create(dat);
        sioClient.socket()->Emit("AUDIO_IN", sio::message::list(m));
    }
    else
    {
        // TODO
        qCritical("Cannot send samples without open socket! Aborting.");
        //audio->StopAudio();
    }
}

void UwaVClient::onSamplesReceived(const sio::message::ptr &msg)
{
    // Samples received get em ready for the sound card!!!!

    // ...This is a complicated payload.
    int reportedSampleRate = 0;
    int reportedSampleNum = 0;
    double inputLevel;
    double outputLevel;
    double inputDecibels;
    double outputDecibels;
    int inferenceTimeMs;
    std::shared_ptr<const std::string> data = nullptr;

    std::map keyvals = msg->get_map();

    for (const std::pair<std::string, sio::message::ptr> &kv : keyvals)
    {
        //if (kv.first == "sample_rate" && kv.second->get_flag() == sio::message::flag_integer)
        //{
        //    reportedSampleRate = kv.second->get_int();
        //}
        if (kv.first == "sample_count" && kv.second->get_flag() == sio::message::flag_integer)
        {
            reportedSampleNum = kv.second->get_int();
        }
        else if (kv.first == "timestamp" && kv.second->get_flag() == sio::message::flag_double)
        {
            // Ignore for now
        }
        else if (kv.first == "input_level_db" && kv.second->get_flag() == sio::message::flag_double)
        {
            if (primaryInputDeviceNum >= audio->GetInputListCount())
            {
                // Using server for audio input
                inputDecibels = round(kv.second->get_double());
                audio->signaler->currentInputRMSChangedTo(inputDecibels);
            }
        }
        else if (kv.first == "output_level_db" && kv.second->get_flag() == sio::message::flag_double)
        {
            //qInfo() << "Out: " << sioToString(kv.second).c_str();
            if (primaryOutputDeviceNum >= audio->GetOutputListCount())
            {
                outputDecibels = round(kv.second->get_double());
                audio->signaler->currentOutputRMSChangedTo(outputDecibels);
            }
        }
        else if (kv.first == "inference_time_ms" && kv.second->get_flag() == sio::message::flag_double)
        {
            inferenceTimeMs = round(kv.second->get_double());
            ui->labelInferTime->setText(QString::number(inferenceTimeMs).append(" ms"));
        }
        else if (kv.first == "samples" && kv.second->get_flag() == sio::message::flag_binary)
        {
            data = kv.second->get_binary();
        }
        //qInfo() << kv.first.c_str() << ": " << sioToString(kv.second).c_str();
    }
    if (reportedSampleNum > 0 && data != nullptr)
    {
        // Final sanity check
        if (reportedSampleNum != numOutputSamplesPerChunk)
        {
            // TODO: this only works when in/out sample rate match.
            // Add "in" and "out" samples per chunk once functionality
            // is added
            qCritical("CRITICAL: Input/output rate mismatch from server (%i / %i). Stopping conversion.", reportedSampleNum, numOutputSamplesPerChunk);
            serverStarting = false;
            serverStarted = false;
            numInputSamplesPerChunk = 0;
            numOutputSamplesPerChunk = 0;
            sioClient.socket()->Emit("SERVER_STOP");
            qInfo ("Forcing stream disconnect: rate mismatch");    // Tracing crash source
            audio->StopAudio();
            return;
        }
        //if (reportedSampleNum != data->length() / sizeof(flaot))
        //{
        //    return;
        //}
        // TODO: double check that out samplerate matches the
        // audio output buffer & doesn't need conversion

        audio->outBuffer()->writeRawData(data, reportedSampleNum);
    }
}

/* Local audio provider has signaled that it has completed its
 * initialization and has an initial list of input/output devices
 * available on this computer */
void UwaVClient::on_DeviceListsReady()
{
    // The main thread was previously locked to initialize
    // the audio class, so it is safe to assume this is the
    // first population & we don't need to do any list juggling
    // at this stage.
    ui->comboAudioSinkLocal->addItems(*audio->GetDeviceList(false));
    ui->comboAudioSinkLocal->addItem("Output Audio on Server"); // Placeholder, relied on by LocalConfig

    ui->comboAudioSrcLocal->addItems(*audio->GetDeviceList(true));
    ui->comboAudioSrcLocal->addItem("Input Audio on Server");   // Placeholder, relied on by LocalConfig

    // Should only be called once, near program start but after UI
    // is up and objects have been created.
    savedConfig->Load(ui);
}

/***********************************************************************************
 *                   UI EVENTS
 ***********************************************************************************/
void UwaVClient::on_InputLevelChanged(float level)
{
    // We want to absolutely minimize processing when loading/unloading the
    // pipewire buffers, so no conversion has been performed on 'level' --
    // it should represent the normalized RMS value of the last chunk
    // received, and will need to be converted to something user-legible.

    // Approx range with a floating point input is -140 to -4 dBFS

    if (level != 0)
    {
        int sliderLevel = round(level * 1000);
        if (sliderLevel > 100) sliderLevel = 100;

        level = round(20 * log10(level));

        // Have had some issues with level != 0, but log10
        // still coming out to NaN (as if computing log(0))
        if (level != level) // NaN unless -ffast-math
        {
            ui->labelInLevel->setText("--");
            ui->progressBarInLevel->setValue(0);
        }
        else
        {
            ui->labelInLevel->setText(QString::number(level).append(" dBFS"));
            ui->progressBarInLevel->setValue(sliderLevel);
        }
    }
    else
    {
        ui->labelInLevel->setText("--");
        ui->progressBarInLevel->setValue(0);
    }

    ui->progressBarInLevel->update();   // Gets lazy with numbers tuned off
}

void UwaVClient::on_OutputLevelChanged(float level)
{
    // We want to absolutely minimize processing when loading/unloading the
    // pipewire buffers, so no conversion has been performed on 'level' --
    // it should represent the normalized RMS value of the last chunk
    // received, and will need to be converted to something user-legible.

    // Approx range with a floating point input is -140 to -4 dBFS

    if (level != 0)
    {
        int sliderLevel = round(level * 1000);
        if (sliderLevel > 100) sliderLevel = 100;

        level = round(20 * log10(level));

        if (level != level) // NaN unless -ffast-math
        {
            ui->labelOutLevel->setText("--");
            ui->progressBarOutLevel->setValue(0);
        }
        else
        {
            ui->labelOutLevel->setText(QString::number(level).append(" dBFS"));
            ui->progressBarOutLevel->setValue(sliderLevel);
        }
    }
    else
    {
        ui->labelOutLevel->setText("--");
        ui->progressBarOutLevel->setValue(0);
    }

    ui->progressBarOutLevel->update();   // Gets lazy with numbers tuned off
}

void UwaVClient::on_InputDeviceAdded(int index, std::string name)
{
    ui->comboAudioSrcLocal->insertItem(index, QString::fromStdString(name));
    if (savedConfig->GetSavedInputDevice() == name)
    {
        ui->comboAudioSrcLocal->setCurrentIndex(index);
    }
}

void UwaVClient::on_OutputDeviceAdded(int index, std::string name)
{
    ui->comboAudioSinkLocal->insertItem(index, QString::fromStdString(name));
    if (savedConfig->GetSavedOutputDevice() == name)
    {
        ui->comboAudioSinkLocal->setCurrentIndex(index);
    }
}

void UwaVClient::on_InputDeviceRemoved(int index)
{
    if (ui->comboAudioSrcLocal->currentIndex() == index)
    {
        // The device that just disappeared from our audio
        // provider was the current audio device.
        ui->comboAudioSrcLocal->setCurrentIndex(-1);
        //ui->comboAudioSrcLocal->setCurrentText("");

        if (sioClient.opened())
        {
            serverStarting = false;
            serverStarted = false;
            numInputSamplesPerChunk = 0;
            numOutputSamplesPerChunk = 0;
            sioClient.socket()->Emit("SERVER_STOP");
            qInfo ("Forcing stream disconnect: input device removed");    // Tracing crash source
            audio->StopAudio();
            audio->SetActiveInput(-1);
        }
    }

    // As long as indexes always shift exactly the
    // same way, the front of our device list should
    // always mirror the u_AudioProvider's internal
    // list.
    ui->comboAudioSrcLocal->removeItem(index);
}

void UwaVClient::on_OutputDeviceRemoved(int index)
{
    if (ui->comboAudioSinkLocal->currentIndex() == index)
    {
        // The device that just disappeared from our audio
        // provider was the current audio device.
        ui->comboAudioSinkLocal->setCurrentIndex(-1);
        ui->comboAudioSinkLocal->setCurrentText("");

        if (sioClient.opened())
        {
            serverStarting = false;
            serverStarted = false;
            numInputSamplesPerChunk = 0;
            numOutputSamplesPerChunk = 0;
            sioClient.socket()->Emit("SERVER_STOP");
            qInfo ("Forcing stream disconnect: device removed");    // Tracing crash source
            audio->StopAudio();
            audio->SetActiveOutput(-1);
        }
    }
    // As long as indexes always shift exactly the
    // same way, the front of our device list should
    // always mirror the u_AudioProvider's internal
    // list.
    ui->comboAudioSinkLocal->removeItem(index);
}

void UwaVClient::on_AudioStreamSuccess()
{
    ui->labelConnectState->setText("Streaming");
    chunkSize = ui->sliderChunkSize->value();

    if (audio->inStreamConnected)   // Buffer won't be guaranteed to exist otherwise
    {
        numInputSamplesPerChunk = (chunkSize * audio->inBuffer()->sampleRate) * 0.001;
        qInfo("Input samples per chunk: %i", numInputSamplesPerChunk);
    }

    if (audio->outStreamConnected)  // Buffer won't be guaranteed to exist otherwise
        numOutputSamplesPerChunk = (chunkSize * audio->outBuffer()->sampleRate)  * 0.001;

    if (!serverStarting && !serverStarted && sioClient.opened())
    {
        sioClient.socket()->Emit("SERVER_START");
        serverStarting = true;
    }
}
void UwaVClient::on_AudioStreamEnd()
{
    // Fallback status referring to server connection,
    // not current audio state.
    if (sioClient.opened())
    {
        ui->labelConnectState->setText("Connected.");
    }
    else
    {
        ui->labelConnectState->setText("Disconnected.");
        ui->labelConnectState->setEnabled(false);
    }
    numInputSamplesPerChunk = 0;
    numOutputSamplesPerChunk = 0;
}
void UwaVClient::on_AudioStreamConnecting()
{
    ui->labelConnectState->setText("Starting...");
}

void UwaVClient::on_comboAudioSrcLocal_currentIndexChanged(int index)
{
    if (primaryInputDeviceNum == index)
    {
        return;
    }

    // Index should align with the local device list
    // u_audioprovider should be able to manage the rest
    if (index >= 0 && index < ui->comboAudioSrcLocal->count() - 1)
    {
        audio->SetActiveInput(index);
        audio->SetInputGateTail(0.5);   // Might need recalculation if different sample rate

        primaryInputDeviceNum = index;
        primaryInputDeviceDesc = ui->comboAudioSrcLocal->currentText();
        primaryInputDeviceName = audio->GetDeviceNameFromID(index, true);

        savedConfig->SetSavedInputDevice(primaryInputDeviceDesc.toStdString());
    }
    else if (index == ui->comboAudioSrcLocal->count() - 1)
    {
        // TODO: Set output to stream over network instead, deactivate
        // local audio stuff if applicable

        audio->SetActiveInput(-1);

        primaryInputDeviceNum = index;
        primaryInputDeviceName = "TODO: Populate network device names";
        primaryInputDeviceDesc = "(Audio will output on the server)";

        savedConfig->SetSavedInputDevice("server");
    }
    else if (index == -1)
    {
        // This gets set back to -1 when device removal clears the
        // current entry.
        primaryInputDeviceNum = -1;
        primaryInputDeviceName = "Selected Input No Longer Present!";
        primaryInputDeviceDesc = "Please select a new input device.";
    }
    else
    {
        qErrnoWarning("ERROR: Selected non-existent input option??");
        return;
    }

    // Update user feedback
    // There are better ways to do this, it basically results in 4 new strings getting
    // created and copied on the fly, but it gets the job done. TODO: fix? Do I care?
    // .append() is probs better.
    ui->labelInputDevice->setText("Input device: <b>" + primaryInputDeviceDesc + "</b><br /><i>(" + primaryInputDeviceName + ")</i>");
}

void UwaVClient::on_comboAudioSinkLocal_currentIndexChanged(int index)
{
    if (primaryOutputDeviceNum == index)
    {
        return;
    }

    // Index should align with the local device list
    // u_audioprovider should be able to manage the rest
    if (index >= 0 && index < ui->comboAudioSinkLocal->count() - 1)
    {
        audio->SetActiveOutput(index);

        primaryOutputDeviceNum = index;
        primaryOutputDeviceDesc = ui->comboAudioSinkLocal->currentText();
        primaryOutputDeviceName = audio->GetDeviceNameFromID(index, false);

        savedConfig->SetSavedOutputDevice(primaryOutputDeviceDesc.toStdString());
    }
    else if (index == ui->comboAudioSinkLocal->count() - 1)
    {
        // TODO: Set output to stream over network instead, deactivate
        // local audio stuff if applicable

        audio->SetActiveOutput(-1);

        primaryOutputDeviceNum = index;
        primaryOutputDeviceName = "TODO: Populate network device names";
        primaryOutputDeviceDesc = "(Audio will output on the server)";

        savedConfig->SetSavedOutputDevice("server");
    }
    else if (index == -1)
    {
        // This gets set back to -1 when device removal clears the
        // current entry.
        primaryOutputDeviceNum = -1;
        primaryOutputDeviceName = "Selected Output No Longer Present!";
        primaryOutputDeviceDesc = "Please select a new output device.";
    }
    else
    {
        qErrnoWarning("ERROR: Selected non-existent output option??");
        return;
    }

    // Update user feedback
    // There are much better ways to do this, it basically results in 4 new strings getting
    // created and copied on the fly, but it gets the job done. TODO: fix? Do I care?
    // .append() is probs better.
    ui->labelOutputDevice->setText("Output device: <b>" + primaryOutputDeviceDesc + "</b><br /><i>(" + primaryOutputDeviceName + ")</i>");

    //ValidateStartStopState();
}

void UwaVClient::on_buttonStart_clicked()
{
    if (ValidateStartStopState())
    {
        ui->buttonStart->setEnabled(false);
        ui->buttonStop->setEnabled(true);

        // Start audio streams
        // TODO: Might want to move this to the SERVER_STARTED event
        // so that we're only sending audio once we know everything
        // is running
        // TODO: Moved SERVER_START over to the stream_connected
        // event for now
        audio->StartAudio();
    }
}

void UwaVClient::on_buttonStop_clicked()
{
    ui_LocalStop();
    serverStarting = false;
    serverStarted = false;
    numInputSamplesPerChunk = 0;
    numOutputSamplesPerChunk = 0;
    if (sioClient.opened())
    {
        sioClient.socket()->Emit("SERVER_STOP");
    }
    audio->StopAudio();
}

void UwaVClient::on_comboServerSelect_currentTextChanged(const QString &arg1)
{
    // User has selected a previously valid server from the drop-down, or
    // typed in a new one themselves. Parse
    //qInfo() << "Combo edit text changed to: " << arg1;
}

void UwaVClient::on_comboServerSelect_editTextChanged(const QString &arg1)
{
    //qInfo() << "Typing: " << arg1;
}

void UwaVClient::on_comboServerSelect_currentIndexChanged(int index)
{
    // This is also the function that gets triggered when the user
    // presses enter after typing something in.
    AttemptConnectionTo(ui->comboServerSelect->currentText());
}

void UwaVClient::on_buttonConnect_clicked()
{
    if (connectRetryState)
    {
        // Button is in "Cancel" mode during connection attempt
        connectRetryState = false;
        ui->labelConnectState->setText("Disconnected");
        ui->buttonConnect->setText("Connect");
        ui->comboServerSelect->setEnabled(true);

        ui_ServerDisconnect();
    }
    else if (sioClient.opened())
    {
        sioClient.close();

        // sioClient.close() is asynchronous, don't re-enable
        // button until it triggers its onClose event
        ui->labelConnectState->setEnabled(false);
        ui->labelConnectState->setText("Disconnecting");
        ui->buttonConnect->setText("Disconnecting");
    }
    else
    {
        // Logic for when user types something and clicks "connect"
        // instead of pressing enter, might want to make sure
        // it saves to the combo list here (doesn't happen auto-
        // magically)
        AttemptConnectionTo(ui->comboServerSelect->currentText());
    }
}

void UwaVClient::on_sliderInputGain_sliderMoved(int position)
{
    // TODO: Update audio provider, send event to server
    std::chrono::steady_clock::time_point newTime = std::chrono::steady_clock::now();

    if (timeBetweenSliderUpdates < newTime - lastSliderUpdateTime)
    {
        EmitDouble("UPDATE_PARAM", "input_volume", static_cast<double>(position) * 0.01);

        lastSliderUpdateTime = newTime;
    }

    if (audio->GetActiveInput() != -1)
    {
        audio->SetInputGain(static_cast<float>(position) * 0.01);
    }

    // Update UI
    ui->labelInputGain->setText(QString::number(position).append("%"));
}

void UwaVClient::on_sliderOutputGain_sliderMoved(int position)
{
    // TODO: Update audio provider, send event to server
    std::chrono::steady_clock::time_point newTime = std::chrono::steady_clock::now();

    if (timeBetweenSliderUpdates < newTime - lastSliderUpdateTime)
    {
        EmitDouble("UPDATE_PARAM", "output_volume", static_cast<double>(position) * 0.01);
        lastSliderUpdateTime = newTime;
    }

    if (audio->GetActiveOutput() != -1)
    {
        audio->SetOutputGain(static_cast<float>(position) * 0.01);
    }

    // Update UI
    ui->labelOutputGain->setText(QString::number(position).append("%"));
}

void UwaVClient::on_sliderPitchShift_sliderMoved(int position)
{
    std::chrono::steady_clock::time_point newTime = std::chrono::steady_clock::now();

    if (timeBetweenSliderUpdates < newTime - lastSliderUpdateTime)
    {
        EmitInteger("UPDATE_PARAM", "pitch_shift_semitones", ui->sliderPitchShift->value());
        lastSliderUpdateTime = newTime;
    }

    // Update UI
    ui->labelPitchShift->setText(QString::number(position).append(" st"));
}

void UwaVClient::on_sliderChunkSize_sliderMoved(int position)
{
    std::chrono::steady_clock::time_point newTime = std::chrono::steady_clock::now();

    // Server is currently hard-coded to 10ms hops
    position = (int)round((float)position / 10) * 10;

    if (timeBetweenSliderUpdates < newTime - lastSliderUpdateTime)
    {
        EmitDouble("UPDATE_PARAM", "chunk_size_s", static_cast<double>(position) * 0.001);
        lastSliderUpdateTime = newTime;
    }

    // Update UI
    ui->labelChunkSize->setText(QString::number(position).append(" ms"));
}

void UwaVClient::on_sliderExtraSize_sliderMoved(int position)
{
    std::chrono::steady_clock::time_point newTime = std::chrono::steady_clock::now();
    position = (int)round((float)position / 50) * 50;

    if (timeBetweenSliderUpdates < newTime - lastSliderUpdateTime)
    {
        EmitDouble("UPDATE_PARAM", "chunk_pad_s", static_cast<double>(position) * 0.001);
        lastSliderUpdateTime = newTime;
    }

    // Update UI
    ui->labelExtraSize->setText(QString::number(position).append(" ms"));
}

void UwaVClient::on_sliderInputGain_sliderPressed()
{
    // Set time - should only send updates to server periodically,
    // these things flood otherwise
    lastSliderUpdateTime = std::chrono::steady_clock::now();
    
}

void UwaVClient::on_sliderOutputGain_sliderPressed()
{
    // Set time - should only send updates to server periodically,
    // these things flood otherwise
    lastSliderUpdateTime = std::chrono::steady_clock::now();
}

void UwaVClient::on_sliderPitchShift_sliderPressed()
{
    // Set time - should only send updates to server periodically,
    // these things flood otherwise
    lastSliderUpdateTime = std::chrono::steady_clock::now();
}

void UwaVClient::on_sliderChunkSize_sliderPressed()
{
    // Set time - should only send updates to server periodically,
    // these things flood otherwise
    lastSliderUpdateTime = std::chrono::steady_clock::now();
}

void UwaVClient::on_sliderExtraSize_sliderPressed()
{
    // Set time - should only send updates to server periodically,
    // these things flood otherwise
    lastSliderUpdateTime = std::chrono::steady_clock::now();
}

void UwaVClient::on_sliderPitchShift_sliderReleased()
{
    EmitInteger("UPDATE_PARAM", "pitch_shift_semitones", ui->sliderPitchShift->value());
}

void UwaVClient::on_sliderInputGain_sliderReleased()
{
    EmitDouble("UPDATE_PARAM", "input_volume", static_cast<double>(ui->sliderInputGain->value()) * 0.01);
}

void UwaVClient::on_sliderOutputGain_sliderReleased()
{
    // TODO: Final update audio provider, send event to server
    EmitDouble("UPDATE_PARAM", "output_volume", static_cast<double>(ui->sliderOutputGain->value()) * 0.01);
}

void UwaVClient::on_sliderChunkSize_sliderReleased()
{
    chunkSize = ui->sliderChunkSize->value();
    chunkSize = (int)round((float)chunkSize / 10) * 10;
    //numSamplesPerChunk = (chunkSize * audio->inBuffer()->sampleRate) * 0.001;

    EmitDouble("UPDATE_PARAM", "chunk_size_s", static_cast<double>(chunkSize) * 0.001);
    //qInfo("Chunk size is now %i ms (%i samples)", chunkSize, numSamplesPerChunk);

    ui->sliderChunkSize->setSliderPosition(chunkSize);
    //ui->labelChunkSize->setText(QString::number(chunkSize).append(" ms"));
}

void UwaVClient::on_sliderExtraSize_sliderReleased()
{
    int position = (int)round((float)ui->sliderExtraSize->value() / 50) * 50;
    ui->sliderExtraSize->setSliderPosition(position);

    EmitDouble("UPDATE_PARAM", "chunk_pad_s", static_cast<double>(ui->sliderExtraSize->value()) * 0.001);
}

void UwaVClient::on_sliderCrossfadeSize_sliderMoved(int position)
{
    std::chrono::steady_clock::time_point newTime = std::chrono::steady_clock::now();

    if (timeBetweenSliderUpdates < newTime - lastSliderUpdateTime)
    {
        EmitDouble("UPDATE_PARAM", "crossfade_s", static_cast<double>(position) * 0.001);
        lastSliderUpdateTime = newTime;
    }

    // Update UI
    ui->labelCrossfadeSize->setText(QString::number(position).append(" ms"));
}


void UwaVClient::on_sliderFormantShift_sliderMoved(int position)
{
    std::chrono::steady_clock::time_point newTime = std::chrono::steady_clock::now();

    if (timeBetweenSliderUpdates < newTime - lastSliderUpdateTime)
    {
        EmitInteger("UPDATE_PARAM", "formant_shift", position);
        lastSliderUpdateTime = newTime;
    }

    // Update UI
    ui->labelFormantShift->setText(QString::number(position));
}


void UwaVClient::on_sliderMatchRMS_sliderMoved(int position)
{
    std::chrono::steady_clock::time_point newTime = std::chrono::steady_clock::now();

    if (timeBetweenSliderUpdates < newTime - lastSliderUpdateTime)
    {
        EmitDouble("UPDATE_PARAM", "use_output_rms", 1 - (static_cast<double>(position) * 0.01));
        lastSliderUpdateTime = newTime;
    }

    // Update UI
    ui->labelMatchRMS->setText(QString::number(position).append("%"));
}

void UwaVClient::on_sliderCrossfadeSize_sliderPressed()
{
    // Set time - should only send updates to server periodically,
    // these things flood otherwise
    lastSliderUpdateTime = std::chrono::steady_clock::now();
}


void UwaVClient::on_sliderFormantShift_sliderPressed()
{
    // Set time - should only send updates to server periodically,
    // these things flood otherwise
    lastSliderUpdateTime = std::chrono::steady_clock::now();
}


void UwaVClient::on_sliderMatchRMS_sliderPressed()
{
    // Set time - should only send updates to server periodically,
    // these things flood otherwise
    lastSliderUpdateTime = std::chrono::steady_clock::now();
}



void UwaVClient::on_sliderCrossfadeSize_sliderReleased()
{
    EmitDouble("UPDATE_PARAM", "crossfade_s", static_cast<double>(ui->sliderCrossfadeSize->value()) * 0.001);
}


void UwaVClient::on_sliderFormantShift_sliderReleased()
{
    EmitInteger("UPDATE_PARAM", "formant_shift", ui->sliderFormantShift->value());
}


void UwaVClient::on_sliderMatchRMS_sliderReleased()
{
    EmitDouble("UPDATE_PARAM", "use_output_rms", 1 - (static_cast<double>(ui->sliderMatchRMS->value()) * 0.01));
}


void UwaVClient::on_checkUseFP32_toggled(bool checked)
{
    EmitBoolean("UPDATE_PARAM", "use_fp32", checked);
}


void UwaVClient::on_checkUseJIT_toggled(bool checked)
{
    EmitBoolean("UPDATE_PARAM", "use_jit", checked);
}


void UwaVClient::on_checkNoiseReduce_toggled(bool checked)
{
    EmitBoolean("UPDATE_PARAM", "use_noise_reduction", checked);
}


void UwaVClient::on_checkUseVocode_toggled(bool checked)
{
    EmitBoolean("UPDATE_PARAM", "use_vocode_smoothing", checked);
}


void UwaVClient::on_sliderGateRMS_sliderPressed()
{
    // Set time - should only send updates to server periodically,
    // these things flood otherwise
    lastSliderUpdateTime = std::chrono::steady_clock::now();
}


void UwaVClient::on_sliderGateRMS_sliderMoved(int position)
{
    std::chrono::steady_clock::time_point newTime = std::chrono::steady_clock::now();
    if (position == ui->sliderGateRMS->minimum())
    {
        position = 0;   // Deactivated
    }
    if (timeBetweenSliderUpdates < newTime - lastSliderUpdateTime)
    {
        EmitInteger("UPDATE_PARAM", "input_gate_db", position);
        lastSliderUpdateTime = newTime;
    }

    if (audio->GetActiveInput() != -1)
    {
        audio->SetInputGate(position);
    }

    // Update UI
    if (position == 0)
    {
        ui->labelGateRMS->setText("Off");
    }
    else
    {
        ui->labelGateRMS->setText(QString::number(position));
    }
}


void UwaVClient::on_sliderGateRMS_sliderReleased()
{
    int pos = ui->sliderGateRMS->value();
    if (pos == ui->sliderGateRMS->minimum())
    {
        pos = 0;    // Deactivate
    }
    if (audio->GetActiveInput() != -1)
    {
        audio->SetInputGate(pos);
    }
    // Update UI
    if (pos == 0)
    {
        ui->labelGateRMS->setText("Off");
    }
    else
    {
        ui->labelGateRMS->setText(QString::number(pos));
    }
    EmitInteger("UPDATE_PARAM", "input_gate_db", pos);
}


void UwaVClient::on_buttonServerExit_clicked()
{
    if (sioClient.opened())
    {
        sioClient.socket()->Emit("SERVER_REQUEST_EXIT");
    }
}

