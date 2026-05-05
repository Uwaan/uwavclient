#ifndef U_RINGBUFFER_H
#define U_RINGBUFFER_H
#include <atomic>
#include <memory>
#include <string>

// A simple ringbuffer for single producer, single consumer use.
// NOTE: Internal format is a 32 bit integer because the network
// transport only understands how to package integer arrays
class u_RingBuffer
{
public:
    int bufferSize;         // # elements
    int sampleRate;
    int prefillNumSamples;
    bool prefillComplete = false;

    u_RingBuffer(int sampleRate);
    ~u_RingBuffer();

    int getWriteAheadLength();

    void writeAudioData(const float *samples, int count);
    void readAudioData(float *samples, int count);

    void writeRawData(const std::shared_ptr<const std::string> &data, int count);
    const std::shared_ptr<const std::string> readRawData(int count);

    void discardSamples(int count);

private:
    std::atomic<int> readPos;
    std::atomic<int> writePos;
    std::atomic<int> writeAheadWindow;   // # array positions that write is ahead of read
    std::atomic<int> readsSinceLastWarn;
    std::atomic<int> writesSinceLastWarn;
    char *buffer;
};

#endif // U_RINGBUFFER_H
