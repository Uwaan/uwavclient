#include <stdlib.h>
#include <string>
#include <vector>
#include <QDebug>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/ringbuffer.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include "u_audioprovider.h"
#include "spa/pod/builder.h"
#include "u_enginestatistics.h"

#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_CHANNELS 1

/***********************************************************************************
 *                           Callback delcarations
 ***********************************************************************************/

/* Writing out the full structure because, although
 * unspecified items would be an implicit null, I want
 * to keep the documentation handy & also, C++ doesnt
 * let you specify the way C does (need to define
 * inline and not just define the .event you want) */

static const struct pw_registry_events registry_events =
    { PW_VERSION_REGISTRY_EVENTS,
        u_AudioProvider::onRegistryAdd,
        u_AudioProvider::onRegistryRemove };

static const struct pw_core_events core_events =
    { PW_VERSION_CORE_EVENTS,
        NULL /* info */,
        u_AudioProvider::onCoreDone /* core_done */,
        NULL /* ping */,
        NULL /* error */,
        NULL /* remove_id */,
        NULL /* bound_id */,
        NULL /* add_mem */,
        NULL /* remove_mem */,
        NULL /* bound_props */ };

/* Deprecated, keeping commented as a reference for of each of
 * these fields
static const struct pw_stream_events stream_events =
    { PW_VERSION_STREAM_EVENTS,
     NULL, // destroy
     NULL, // state_changed
     NULL, // control_info
     NULL, // io_changed
     NULL, // param_changed
     NULL, // add_buffer
     NULL, // remove_buffer
     u_AudioProvider::onProcessOutput
     NULL, // drained
     NULL, // spa_command
     NULL, }; // trigger_done
*/

static const struct pw_stream_events outputStreamEvents =
    { PW_VERSION_STREAM_EVENTS,
        NULL, u_AudioProvider::onOutputStreamChanged, NULL, NULL, NULL, NULL, NULL, u_AudioProvider::onProcessOutput, NULL, NULL, NULL
    };


static const struct pw_stream_events inputStreamEvents =
    { PW_VERSION_STREAM_EVENTS,
        NULL, u_AudioProvider::onInputStreamChanged, NULL, NULL, NULL, NULL, NULL, u_AudioProvider::onProcessInput, NULL, NULL, NULL
     };


/***********************************************************************************
 *                   Methods
 ***********************************************************************************/

// Initializes a new AudioProvider and performs initial Pipewire session setup.
u_AudioProvider::u_AudioProvider(int argc, char *argv[])
{
    signaler = new u_EngineStatistics();
    this->inStreamConnected = false;
    this->outStreamConnected = false;
    int err;

    /* Not sure if there is any error handling code needed with this
     * Seems to need argc and argv for some reason that I'm too dumb
     * to figure out */
    pw_init(&argc, &argv);
    qInfo("Initialized libpipewire %s", pw_get_headers_version());

    pwThreadLoop = pw_thread_loop_new("uwavclient-audio-thread", NULL);

    // Testing whether we can start a thread now and leave it running.
    if ((err = pw_thread_loop_start(pwThreadLoop)) < 0)
    {
        qCritical("ERROR: Pipewire loop encountered error!! %d", err);
    }
    else
    {
        threadRunning = true;
    }

    // Take over the lock on all pipewire functions. This will cause
    // all commands to queue until the lock is released or a _wait
    // is called.
    pw_thread_loop_lock(pwThreadLoop);

    // The context object is our primary means of accessing
    // pipewire, it provides a proxy to the core and handles
    // configuration, modules, loading of settings, etc
    pwContext = pw_context_new(pw_thread_loop_get_loop(pwThreadLoop),
                               NULL, /* properties */
                               0 /* user_data size */);
    if (pwContext == NULL)
    {
        qFatal("FATAL: Unable to get pipewire context.");
    }

    // Get a proxy to the pipewire core object.
    pwCore = pw_context_connect(pwContext,
                                NULL, /* properties */
                                0 /* user_data_size */);
    if (pwCore == NULL)
    {
        qFatal("FATAL: Unable to connect to pipewire core.");
    }

    // We can set these now because they will never change.
    pwInputProps = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                     PW_KEY_MEDIA_CATEGORY, "Capture",
                                     PW_KEY_MEDIA_ROLE, "Communication",
                                     NULL );
    pwOutputProps = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                      PW_KEY_MEDIA_CATEGORY, "Playback",
                                      PW_KEY_MEDIA_ROLE, "Communication",
                                      NULL );

    pwInputStreamFlags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS);
    pwOutputStreamFlags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS);

    // The pipewire registry is a singleton containing all global
    // objects on the pipewire instance. They can be modules, nodes,
    // or (most importantly for our purposes) input/output devices.
    pwRegistry = pw_core_get_registry(pwCore, PW_VERSION_REGISTRY, 0 /* user_data_size */);
    pw_thread_loop_unlock(pwThreadLoop);
}

