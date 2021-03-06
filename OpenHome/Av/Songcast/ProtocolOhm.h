#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Optional.h>
#include <OpenHome/Av/Songcast/ProtocolOhBase.h>

namespace OpenHome {
    class Environment;
namespace Media {
    class TrackFactory;
    class IClockPullerTimestamp;
}
namespace Av {

class IUnicastOverrideObserver
{
public:
    virtual ~IUnicastOverrideObserver() {}
    virtual void UnicastOverrideEnabled() = 0;
    virtual void UnicastOverrideDisabled() = 0;
};

class IOhmMsgFactory;
class IOhmTimestamper;

class ProtocolOhm : public ProtocolOhBase, public IUnicastOverrideObserver
{
public:
    ProtocolOhm(Environment& aEnv, IOhmMsgFactory& aMsgFactory, Media::TrackFactory& aTrackFactory,
                Optional<IOhmTimestamper> aTimestamper, const Brx& aMode,
                Optional<Av::IOhmMsgProcessor> aOhmMsgProcessor);
private: // from IUnicastOverrideObserver
    void UnicastOverrideEnabled() override;
    void UnicastOverrideDisabled() override;
private: // from ProtocolOhBase
    Media::ProtocolStreamResult Play(TIpAddress aInterface, TUint aTtl, const Endpoint& aEndpoint) override;
private: // from Media::Protocol
    void Interrupt(TBool aInterrupt) override;
private: // from IStreamHandler
    TUint TryStop(TUint aStreamId) override;
private:
    void ResetClockPuller();
private:
    TUint iNextFlushId;
    TBool iStopped;
    Mutex iStoppedLock;
    Semaphore iSemSenderUnicastOverride;
    std::atomic<TBool> iSenderUnicastOverrideEnabled;
};

} // namespace Av
} // namespace OpenHome

