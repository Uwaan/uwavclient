#ifndef U_ENGINESTATISTICS_H
#define U_ENGINESTATISTICS_H

#include <QObject>

// This started as a general statistics-handling object
// but has kind of morphed into a wrapper for Qt
// signals instead
class u_EngineStatistics : public QObject
{
    Q_OBJECT

    // Q_OBJECT macro leaves us in a 'private' block
    float currentInputPeak;
    float currentOutputPeak;

public:
    u_EngineStatistics();
    void inputBufferWritten(int newDepth);
    void setCurrentInputRMS(float value);
    void setCurrentOutputRMS(float value);

public slots:
    void setHostProcessorUsage(float value);
    void setHostMemoryUsage(int value);
    void setServerProcessorUsage(float value);
    void setServerMemoryUsage(int value);
    void setServerGfxUsage(float value);
    void setServerGfxMemUsage(int value);

signals:
    void currentInputRMSChangedTo(float value);
    void currentOutputRMSChangedTo(float value);
    void signalInputBufferWrite();
    void deviceListsReady();
    void inputDeviceAdded(int index, std::string name);
    void outputDeviceAdded(int index, std::string name);
    void inputDeviceRemoved(int index);
    void outputDeviceRemoved(int index);
    void streamStarted();
    void streamStopped();
    void streamConnecting();
};

#endif // U_ENGINESTATISTICS_H
