/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    MsQuic Connection Wrapper

--*/

#include "precomp.h"

TestConnection::TestConnection(
    _In_ HQUIC Handle,
    _In_ NEW_STREAM_CALLBACK_HANDLER NewStreamCallbackHandler,
    _In_ bool Server,
    _In_ bool AutoDelete,
    _In_ bool UseSendBuffer
    ) :
    QuicConnection(Server ? Handle : nullptr),
    Context(nullptr), IsServer(Server), IsStarted(Server), IsConnected(false), Resumed(false),
    PeerAddrChanged(false), PeerClosed(false), ExpectedResumed(false),
    ExpectedTransportCloseStatus(QUIC_STATUS_SUCCESS),
    ExpectedPeerCloseErrorCode(QUIC_TEST_NO_ERROR),
    TransportClosed(false), IsShutdown(false),
    ShutdownTimedOut(false), AutoDelete(AutoDelete),
    NewStreamCallback(NewStreamCallbackHandler), ShutdownCompleteCallback(nullptr),
    UseSendBuffer(UseSendBuffer)
{
    QuicEventInitialize(&EventConnectionComplete, TRUE, FALSE);
    QuicEventInitialize(&EventPeerClosed, TRUE, FALSE);
    QuicEventInitialize(&EventShutdownComplete, TRUE, FALSE);

    if (IsServer) {
        if (QuicConnection == nullptr) {
            TEST_FAILURE("Invalid handle passed into TestConnection.");
        } else {
            MsQuic->SetCallbackHandler(QuicConnection, (void*)QuicConnectionHandler, this);
        }
    } else {
        QUIC_STATUS Status =
            MsQuic->ConnectionOpen(
                Handle,
                QuicConnectionHandler,
                this,
                &QuicConnection);
        if (QUIC_FAILED(Status)) {
            TEST_FAILURE("MsQuic->ConnectionOpen failed, 0x%x.", Status);
            QuicConnection = nullptr;
        }

        BOOLEAN Opt = UseSendBuffer;
        Status =
            MsQuic->SetParam(
                QuicConnection,
                QUIC_PARAM_LEVEL_CONNECTION,
                QUIC_PARAM_CONN_SEND_BUFFERING,
                sizeof(Opt),
                (uint8_t*)&Opt);
        if (QUIC_FAILED(Status)) {
            TEST_FAILURE("MsQuicSetParam(SEND_BUFFERING) failed, 0x%x.", Status);
        }
    }

    //
    // Test code uses self-signed certificates, so we cannot validate the root.
    //
    SetCertValidationFlags(
        QUIC_CERTIFICATE_FLAG_IGNORE_UNKNOWN_CA |
        QUIC_CERTIFICATE_FLAG_IGNORE_CERTIFICATE_CN_INVALID);
}

TestConnection::~TestConnection()
{
    MsQuic->ConnectionClose(QuicConnection);
    QuicEventUninitialize(EventShutdownComplete);
    QuicEventUninitialize(EventPeerClosed);
    QuicEventUninitialize(EventConnectionComplete);
}

QUIC_STATUS
TestConnection::Start(
    _In_ QUIC_ADDRESS_FAMILY Family,
    _In_opt_z_ const char* ServerName,
    _In_ uint16_t ServerPort // Host byte order
    )
{
    QUIC_STATUS Status;
    if (QUIC_SUCCEEDED(
        Status = MsQuic->ConnectionStart(
            QuicConnection,
            Family,
            ServerName,
            ServerPort))) {
        IsStarted = true;
        return Status;
    }
    return Status;
}

void
TestConnection::Shutdown(
    _In_ QUIC_CONNECTION_SHUTDOWN_FLAGS Flags,
    _In_ QUIC_UINT62 ErrorCode
    )
{
    MsQuic->ConnectionShutdown(
        QuicConnection,
        Flags,
        ErrorCode);
}

TestStream*
TestConnection::NewStream(
    _In_opt_ STREAM_SHUTDOWN_CALLBACK_HANDLER StreamShutdownHandler,
    _In_ QUIC_STREAM_OPEN_FLAGS Flags
    )
{
    return TestStream::FromConnectionHandle(QuicConnection, StreamShutdownHandler, Flags);
}

bool
TestConnection::WaitForConnectionComplete()
{
    if (!QuicEventWaitWithTimeout(EventConnectionComplete, TestWaitTimeout)) {
        TEST_FAILURE("WaitForConnectionComplete timed out after %u ms.", TestWaitTimeout);
        return false;
    }
    return true;
}