u_AudioProvider::~u_AudioProvider()
{
    if (threadRunning)
    {
        pw_thread_loop_stop(pwThreadLoop);
    }

    // Pretty sure the application will crash on exit if we never
    // called Init() to set these, just make sure Init() is always
    // called on startup
    spa_hook_remove(&pwCoreListener);
    spa_hook_remove(&pwRegistryListener);

    if (pwInputData.stream != nullptr)
    {
        pw_stream_disconnect(pwInputData.stream);
        pw_stream_destroy(pwInputData.stream);
    }
    if (pwOutputData.stream != nullptr)
    {
        pw_stream_disconnect(pwOutputData.stream);
        pw_stream_destroy(pwOutputData.stream);
    }
    if (pwInputData.buffer != nullptr) delete(pwInputData.buffer);
    if (pwOutputData.buffer != nullptr) delete(pwOutputData.buffer);

    // Not sure if order matters here but leaving it in reverse
    // order from their declaration/initialization

    pw_properties_free(pwInputProps);
    pw_properties_free(pwOutputProps);
    pw_proxy_destroy((struct pw_proxy*)pwRegistry);
    pw_core_disconnect(pwCore);
    pw_context_destroy(pwContext);
    pw_thread_loop_destroy(pwThreadLoop);
    pw_deinit();

    delete(signaler);

    if (currentInputDevice->isOrphan) delete currentInputDevice;
    if (currentOutputDevice->isOrphan) delete currentOutputDevice;

    // Same as above, this will screw up if Init()
    // was never called
    inputDeviceList->clear();
    outputDeviceList->clear();
    delete inputDeviceList;
    delete outputDeviceList;
}

// Empties the device lists stored by this class, initializing them
// if they do not yet exist.
void u_AudioProvider::EmptyDeviceLists()
{
    if (!inputDeviceList)
    {
        // Object is uninitialized
        inputDeviceList = new std::vector<u_InterfaceDescriptor>;
    }
    else
    {
        if (currentInputDevice != nullptr)
        {
            // currentInputDevice is a pointer to an item in this list.
            // Since the only time this function gets called is when
            // we are about to rebuild, it will dangle and cause
            // problems later.
            if (!currentInputDevice->isOrphan)
            {
                // Leave it as-is and leave the stream running. The
                // device list will repopulate, and any future selection
                // (including the device we're still on) will
                // trigger a reassignment/stream rebuild
                currentInputDevice = new u_InterfaceDescriptor(*currentInputDevice);
                currentInputDevice->isOrphan = true;
            }
        }
        inputDeviceList->clear();
    }

    if (!outputDeviceList)
    {
        // Object is uninitialized
        outputDeviceList = new std::vector<u_InterfaceDescriptor>;
    }
    else
    {
        if (currentOutputDevice != nullptr)
        {
            // Same as above; create an orphan if there is a working
            // stream using this device & clean it up later
            if (!currentOutputDevice->isOrphan)
            {
                currentOutputDevice = new u_InterfaceDescriptor(*currentOutputDevice);
                currentOutputDevice->isOrphan = true;
            }
        }
        outputDeviceList->clear();
    }
}

// Actual initialization was done when this class's constructor was called, but this
// will set up the initial loop & do a run to receive the pipewire registry
void u_AudioProvider::Init()
{
    pw_thread_loop_lock(pwThreadLoop);

    // First thing to do is set up a listener, ask the core for current
    // registry contents, and listen to its responses to find the
    // available I/O nodes for this computer.

    EmptyDeviceLists();     // Empty now, will start filling once roundtrip starts
    pwCoreRegistryData = { inputDeviceList, outputDeviceList, signaler, true };
    pwCoreRoundtripData = { 0, &pwCoreRegistryData, pwThreadLoop };

    // When loop unlocks, a bunch of "new item in registry" events will
    // be queued up to be sent to us. Events are queued and processed
    // in order.

    // The registry listener will be set up to capture those initial update
    // events. The core listener only needs to capture the "done" event
    // which is added to the end of the queue when we call pw_core_sync.
    spa_zero(pwRegistryListener);
    //spa_zero(pwCoreListener);
    pw_registry_add_listener(pwRegistry, &pwRegistryListener, &registry_events, &pwCoreRegistryData);
    pw_core_add_listener(pwCore, &pwCoreListener, &core_events, &pwCoreRoundtripData);

    pwCoreRoundtripData.pending = pw_core_sync(pwCore, PW_ID_CORE, 0);

    // This will cause THIS thread to sleep until the on_core_done event
    // sends a signal. We are holding the outer lock, so nothing else
    // can be processed on the thread in the meantime.
    pw_thread_loop_wait(pwThreadLoop);
    pw_thread_loop_unlock(pwThreadLoop);

    // Any code below this point will be executed after the
    // pw_thread_loop_signal is sent
}

// Sets the input gate, in dBFS (0 to disable)
void u_AudioProvider::SetInputGate(int dbfs)
{
    // This value gets accessed by other threads;
    // remain atomic pls
    if (dbfs == 0 || dbfs < -144)
    {
        gateRMS.store(0, std::memory_order_relaxed);
    }
    else
    {
        // dbfs = round(20 * log10(amp));
        float f = powf(10, (static_cast<float>(dbfs) / 20));
        gateRMS.store(f, std::memory_order_relaxed);
    }
}

// The input gate as implemented seems jittery, so: this
// sets the number of seconds that, once the input level
// falls below the gate thershold, we will leave input
// open anyway
void u_AudioProvider::SetInputGateTail(float seconds)
{
    // TODO: will need to implement with serverRequestedInSampleRate
    // if we ever set things up to negotiate that
    gateClosingTail = round(static_cast<float>(currentInputDevice->sampleRate) * seconds);
}

// Sets the input gain, as a multiplier (1.0 = no change)
void u_AudioProvider::SetInputGain(float multiple)
{
    // This value gets accessed by other threads
    if (multiple >= 0 && multiple <= 2.0)
    {
        inputGain.store(multiple, std::memory_order_relaxed);
    }
    else
    {
        // idk
        inputGain.store(1.0, std::memory_order_relaxed);
    }
}
void u_AudioProvider::SetOutputGain(float multiple)
{
    if (multiple >= 0 && multiple <= 2.0)
    {
        outputGain.store(multiple, std::memory_order_relaxed);
    }
    else
    {
        // why
        outputGain.store(1.0, std::memory_order_relaxed);
    }
}

