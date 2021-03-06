#include <OpenHome/Media/Protocol/ProtocolFactory.h>
#include <OpenHome/Media/Protocol/Protocol.h>
#include <OpenHome/Media/Protocol/ProtocolHls.h>
#include <OpenHome/Exception.h>
#include <OpenHome/Private/Debug.h>
#include <OpenHome/Types.h>
#include <OpenHome/Private/Http.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Private/Timer.h>
#include <OpenHome/Private/Uri.h>
#include <OpenHome/Media/Debug.h>
#include <OpenHome/Private/Parser.h>
#include <OpenHome/Private/Ascii.h>
#include <OpenHome/Media/Supply.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Net/Private/Globals.h>
#include <OpenHome/Os.h>
#include <OpenHome/OsWrapper.h>

#include <algorithm>

namespace OpenHome {
namespace Media {

class ProtocolHls : public Protocol
{
public:
    ProtocolHls(Environment& aEnv, const Brx& aUserAgent);
    ~ProtocolHls();
private: // from Protocol
    void Initialise(MsgFactory& aMsgFactory, IPipelineElementDownstream& aDownstream) override;
    void Interrupt(TBool aInterrupt) override;
    ProtocolStreamResult Stream(const Brx& aUri) override;
    ProtocolGetResult Get(IWriter& aWriter, const Brx& aUri, TUint64 aOffset, TUint aBytes) override;
    void Deactivated() override;
    EStreamPlay OkToPlay(TUint aStreamId) override;
    TUint TrySeek(TUint aStreamId, TUint64 aOffset) override;
    TUint TryStop(TUint aStreamId) override;
private:
    void Reinitialise();
    void StartStream(const Uri& aUri);
    TBool IsCurrentStream(TUint aStreamId) const;
    void WaitForDrain();
private:
    TimerFactory iTimerFactory;
    Supply* iSupply;
    Semaphore iSemReaderM3u;
    PlaylistProvider iPlaylistProvider;
    HlsReloadTimer iReloadTimer;
    HlsM3uReader iM3uReader;
    SegmentProvider iSegmentProvider;
    SegmentStreamer iSegmentStreamer;
    TUint iStreamId;
    TBool iStarted;
    TBool iStopped;
    ContentProcessor* iContentProcessor;
    TUint iNextFlushId;
    Semaphore iSem;
    Mutex iLock;
    //std::atomic<TBool> iStreamHasGoneBuffering;
};

};  // namespace Media
};  // namespace OpenHome

using namespace OpenHome;
using namespace OpenHome::Media;


Protocol* ProtocolFactory::NewHls(Environment& aEnv, const Brx& aUserAgent)
{ // static
    /**
     * It would be very desirable to pass references into ProtocolHls and to
     * create a wrapper around it which owns those objects passed as references.
     * However, due to Protocol being a base class, that is not possible.
     * Instead, the best that can be done, short of moving Protocol methods
     * into an IProtocol interface, is to require ProtocolHls to take ownership
     * of objects passed in.
     */
    return new ProtocolHls(aEnv, aUserAgent);
}


// HttpHeaderConnection

const Brn Media::HttpHeaderConnection::kConnectionClose("close");
const Brn Media::HttpHeaderConnection::kConnectionKeepAlive("keep-alive");
const Brn Media::HttpHeaderConnection::kConnectionUpgrade("upgrade");

TBool Media::HttpHeaderConnection::Close() const
{
    return (Received() ? iClose : false);
}

TBool Media::HttpHeaderConnection::KeepAlive() const
{
    return (Received() ? iKeepAlive : false);
}

TBool Media::HttpHeaderConnection::Upgrade() const
{
    return (Received() ? iUpgrade : false);
}

TBool Media::HttpHeaderConnection::Recognise(const Brx& aHeader)
{
    return Ascii::CaseInsensitiveEquals(aHeader, Http::kHeaderConnection);
}

void Media::HttpHeaderConnection::Process(const Brx& aValue)
{
    iClose = false;
    iKeepAlive = false;
    iUpgrade = false;
    if (Ascii::CaseInsensitiveEquals(aValue, kConnectionClose)) {
        iClose = true;
        SetReceived();
    }
    else if (Ascii::CaseInsensitiveEquals(aValue, kConnectionKeepAlive)) {
        iKeepAlive = true;
        SetReceived();
    }
    else if (Ascii::CaseInsensitiveEquals(aValue, kConnectionUpgrade)) {
        iUpgrade = true;
        SetReceived();
    }
}


// HttpSocket::ReaderUntilDynamic
    
HttpSocket::ReaderUntilDynamic::ReaderUntilDynamic(TUint aMaxBytes, IReader& aReader)    : ReaderUntil(aMaxBytes, aReader)
    , iBuf(aMaxBytes)
{
}

TByte* HttpSocket::ReaderUntilDynamic::Ptr()
{
    return const_cast<TByte*>(iBuf.Ptr());
}


// HttpSocket::Swd

HttpSocket::Swd::Swd(TUint aMaxBytes, IWriter& aWriter)
    : Swx(aMaxBytes, aWriter)
    , iBuf(aMaxBytes)
{
}

TByte* HttpSocket::Swd::Ptr()
{
    return const_cast<TByte*>(iBuf.Ptr());
}


// HttpSocket

const Brn HttpSocket::kSchemeHttp("http");

HttpSocket::HttpSocket(Environment& aEnv, const Brx& aUserAgent, TUint aReadBufferBytes, TUint aWriteBufferBytes, TUint aConnectTimeoutMs, TUint aResponseTimeoutMs, TUint aReceiveTimeoutMs, TBool aFollowRedirects)
    : iEnv(aEnv)
    , iUserAgent(aUserAgent)
    , iConnectTimeoutMs(aConnectTimeoutMs)
    , iResponseTimeoutMs(aResponseTimeoutMs)
    , iReceiveTimeoutMs(aReceiveTimeoutMs)
    , iFollowRedirects(aFollowRedirects)
    , iReadBuffer(aReadBufferBytes, iTcpClient)
    , iReaderUntil(aReadBufferBytes, iReadBuffer)
    , iReaderResponse(aEnv, iReaderUntil)
    , iWriteBuffer(aWriteBufferBytes, iTcpClient)
    , iWriterRequest(iWriteBuffer)
    , iDechunker(iReaderUntil)
    , iConnected(false)
    , iRequestHeadersSent(false)
    , iResponseReceived(false)
    , iCode(-1)
    , iContentLength(-1)
    , iBytesRemaining(-1)
    , iMethod(Http::kMethodGet)
    , iPersistConnection(true)
{
    iReaderResponse.AddHeader(iHeaderConnection);
    iReaderResponse.AddHeader(iHeaderContentLength);
    iReaderResponse.AddHeader(iHeaderLocation);
    iReaderResponse.AddHeader(iHeaderTransferEncoding);
}

HttpSocket::~HttpSocket()
{
    Disconnect();
}

void HttpSocket::SetUri(const Uri& aUri)
{
    // Check if new endpoint is same as current endpoint. If so, possible to re-use connection.

    if (aUri.Scheme() != kSchemeHttp) {
        THROW(HttpSocketUriError);
    }

    TBool baseUrlChanged = false;
    if (aUri.Scheme() != iUri.Scheme()
            || aUri.Host() != iUri.Host()
            || aUri.Port() != iUri.Port()) {
        baseUrlChanged = true;
    }
    LOG(kMedia, "HttpSocket::SetUri baseUrlChanged: %u\n\tiUri: %.*s\n\taUri: %.*s\n", baseUrlChanged, PBUF(iUri.AbsoluteUri()), PBUF(aUri.AbsoluteUri()));

    TInt port = aUri.Port();
    if (port == Uri::kPortNotSpecified) {
        port = kDefaultHttpPort;
    }

    try {
        if (baseUrlChanged) {
            // Endpoint constructor may throw NetworkError if unable to resolve host.
            const Endpoint ep(port, aUri.Host());
            // New base URL is not the same as current base URL.
            // New connection required.
            Disconnect();
            iEndpoint.Replace(ep);
        }

        LOG(kMedia, "HttpSocket::SetUri iPersistConnection: %u\n", iPersistConnection);
        if (!iPersistConnection) {
            // Previous response required that this connection not be re-used.
            // Call Disconnect() here in case, for some unknown reason, previous client of this HttpSocket didn't read until end of stream and trigger Disconnect() in the Read() method, or in case there was some error in stream length, or for any other reason.
            Disconnect();
        }

        // Set iUri here, as disconnect may have cleared it.
        try {
            iUri.Replace(aUri.AbsoluteUri());
        }
        catch (const UriError&) {
            THROW(HttpSocketUriError);
        }

        iReaderResponse.Flush();
        iDechunker.ReadFlush();
        iDechunker.SetChunked(false);
        iRequestHeadersSent = false;
        iResponseReceived = false;
        iCode = -1;
        iContentLength = -1;
        iBytesRemaining = -1;
        iPersistConnection = true;
        try {
            //iWriterRequest.WriteFlush();
            //iWriteBuffer.WriteFlush();
        }
        catch (const WriterError&) {
            // Nothing to do.
        }
    }
    catch (const NetworkError&) {
        LOG(kHttp, "HttpSocket::SetUri error setting address and port\n");
        THROW(HttpSocketUriError);
    }
}