bool
TestConnection::WaitForZeroRttTicket()
{
    uint32_t TryCount = 0;
    while (TryCount++ < 20) {
        if (HasNewZeroRttTicket()) {
            break;
        }
        QuicSleep(100);
    }
    if (TryCount == 20) {
        TEST_FAILURE("WaitForZeroRttTicket failed.");
        return false;
    }
    return true;
}

bool
TestConnection::WaitForShutdownComplete()
{
    if (IsStarted) {
        if (!QuicEventWaitWithTimeout(EventShutdownComplete, TestWaitTimeout)) {
            TEST_FAILURE("WaitForShutdownComplete timed out after %u ms.", TestWaitTimeout);
            return false;
        }
    }
    return true;
}

bool
TestConnection::WaitForPeerClose()
{
    if (!QuicEventWaitWithTimeout(EventPeerClosed, TestWaitTimeout)) {
        TEST_FAILURE("WaitForPeerClose timed out after %u ms.", TestWaitTimeout);
        return false;
    }
    return true;
}

//
// Connection Parameters
//

QUIC_STATUS
TestConnection::ForceKeyUpdate()
{
    QUIC_STATUS Status;
    uint32_t Try = 0;

    do {
        //
        // Forcing a key update is only allowed when the handshake is confirmed.
        // So, even if the caller waits for connection complete, it's possible
        // the call can fail with QUIC_STATUS_INVALID_STATE. To get around this
        // we allow for a couple retries (with some sleeps).
        //
        if (Try != 0) {
            QuicSleep(100);
        }
        Status =
            MsQuic->SetParam(
                QuicConnection,
                QUIC_PARAM_LEVEL_CONNECTION,
                QUIC_PARAM_CONN_FORCE_KEY_UPDATE,
                0,
                nullptr);

    } while (Status == QUIC_STATUS_INVALID_STATE && ++Try <= 3);

    return Status;
}

QUIC_STATUS
TestConnection::ForceCidUpdate()
{
    QUIC_STATUS Status;
    uint32_t Try = 0;

    do {
        //
        // Forcing a CID update is only allowed when the handshake is confirmed.
        // So, even if the caller waits for connection complete, it's possible
        // the call can fail with QUIC_STATUS_INVALID_STATE. To get around this
        // we allow for a couple retries (with some sleeps).
        //
        if (Try != 0) {
            QuicSleep(100);
        }
        Status =
            MsQuic->SetParam(
                QuicConnection,
                QUIC_PARAM_LEVEL_CONNECTION,
                QUIC_PARAM_CONN_FORCE_CID_UPDATE,
                0,
                nullptr);

    } while (Status == QUIC_STATUS_INVALID_STATE && ++Try <= 3);

    return Status;
}

QUIC_STATUS
TestConnection::SetTestTransportParameter(
    _In_ const QUIC_PRIVATE_TRANSPORT_PARAMETER* TransportParameter
    )
{
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_TEST_TRANSPORT_PARAMETER,
            sizeof(*TransportParameter),
            TransportParameter);
}

uint32_t
TestConnection::GetQuicVersion()
{
    uint32_t value;
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_QUIC_VERSION,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        value = 0;
        TEST_FAILURE("MsQuic->GetParam(CONN_QUIC_VERSION) failed, 0x%x.", Status);
    }
    return value;
}

QUIC_STATUS
TestConnection::SetQuicVersion(
    uint32_t value
    )
{
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_QUIC_VERSION,
            sizeof(value),
            &value);
}

QUIC_STATUS
TestConnection::GetLocalAddr(
    _Out_ QuicAddr &localAddr
    )
{
    uint32_t Size = sizeof(localAddr.SockAddr);
    return
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_LOCAL_ADDRESS,
            &Size,
            &localAddr.SockAddr);
}