// Set up the input stream and connect it to the pipewire graph
// Sample rate of 0 will use the current input device's default
// rate.
// NOTE: This function does not perform any locking.
void u_AudioProvider::createInputStream(std::string name, uint32_t sampleRate)
{
    if (pwInputData.stream != nullptr)
    {
        qErrnoWarning("ERROR: Please destroy input stream before attempting to set it up again.");
        return;
    }
    else if (currentInputDevice == nullptr)
    {
        qErrnoWarning("ERROR: Please set input device before attempting to set up an input stream.");
        return;
    }

    qInfo("Setting up input stream.");
    // Re-initialize the input data struct.
    //pwInputData = { 0, };

    //pwInputData.mainLoop = pwMainLoop;
    pwInputData.threadLoop = pwThreadLoop;
    pwInputData.loop = pw_thread_loop_get_loop(pwThreadLoop);
    pwInputData.eStats = signaler;

    assert(pwInputData.buffer == nullptr && "LEAK: Pipewire input buffer was not destroyed before re-creation.");

    if (sampleRate == 0)
    {
        // Sample rate was not specified; use device default
        sampleRate = static_cast<uint32_t>(currentInputDevice->sampleRate);
        pwInputData.buffer = new u_RingBuffer(sampleRate);
    }
    else
    {
        pwInputData.buffer = new u_RingBuffer(sampleRate);
    }

    // There's probably a better way of slowing down UI updates
    // using the current time or something but honestly this
    // is good enough for me
    pwInputData.ticksBetweenUpdates = 3;
    pwInputData.currentTicks = 0;

    // Turns out we can't use persistent props objects because they get
    // consumed by libpipewire upon stream start. Make a copy for this
    // individual stream's use
    pw_properties *streamProps = pw_properties_copy(pwInputProps);
    pw_properties_set(streamProps, PW_KEY_TARGET_OBJECT, currentInputDevice->device_name.c_str());

    /* Our buffer utilizes a char[] in order to specify its size, in
     * bytes. Audio data can be represented by any combination of
     * formats (int8, float32, etc) and can change.
     *
     * SPA plugins are used by pipewire to resample, upmix, downmix,
     * and/or re-type streams as needed between different nodes.
     *
     * The "pod" (plain old data) builder defines how we want our
     * format, sample rate, and channels to be formatted/received.  */
    pwInputAudioInfo = SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,     // TODO: support format change
            .rate = sampleRate,
            .channels = 1); // .channels = static_cast<uint32_t>(currentInputDevice->channels));

    pwInputPodBuilder = SPA_POD_BUILDER_INIT(pwInputBuffer, sizeof(pwInputBuffer));
    pwInputSPAParams[0] = spa_format_audio_raw_build(&pwInputPodBuilder, SPA_PARAM_EnumFormat, &pwInputAudioInfo);

    //pwInputData.stream = pw_stream_new(pwCore, "uwavclient-in", pwInputProps);

    /* Create a new stream object. This will not connect to the pipewire core
     * or generate any nodes just yet, although it will call pw_context_connect
     * if that has not already been done. */
    pwInputData.stream = pw_stream_new_simple(
        pwInputData.loop,
        name.c_str(),
        streamProps,
        &inputStreamEvents,
        &pwInputData);
    //pwInputData.isRunning = true;

    if (pwInputData.stream == nullptr)
    {
        qCritical("CRITICAL: Unable to create new input stream! (pw_stream_new_simple failed)");
        delete streamProps;
        return;
    }

    /* This is the function that finally takes all of our setup and
     * creates a Pipewire node that can be accessed on the master
     * graph and connected to by other nodes. */
    if (pw_stream_connect(pwInputData.stream, PW_DIRECTION_INPUT, PW_ID_ANY, pwInputStreamFlags, pwInputSPAParams, 1) < 0)
    {
        qCritical("CRITICAL: Unable to connect input stream to pipewire core!!");
        pw_stream_disconnect(pwInputData.stream);
        pw_stream_destroy(pwInputData.stream);
        pwInputData.stream = nullptr;
        delete(pwInputData.buffer);
        pwInputData.buffer = nullptr;
        return;
    }
}