const Brn HttpSocket::GetRequestMethod() const
{
    return iMethod;
}

void HttpSocket::SetRequestMethod(const Brx& aMethod)
{
    // FIXME - should maybe throw exception if not connected

    // Invalid operation to set this following a call to Connect().
    if (aMethod == Http::kMethodGet) {
        iMethod.Set(Http::kMethodGet);
    }
    else if (aMethod == Http::kMethodPost) {
        iMethod.Set(Http::kMethodPost);
    }
    else {
        THROW(HttpSocketMethodInvalid);
    }
}

void HttpSocket::Connect()
{
    // Underlying socket may already be open and connected if this new connection is part of an HTTP persistent connection.

    if (!iConnected) {
        // Connect.
        iTcpClient.Open(iEnv);
        try {
            LOG(kHttp, "HttpSocket::Connect connecting...\n");
            iTcpClient.Connect(iEndpoint, iConnectTimeoutMs);
        }
        catch (const NetworkTimeout&) {
            Disconnect();   // FIXME - correct, or up to caller to do?
            LOG(kHttp, "<HttpSocket::Connect caught NetworkTimeout\n");
            THROW(HttpSocketConnectionError);
        }
        catch (const NetworkError&) {
            Disconnect();   // FIXME - correct, or up to caller to do?
            LOG(kHttp, "<HttpSocket::Connect caught NetworkError\n");
            THROW(HttpSocketConnectionError);
        }

        // Not all implementations support a receive timeout.
        // So, consume exceptions from such implementations.
        try {
            iTcpClient.SetRecvTimeout(iReceiveTimeoutMs);
        }
        catch (NetworkError&) {
            LOG(kHttp, "Unable to set recv timeout of %u\n", iReceiveTimeoutMs);
        }
        iConnected = true;
        LOG(kHttp, "<HttpReader::Connect\n");
    }
}

void HttpSocket::Disconnect()
{
    LOG(kHttp, "HttpReader::Disconnect\n");
    if (iConnected) {
        iReaderResponse.Flush();
        iDechunker.ReadFlush();
        try {
            // iWriterRequest.WriteFlush();
            // iWriteBuffer.WriteFlush();
        }
        catch (const WriterError&) {
            // Nothing to do.
            LOG(kMedia, "HttpSocket::Disconnect caught WriterError\n");
        }
        iTcpClient.Close();
    }
    iDechunker.SetChunked(false);
    iConnected = false;
    iRequestHeadersSent = false;
    iResponseReceived = false;
    iCode = -1;
    iContentLength = -1;
    iBytesRemaining = -1;
    iUri.Clear();
    iPersistConnection = true;

    try {
        iEndpoint.SetAddress(0);
        iEndpoint.SetPort(0);
    }
    catch (const NetworkError&) {
    }
}

IReader& HttpSocket::GetInputStream()
{
    Connect();
    SendRequestHeaders();
    ProcessResponse();
    return *this;
}

IWriter& HttpSocket::GetOutputStream()
{
    Connect();
    return iWriterRequest;
}

TInt HttpSocket::GetResponseCode()
{
    Connect();
    SendRequestHeaders();
    ProcessResponse();
    return iCode;
}

TInt HttpSocket::GetContentLength()
{
    Connect();
    SendRequestHeaders();
    ProcessResponse();
    return iContentLength;
}

void HttpSocket::Interrupt(TBool aInterrupt)
{
    iTcpClient.Interrupt(aInterrupt);
}

Brn HttpSocket::Read(TUint aBytes)
{
    if (!iConnected || !iResponseReceived) {
        THROW(ReaderError);
    }

    TUint bytes = aBytes;
    if (iContentLength != -1) {
        // This stream has a content length, so need to keep track of bytes read to support correct IReader behaviour.
        if (iBytesRemaining < 0) {
            // Trying to read beyond end of stream.
            THROW(ReaderError);
        }
        if (iBytesRemaining == 0) {
            // End-of-stream signifier.
            iBytesRemaining = -1;

            if (!iPersistConnection) {
                // We're done reading from this stream and cannot re-use connection. Close connection and free up underlying resources.
                Disconnect();
            }

            return Brx::Empty();
        }
        if (iBytesRemaining > 0) {
            const TUint bytesRemaining = iBytesRemaining;
            if (bytesRemaining < bytes) {
                bytes = bytesRemaining;
            }
        }
    }

    ;
    try {
        Brn buf = iDechunker.Read(bytes);
        // If reading from a stream of known length, update bytes remaining.
        if (iBytesRemaining > 0) {
            iBytesRemaining -= buf.Bytes();
        }

        // Dechunker returns a buffer of 0 bytes in length when end-of-stream reached (conforming to IReader interface).
        if (buf.Bytes() == 0) {
            if (!iPersistConnection) {
                // We're done reading from this stream and cannot re-use connection. Close connection and free up underlying resources.
                Disconnect();
            }
        }

        return buf;
    }
    catch (const ReaderError&) {
        // Break in stream. Close connection.
        Disconnect();
        throw;
    }
}

void HttpSocket::ReadFlush()
{
    iDechunker.ReadFlush();
}

void HttpSocket::ReadInterrupt()
{
    iDechunker.ReadInterrupt();
}

void HttpSocket::WriteRequest(const Uri& aUri, Brx& aMethod)
{
    LOG(kMedia, ">HttpSocket::WriteRequest aUri: %.*s, aMethod: %.*s\n", PBUF(aUri.AbsoluteUri()), PBUF(aMethod));
    try {
        iWriterRequest.WriteMethod(aMethod, aUri.PathAndQuery(), Http::eHttp11);

        TInt port = aUri.Port();
        if (port == Uri::kPortNotSpecified) {
            port = kDefaultHttpPort;
        }
        Http::WriteHeaderHostAndPort(iWriterRequest, aUri.Host(), port);

        if (iUserAgent.Bytes() > 0) {
            iWriterRequest.WriteHeader(Http::kHeaderUserAgent, iUserAgent);
        }
        iWriterRequest.WriteFlush();
    }
    catch(const WriterError&) {
        LOG(kHttp, "<HttpReader::WriteRequest caught WriterError\n");
        THROW(HttpSocketRequestError);
    }
}

TUint HttpSocket::ReadResponse()
{
    try {
        iReaderResponse.Read(iResponseTimeoutMs);
    }
    catch(HttpError&) {
        LOG(kHttp, "HttpReader::ReadResponse caught HttpError\n");
        THROW(HttpSocketResponseError);
    }
    catch(ReaderError&) {
        LOG(kHttp, "HttpReader::ReadResponse caught ReaderError\n");
        THROW(HttpSocketResponseError);
    }

    const auto code = iReaderResponse.Status().Code();
    LOG(kHttp, "HttpReader::ReadResponse code %u\n", code);
    return code;
}

