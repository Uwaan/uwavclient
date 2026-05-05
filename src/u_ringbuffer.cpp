#include <cstring>
#include <QDebug>
#include "u_ringbuffer.h"

// Output buffer pre-fill (in seconds) to prevent stuttering
// in the event of input/network/inference timing jitter
// Hoping we can get away with a really low value
#ifndef U_OUTPUT_PREFILL
#define U_OUTPUT_PREFILL 0.005
#endif

// A simple ring buffer - intended for single producer, single
// consumer use. Will return zero-filled structure on buffer
// underflow, and drop overflows.
u_RingBuffer::u_RingBuffer(int sampleRate)
{
    this->sampleRate = sampleRate;
    prefillNumSamples = static_cast<int>(sampleRate * U_OUTPUT_PREFILL);

    readPos = 0;
    writePos = 0;
    writeAheadWindow = 0;

    // Maximum ui-settable chunk size is 500ms - set the buffer to
    // ensure we always have at least that much
    bufferSize = (sampleRate / 2) + prefillNumSamples;
    buffer = new char[bufferSize * sizeof(float)];

    // Initialize buffer
    for (int i = 0; i < bufferSize * (int)sizeof(float); i++)
    {
        buffer[i] = 0;
    }

    // This is a bit cheap but we do want the warnings
    // to fire immediately the first time, before
    // accumulating a count and rate limiting warnings
    readsSinceLastWarn = sampleRate;
    writesSinceLastWarn = sampleRate;
}

u_RingBuffer::~u_RingBuffer()
{
    delete(buffer);
}

void u_RingBuffer::writeAudioData(const float *samples, int count)
{
    // TODO: assert() in main to make sure program only runs if float is 32 bit
    int writeToEnd;

    if (writesSinceLastWarn < sampleRate)
    {
        writesSinceLastWarn += count;
    }

    if (writePos + count <= bufferSize)
    {
        // Easy
        if (writeAheadWindow + count <= bufferSize)
        {
            memcpy(&buffer[writePos * sizeof(float)], samples, count * sizeof(float));
            writePos += count;
            writeAheadWindow += count;

            if (writePos == bufferSize)
            {
                writePos = 0;
            }
            if (!prefillComplete && writeAheadWindow >= prefillNumSamples)
            {
                prefillComplete = true;
            }
        }
        else
        {
            // Error state - buffer overflow

            if (writesSinceLastWarn >= sampleRate)
            {
                qWarning("Unable to process pipewire data into buffer! (Overflow)");
                writesSinceLastWarn = 0;
            }
            return;
        }
    }
    else
    {
        // This is a wraparound
        if (writeAheadWindow + count <= bufferSize)
        {
            writeToEnd = bufferSize - writePos;
            memcpy(&buffer[writePos * sizeof(float)], samples, writeToEnd * sizeof(float));
            memcpy(&buffer[0], &samples[writeToEnd], (count - writeToEnd) * sizeof(float));
            writePos = count - writeToEnd;
            writeAheadWindow += count;

            if (!prefillComplete && writeAheadWindow >= prefillNumSamples)
            {
                prefillComplete = true;
            }
        }
        else
        {
            // Error state - buffer overflow
            if (writesSinceLastWarn >= sampleRate)
            {
                qWarning("Unable to process pipewire data into buffer! (Overflow)");
                writesSinceLastWarn = 0;
            }
            return;
        }
    }
}

void u_RingBuffer::readAudioData(float *samples, int count)
{
    // TODO: assert() in main to make sure program only runs if float is 32 bit
    int readToEnd;

    if (readsSinceLastWarn < sampleRate)
    {
        readsSinceLastWarn += count;
    }

    if (count > writeAheadWindow || !prefillComplete)
    {
        // Buffer underflow. We don't want a looping buffer to sound like a
        // buzzsaw, so send an array of 0s without advancing the read head.
        // This will silence the audio stream for a moment, increasing round
        // trip latency a bit to give the write head more time to advance.
        for (int i = 0; i < count; i++)
        {
            samples[i] = 0.0;
        }

        if (readsSinceLastWarn >= sampleRate && prefillComplete)
        {
            // Suppressing warning for now.
            //qWarning("Unable to provide pipewire data from buffer! (Underflow)");
            readsSinceLastWarn = 0;
        }
    }
    else if (readPos + count <= bufferSize)
    {
        // Easy
        memcpy(samples, &buffer[readPos * sizeof(float)], count * sizeof(float));
        readPos += count;
        writeAheadWindow -= count;

        if (readPos == bufferSize)
        {
            readPos = 0;
        }

        if (writeAheadWindow <= 0)
        {
            // If buffer runs out (or is exhausted intentionally), run
            // a new pre-fill for this output
            prefillComplete = false;
            //qWarning("Output buffer empty; resetting. This will result in a pause.");
        }
    }
    else
    {
        // This is a wraparound
        readToEnd = bufferSize - readPos;
        memcpy(samples, &buffer[readPos * sizeof(float)], readToEnd * sizeof(float));
        memcpy(&samples[readToEnd], &buffer[0], (count - readToEnd) * sizeof(float));
        readPos = count - readToEnd;
        writeAheadWindow -= count;

        if (writeAheadWindow <= 0)
        {
            // If buffer runs out (or is exhausted intentionally), run
            // a new pre-fill for this output
            prefillComplete = false;
            //qWarning("Output buffer empty; resetting. This will result in a pause.");
        }
    }
}