// Set up the output stream and connect it to the pipewire graph
// Sample rate of 0 will use the current output device's default
// rate.
// NOTE: This function does not perform any locking.
void u_AudioProvider::createOutputStream(std::string name, uint32_t sampleRate)
{
    if (pwOutputData.stream != nullptr)
    {
        qErrnoWarning("ERROR: Please destroy output stream before attempting to set it up again.");
        return;
    }
    else if (currentOutputDevice == nullptr)
    {
        qErrnoWarning("ERROR: Please set output device before attempting to set up an output stream.");
        return;
    }

    qInfo("Setting up output stream.");
    // Re-initialize the output data struct.
    //pwOutputData = { 0, };

    pwOutputData.threadLoop = pwThreadLoop;
    pwOutputData.loop = pw_thread_loop_get_loop(pwThreadLoop);
    pwOutputData.eStats = signaler;

    assert(pwOutputData.buffer == nullptr && "LEAK: Pipewire output buffer was not destroyed before re-creation.");
    if (sampleRate == 0)
    {
        // Sample rate was not specified; use device default
        pwOutputData.buffer = new u_RingBuffer(DEFAULT_SAMPLE_RATE);
        sampleRate = static_cast<uint32_t>(currentOutputDevice->sampleRate);
    }
    else
    {
        pwOutputData.buffer = new u_RingBuffer(sampleRate);
    }

    // There's probably a better way of slowing down UI updates
    // using the current time or something but honestly this
    // is good enough for me
    pwOutputData.ticksBetweenUpdates = 3;
    pwOutputData.currentTicks = 0;

    // Turns out we can't use persistent props objects because they get
    // consumed by libpipewire upon stream start. Make a copy for this
    // individual stream's use
    pw_properties *streamProps = pw_properties_copy(pwOutputProps);
    pw_properties_set(streamProps, PW_KEY_TARGET_OBJECT, currentOutputDevice->device_name.c_str());

    /* Our buffer utilizes a char[] in order to specify its size, in
     * bytes. Audio data can be represented by any combination of
     * formats (int8, float32, etc) and can change.
     *
     * SPA plugins are used by pipewire to resample, upmix, downmix,
     * and/or re-type streams as needed between different nodes.
     *
     * The "pod" (plain old data) builder defines how we want our
     * format, sample rate, and channels to be formatted/received.  */
    pwOutputAudioInfo = SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,     // TODO: support format change
            .rate = sampleRate,
            .channels = 1); // .channels = static_cast<uint32_t>(currentInputDevice->channels));

    pwOutputPodBuilder = SPA_POD_BUILDER_INIT(pwOutputBuffer, sizeof(pwOutputBuffer));
    pwOutputSPAParams[0] = spa_format_audio_raw_build(&pwOutputPodBuilder, SPA_PARAM_EnumFormat, &pwOutputAudioInfo);

    //pwOutputData.stream = pw_stream_new(pwCore, "uwavclient-out", pwInputProps);

    /* Create a new stream object. This will not connect to the pipewire core
     * or generate any nodes just yet, although it will call pw_context_connect
     * if that has not already been done. */
    pwOutputData.stream = pw_stream_new_simple(
        pwOutputData.loop,
        name.c_str(),
        streamProps,
        &outputStreamEvents,
        &pwOutputData);
    //pwOutputData.isRunning = true;

    if (pwOutputData.stream == nullptr)
    {
        qCritical("CRITICAL: Unable to create new output stream! (pw_stream_new_simple failed)");
        delete streamProps;
        return;
    }

    /* This is the function that finally takes all of our setup and
     * creates a Pipewire node that can be accessed on the master
     * graph and connected to by other nodes. */
    if (pw_stream_connect(pwOutputData.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, pwOutputStreamFlags, pwOutputSPAParams, 1) < 0)
    {
        qCritical("CRITICAL: Unable to connect output stream to pipewire core!!");
        pw_stream_disconnect(pwOutputData.stream);
        pw_stream_destroy(pwOutputData.stream);
        pwOutputData.stream = nullptr;
        delete(pwOutputData.buffer);
        pwOutputData.buffer = nullptr;
        return;
    }
}

// Destroys the open input stream, if running.
// NOTE: This function does not perform any locking.
void u_AudioProvider::destroyInputStream()
{
    if (pwInputData.stream != nullptr)
    {
        // TODO: Got a weird crash once when stopping stream
        // after the (server-side) conversion engine failed to
        // start. Investigate?
        pw_stream_disconnect(pwInputData.stream);
        pwInputData.isRunning = false;
        pw_stream_destroy(pwInputData.stream);
        delete(pwInputData.buffer);

        pwInputData.stream = nullptr;
        pwInputData.buffer = nullptr;
        qInfo("Input stream disconnected.");
    }
    inStreamConnected = false;
}

// Destroys the open output stream, if running.
// NOTE: This function does not perform any locking.
void u_AudioProvider::destroyOutputStream()
{
    if (pwOutputData.stream != nullptr)
    {
        // TODO: Got a weird crash once when stopping stream
        // after the (server-side) conversion engine failed to
        // start. Investigate?
        pw_stream_disconnect(pwOutputData.stream);
        pwOutputData.isRunning = false;
        pw_stream_destroy(pwOutputData.stream);
        delete(pwOutputData.buffer);

        pwOutputData.stream = nullptr;
        pwOutputData.buffer = nullptr;
        qInfo("Output stream disconnected.");
    }
    outStreamConnected = false;
}

// Creates and starts input and/or output streams, if currrent
// in/out devices exist.
// NOTE: This function performs thread locking.
void u_AudioProvider::StartAudio()
{
    pw_stream_state sState;
    const char *err;

    if (inStreamConnected || outStreamConnected)
    {
        qWarning("WARNING: Attempting to start audio, but stream(s) are already connected!");
        return;
    }

    // Thread may already be running; lock in case it is
    pw_thread_loop_lock(pwThreadLoop);

    // Start input provider, if any
    if (currentInputDevice != nullptr && currentInputDevice->internal_id > -1)
    {
        if (serverRequestedInSampleRate != 0)
        {
            createInputStream("uwavclient-in", serverRequestedInSampleRate);
        }
        else
        {
            createInputStream("uwavclient-in");
        }
    }

    if (currentOutputDevice != nullptr && currentOutputDevice->internal_id > -1)
    {
        createOutputStream("uwavclient-out");
    }

    pw_thread_loop_unlock(pwThreadLoop);

    /*
    if (currentInputDevice != nullptr || currentOutputDevice != nullptr)
    {
        // On the chance something else may have followed some path
        // to close out the thread loop (e.g. selected server-side
        // in/out earlier), restart the loop

        if (!threadRunning)
        {
            pw_thread_loop_start(pwThreadLoop);
            threadRunning = true;
        }

        // TODO: For some reason, the input/output streams never
        // seemed to actually receive callbacks unless we trigger
        // a timed_wait and let it time out...
        // Remove this comment once everything is confirmed working.
        //pw_thread_loop_timed_wait(pwInputData.threadLoop, 2.0);
    }*/
}

