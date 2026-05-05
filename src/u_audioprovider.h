#ifndef U_AUDIOPROVIDER_H
#define U_AUDIOPROVIDER_H
#include <string>
#include <QString>
#include <QStringList>
#include <vector>
#include <pipewire/pipewire.h>
#include "spa/param/audio/raw.h"
#include "spa/pod/builder.h"
#include "u_interfacedescriptor.h"
#include "u_enginestatistics.h"
#include "u_ringbuffer.h"

/*** Structs used for pipewire events/data exchange. ***
 *
 * When callbacks are triggered, pipewire likes to send
 * us back a pointer to a user-defined data structures.
 * Keep state information & connection info here so that
 * it is available during processing. */

/* Helper struct that will shuttle pointers and information
 * back and forth with pipewire.
 */
struct pwStreamData {
    struct pw_thread_loop *threadLoop = nullptr;
    struct pw_loop *loop = nullptr;
    struct pw_stream *stream = nullptr;
    u_EngineStatistics *eStats = nullptr;
    u_RingBuffer *buffer = nullptr;

    bool isRunning = false;
    int ticksBetweenUpdates = 3;
    int currentTicks = 0;

    bool isGating = false;
    int tailCounter = 0;
};

/* Helper struct for when registry info is received
 * (at program start) or updated */
struct pwRegistryData {
    std::vector<u_InterfaceDescriptor> *inDeviceList;
    std::vector<u_InterfaceDescriptor> *outDeviceList;
    u_EngineStatistics *signaler;
    bool firstRun;
};

/* Helper struct for initial pipewire 'roundtrip' event */
struct pwRoundtripData {
    int pending;
    pwRegistryData *registryStruct;
    struct pw_thread_loop *threadLoop;
};

/* Pipewire-based audio input/output handler. This class
 * does not implement a singleton pattern but pipewire
 * callbacks require it to have static methods/fields
 * so please ONLY create one.
  */
class u_AudioProvider
{
public:
    u_AudioProvider(int argc, char *argv[]);
    ~u_AudioProvider();

    inline static std::atomic<float> gateRMS = 0;
    inline static std::atomic<int> gateClosingTail = 24000;
    inline static std::atomic<float> inputGain = 1.0;
    inline static std::atomic<float> outputGain = 1.0;
    inline static bool inStreamConnected = false;
    inline static bool outStreamConnected = false;
    inline static bool threadRunning = false;
    u_EngineStatistics *signaler;

    u_RingBuffer *inBuffer();
    u_RingBuffer *outBuffer();

    /* Callbacks */
    static void onProcessInput(void *streamStruct);
    static void onProcessOutput(void *streamStruct);
    static void onCoreInfo(void *data, const struct pw_core_info *info);
    static void onInputStreamChanged(void *streamStruct, enum pw_stream_state oldState, enum pw_stream_state newState, const char *error);
    static void onOutputStreamChanged(void *streamStruct, enum pw_stream_state oldState, enum pw_stream_state newState, const char *error);

    static void onRegistryAdd(void *registryStruct, uint32_t id,
                               uint32_t permissions,
                               const char *type,
                               uint32_t version,
                               const struct spa_dict *props);
    static void onRegistryRemove(void *registryStruct, uint32_t id);

    static void onCoreDone(void *roundtripStruct, uint32_t id, int seq);
    /* End Callbacks */

    QStringList *GetDeviceList(bool useInputList);
    QString GetDeviceNameFromID(int deviceListIndex, bool useInputList);

    void SetInputGate(int dbfs = 0);
    void SetInputGateTail(float seconds = 0.5);
    void SetInputGain(float multiple);
    void SetOutputGain(float multiple);
    void ServerRequestedInSampleRate(uint32_t sr);
    void SetActiveInput(int deviceListIndex);
    void SetActiveOutput(int deviceListIndex);
    int GetActiveInput();
    int GetActiveOutput();
    int GetInputListCount();
    int GetOutputListCount();

    void Init();
    void StartAudio();
    void StopAudio();

private:

    // These will go crazy if we ever have more than one u_AudioProvider
    // but are needed because pipewire callbacks (e.g. the registry event
    // that populates these lists) need to be static functions
    std::vector<u_InterfaceDescriptor> *inputDeviceList = nullptr;
    std::vector<u_InterfaceDescriptor> *outputDeviceList = nullptr;
    inline static u_InterfaceDescriptor *currentInputDevice = nullptr;
    inline static u_InterfaceDescriptor *currentOutputDevice = nullptr;
    int serverRequestedInSampleRate = 0;

    void createInputStream(std::string name, uint32_t sampleRate = 0);
    void createOutputStream(std::string name, uint32_t sampleRate = 0);
    void destroyInputStream();
    void destroyOutputStream();

    struct pw_thread_loop *pwThreadLoop;
    struct pw_context *pwContext;
    struct pw_core *pwCore;
    struct pw_registry *pwRegistry;
    struct spa_hook pwRegistryListener;
    struct spa_hook pwCoreListener;
    struct pwRoundtripData pwCoreRoundtripData;
    struct pwRegistryData pwCoreRegistryData;

    struct pw_properties *pwInputProps;
    struct pw_properties *pwOutputProps;
    struct spa_pod_builder pwInputPodBuilder;
    struct spa_pod_builder pwOutputPodBuilder;
    struct spa_audio_info_raw pwInputAudioInfo;
    struct spa_audio_info_raw pwOutputAudioInfo;
    pw_stream_flags pwInputStreamFlags;
    pw_stream_flags pwOutputStreamFlags;
    const struct spa_pod *pwInputSPAParams[1];
    const struct spa_pod *pwOutputSPAParams[1];
    struct pwStreamData pwInputData;
    struct pwStreamData pwOutputData;

    // These define a region of memory that pipewire will write
    // into. These should not be touched by anything except
    // to send a pointer off to the pipewire API.
    char pwInputBuffer[1024];       // NOTE: char is a platform-independent stand-in for 1 byte
    char pwOutputBuffer[1024];

    void EmptyDeviceLists();
};

#endif // U_AUDIOPROVIDER_H