const std::shared_ptr<const std::string> u_RingBuffer::readRawData(int count)
{
    int readToEnd;
    std::shared_ptr<std::string> binData;
    binData = std::make_shared<std::string>(count * sizeof(float), '\0');

    if (readsSinceLastWarn < sampleRate)
    {
        readsSinceLastWarn += count;
    }

    if (count > writeAheadWindow)
    {
        // Buffer underflow, see note on base function I'm not gonna
        // copy paste it

        // No longer needed? Initialized 0s from start
        //for (int i = 0; i < count; i++)
        //{
        //    binData->data()[i] = 0;
        //}

        if (readsSinceLastWarn >= sampleRate)
        {
            qWarning("Unable to provide raw data from buffer! (Underflow)");
            readsSinceLastWarn = 0;
        }

        return binData;
    }
    else if (readPos + count <= bufferSize)
    {
        // Easy
        memcpy(binData->data(), &buffer[readPos * sizeof(float)], count * sizeof(float));
        readPos += count;
        writeAheadWindow -= count;

        // NOTE: Not honoring pre-fill here because RawData just needs to go out
        // as fast as possible - jitter will be accounted for at the readAudioData
        // endpoint
    }
    else
    {
        readToEnd = bufferSize - readPos;
        memcpy(binData->data(), &buffer[readPos * sizeof(float)], readToEnd * sizeof(float));
        memcpy(binData->data() + readToEnd * sizeof(float), &buffer[0], (count - readToEnd) * sizeof(float));
        readPos = count - readToEnd;
        writeAheadWindow -= count;

        // NOTE: Not honoring pre-fill here because RawData just needs to go out
        // as fast as possible - jitter will be accounted for at the readAudioData
        // endpoint
    }

    return binData;
}

void u_RingBuffer::writeRawData(const std::shared_ptr<const std::string> &binData, int count)
{
    int writeToEnd;

    if (writesSinceLastWarn < sampleRate)
    {
        writesSinceLastWarn += count;
    }

    if (writeAheadWindow + count > bufferSize)
    {
        // Buffer overflow

        if (writesSinceLastWarn >= sampleRate)
        {
            qWarning("Unable to write raw data to buffer! (Overflow)");
            writesSinceLastWarn = 0;
        }
        return;
    }

    if (writePos + count <= bufferSize)
    {
        // Easy
        memcpy(&buffer[writePos * sizeof(float)], binData->data(), count * sizeof(float));

        writePos += count;
        writeAheadWindow += count;

        if (writePos == bufferSize)
        {
            writePos = 0;
        }

        if (!prefillComplete && writeAheadWindow >= prefillNumSamples)
        {
            prefillComplete = true;
        }
    }
    else
    {
        // Wraparound 2X WRITE COMBO
        writeToEnd = bufferSize - writePos;
        memcpy(&buffer[writePos * sizeof(float)], binData->data(), writeToEnd * sizeof(float));
        memcpy(&buffer[0], binData->data() + writeToEnd * sizeof(float), (count - writeToEnd) * sizeof(float));
        writePos = count - writeToEnd;
        writeAheadWindow += count;

        if (!prefillComplete && writeAheadWindow >= prefillNumSamples)
        {
            prefillComplete = true;
        }
    }
}

void u_RingBuffer::discardSamples(int count)
{
    // Nearly the same as a read operation, but without returning
    // anything - just move the read head

    if (count > writeAheadWindow)
    {
        // Not really a buffer underflow in this circumstance, I guess
        readPos = 0;
        writePos = 0;
        writeAheadWindow = 0;
        prefillComplete = false;
    }
    else if (readPos + count <= bufferSize)
    {
        // Easy
        readPos += count;
        writeAheadWindow -= count;

        if (readPos == bufferSize)
        {
            readPos = 0;
        }

        if (writeAheadWindow <= 0)
        {
            prefillComplete = false;
        }
    }
    else
    {
        // This is a wraparound
        int readToEnd;
        readToEnd = bufferSize - readPos;
        readPos = count - readToEnd;
        writeAheadWindow -= count;

        if (writeAheadWindow <= 0)
        {
            prefillComplete = false;
        }
    }
}

int u_RingBuffer::getWriteAheadLength()
{
    return writeAheadWindow;
}