// Stops and destroys any running input or output streams.
// NOTE: This function performs thread locking.
void u_AudioProvider::StopAudio()
{
    pw_thread_loop_lock(pwThreadLoop);

    destroyInputStream();
    destroyOutputStream();

    pw_thread_loop_unlock(pwThreadLoop);

    /*
    if (threadRunning)
    {
        pw_thread_loop_stop(pwThreadLoop);
        threadRunning = false;
    }*/
}

void u_AudioProvider::SetActiveInput(int deviceListIndex)
{
    if (deviceListIndex < 0 || deviceListIndex >= inputDeviceList->size())
    {
        // The input selected by the user is either invalid (-1)
        // or is set to a device that does not correspond to our
        // device list (i.e. input on server). Unset current device.
        if (currentInputDevice != nullptr)
        {
            if (currentInputDevice->isOrphan)
            {
                // This object was created with 'new'
                delete(currentInputDevice);
            }
            // If not orphan, then the pointer points to an item that presumably
            // remains in the device list, so dropping our reference to it here
            // should not leak memory
            currentInputDevice = nullptr;

            if (pwInputData.stream != nullptr)
            {
                pw_thread_loop_lock(pwThreadLoop);
                destroyInputStream();       // Does not lock itself
                pw_thread_loop_unlock(pwThreadLoop);
            }
        }
        return;
    }

    if (currentInputDevice == nullptr || currentInputDevice->internal_id != deviceListIndex)
    {
        // The device that was selected is not the current device
        //qInfo("Setting active input to #%i", deviceListIndex);

        if (currentInputDevice != nullptr && currentInputDevice->isOrphan)
        {
            // This ID got set if the device list was rebuilt with a
            // running stream, and the entry was orphaned in the
            // process. The orphan was created using new, so make
            // sure it doesn't leak.
            delete(currentInputDevice);
            currentInputDevice = nullptr;
        }

        currentInputDevice = &inputDeviceList->at(deviceListIndex);

        if (pwInputData.stream != nullptr)
        {
            // ..and the stream is running so we need to do a hot-swap

            // Server may have negotiated a lower (model-specific) rate
            // for us to be sending
            uint32_t sr = pwInputAudioInfo.rate;
            if (serverRequestedInSampleRate != 0) sr = serverRequestedInSampleRate;

            pw_thread_loop_lock(pwThreadLoop);
            destroyInputStream();
            createInputStream("uwavclient-in", sr);
            pw_thread_loop_unlock(pwThreadLoop);
        }
    }
}

void u_AudioProvider::SetActiveOutput(int deviceListIndex)
{
    if (deviceListIndex < 0 || deviceListIndex >= outputDeviceList->size())
    {
        // The output selected by the user is either invalid (-1)
        // or is set to a device that does not correspond to our
        // device list (i.e. output on server). Unset current device.
        if (currentOutputDevice != nullptr)
        {
            if (currentOutputDevice->isOrphan)
            {
                // This object was created with 'new'
                delete(currentOutputDevice);
            }
            // If not orphan, then the pointer points to an item that presumably
            // remains in the device list, so dropping our reference to it here
            // should not leak memory
            currentOutputDevice = nullptr;

            if (pwOutputData.stream != nullptr)
            {
                pw_thread_loop_lock(pwThreadLoop);
                destroyOutputStream();      // Does not lock itself
                pw_thread_loop_unlock(pwThreadLoop);
            }
        }
        return;
    }

    if (currentOutputDevice == nullptr || currentOutputDevice->internal_id != deviceListIndex)
    {
        // The device that was selected is not the current device
        //qInfo("Setting active output to #%i", deviceListIndex);

        if (currentOutputDevice != nullptr && currentOutputDevice->isOrphan)
        {
            // This ID got set if the device list was rebuilt with a
            // running stream, and the entry was orphaned in the
            // process. The orphan was created using new, so make
            // sure it doesn't leak.
            delete(currentOutputDevice);
            currentOutputDevice = nullptr;
        }

        currentOutputDevice = &outputDeviceList->at(deviceListIndex);

        if (pwOutputData.stream != nullptr)
        {
            // ..and the stream is running so we need to do a hot-swap
            pw_thread_loop_lock(pwThreadLoop);
            destroyOutputStream();
            createOutputStream("uwavclient-out");
            pw_thread_loop_unlock(pwThreadLoop);
        }
    }
}

void u_AudioProvider::ServerRequestedInSampleRate(uint32_t sr)
{
    if (pwInputData.stream != nullptr
            && sr != serverRequestedInSampleRate
            && (currentInputDevice != nullptr && sr != currentInputDevice->sampleRate))
    {
        // Need to tear down and re-establish stream at a new
        // sample rate, because pw_properties gets passed on
        // stream start and if there's a way to switch it while
        // running, then I don't know what that way is
        pw_thread_loop_lock(pwThreadLoop);
        destroyInputStream();
        createInputStream("uwavclient-in", sr);
        pw_thread_loop_unlock(pwThreadLoop);
    }
    serverRequestedInSampleRate = sr;

    // TODO: Need to check whether pipewire honored our
    // requested sample rate?
}

// Creates a list of available devices, presumably for display in the UI
 QStringList *u_AudioProvider::GetDeviceList(bool useInputList)
{
    QStringList *list = new QStringList();

    if (useInputList)
    {
        for (u_InterfaceDescriptor &item : *inputDeviceList)
        {
            list->push_back(QString::fromStdString(item.device_desc));
        }
    }
    else
    {
        for (u_InterfaceDescriptor &item : *outputDeviceList)
        {
            list->push_back(QString::fromStdString(item.device_desc));
        }
    }

    // TODO: Check for memory leaks in parent. Not sure if dereferencing
    // a 'new' downstream to give it to the Qt framework is safe
    return list;
}