void HttpSocket::SendRequestHeaders()
{
    if (!iRequestHeadersSent) {
        // Send request headers.
        try {
            WriteRequest(iUri, iMethod);
            iRequestHeadersSent = true;
        }
        catch (const HttpSocketRequestError&) {
            Disconnect();   // FIXME - correct, or up to caller to do?
            THROW(HttpSocketConnectionError);
        }
    }

    // If this is a GET request, the request is now complete.
    // If this is a POST request, caller should call GetOutputStream() and write request body.
    // Following either of the above, GetInputStream(), GetResponseCode(), etc., can be called to handle responses to the request.
}

void HttpSocket::ProcessResponse()
{
    if (!iResponseReceived) {
        try {
            for (;;) { // loop until we don't get a redirection response (i.e. normally don't loop at all!)
                const TUint code = ReadResponse();

                // Check for redirection
                if (code >= HttpStatus::kRedirectionCodes && code < HttpStatus::kClientErrorCodes) {
                    if (iFollowRedirects && iMethod == Http::kMethodGet) {
                        if (!iHeaderLocation.Received()) {
                            LOG(kHttp, "<HttpReader::ProcessResponse expected redirection but did not receive a location field. code: %d\n", code);
                            THROW(HttpSocketError);
                        }

                        try {
                            Uri uri(iHeaderLocation.Location());
                            WriteRequest(uri, iMethod);
                        }
                        catch (const UriError&) {
                            LOG(kHttp, "<HttpReader::ProcessResponse caught UriError\n");
                            THROW(HttpSocketError);
                        }
                        continue;
                    }
                    // Not following redirects.
                }
                else if (code >= HttpStatus::kClientErrorCodes) {
                    LOG(kHttp, "<HttpReader::ProcessResponse received error code: %u\n", code);
                }

                // Not a redirect so is final response; set response state and return.
                if (code != 0) {
                    if (iHeaderTransferEncoding.IsChunked()) {
                        iDechunker.SetChunked(true);
                        iContentLength = -1;
                        iBytesRemaining = -1;
                    }
                    else {
                        iContentLength = iHeaderContentLength.ContentLength();
                        iBytesRemaining = iContentLength;
                    }
                    iResponseReceived = true;
                    iCode = code;

                    // See https://tools.ietf.org/html/rfc7230#section-6.3 for persistence evaluation logic.
                    iPersistConnection = true;
                    if (iHeaderConnection.Close()) {
                        iPersistConnection = false;
                    }
                    else if (iReaderResponse.Version() == Http::EVersion::eHttp11) {
                        iPersistConnection = true;
                    }
                    else if (iReaderResponse.Version() == Http::EVersion::eHttp10 && iHeaderConnection.KeepAlive()) {
                        iPersistConnection = true;
                    }
                    else {
                        iPersistConnection = false;
                    }

                    return;
                }
            }
        }
        catch (const HttpSocketRequestError&) {
            LOG(kHttp, "<HttpReader::ProcessResponse caught HttpSocketRequestError\n");
            Disconnect();   // FIXME - correct, or up to caller to do?
            THROW(HttpSocketError);
        }
        catch (const HttpSocketResponseError&) {
            LOG(kHttp, "<HttpReader::ProcessResponse caught HttpSocketResponseError\n");
            Disconnect();   // FIXME - correct, or up to caller to do?
            THROW(HttpSocketError);
        }
    }
}


// ReaderProxy

ReaderProxy::ReaderProxy()
    : iReader(nullptr)
    , iLock("RPRL")
{
}

TBool ReaderProxy::IsReaderSet() const
{
    AutoMutex _(iLock);
    if (iReader != nullptr) {
        return true;
    }
    return false;
}

void ReaderProxy::SetReader(IReader& aReader)
{
    AutoMutex _(iLock);
    iReader = &aReader;
}

void ReaderProxy::Clear()
{
    AutoMutex _(iLock);
    iReader = nullptr;
}

Brn ReaderProxy::Read(TUint aBytes)
{
    // Can't hold lock while calling iReader->Read(), as that will block, and lock will need to be acquired if ReadInterrupt() is called, to check if iReader != nullptr.
    // Only other call that it's valid/safe to make on this class (from another thread) when in this method is ReadInterrupt().
    IReader* reader = nullptr;
    {
        AutoMutex _(iLock);
        reader = iReader;
    }
    if (reader != nullptr) {
        return reader->Read(aBytes);
    }

    // No reader currently associated.
    THROW(ReaderError);
}

void ReaderProxy::ReadFlush()
{
    AutoMutex _(iLock);
    if (iReader != nullptr) {
        iReader->ReadFlush();
    }
}

void ReaderProxy::ReadInterrupt()
{
    AutoMutex _(iLock);
    if (iReader != nullptr) {
        iReader->ReadInterrupt();
    }
}


// ReaderLoggerTime

ReaderLoggerTime::ReaderLoggerTime(const TChar* aId, IReader& aReader, TUint aNormalReadLimitMs)
    : iId(aId)
    , iReader(aReader)
    , iNormalReadLimitMs(aNormalReadLimitMs)
{
}

Brn ReaderLoggerTime::Read(TUint aBytes)
{
    OsContext* osCtx = gEnv->OsCtx();
    TUint readStartMs = 0;
    try {
        readStartMs = Os::TimeInMs(osCtx);
        const auto buf = iReader.Read(aBytes);
        const TUint readEndMs = Os::TimeInMs(osCtx);
        const TUint durationMs = readEndMs - readStartMs;

        // Log info about slow Read() calls.
        if (durationMs >= iNormalReadLimitMs) {
            LOG(kMedia, "ReaderLoggerTime::Read %s Exceptional read. aBytes: %u, buf.Bytes(): %u, duration: %u ms (start: %u, end: %u).\n", iId, aBytes, buf.Bytes(), durationMs, readStartMs, readEndMs);
        }
        return buf;
    }
    catch (const ReaderError&) {
        const TUint readEndMs = Os::TimeInMs(osCtx);
        const TUint durationMs = readEndMs - readStartMs;

        // Log info about slow Read() calls.
        if (durationMs >= iNormalReadLimitMs) {
            LOG(kMedia, "ReaderLoggerTime::Read %s ReaderError after exceptional read. aBytes: %u, duration: %u ms (start: %u, end: %u).\n", iId, aBytes, durationMs, readStartMs, readEndMs);
        }
        throw;
    }
}

void ReaderLoggerTime::ReadFlush()
{
    iReader.ReadFlush();
}

void ReaderLoggerTime::ReadInterrupt()
{
    iReader.ReadInterrupt();
}


// ReaderLogger

ReaderLogger::ReaderLogger(const TChar* aId, IReader& aReader)
    : iId(aId)
    , iReader(aReader)
    , iEnabled(false)
{
}

void ReaderLogger::SetEnabled(TBool aEnabled)
{
    iEnabled = aEnabled;
}

Brn ReaderLogger::Read(TUint aBytes)
{
    try {
        auto buf = iReader.Read(aBytes);
        if (iEnabled) {
            Log::Print("ReaderLogger::Read %s, aBytes: %u, buf.Bytes(): %u, buf:\n\t%.*s\n", iId, aBytes, buf.Bytes(), PBUF(buf));
        }
        return buf;
    }
    catch (const ReaderError&) {
        if (iEnabled) {
            Log::Print("ReaderLogger::Read %s, aBytes: %u, caught ReaderError.\n", iId, aBytes);
        }
        throw;
    }
}

void ReaderLogger::ReadFlush()
{
    iReader.ReadFlush();
}

void ReaderLogger::ReadInterrupt()
{
    iReader.ReadInterrupt();
}


// UriLoader

UriLoader::UriLoader(Environment& aEnv, const Brx& aUserAgent, ITimerFactory& aTimerFactory, TUint aRetryInterval)
    : iSocket(aEnv, aUserAgent)
    , iRetryInterval(aRetryInterval)
    , iInterrupted(false)
    , iSemRetry("URIS", 0)
{
    iTimerRetry = aTimerFactory.CreateTimer(MakeFunctor(iSemRetry, &Semaphore::Signal), "UriLoader");
}

UriLoader::~UriLoader()
{
    iInterrupted = true;
    iSocket.Interrupt(true);
    iTimerRetry->Cancel();
    iSemRetry.Signal();
    delete iTimerRetry;
}

