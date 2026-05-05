#include "u_enginestatistics.h"

u_EngineStatistics::u_EngineStatistics()
{
    currentInputPeak = 0.0f;
    currentOutputPeak = 0.0f;
}

void u_EngineStatistics::setCurrentInputRMS(float value)
{
    currentInputPeak = value;
    emit currentInputRMSChangedTo(value);
}

void u_EngineStatistics::setCurrentOutputRMS(float value)
{
    currentOutputPeak = value;
    emit currentOutputRMSChangedTo(value);
}

void u_EngineStatistics::inputBufferWritten(int newDepth)
{
    emit signalInputBufferWrite();
}

// TODO
void u_EngineStatistics::setHostProcessorUsage(float value) {}
void u_EngineStatistics::setHostMemoryUsage(int value) {}
void u_EngineStatistics::setServerProcessorUsage(float value) {}
void u_EngineStatistics::setServerMemoryUsage(int value) {}
void u_EngineStatistics::setServerGfxUsage(float value) {}
void u_EngineStatistics::setServerGfxMemUsage(int value) {}