// Omg read the name of the function
QString u_AudioProvider::GetDeviceNameFromID(int deviceListIndex, bool useInputList)
{
    u_InterfaceDescriptor item;

    // TODO: bounds check
    if (useInputList)
    {
        if (deviceListIndex >= 0 && deviceListIndex < inputDeviceList->size())
        {
            return QString::fromStdString(inputDeviceList->at(deviceListIndex).device_name);
        }
        // Out of bounds
        qErrnoWarning("ERROR: Performed lookup on invalid input device ID!");
        return QString::fromStdString("(Invalid Input Device ID)");
    }

    if (deviceListIndex >= 0 && deviceListIndex < outputDeviceList->size())
    {
        return QString::fromStdString(outputDeviceList->at(deviceListIndex).device_name);
    }
    // Out of bounds
    qErrnoWarning("ERROR: Performed lookup on invalid output device ID!");
    return QString::fromStdString("(Invalid Output Device ID)");
}

int u_AudioProvider::GetActiveInput()
{
    if (currentInputDevice != nullptr)
    {
        return currentInputDevice->internal_id;
    }
    return -1;
}

int u_AudioProvider::GetActiveOutput()
{
    if (currentOutputDevice != nullptr)
    {
        return currentOutputDevice->internal_id;
    }
    return -1;
}

int u_AudioProvider::GetInputListCount()
{
    if (inputDeviceList)
    {
        return inputDeviceList->size();
    }
    return -1;
}

int u_AudioProvider::GetOutputListCount()
{
    if (outputDeviceList)
    {
        return outputDeviceList->size();
    }
    return -1;
}

u_RingBuffer *u_AudioProvider::inBuffer()
{
    assert(pwInputData.buffer != nullptr && "Attempted to access nonexistent input buffer!");
    return pwInputData.buffer;
}
u_RingBuffer *u_AudioProvider::outBuffer()
{
    // TODO: BUG: This gets triggered if server crashes/disconnects, then audio is
    // started, then audio is stopped
    assert(pwOutputData.buffer != nullptr && "Attempted to access nonexistent output buffer!");
    return pwOutputData.buffer;
}
/***********************************************************************************
 *                   Event Receivers
 ***********************************************************************************/

/* These will be triggered when pipewire wants various
 * things for us or needs to provide various things
 * (e.g. registry objects) to us. Pointers to these
 * functions are set up in the pw_stream_events
 * struct that is sent on initialization. */

// Time to process input (from mic) data
void u_AudioProvider::onProcessInput(void *streamStruct)
{
    pwStreamData *data = (pwStreamData*)streamStruct;
    struct pw_buffer *pwBuffer;
    struct spa_buffer *spaBuffer;
    float *samples, rms, sum;
    uint32_t n, numSamples;

    if ((pwBuffer = pw_stream_dequeue_buffer(data->stream)) == NULL)
    {
        qCritical("WARN: Pipewire input out of buffers???");
        return;
    }

    if (!inStreamConnected || data->buffer == nullptr)
    {
        // We are receiving input, but the I/O loop hasn't been completely
        // finalized yet. When first connecting a stream, there seem to
        // be a few of these callbacks that occur before setup is even done.
        // Discard the input.

        // Pipewire needs you to return its buffer so it can reuse them
        // btw
        pw_stream_queue_buffer(data->stream, pwBuffer);
        return;
    }

    spaBuffer = pwBuffer->buffer;

    // Check null on the first element juuuuuust in case
    if ((samples = (float*)spaBuffer->datas[0].data) == NULL)
    {
        pw_stream_queue_buffer(data->stream, pwBuffer);
        return;
    }

    numSamples = spaBuffer->datas[0].chunk->size / sizeof(float);

    // Amplify & calculate RMS for this chunk
    sum = 0.0f;
    for (n = 0; n < numSamples; n++) // need stride across elements if channels < 1
    {
        samples[n] *= inputGain;
        sum += samples[n] * samples[n]; // Square
    }
    rms = sqrt(sum/static_cast<float>(numSamples)); // Root, Mean

    // Gate or write to buffer
    if (gateRMS != 0)
    {
        if (rms < gateRMS)
        {
            // This input is below the gate, check whether it
            // has been there long enough to actually cut input
            if (data->tailCounter > gateClosingTail)
            {
                rms = 0;
                data->isGating = true;
            }
            else
            {
                data->tailCounter += numSamples;
                data->buffer->writeAudioData(samples, numSamples);
                data->eStats->signalInputBufferWrite();
            }
        }
        else
        {
            // This input is above the gate
            if (data->isGating)
            {
                data->isGating = false;
                data->tailCounter = 0;
            }

            data->buffer->writeAudioData(samples, numSamples);
            data->eStats->signalInputBufferWrite();
        }
    }
    else
    {
        // Gate disabled
        data->buffer->writeAudioData(samples, numSamples);
        data->eStats->signalInputBufferWrite();
    }

    //rms = SPA_CLAMPF(max * 30, 0.0f, 39.0f);
    data->currentTicks--;
    if (data->currentTicks <= 0)
    {
        // Do an update
        data->eStats->setCurrentInputRMS(rms);
        data->currentTicks = data->ticksBetweenUpdates;
    }

    // On the capture side, this appears to recycle the buffer so
    // that it can be reused by pipewire
    pw_stream_queue_buffer(data->stream, pwBuffer);
}