QUIC_STATUS
TestConnection::SetLocalAddr(
    _In_ const QuicAddr &localAddr
    )
{
    QUIC_STATUS Status;
    uint32_t Try = 0;
    uint32_t Size = sizeof(localAddr.SockAddr);

    do {
        //
        // If setting the new local address right after handshake complete, it's
        // possible the handshake hasn't been confirmed yet, and this call will
        // fail with QUIC_STATUS_INVALID_STATE (because the client's not allowed
        // to change IP until handshake confirmation). To get around this we
        // allow for a couple retries (with some sleeps).
        //
        if (Try != 0) {
            QuicSleep(100);
        }
        Status =
            MsQuic->SetParam(
                QuicConnection,
                QUIC_PARAM_LEVEL_CONNECTION,
                QUIC_PARAM_CONN_LOCAL_ADDRESS,
                Size,
                &localAddr.SockAddr);

    } while (Status == QUIC_STATUS_INVALID_STATE && ++Try <= 3);

    return Status;
}

QUIC_STATUS
TestConnection::GetRemoteAddr(
    _Out_  QuicAddr &remoteAddr
    )
{
    uint32_t Size = sizeof(remoteAddr.SockAddr);
    return
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_REMOTE_ADDRESS,
            &Size,
            &remoteAddr.SockAddr);
}

QUIC_STATUS
TestConnection::SetRemoteAddr(
    _In_ const QuicAddr &remoteAddr
    )
{
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_REMOTE_ADDRESS,
            sizeof(remoteAddr.SockAddr),
            &remoteAddr.SockAddr);
}

uint64_t
TestConnection::GetIdleTimeout()
{
    uint64_t value;
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_IDLE_TIMEOUT,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        value = 0;
        TEST_FAILURE("MsQuic->GetParam(CONN_IDLE_TIMEOUT) failed, 0x%x.", Status);
    }
    return value;
}

QUIC_STATUS
TestConnection::SetIdleTimeout(
    uint64_t value
    )
{
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_IDLE_TIMEOUT,
            sizeof(value),
            &value);
}

uint32_t
TestConnection::GetDisconnectTimeout()
{
    uint32_t value;
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_DISCONNECT_TIMEOUT,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        value = 0;
        TEST_FAILURE("MsQuic->GetParam(CONN_DISCONNECT_TIMEOUT) failed, 0x%x.", Status);
    }
    return value;
}

QUIC_STATUS
TestConnection::SetDisconnectTimeout(
    uint32_t value
    )
{
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_DISCONNECT_TIMEOUT,
            sizeof(value),
            &value);
}

uint16_t
TestConnection::GetPeerBidiStreamCount()
{
    uint16_t value;
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_PEER_BIDI_STREAM_COUNT,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        value = 0;
        TEST_FAILURE("MsQuic->GetParam(CONN_PEER_BIDI_STREAM_COUNT) failed, 0x%x.", Status);
    }
    return value;
}

QUIC_STATUS
TestConnection::SetPeerBidiStreamCount(
    uint16_t value
    )
{
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_PEER_BIDI_STREAM_COUNT,
            sizeof(value),
            &value);
}

uint16_t
TestConnection::GetPeerUnidiStreamCount()
{
    uint16_t value;
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_PEER_UNIDI_STREAM_COUNT,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        value = 0;
        TEST_FAILURE("MsQuic->GetParam(CONN_PEER_UNIDI_STREAM_COUNT) failed, 0x%x.", Status);
    }
    return value;
}

QUIC_STATUS
TestConnection::SetPeerUnidiStreamCount(
    uint16_t value
    )
{
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_PEER_UNIDI_STREAM_COUNT,
            sizeof(value),
            &value);
}

uint16_t
TestConnection::GetLocalBidiStreamCount()
{
    uint16_t value;
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        value = 0;
        TEST_FAILURE("MsQuic->GetParam(CONN_LOCAL_BIDI_STREAM_COUNT) failed, 0x%x.", Status);
    }
    return value;
}

uint16_t
TestConnection::GetLocalUnidiStreamCount()
{
    uint16_t value;
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        value = 0;
        TEST_FAILURE("MsQuic->GetParam(CONN_LOCAL_UNIDI_STREAM_COUNT) failed, 0x%x.", Status);
    }
    return value;
}

QUIC_STATISTICS
TestConnection::GetStatistics()
{
    QUIC_STATISTICS value = {};
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_STATISTICS,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        TEST_FAILURE("MsQuic->GetParam(CONN_STATISTICS) failed, 0x%x.", Status);
    }
    return value;
}

uint32_t
TestConnection::GetCertValidationFlags()
{
    BOOLEAN value;
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_CERT_VALIDATION_FLAGS,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        value = 0;
        TEST_FAILURE("MsQuic->GetParam(CONN_CERT_VALIDATION_FLAGS) failed, 0x%x.", Status);
    }
    return value;
}