IReader& UriLoader::Load(const Uri& aUri)
{
    LOG(kMedia, "UriLoader::Load aUri: %.*s\n", PBUF(aUri.AbsoluteUri()));
    for (;;) {
        try {
            iSemRetry.Clear();
            iSocket.SetUri(aUri);
            iSocket.SetRequestMethod(Http::kMethodGet);

            const TInt code = iSocket.GetResponseCode();
            LOG(kMedia, "UriLoader::Load code: %d\n", code);
            if (code == -1) {
                THROW(UriLoaderError);
            }

            const TUint codeUint = code;
            if (codeUint == HttpStatus::kOk.Code()) {
                return iSocket.GetInputStream();
            }
            else {
                // Bad response code.
                THROW(UriLoaderError);
            }
        }
        catch (const HttpSocketUriError&) {
            // Could indicate bad URI, or just failed to do DNS lookup for endpoint.
            const TBool interrupted = iInterrupted;
            LOG(kMedia, "UriLoader::Load caught HttpSocketUriError, iInterrupted: %u\n", interrupted);


            // FIXME - up to this to tell socket to disconnect, or should socket itself have done it before throwing exception?


            if (iInterrupted) {
                THROW(UriLoaderError);
            }

            iTimerRetry->FireIn(iRetryInterval);
            iSemRetry.Wait();
        }
        catch (const HttpSocketConnectionError&) {
            const TBool interrupted = iInterrupted;
            LOG(kMedia, "UriLoader::Load caught HttpSocketConnectionError, iInterrupted: %u\n", interrupted);


            // FIXME - up to this to tell socket to disconnect, or should socket itself have done it before throwing exception?


            // NetworkError is thrown by underlying socket when interrupted, which is wrapped in HttpSocketConnectionError at connection time. Need to check if this exception was thrown because iInterrupt flag was set.
            if (iInterrupted) {
                THROW(UriLoaderError);
            }

            iTimerRetry->FireIn(iRetryInterval);
            iSemRetry.Wait();
        }
        catch (const HttpSocketError&) {
            const TBool interrupted = iInterrupted;
            LOG(kMedia, "UriLoader::Load caught HttpSocketError, iInterrupted: %u\n", interrupted);


            // FIXME - up to this to tell socket to disconnect, or should socket itself have done it before throwing exception?


            if (iInterrupted) {
                THROW(UriLoaderError);
            }

            iTimerRetry->FireIn(iRetryInterval);
            iSemRetry.Wait();
        }
    }
}

void UriLoader::Reset()
{
    // Must not be in Load() call when this called.
    // Caller of Interrupt(true) must also call Interrupt(false). Interrupts are not cleared here.
    iSocket.Disconnect();
}

void UriLoader::Interrupt(TBool aInterrupt)
{
    LOG(kMedia, "UriLoader::Interrupt aInterrrupt: %u\n", aInterrupt);
    iInterrupted = aInterrupt;
    iSocket.Interrupt(aInterrupt);
}


// PlaylistProvider

PlaylistProvider::PlaylistProvider(Environment& aEnv, const Brx& aUserAgent, ITimerFactory& aTimerFactory)
    : iLoader(aEnv, aUserAgent, aTimerFactory, kConnectRetryIntervalMs)
{
}

void PlaylistProvider::SetUri(const Uri& aUri)
{
    try {
        iUri.Replace(aUri.AbsoluteUri());
    }
    catch (const UriError&) {
        THROW(HlsPlaylistProviderError);
    }
}

void PlaylistProvider::Reset()
{
    iLoader.Reset();
    iUri.Clear();
}

IReader& PlaylistProvider::Reload()
{
    LOG(kMedia, ">PlaylistProvider::Reload\n");
    try {
        auto& reader = iLoader.Load(iUri);
        LOG(kMedia, "<PlaylistProvider::Reload reloaded\n");
        return reader;
    }
    catch (const UriLoaderError&) {
        LOG(kMedia, "<PlaylistProvider::Reload caught UriLoaderError\n");
        THROW(HlsPlaylistProviderError);
    }
}

const Uri& PlaylistProvider::GetUri() const
{
    return iUri;
} 

void PlaylistProvider::InterruptPlaylistProvider(TBool aInterrupt)
{
    iLoader.Interrupt(aInterrupt);
}


// SegmentProvider

SegmentProvider::SegmentProvider(Environment& aEnv, const Brx& aUserAgent, ITimerFactory& aTimerFactory, ISegmentUriProvider& aProvider)
    : iLoader(aEnv, aUserAgent, aTimerFactory, kConnectRetryIntervalMs)
    , iProvider(aProvider)
{
}

void SegmentProvider::Reset()
{
    iLoader.Reset();
}

IReader& SegmentProvider::NextSegment()
{
    try {
        Uri uri;
        iProvider.NextSegmentUri(uri);
        return iLoader.Load(uri);
    }
    catch (const HlsSegmentUriError&) {
        // From NextSegmentUri().
        THROW(HlsSegmentError);
    }
    catch (const HlsEndOfStream&) {
        // From NextSegmentUri().
        throw;
    }
    catch (const UriLoaderError&) {
        // From Load().
        THROW(HlsSegmentError);
    }
}

void SegmentProvider::InterruptSegmentProvider(TBool aInterrupt)
{
    iLoader.Interrupt(aInterrupt);
}


// SegmentDescriptor

SegmentDescriptor::SegmentDescriptor(TUint64 aIndex, const Brx& aUri, TUint aDurationMs)
    : iIndex(aIndex)
    , iUri(aUri)
    , iDurationMs(aDurationMs)
{
}

SegmentDescriptor::SegmentDescriptor(const SegmentDescriptor& aDescriptor)
    : iIndex(aDescriptor.iIndex)
    , iUri(Brn(aDescriptor.iUri))
    , iDurationMs(aDescriptor.iDurationMs)
{
}

TUint64 SegmentDescriptor::Index() const
{
    return iIndex;
}

const Brx& SegmentDescriptor::SegmentUri() const
{
    return iUri;
}

void SegmentDescriptor::AbsoluteUri(const Uri& aBaseUri, Uri& aUriOut) const
{
    // Segment URI MAY be relative.
    // If it is relative, it is relative to URI of playlist that contains it.
    static const Brn kSchemeHttp("http");

    if (iUri.Bytes() > kSchemeHttp.Bytes()
            && Brn(iUri.Ptr(), kSchemeHttp.Bytes()) == kSchemeHttp) {
        // Segment URI is absolute.

        // May throw UriError.
        aUriOut.Replace(iUri);
    }
    else {
        // Segment URI is relative.
        Bws<Uri::kMaxUriBytes> uriBuf;
        uriBuf.Replace(aBaseUri.Scheme());
        uriBuf.Append("://");
        uriBuf.Append(aBaseUri.Host());
        TInt port = aBaseUri.Port();
        if (port > 0) {
            uriBuf.Append(":");
            Ascii::AppendDec(uriBuf, aBaseUri.Port());
        }

        // Get URI path minus file.
        Parser uriParser(aBaseUri.Path());
        while (!uriParser.Finished()) {
            Brn fragment = uriParser.Next('/');
            if (!uriParser.Finished()) {
                uriBuf.Append(fragment);
                uriBuf.Append("/");
            }
        }

        // May throw UriError.
        aUriOut.Replace(uriBuf, iUri);
    }
}

TUint SegmentDescriptor::DurationMs() const
{
    return iDurationMs;
}


// HlsPlaylistParser

const TUint HlsPlaylistParser::kMaxM3uVersion;
const TUint HlsPlaylistParser::kMaxLineBytes;

HlsPlaylistParser::HlsPlaylistParser()
    : iReaderProxy()
    , iReaderLogger("HlsPlaylistParser", iReaderProxy)
    , iReaderUntil(iReaderLogger)
    , iTargetDurationMs(0)
    , iSequenceNo(0)
    , iEndList(false)
    , iEndOfStreamReached(false)
    , iNextLine(Brx::Empty())
    , iUnsupported(false)
    , iInvalid(false)
{
    //iReaderLogger.SetEnabled(true);
}