// Time to process output (to speakers) data
void u_AudioProvider::onProcessOutput(void *streamStruct)
{
    pwStreamData *data = (pwStreamData*)streamStruct;
    struct pw_buffer *pwBuffer;
    struct spa_buffer *spaBuffer;
    int numSamples, n;
    float *samples, rms, sum;

    if ((pwBuffer = pw_stream_dequeue_buffer(data->stream)) == NULL)
    {
        qCritical("WARN: Pipewire output out of buffers???");
        pw_log_warn("Out of buffers: %m");
        return;
    }

    spaBuffer = pwBuffer->buffer;
    if ((samples = (float*)spaBuffer->datas[0].data) == NULL || !outStreamConnected)
    {
        // Again, sometimes a few of these callbacks fire before
        // a stream is entirely initialized. Discard this event.

        // Pipewire needs you to re-queue its buffers
        pw_stream_queue_buffer(data->stream, pwBuffer);
        return;
    }

    // Pipewire gives us an empty buffer and expects it to be
    // returned. The amount of samples it wants us to send can be
    // found in pw_buffer::requested, but in any event we should
    // NOT attempt to send more data than can be contained in
    // spa_buffer::maxsize.
    numSamples = spaBuffer->datas[0].maxsize / sizeof(float);
    //qInfo("onProcessOutput: Buffer requesting %lu of samples, can hold max %i. Write-ahead is %i.", pwBuffer->requested, numSamples, data->buffer->getWriteAheadLength());

    if (pwBuffer->requested)
    {
        numSamples = SPA_MIN(pwBuffer->requested, numSamples);
    }
    numSamples = SPA_MIN(data->buffer->getWriteAheadLength(), numSamples);
    data->buffer->readAudioData(samples, numSamples);

    // Amplify + calculate RMS for this chunk
    sum = 0.0f;

    for (n = 0; n < numSamples; n++) // need stride across elements if channels < 1
    {
        samples[n] *= outputGain;
        sum += samples[n] * samples[n]; // Square
    }
    rms = sqrt(sum/static_cast<float>(numSamples)); // Root, Mean

    data->currentTicks--;
    if (data->currentTicks <= 0)
    {
        // Do an update
        data->eStats->setCurrentOutputRMS(rms);
        data->currentTicks = data->ticksBetweenUpdates;
    }

    spaBuffer->datas[0].chunk->offset = 0;  // I thought it would be obvious we're starting at 0 but ok
    spaBuffer->datas[0].chunk->stride = sizeof(float);  // # of interleaved channels
    spaBuffer->datas[0].chunk->size = numSamples * sizeof(float);   // Is in bytes

    pw_stream_queue_buffer(data->stream, pwBuffer);
}

void u_AudioProvider::onCoreInfo(void *data, const struct pw_core_info *info)
{
    // Stub, to preserve structure if we ever need to utilize it in a
    // pw_core_event
    qDebug("!! Received an unidentified onCoreInfo event !!");
}

// Handle receipt of a pipewire object registry event - currently
// only used to receive initial list of all objects in the registry
void u_AudioProvider::onRegistryAdd(void *registryStruct, uint32_t id,
                                     uint32_t permissions,
                                     const char *type,
                                     uint32_t version,
                                     const struct spa_dict *props)
{
    pwRegistryData *data = (pwRegistryData *)registryStruct;
    const struct spa_dict_item *item;
    u_InterfaceDescriptor pw_object;
    std::string key;
    std::string val;
    std::string itemType = type;    // Ugh fuck C strings, implicit convert to std::string
    //qInfo("Registry event received.");

    if (!data->inDeviceList)
    {
        qErrnoWarning("ERROR: Pipewire notified of new input device, but list has not been initialized!");
        return;
    }
    if (!data->outDeviceList)
    {
        qErrnoWarning("ERROR: Pipewire notified of new output device, but list has not been initialized!");
        return;
    }

    if (itemType == "PipeWire:Interface:Node")
    {
        // Wish there was a more elegant way to do this
        pw_object = {-1, id, 48000, 1, 0.7, false, false, false, "", ""};

        spa_dict_for_each( item, props )
        {
            key = item->key;
            val = item->value;

            if (key == "node.description")
            {
                pw_object.device_desc = item->value;
            }
            else if (key == "node.name")
            {
                pw_object.device_name = item->value;
            }
            else if (key == "media.class")
            {
                if (val == "Audio/Sink")
                {
                    pw_object.isSink = true;
                }
                else if (val == "Audio/Source")
                {
                    pw_object.isSource = true;
                }
            }
        }

        // Future reminder: this callback gets triggered *by the pipewire thread*,
        // so, while keeping it fast/clean isn't as critical as it is with the stream
        // callbacks, we really shouldn't be doing big lookups/sorts/allocations here.
        //
        // Just signal the main thread to figure out whether the device change means
        // that list entries need juggling or streams need to be started/stopped, etc.

        if (pw_object.isSource)
        {
            pw_object.internal_id = data->inDeviceList->size();    // Assign next index
            data->inDeviceList->push_back(pw_object);

            if (!data->firstRun)
            {
                // Signal new devices on the fly only after initialization is complete
                data->signaler->inputDeviceAdded(pw_object.internal_id, pw_object.device_desc);
                qInfo("New input device: %s", pw_object.device_desc.c_str());
            }
            //qInfo("Discovered source, object id %u:\n\tNode Desc: %s\n\tNode Name: %s", id, pw_object.device_desc.c_str(), pw_object.device_name.c_str());
        }
        else if( pw_object.isSink )
        {
            pw_object.internal_id = data->outDeviceList->size();   // Assign next index
            data->outDeviceList->push_back(pw_object);

            if (!data->firstRun)
            {
                // Signal new devices on the fly only after initialization is complete
                data->signaler->outputDeviceAdded(pw_object.internal_id, pw_object.device_desc);
                qInfo("New output device: %s", pw_object.device_desc.c_str());
            }
            //qInfo("Discovered sink, object id %u:\n\tNode Desc: %s\n\tNode Name: %s", id, pw_object.device_desc.c_str(), pw_object.device_name.c_str());
        }
    }
}