QUIC_STATUS
TestConnection::SetCertValidationFlags(
    uint32_t value
    )
{
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_CERT_VALIDATION_FLAGS,
            sizeof(value),
            &value);
}

uint32_t
TestConnection::GetKeepAlive()
{
    uint32_t value;
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_KEEP_ALIVE,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        value = 0;
        TEST_FAILURE("MsQuic->GetParam(CONN_KEEP_ALIVE) failed, 0x%x.", Status);
    }
    return value;
}

QUIC_STATUS
TestConnection::SetKeepAlive(
    uint32_t value
    )
{
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_KEEP_ALIVE,
            sizeof(value),
            &value);
}

bool
TestConnection::GetShareUdpBinding()
{
    BOOLEAN value;
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_SHARE_UDP_BINDING,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        value = 0;
        TEST_FAILURE("MsQuic->GetParam(CONN_SHARE_UDP_BINDING) failed, 0x%x.", Status);
    }
    return value != FALSE;
}

QUIC_STATUS
TestConnection::SetShareUdpBinding(
    bool value
    )
{
    BOOLEAN bValue = value ? TRUE : FALSE;
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_SHARE_UDP_BINDING,
            sizeof(bValue),
            &bValue);
}

QUIC_STREAM_SCHEDULING_SCHEME
TestConnection::GetPriorityScheme()
{
    QUIC_STREAM_SCHEDULING_SCHEME value;
    uint32_t valueSize = sizeof(value);
    QUIC_STATUS Status =
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_STREAM_SCHEDULING_SCHEME,
            &valueSize,
            &value);
    if (QUIC_FAILED(Status)) {
        value = QUIC_STREAM_SCHEDULING_SCHEME_FIFO;
        TEST_FAILURE("MsQuic->GetParam(CONN_PRIORITY_SCHEME) failed, 0x%x.", Status);
    }
    return value;
}

QUIC_STATUS
TestConnection::SetPriorityScheme(
    QUIC_STREAM_SCHEDULING_SCHEME value
    )
{
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_STREAM_SCHEDULING_SCHEME,
            sizeof(value),
            &value);
}

QUIC_STATUS
TestConnection::SetSecurityConfig(
    QUIC_SEC_CONFIG* value
    )
{
    return
        MsQuic->SetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_SEC_CONFIG,
            sizeof(value),
            &value);
}

bool
TestConnection::HasNewZeroRttTicket()
{
    uint32_t ResumptionStateLength = 0;
    return
        QUIC_STATUS_BUFFER_TOO_SMALL ==
        MsQuic->GetParam(
            QuicConnection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_RESUMPTION_STATE,
            &ResumptionStateLength,
            nullptr);
}

QUIC_STATUS
TestConnection::HandleConnectionEvent(
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    switch (Event->Type) {

    case QUIC_CONNECTION_EVENT_CONNECTED:
        IsConnected = true;
        Resumed = Event->CONNECTED.SessionResumed != FALSE;
        if (!Resumed && ExpectedResumed) {
            TEST_FAILURE("Resumption was expected!");
        }
        QuicEventSet(EventConnectionComplete);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        TransportClosed = true;
        TransportCloseStatus = Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status != ExpectedTransportCloseStatus) {
            TEST_FAILURE("Unexpected transport Close Error, %u", Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        }
        QuicEventSet(EventConnectionComplete);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        PeerClosed = true;
        PeerCloseErrorCode = Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode;
        if (Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode != ExpectedPeerCloseErrorCode) {
            TEST_FAILURE("App Close Error, %llu", Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        }
        QuicEventSet(EventConnectionComplete);
        QuicEventSet(EventPeerClosed);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        IsShutdown = TRUE;
        ShutdownTimedOut = Event->SHUTDOWN_COMPLETE.PeerAcknowledgedShutdown == FALSE;
        QuicEventSet(EventShutdownComplete);
        if (ShutdownCompleteCallback) {
            ShutdownCompleteCallback(this);
        }
        if (AutoDelete) {
            delete this;
        }
        break;

    case QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED:
        PeerAddrChanged = true;
        break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        if (Event->PEER_STREAM_STARTED.Stream == nullptr) {
            TEST_FAILURE("Null Stream");
        }
        NewStreamCallback(
            this,
            Event->PEER_STREAM_STARTED.Stream,
            Event->PEER_STREAM_STARTED.Flags);
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}