void HlsPlaylistParser::Parse(IReader& aReader)
{
    // At this point, the old IReader is considered invalid. However, calling ReadFlush() on anything in the reader chain will result in the call being passed to the previous, and now invalid, IReader, which could, in fact, be getting re-used behind the scenes, so will cause a flush on the new IReader.
    // For safety, first clear iReaderProxy, to disassociate from previous IReader. Then is is safe to clear iReaderUntil, as iReaderProxy will consume any ReadFlush() call passed on.
    iReaderProxy.Clear();
    iReaderUntil.ReadFlush();   // Only safe to do this after iReaderProxy.Clear(), as don't want ReadFlush() call being passed down to previous, now invalid, IReader.
    iReaderProxy.SetReader(aReader);

    iTargetDurationMs = 0;
    iSequenceNo = 0;
    iEndList = false;
    iEndOfStreamReached = false;
    iNextLine.Set(Brx::Empty());
    iUnsupported = false;
    iInvalid = false;

    PreProcess();
}

void HlsPlaylistParser::Reset()
{
    iReaderProxy.Clear();
    iReaderUntil.ReadFlush();

    iTargetDurationMs = 0;
    iSequenceNo = 0;
    iEndList = false;
    iEndOfStreamReached = false;
    iNextLine.Set(Brx::Empty());
    iUnsupported = false;
    iInvalid = false;
}

TUint HlsPlaylistParser::TargetDurationMs() const
{
    return iTargetDurationMs;
}

TBool HlsPlaylistParser::StreamEnded() const
{
    return iEndOfStreamReached;
}

SegmentDescriptor HlsPlaylistParser::GetNextSegmentUri()
{
    LOG(kMedia, ">HlsPlaylistParser::GetNextSegmentUri\n");
    if (iUnsupported) {
        THROW(HlsPlaylistUnsupported);
    }
    if (iInvalid) {
        THROW(HlsPlaylistInvalid);
    }
    TUint durationMs = 0;
    Brn segmentUri = Brx::Empty();
    try {
        TBool expectUri = false;

        // Process until next segment found.
        while (segmentUri.Bytes() == 0) {
            if (iEndOfStreamReached) {
                THROW(HlsEndOfStream);
            }

            // Skip any empty lines (or read first line, if not already cached).
            if (iNextLine.Bytes() == 0) {
                ReadNextLine();
            }

            if (expectUri) {
                segmentUri = Ascii::Trim(iNextLine);
                expectUri = false;
                LOG(kMedia, "<HlsPlaylistParser::GetNextSegmentUri segmentUri: %.*s\n", PBUF(segmentUri));
            }
            else {
                Parser p(iNextLine);
                Brn tag = p.Next(':');
                if (tag == Brn("#EXTINF")) {
                    Brn durationBuf = p.Next(',');
                    Parser durationParser(durationBuf);
                    Brn durationWhole = durationParser.Next('.');
                    durationMs = Ascii::Uint(durationWhole) * kMillisecondsPerSecond;
                    if (!durationParser.Finished()) {
                        // Looks like duration is a float.
                        // Duration is only guaranteed to be int in version 2 and below
                        Brn durationDecimalBuf = durationParser.Next();
                        if (!durationParser.Finished() && durationDecimalBuf.Bytes()>3) {
                            // Error in M3U8 format.
                            LOG(kMedia, "HlsPlaylistParser::GetNextSegmentUri error while parsing duration of next segment. durationDecimalBuf: %.*s\n", PBUF(durationDecimalBuf));
                            THROW(HlsPlaylistUnsupported);
                        }
                        TUint durationDecimal = Ascii::Uint(durationDecimalBuf);
                        durationMs += durationDecimal;
                    }
                    LOG(kMedia, "HlsPlaylistParser::GetNextSegmentUri durationMs: %u\n", durationMs);
                    expectUri = true;
                }
                else if (tag == Brn("#EXT-X-ENDLIST")) {
                    iEndList = true;
                }
            }
            iNextLine = Brx::Empty();
        }
    }
    catch (AsciiError&) {
        LOG(kMedia, "<HlsPlaylistParser::GetNextSegmentUri AsciiError\n");
        THROW(HlsPlaylistInvalid);  // Malformed playlist.
    }
    catch (ReaderError&) {
        LOG(kMedia, "<HlsPlaylistParser::GetNextSegmentUri ReaderError\n");
        if (iEndList) {
            iEndOfStreamReached = true;
            THROW(HlsEndOfStream);
        }
        THROW(HlsNoMoreSegments);
    }

    SegmentDescriptor sd(iSequenceNo, segmentUri, durationMs);
    iSequenceNo++;
    return sd;
}

void HlsPlaylistParser::Interrupt(TBool /*aInterrupt*/)
{
    iReaderUntil.ReadInterrupt();
}

void HlsPlaylistParser::PreProcess()
{
    // Process until first media segment found.
    // This may not be the most foolproof way of parsing, as the spec appears to suggest that EXT-X-TARGETDURATION can appear anywhere in the playlist (i.e., after media segments). However, most well-formed playlists appear to have the EXT-X-TARGETDURATION tag before any media segments.
    TBool mediaFound = false;
    try {
        while (!mediaFound) {
            ReadNextLine();
            Parser p(iNextLine);
            Brn tag = p.Next(':');

            if (tag == Brn("#EXT-X-VERSION")) {
                const auto version = Ascii::Uint(p.Next());
                if (version > kMaxM3uVersion) {
                    LOG(kMedia, "Unsupported M3U version. Max supported version: %u, version encountered: %u\n", kMaxM3uVersion, version);
                    iUnsupported = true;
                    THROW(HlsPlaylistUnsupported);
                }
            }
            if (tag == Brn("#EXT-X-MEDIA-SEQUENCE")) {
                // If this isn't found, it must be assumed that first segment in playlist is 0.
                auto buf = p.Next();
                const TUint64 mediaSeq = Ascii::Uint64(buf);
                iSequenceNo = mediaSeq;
                LOG(kMedia, "HlsM3uReader::PreprocessM3u mediaSeq: %llu\n", mediaSeq);
            }
            else if (tag == Brn("#EXT-X-TARGETDURATION")) {
                iTargetDurationMs = Ascii::Uint(p.Next()) * kMillisecondsPerSecond;
                LOG(kMedia, "HlsM3uReader::PreprocessM3u iTargetDurationMs: %u\n", iTargetDurationMs);
            }
            else if (tag == Brn("#EXT-X-ENDLIST")) {
                iEndList = true;
                LOG(kMedia, "HlsM3uReader::PreprocessM3u found #EXT-X-ENDLIST\n");
            }
            else if (tag == Brn("#EXTINF")) {
                // EXT-X-MEDIA-SEQUENCE MUST appear before EXTINF, so must
                // have seen it by now if present.
                mediaFound = true;
                // Already encountered a segment entry, so keep iNextLine cached for first call to GetNextSegment().
            }
        }
    }
    catch (AsciiError&) {
        LOG(kMedia, "HlsM3uReader::PreprocessM3u AsciiError\n");
        iInvalid = true;
        THROW(HlsPlaylistInvalid); // Malformed playlist.
    }
    catch (ReaderError&) {
        // Break in stream. Could be because:
        // - ReaderUntil has reached end of stream.
        // - There has ben an unexpected break in stream.
        // - Stream has been interrupted by another thread.
        LOG(kMedia, "HlsM3uReader::PreprocessM3u ReaderError\n");
        THROW(HlsNoMoreSegments);
    }
}

void HlsPlaylistParser::ReadNextLine()
{
    // May throw ReaderError (on stream end, stream interruption, or unexpected break in stream).
    iNextLine.Set(iReaderUntil.ReadUntil(Ascii::kLf));
}


// HlsReloadTimer

HlsReloadTimer::HlsReloadTimer(Environment& aEnv, ITimerFactory& aTimerFactory)
    : iCtx(*aEnv.OsCtx())
    , iResetTimeMs(0)
    , iSem("HRTS", 0)
{
    iTimer = aTimerFactory.CreateTimer(MakeFunctor(*this, &HlsReloadTimer::TimerFired), "HlsReloadTimer");
}