void u_AudioProvider::onRegistryRemove(void *registryStruct, uint32_t id)
{
    pwRegistryData *data = (pwRegistryData *)registryStruct;
    int removedIndex = -1;
    bool fromInputList = false;
    int i;

    // Future reminder: this callback gets triggered *by the pipewire thread*,
    // so, while keeping it fast/clean isn't as critical as it is with the stream
    // callbacks, we really shouldn't be doing big lookups/sorts/allocations here.
    //
    // This one is a little more complicated because we do need to remove
    // the device by its pipewire node ID, and not its list index, but still:
    // just get it done in O(n) time and signal the main thread to figure out
    // what to do next.

    for (i = 0; i < data->inDeviceList->size(); i++)
    {
        if (id == data->inDeviceList->at(i).pw_device_id)
        {
            removedIndex = i;
            qInfo("Input device removed: %s", data->inDeviceList->at(i).device_desc.c_str());
            data->inDeviceList->erase(data->inDeviceList->begin() + i);
            fromInputList = true;
            break;
        }
    }
    if (removedIndex < 0)
    {
        for (i = 0; i < data->outDeviceList->size(); i++)
        {
            if (id == data->outDeviceList->at(i).pw_device_id)
            {
                removedIndex = i;
                qInfo("Output device removed: %s", data->outDeviceList->at(i).device_desc.c_str());
                data->outDeviceList->erase(data->outDeviceList->begin() + i);
                break;
            }
        }
    }

    if (removedIndex >= 0)
    {
        // An item was removed from one of the lists.
        if (fromInputList)
        {
            data->signaler->inputDeviceRemoved(removedIndex);
        }
        else
        {
            data->signaler->outputDeviceRemoved(removedIndex);
        }
    }
}

void u_AudioProvider::onInputStreamChanged(void *streamStruct, enum pw_stream_state oldState, enum pw_stream_state newState, const char *err)
{
    pwStreamData *data = (pwStreamData *)streamStruct;
    data->isRunning = false; // Ensure only true if streaming

    if (newState == PW_STREAM_STATE_ERROR)
    {
        qWarning("WARNING: Input stream error: %s", err);
        inStreamConnected = false;
    }
    else if (newState == PW_STREAM_STATE_UNCONNECTED)
    {
        //qInfo("Input stream disconnected. %s", err);
        inStreamConnected = false;
        data->eStats->streamStopped();
    }
    else if (newState == PW_STREAM_STATE_CONNECTING)
    {
        qInfo("Input stream connecting... %s", err);
        data->eStats->streamConnecting();
    }
    else if (newState == PW_STREAM_STATE_PAUSED)
    {
        // "Paused" state will occur after buffer negotiation
        // is complete but before any data is exchanged.
        // May also land here on failure to negotiate
        // requested sample rate? (check into)
        //qInfo("Input stream paused. %s", err);
        //if (oldState == PW_STREAM_STATE_STREAMING)
        //{
        //inStreamConnected = false;
        //data->isRunning = false;
        //}
    }
    else if (newState == PW_STREAM_STATE_STREAMING)
    {
        // Data transport has started

        inStreamConnected = true;
        data->isRunning = true;
        if (currentOutputDevice == nullptr || (currentOutputDevice != nullptr && inStreamConnected))
        {
            data->eStats->streamStarted();
        }
        qInfo("Input stream successfully connected. %s", err);
    }
}

void u_AudioProvider::onOutputStreamChanged(void *streamStruct, enum pw_stream_state oldState, enum pw_stream_state newState, const char *err)
{
    pwStreamData *data = (pwStreamData *)streamStruct;
    data->isRunning = false; // Ensure only true if streaming

    if (newState == PW_STREAM_STATE_ERROR)
    {
        qWarning("WARNING: Output stream error: %s", err);
        outStreamConnected = false;
    }
    else if (newState == PW_STREAM_STATE_UNCONNECTED)
    {
        //qInfo("Output stream disconnected. %s", err);
        outStreamConnected = false;
    }
    else if (newState == PW_STREAM_STATE_CONNECTING)
    {
        qInfo("Output stream connecting... %s", err);
    }
    else if (newState == PW_STREAM_STATE_PAUSED)
    {
        // "Paused" state will occur after buffer negotiation
        // is complete but before any data is exchanged.
        //qInfo("Output stream paused. %s", err);
        //if (oldState == PW_STREAM_STATE_STREAMING)
        //{
            //outStreamConnected = false;
            //data->isRunning = false;
        //}
    }
    else if (newState == PW_STREAM_STATE_STREAMING)
    {
        // Data transport has started

        outStreamConnected = true;
        data->isRunning = true;
        if (currentInputDevice == nullptr || (currentInputDevice != nullptr && inStreamConnected))
        {
            data->eStats->streamStarted();
        }
        qInfo("Output stream successfully connected. %s", err);
    }
}

// Core sends a "done" event when pw_core_sync is called to request
// one in sequence. Currently we are using this to discover when
// we have finished receiving registry updates, and stop the main_loop.
void u_AudioProvider::onCoreDone(void *data, uint32_t id, int seq)
{
    struct pwRoundtripData *d = (pwRoundtripData*)data;
    int i;

    if (id == PW_ID_CORE && seq == d->pending)
    {       
        qInfo("Completed query of audio object registry.");

        // We previously called pw_thread_loop_wait - send a signal
        // to stop blocking
        pw_thread_loop_signal(d->threadLoop, false);
        d->registryStruct->signaler->deviceListsReady();
        d->registryStruct->firstRun = false;
    }
    else
    {
        qCritical("!! Received an unidentified onCoreDone event !!");
    }
}

