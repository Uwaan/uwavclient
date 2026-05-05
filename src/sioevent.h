#ifndef SIOEVENT_H
#define SIOEVENT_H

#include <string>
#include "sio/sio_client.h"

class SioEvent
{
    const std::string &get_nsp() const;
    const std::string &get_name() const;
    const sio::message::ptr &get_message() const;
    bool need_ack() const;
    void put_ack_message(sio::message::ptr const &ack_message);
    sio::message::ptr const &get_ack_message() const;
};

#endif // SIOEVENT_H
