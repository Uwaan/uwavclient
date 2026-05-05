#ifndef U_INTERFACEDESCRIPTOR_H
#define U_INTERFACEDESCRIPTOR_H

#include <cstdint>
#include <string>

struct u_InterfaceDescriptor {
    int internal_id = -1;
    uint32_t pw_device_id;
    int sampleRate = 44100;
    int channels = 1;
    float volume = 0.7;

    bool isSource;
    bool isSink;
    bool isOrphan;
    // pipewire is programmed in C and it might make more sense to use
    // C strings here.... but a conversion exists and it's just easier
    // to work with std::string and that's what I'm gonna do

    // If this does end up switching to C strings, the longest device
    // desc in my PC is 121 chars and the longest device name is
    // 89 chars
    std::string device_name;
    std::string device_desc;
};


#endif // U_INTERFACEDESCRIPTOR_H
