#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Media/Pipeline/AudioReservoir.h>
#include <OpenHome/Media/Pipeline/Reporter.h>

namespace OpenHome {
namespace Media {

class ISpotifyReporter
{
public:
    virtual TUint64 SubSamples() = 0;
    virtual TUint64 SubSamplesDiff(TUint64 aPrevSubSamples) = 0;
    virtual ~ISpotifyReporter() {}
};

class ITrackChangeObserver
{
public:
    virtual void TrackChanged(Track* aTrack, TUint aDurationMs) = 0;
    virtual ~ITrackChangeObserver() {}
};

/*
 * Element to report number of samples seen since last MsgMode.
 */
class SpotifyReporter : public IPipelineElementUpstream, public ISpotifyReporter, public ITrackChangeObserver, private IMsgProcessor, private INonCopyable
{
private:
    static const Brn kInterceptMode;
public:
    SpotifyReporter(IPipelineElementUpstream& aUpstreamElement, MsgFactory& aMsgFactory);
    ~SpotifyReporter();
public: // from IPipelineElementUpstream
    Msg* Pull() override;
public: // from ISpotifyReporter
    TUint64 SubSamples() override;
    TUint64 SubSamplesDiff(TUint64 aPrevSamples) override;
private: // from ITrackChangeObserver
    void TrackChanged(Track* aTrack, TUint aDurationMs) override;
    // FIXME - can also derive from new pipeline element classes and only implement subset of IMsgProcessor methods that are actually required.
private: // IMsgProcessor
    Msg* ProcessMsg(MsgMode* aMsg) override;
    Msg* ProcessMsg(MsgTrack* aMsg) override;
    Msg* ProcessMsg(MsgDrain* aMsg) override;
    Msg* ProcessMsg(MsgDelay* aMsg) override;
    Msg* ProcessMsg(MsgEncodedStream* aMsg) override;
    Msg* ProcessMsg(MsgAudioEncoded* aMsg) override;
    Msg* ProcessMsg(MsgMetaText* aMsg) override;
    Msg* ProcessMsg(MsgStreamInterrupted* aMsg) override;
    Msg* ProcessMsg(MsgHalt* aMsg) override;
    Msg* ProcessMsg(MsgFlush* aMsg) override;
    Msg* ProcessMsg(MsgWait* aMsg) override;
    Msg* ProcessMsg(MsgDecodedStream* aMsg) override;
    Msg* ProcessMsg(MsgBitRate* aMsg) override;
    Msg* ProcessMsg(MsgAudioPcm* aMsg) override;
    Msg* ProcessMsg(MsgSilence* aMsg) override;
    Msg* ProcessMsg(MsgPlayable* aMsg) override;
    Msg* ProcessMsg(MsgQuit* aMsg) override;
private:
    void ClearDecodedStreamLocked();
    void UpdateDecodedStreamLocked(MsgDecodedStream& aMsg);
    TUint64 TrackLengthSamplesLocked() const;
    MsgDecodedStream* CreateMsgDecodedStreamLocked() const;
    // Expects duration of form: H+:MM:SS[.F0/F1]
    static TUint ParseDurationMs(const Brx& aDuration);
private:
    IPipelineElementUpstream& iUpstreamElement;
    MsgFactory& iMsgFactory;
    TUint iTrackDurationMs;
    TUint64 iTrackOffsetSamples;
    Track* iTrackPending;
    TBool iMsgDecodedStreamPending;
    MsgDecodedStream* iDecodedStream;
    TUint64 iSubSamples;
    TBool iInterceptMode;
    Mutex iLock;
};

} // namespace Media
} // namespace OpenHome