HlsReloadTimer::~HlsReloadTimer()
{
    delete iTimer;
}

void HlsReloadTimer::Restart()
{
    iTimer->Cancel();
    iSem.Clear();
    iResetTimeMs = Os::TimeInMs(&iCtx);
}

void HlsReloadTimer::Wait(TUint aWaitMs)
{
    const TUint timeNowMs = Os::TimeInMs(&iCtx);
    TUint elapsedTimeMs = 0;

    // Can only handle a single wrap of Os::TimeInMs().
    if (timeNowMs >= iResetTimeMs) {
        elapsedTimeMs = timeNowMs - iResetTimeMs;
    }
    else {
        elapsedTimeMs = (std::numeric_limits<TUint>::max()- iResetTimeMs) + timeNowMs;
    }

    LOG(kMedia, "HlsReloadTimer::Wait aWaitMs: %u, iResetTimeMs: %u, timeNowMs: %u, elapsedTimeMs: %u\n", aWaitMs, iResetTimeMs, timeNowMs, elapsedTimeMs);
    if (aWaitMs > elapsedTimeMs) {
        // Still some time to wait.
        const TUint remainingTimeMs = aWaitMs - elapsedTimeMs;
        LOG(kMedia, "HlsReloadTimer::Wait remainingTimeMs: %u\n", remainingTimeMs);
        iTimer->FireIn(remainingTimeMs);

        const TUint timeBeforeSemSignalMs = Os::TimeInMs(&iCtx);
        iSem.Wait();
        const TUint timeAfterSemSignalMs = Os::TimeInMs(&iCtx);
        const TUint timeWaitingForSemSignalMs = timeAfterSemSignalMs - timeBeforeSemSignalMs;
        LOG(kMedia, "HlsReloadTimer::Wait after iSem.Wait(), timeBeforeSemSignalMs: %u, timeAfterSemSignalMs: %u, timeWaitingForSemSignalMs: %u\n", timeBeforeSemSignalMs, timeAfterSemSignalMs, timeWaitingForSemSignalMs);
    }
}

void HlsReloadTimer::InterruptReloadTimer()
{
    LOG(kMedia, "HlsReloadTimer::InterruptReloadTimer\n");
    iTimer->Cancel();
    iSem.Signal();
}

void HlsReloadTimer::TimerFired()
{
    iSem.Signal();
}


// HlsM3uReader

HlsM3uReader::HlsM3uReader(IHlsPlaylistProvider& aProvider, IHlsReloadTimer& aReloadTimer)
    : iProvider(aProvider)
    , iReloadTimer(aReloadTimer)
    , iLastSegment(0)
    , iPreferredStartSegment(iLastSegment)
    , iNewSegmentEncountered(false)
    , iInterrupted(false)
    , iError(false)
{
}

HlsM3uReader::~HlsM3uReader()
{
}

TBool HlsM3uReader::StreamEnded() const
{
    return iParser.StreamEnded();
}

TBool HlsM3uReader::Error() const
{
    return iError;
}

void HlsM3uReader::Interrupt(TBool aInterrupt)
{
    LOG(kMedia, "HlsM3uReader::Interrupt aInterrupt: %u\n", aInterrupt);
    InterruptSegmentUriProvider(aInterrupt);
}

void HlsM3uReader::Reset()
{
    LOG(kMedia, "HlsM3uReader::Reset\n");
    // It is responsibility of owner of this class to call Interrupt() prior
    // to this (if class is active).

    iLastSegment = 0;
    iPreferredStartSegment = iLastSegment;
    iNewSegmentEncountered = false;
    iReloadTimer.Restart();
    iParser.Reset();
    iError = false;
}

void HlsM3uReader::SetStartSegment(TUint64 aPreferredStartSegment)
{
    iPreferredStartSegment = aPreferredStartSegment;
}

TUint64 HlsM3uReader::LastSegment() const
{
    return iLastSegment;
}

TUint HlsM3uReader::NextSegmentUri(Uri& aUri)
{
    TUint64 sequenceNo = 0;
    TBool reload = false;
    for (;;) {
        try {
            if (reload) {
                reload = false;
                ReloadVariantPlaylist();
            }

            auto sd = iParser.GetNextSegmentUri();
            sequenceNo = sd.Index();

            // Check if we've at least reached the preferred start segment.
            if (sequenceNo >= iPreferredStartSegment) {
                // First segment found for this stream (because we haven't yet processed any segment for this stream, so iLastSegment == 0) or have found next expected segment in stream.
                if (iLastSegment == 0 || sequenceNo == iLastSegment+1) {
                    // Found the right segment.
                    try {

                        // FIXME - is it possible that URI in use by the IPlaylistProvider could ever go out of step with the current playlist in which this segment has been retrieved from?
                        sd.AbsoluteUri(iProvider.GetUri(), aUri);
                        iNewSegmentEncountered = true;
                        iLastSegment = sd.Index();
                        LOG(kMedia, "HlsM3uReader::NextSegmentUri returning sd: %llu\n", sd.Index());
                        return sd.DurationMs();
                    }
                    catch (const UriError&) {
                        // Bad segment URI.
                        THROW(HlsSegmentUriError);
                    }
                }
                else if (sequenceNo > iLastSegment+1) {
                    // Unrecoverable discontinuity.
                    THROW(HlsSegmentUriError);
                }
            }
        }
        catch (const HlsNoMoreSegments&) {
            const TBool interrupted = iInterrupted;
            LOG(kMedia, "HlsM3uReader::NextSegmentUri caught HlsNoMoreSegments, iInterrupted: %u\n", interrupted);
            // If interrupted, don't want to retry.
            if (iInterrupted) {
                THROW(HlsSegmentUriError);
            }
            else {
                reload = true;
            }
        }
        catch (const HlsEndOfStream&) {
            LOG(kMedia, "HlsM3uReader::NextSegmentUri caught HlsEndOfStream");
            throw;
        }
        catch (const HlsPlaylistUnsupported&) {
            LOG(kMedia, "HlsM3uReader::NextSegmentUri caught HlsPlaylistUnsupported");
            iError = true;
            THROW(HlsSegmentUriError);
        }
        catch (const HlsPlaylistInvalid&) {
            LOG(kMedia, "HlsM3uReader::NextSegmentUri caught HlsPlaylistInvalid");
            iError = true;
            THROW(HlsSegmentUriError);
        }
    }
}

void HlsM3uReader::InterruptSegmentUriProvider(TBool aInterrupt)
{
    LOG(kMedia, "HlsM3uReader::InterruptSegmentUriProvider aInterrupt: %u\n", aInterrupt);
    iInterrupted = aInterrupt;
    iReloadTimer.InterruptReloadTimer();
    iProvider.InterruptPlaylistProvider(aInterrupt);
}

