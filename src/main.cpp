#include <QApplication>
#include <QStyleFactory>
#include <pipewire/pipewire.h>
#include "u_audioprovider.h"
#include "uwavclient.h"

UwaVClient *uvc = nullptr;
QtMessageHandler oldMessageHandler = nullptr;

// Wrapper function for catching qt messages & displaying them in the client -
// declared here because we can't pass a reference to a non-static member function
void LogMessageToWindow(QtMsgType messageType, const QMessageLogContext &context, const QString &msg)
{
    if(uvc)
    {
        // NOTE: This function may get triggered by other threads
        // (audio, websockets) so make sure it is marshaled back
        // onto Qt's main thread.
        QMetaObject::invokeMethod(uvc, [messageType, msg]()
            {
                // Not forwarding the context because something something
                // lambda functions and it contains char pointers that
                // probably wont point to the right place when this is
                // invoked?
                uvc->AppendMessage(messageType, msg);
            }, Qt::QueuedConnection);
    }

    // Also pass the message on to whatever logging facility we may have taken
    // control away from.
    if (oldMessageHandler)
    {
        oldMessageHandler(messageType, context, msg);
    }
}

// Wrapper function for catching sio messages & displaying them in the
// client - same drill
void RegisterSioErrorHandlers()
{
    if(uvc)
    {
        // FYI SIOCLIENT CREATES THREADS.
        /*
        void signalSioConnectRetry();
        void signalSioConnectSuccess();
        void signalSioConnectClose(sio::client::close_reason const &closeReason);
        void signalSioSocketOpen(std::string const &nSpace);
        void signalSioSocketClose(std::string const &nSpace);
        void signalSioErrorMsg(sio::message::ptr const &message);
        */
        uvc->GetSioHandler()->set_fail_listener(std::bind(&UwaVClient::signalSioConnectFail, uvc));
        uvc->GetSioHandler()->set_reconnecting_listener(std::bind(&UwaVClient::signalSioConnectRetry, uvc));
        uvc->GetSioHandler()->set_open_listener(std::bind(&UwaVClient::signalSioConnectSuccess, uvc));
        uvc->GetSioHandler()->set_close_listener(std::bind(&UwaVClient::signalSioConnectClose, uvc, std::placeholders::_1));
        uvc->GetSioHandler()->set_socket_open_listener(std::bind(&UwaVClient::signalSioSocketOpen, uvc, std::placeholders::_1));
        uvc->GetSioHandler()->set_socket_close_listener(std::bind(&UwaVClient::signalSioSocketClose, uvc, std::placeholders::_1));

        //uvc->GetSioHandler()->socket()->on_error(&UwaVClient::onSioErrorMsg,uvc);
    }
}

int main(int argc, char *argv[])
{
    oldMessageHandler = qInstallMessageHandler(LogMessageToWindow);
    QApplication a(argc, argv);
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    UwaVClient w;
    uvc = &w;
    w.show();

    w.ui_ServerDisconnect();
    //w.LoadSettings();

    // pw_init really needs argc and argv for some reason
    w.RegisterAudioProvider(new u_AudioProvider(argc, argv));
    RegisterSioErrorHandlers();

    return a.exec();
}