void HlsM3uReader::ReloadVariantPlaylist()
{
    LOG(kMedia, "HlsM3uReader::ReloadVariantPlaylist\n");
    // Timer should be started BEFORE refreshing playlist.
    // However, not very useful if we don't yet have target duration, so just
    // start timer after processing part of playlist.

    if (iParser.TargetDurationMs() > 0) {
        // Not first (re-)load attempt, so may need to delay.

        // Standard reload time.
        TUint targetDurationMs = iParser.TargetDurationMs();
        if (targetDurationMs == 0) {
            THROW(HlsPlaylistInvalid);
        }

        if (!iNewSegmentEncountered) {
            LOG(kMedia, "HlsM3uReader::ReloadVariantPlaylist exhausted file. targetDurationMs: %u\n", targetDurationMs);
            // Valid condition; reloaded playlist but no new segments were ready,
            // so halve standard retry time:
            //
            // From: https://tools.ietf.org/html/draft-pantos-http-live-streaming-14#section-6.3.2
            //
            // If the client reloads a Playlist file and finds that it has not
            // changed then it MUST wait for a period of one-half the target
            // duration before retrying.
            targetDurationMs /= 2;
        }

        // Wait for targetDurationMs, if it has not already elapsed since last reload.
        iReloadTimer.Wait(targetDurationMs);
    }


    {
        if (iInterrupted) {
            LOG(kMedia, "HlsM3uReader::ReloadVariantPlaylist interrupted while waiting to poll playlist\n");
            THROW(HlsSegmentUriError);
        }
    }

    try {
        // Reload() call is blocking. Don't want to call iReaderProxy.Set(iProvider.Reload()) in case an interrupt call comes in, and need to interrupt iReaderProxy, as would cause deadlock.
        iNewSegmentEncountered = false;
        auto& reader = iProvider.Reload();
        iParser.Parse(reader);
    }
    catch (const HlsPlaylistProviderError&) {
        LOG(kMedia, "HlsM3uReader::ReloadVariantPlaylist caught HlsPlaylistProviderError\n");
        // Provider has encountered an unrecoverable error, or has been interrupted.
        THROW(HlsSegmentUriError);
    }

    if (iInterrupted) {
        LOG(kMedia, "HlsM3uReader::ReloadVariantPlaylist interrupted while reloading playlist. Not setting timer.\n");
        THROW(HlsSegmentUriError);
    }
    // Playlist has been loaded; restart timer ticking to know elapsed time on next reload.
    iReloadTimer.Restart();
    LOG(kMedia, "<HlsM3uReader::ReloadVariantPlaylist\n");
}


// SegmentStreamer

SegmentStreamer::SegmentStreamer(ISegmentProvider& aProvider)
    : iProvider(aProvider)
    , iReader()
    , iInterrupted(false)
    , iError(false)
    , iStreamEnded(false)
    , iLock("SEGL")
{
}

TBool SegmentStreamer::Error() const
{
    return iError;
}

void SegmentStreamer::Interrupt(TBool aInterrupt)
{
    AutoMutex _(iLock);
    iInterrupted = aInterrupt;

    iReader.ReadInterrupt();
    iProvider.InterruptSegmentProvider(aInterrupt);
}

Brn SegmentStreamer::Read(TUint aBytes)
{
    if (iStreamEnded) {
        THROW(ReaderError);
    }

    try {
        for (;;) {
            // If no segment currently set, request next segment.
            if (!iReader.IsReaderSet()) {
                // NextSegment() is a blocking call. Don't call iReader.Set(iProvider->NextSegment()) in case an interrupt call comes in and iReader needs to be interrupted.
                auto& reader = iProvider.NextSegment();
                iReader.SetReader(reader);
            }

            const auto buf = iReader.Read(aBytes);
            if (buf.Bytes() > 0) {
                return buf;
            }
            else { // buf == 0
                // End of stream condition for this segment.
                // Must request next segment to maintain stream continuity.
                // Do this by clearing reader and going back to start of loop.
                iReader.ReadFlush();
                iReader.Clear();
            }
        }
    }
    catch (HlsSegmentError&) {
        LOG(kMedia, "SegmentStreamer::Read HlsSegmentError\n");
        iError = true;
        THROW(ReaderError);
    }
    catch (HlsEndOfStream&) {
        LOG(kMedia, "SegmentStreamer::Read HlsEndOfStream\n");
        iStreamEnded = true;
        return Brx::Empty();
    }
}

void SegmentStreamer::ReadFlush()
{
    iReader.ReadFlush();
}

void SegmentStreamer::ReadInterrupt()
{
    LOG(kMedia, "SegmentStreamer::ReadInterrupt\n");
    AutoMutex a(iLock);
    if (!iInterrupted) {
        iInterrupted = true;
        iReader.ReadInterrupt();
        iProvider.InterruptSegmentProvider(iInterrupted);
    }
}

void SegmentStreamer::Reset()
{
    LOG(kMedia, "SegmentStreamer::Reset\n");
    iReader.ReadFlush();
    iReader.Clear();
    iError = false;
    iStreamEnded = false;

    AutoMutex a(iLock);
    iInterrupted = false;
}


// ProtocolHls

ProtocolHls::ProtocolHls(Environment& aEnv, const Brx& aUserAgent)
    : Protocol(aEnv)
    , iTimerFactory(aEnv)
    , iSupply(nullptr)
    , iSemReaderM3u("SM3U", 0)
    , iPlaylistProvider(aEnv, aUserAgent, iTimerFactory)
    , iReloadTimer(aEnv, iTimerFactory)
    , iM3uReader(iPlaylistProvider, iReloadTimer)
    , iSegmentProvider(aEnv, aUserAgent, iTimerFactory, iM3uReader)
    , iSegmentStreamer(iSegmentProvider)
    , iSem("PRTH", 0)
    , iLock("PRHL")
{
}

ProtocolHls::~ProtocolHls()
{
    delete iSupply;
}

void ProtocolHls::Initialise(MsgFactory& aMsgFactory, IPipelineElementDownstream& aDownstream)
{
    iSupply = new Supply(aMsgFactory, aDownstream);
}

void ProtocolHls::Interrupt(TBool aInterrupt)
{
    // LOG(kMedia, "ProtocolHls::Interrupt aInterrupt: %u\n", aInterrupt);
    // iLock.Wait();
    // if (iActive) {
    //     LOG(kMedia, "ProtocolHls::Interrupt(%u)\n", aInterrupt);
    //     if (aInterrupt) {
    //         iStopped = true;
    //     }
    //     iSegmentStreamer.Interrupt(aInterrupt);
    //     iM3uReader.Interrupt(aInterrupt);
    //     iSem.Signal();
    // }
    // iLock.Signal();



    LOG(kMedia, "ProtocolHls::Interrupt aInterrupt: %u\n", aInterrupt);
    iLock.Wait();
    if (iActive) {
        LOG(kMedia, "ProtocolHls::Interrupt(%u)\n", aInterrupt);
        if (aInterrupt) {
            iStopped = true;
        }
        iSem.Signal();
    }
    iSegmentStreamer.Interrupt(aInterrupt);
    iM3uReader.Interrupt(aInterrupt);
    iLock.Signal();
}

ProtocolStreamResult ProtocolHls::Stream(const Brx& aUri)
{
    // There is no notion of a live or seekable stream in HLS!
    //
    // By default, all streams are live.
    //
    // It is legal to perform a seek, as long as it is within the current
    // stream segments available within the variant playlist.
    // (i.e., if the first available segment was some_stream-002.ts, and the
    // user wished to seek to a position that would be in some_stream-001.ts,
    // that seek would be invalid.)
    //
    // It is also legal to attempt to pause an HLS stream (albeit that the
    // position at which it can resume is bounded by the segments available in
    // the variant playlist).
    // (i.e., if paused during some_stream-002.ts and when unpaused first
    // segment now available was some_stream-004.ts, there would be a forced
    // discontinuity in the audio.)
    //
    // Given the limited usefulness of this behaviour (because it is bound by
    // the limits of the periodically changing variant playlist), the use case
    // (why would a client wish to seek during a live radio stream?), and the
    // increased complexity of the code required, just don't allow
    // seeking/pausing.

    Reinitialise();
    Uri uriHls(aUri);
    if (uriHls.Scheme() != Brn("hls")) {
        return EProtocolErrorNotSupported;
    }
    LOG(kMedia, "ProtocolHls::Stream(%.*s)\n", PBUF(aUri));

    if (!iStarted) {
        StartStream(uriHls);
    }

    // Don't want to buffer content from a live stream
    // ...so need to wait on pipeline signalling it is ready to play
    LOG(kMedia, "ProtocolHls::Stream live stream waiting to be (re-)started\n");
    iSegmentProvider.Reset();
    iSegmentStreamer.Reset();
    iPlaylistProvider.Reset();
    iM3uReader.Reset();
    iM3uReader.SetStartSegment(HlsM3uReader::kSeqNumFirstInPlaylist);
    iSem.Wait();
    LOG(kMedia, "ProtocolHls::Stream live stream restart\n");

    // Convert hls:// scheme to http:// scheme
    const Brx& uriHlsBuf = uriHls.AbsoluteUri();
    Parser p(uriHlsBuf);
    p.Next(':');    // skip "hls" scheme
    Bws<Uri::kMaxUriBytes> uriHttpBuf("http:");
    uriHttpBuf.Append(p.NextToEnd());

    Uri uriHttp;
    try {
        uriHttp.Replace(uriHttpBuf);
        iPlaylistProvider.SetUri(uriHttp);
    }
    catch (const UriError&) {
        return EProtocolStreamErrorUnrecoverable;
    }

    if (iContentProcessor == nullptr) {
        iContentProcessor = iProtocolManager->GetAudioProcessor();
    }

    ProtocolStreamResult res = EProtocolStreamErrorRecoverable;
    while (res == EProtocolStreamErrorRecoverable) {
        {
            AutoMutex a(iLock);
            if (iStopped) {
                res = EProtocolStreamStopped;
                break;
            }
        }

        // This will only return EProtocolStreamErrorRecoverable for live streams (i.e., streams of length 0)!
        res = iContentProcessor->Stream(iSegmentStreamer, 0);

        // Check for context of above method returning.
        // i.e., identify whether it was actually caused by:
        //  - TryStop() being called                    (EProtocolStreamStopped)
        //  - end of stream indicated in M3U8           (EProtocolStreamSuccess)
        //  - unrecoverable error (e.g. malformed M3U8) (EProtocolStreamErrorUnrecoverable)
        //  - recoverable interruption in stream        (EProtocolStreamErrorRecoverable)

        TBool stopped = false;
        {
            AutoMutex a(iLock);
            stopped = iStopped;
        }
        if (stopped) {
            res = EProtocolStreamStopped;
            break;
        }
        else if (iM3uReader.StreamEnded()) {
            res = EProtocolStreamSuccess;
            break;
        }
        else if (iM3uReader.Error() || iSegmentStreamer.Error()) {
            // FIXME - is it necessary to check for these errors here, or to return EProtocolStreamErrorUnrecoverable?
            // FIXME - also, ever possible to enter here, given that ContentAudio will only return EProtocolStreamErrorRecoverable on a live stream?

            // Will reach here if:
            // - malformed playlist
            // - malformed segment URI (i.e., specific case of malformed playlist)
            // - bad server response (e.g., file not found, internal server error, etc.)
            res = EProtocolStreamErrorUnrecoverable;
            break;
        }
        else { // res == EProtocolStreamErrorRecoverable
            {
                AutoMutex a(iLock);
                // This stream has ended. Clear iStreamId to prevent TryStop()
                // returning a valid flush id from this point.
                iStreamId = IPipelineIdProvider::kStreamIdInvalid;

                if (iNextFlushId != MsgFlush::kIdInvalid) {
                    // As a successful TryStop() call has come in, protocol should
                    // exit and return state should be EProtocolStreamStopped
                    // (i.e., not EProtocolStreamErrorRecoverable, which is not an
                    // allowed exit state for any protocol).
                    res = EProtocolStreamStopped;

                    // A successful TryStop() call has already come in. Don't
                    // attempt to retry stream. Break from stream loop here and
                    // cleanup code will output flush before exiting.
                    break;
                }
            }

            // Clear all stream handlers.
            iSegmentProvider.Reset();
            iSegmentStreamer.Reset();
            iPlaylistProvider.Reset();
            const auto lastSegment = iM3uReader.LastSegment();
            iM3uReader.Reset();

            // There is no flush pending, and iStreamId has been cleared (so no
            // further TryStop() call will succeed). Safe to drain pipeline now.
            WaitForDrain();

            // Try continue on from previous segment in stream, if possible (even with a discontinuity in audio, still better than potentially repeating already-played segments, if any such segments still present in playlist).
            Reinitialise();
            iPlaylistProvider.SetUri(uriHttp);
            iM3uReader.SetStartSegment(lastSegment+1);
            iContentProcessor = iProtocolManager->GetAudioProcessor();

            StartStream(uriHls);    // Output new MsgEncodedStream to signify discontinuity.
            continue;
        }
    }

    iSegmentProvider.Reset();
    iSegmentStreamer.Reset();
    iPlaylistProvider.Reset();
    iM3uReader.Reset();

    TUint flushId = MsgFlush::kIdInvalid;
    {
        AutoMutex a(iLock);
        if (iNextFlushId != MsgFlush::kIdInvalid) {
            flushId = iNextFlushId;
            iNextFlushId = MsgFlush::kIdInvalid;
        }
        // Clear iStreamId to prevent TrySeek or TryStop returning a valid flush id.
        iStreamId = IPipelineIdProvider::kStreamIdInvalid;
    }
    if (flushId != MsgFlush::kIdInvalid) {
        iSupply->OutputFlush(flushId);
    }

    LOG(kMedia, "<ProtocolHls::Stream res: %d\n", res);
    return res;
}

ProtocolGetResult ProtocolHls::Get(IWriter& /*aWriter*/, const Brx& /*aUri*/, TUint64 /*aOffset*/, TUint /*aBytes*/)
{
    return EProtocolGetErrorNotSupported;
}

void ProtocolHls::Deactivated()
{
    if (iContentProcessor != nullptr) {
        iContentProcessor->Reset();
        iContentProcessor = nullptr;
    }
    iSegmentStreamer.Reset();
    iM3uReader.Reset();
}

EStreamPlay ProtocolHls::OkToPlay(TUint aStreamId)
{
    LOG(kMedia, "> ProtocolHls::OkToPlay(%u)\n", aStreamId);
    const EStreamPlay canPlay = iIdProvider->OkToPlay(aStreamId);
    if (iStreamId == aStreamId) {
        iSem.Signal();
    }
    LOG(kMedia, "< ProtocolHls::OkToPlay(%u) == %s\n", aStreamId, kStreamPlayNames[canPlay]);
    return canPlay;
}

TUint ProtocolHls::TrySeek(TUint /*aStreamId*/, TUint64 /*aOffset*/)
{
    LOG(kMedia, "ProtocolHls::TrySeek\n");
    return MsgFlush::kIdInvalid;
}

TUint ProtocolHls::TryStop(TUint aStreamId)
{
    iLock.Wait();
    const TBool stop = IsCurrentStream(aStreamId);
    if (stop) {
        if (iNextFlushId == MsgFlush::kIdInvalid) {
            /* If a valid flushId is set then We've previously promised to send a Flush but haven't
            got round to it yet.  Re-use the same id for any other requests that come in before
            our main thread gets a chance to issue a Flush */
            iNextFlushId = iFlushIdProvider->NextFlushId();
        }
        iStopped = true;
        iSegmentStreamer.ReadInterrupt();   // Passes this on to chained components downstream.
        iM3uReader.Interrupt(true); // Should be cleared via ::Interrupt(false) call before new stream started.
        iSem.Signal();
    }
    const TUint nextFlushId = iNextFlushId;
    iLock.Signal();
    if (stop) {
        return nextFlushId;
    }
    return MsgFlush::kIdInvalid;
}

void ProtocolHls::Reinitialise()
{
    LOG(kMedia, "ProtocolHls::Reinitialise\n");
    AutoMutex a(iLock);
    iStreamId = IPipelineIdProvider::kStreamIdInvalid;
    iStarted = iStopped = false;
    iContentProcessor = nullptr;
    iNextFlushId = MsgFlush::kIdInvalid;
    (void)iSem.Clear();
}

void ProtocolHls::StartStream(const Uri& aUri)
{
    LOG(kMedia, "ProtocolHls::StartStream\n");
    TBool totalBytes = 0;
    TBool seekable = false;
    TBool live = true;
    iStreamId = iIdProvider->NextStreamId();
    iSupply->OutputStream(aUri.AbsoluteUri(), totalBytes, 0, seekable, live, Multiroom::Allowed, *this, iStreamId);
    iStarted = true;
}

TBool ProtocolHls::IsCurrentStream(TUint aStreamId) const
{
    if (iStreamId != aStreamId || aStreamId == IPipelineIdProvider::kStreamIdInvalid) {
        return false;
    }
    return true;
}

void ProtocolHls::WaitForDrain()
{
    Semaphore semDrain("HLSD", 0);
    iSupply->OutputDrain(MakeFunctor(semDrain, &Semaphore::Signal));
    semDrain.Wait();
}
