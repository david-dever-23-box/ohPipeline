#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Exception.h>
#include <OpenHome/Media/PipelineObserver.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Media/Pipeline/Waiter.h>
#include <OpenHome/Media/Pipeline/Stopper.h>
#include <OpenHome/Media/Pipeline/Reporter.h>
#include <OpenHome/Media/Pipeline/StarvationRamper.h>
#include <OpenHome/Media/MuteManager.h>
#include <OpenHome/Media/Pipeline/Attenuator.h>

EXCEPTION(PipelineStreamNotPausable)

namespace OpenHome {
namespace Media {

enum EPipelineSupportElements {
    EPipelineSupportElementsMandatory             = 0,
    EPipelineSupportElementsLogger                = 1 << 0,
    EPipelineSupportElementsDecodedAudioValidator = 1 << 1,
    EPipelineSupportElementsRampValidator         = 1 << 2,
    EPipelineSupportElementsValidatorMinimal      = 1 << 3,
    EPipelineSupportElementsAudioDumper           = 1 << 4,
    EPipelineSupportElementsAll                   = 0x7fffffff
};

class PipelineInitParams
{
public:
    enum class MuterImpl
    {
        eRampSamples,
        eRampVolume
    };
public:
    static PipelineInitParams* New();
    virtual ~PipelineInitParams();
    // setters
    void SetEncodedReservoirSize(TUint aBytes);
    void SetDecodedReservoirSize(TUint aJiffies);
    void SetGorgerDuration(TUint aJiffies); // amount of audio required before non-pullable sources will start playing
    void SetStarvationRamperMinSize(TUint aJiffies);
    void SetMaxStreamsPerReservoir(TUint aCount);
    void SetLongRamp(TUint aJiffies);
    void SetShortRamp(TUint aJiffies);
    void SetEmergencyRamp(TUint aJiffies);
    void SetSenderMinLatency(TUint aJiffies);
    void SetThreadPriorityMax(TUint aPriority); // highest priority used by pipeline
    void SetThreadPriorities(TUint aStarvationRamper, TUint aCodec, TUint aEvent);
    void SetMaxLatency(TUint aJiffies);
    void SetSupportElements(TUint aElements); // EPipelineSupportElements members OR'd together
    void SetMuter(MuterImpl aMuter);
    void SetDsdSupported(TBool aDsd);
    // getters
    TUint EncodedReservoirBytes() const;
    TUint DecodedReservoirJiffies() const;
    TUint GorgeDurationJiffies() const;
    TUint StarvationRamperMinJiffies() const;
    TUint MaxStreamsPerReservoir() const;
    TUint RampLongJiffies() const;
    TUint RampShortJiffies() const;
    TUint RampEmergencyJiffies() const;
    TUint SenderMinLatency() const;
    TUint ThreadPriorityStarvationRamper() const;
    TUint ThreadPriorityCodec() const;
    TUint ThreadPriorityEvent() const;
    TUint MaxLatencyJiffies() const;
    TUint SupportElements() const;
    MuterImpl Muter() const;
    TBool DsdSupported() const;
private:
    PipelineInitParams();
private:
    TUint iEncodedReservoirBytes;
    TUint iDecodedReservoirJiffies;
    TUint iGorgeDurationJiffies;
    TUint iStarvationRamperMinJiffies;
    TUint iMaxStreamsPerReservoir;
    TUint iRampLongJiffies;
    TUint iRampShortJiffies;
    TUint iRampEmergencyJiffies;
    TUint iSenderMinLatency;
    TUint iThreadPriorityStarvationRamper;
    TUint iThreadPriorityCodec;
    TUint iThreadPriorityEvent;
    TUint iMaxLatencyJiffies;
    TUint iSupportElements;
    MuterImpl iMuter;
    TBool iDsdSupported;
private:
    static const TUint kEncodedReservoirSizeBytes       = 1536 * 1024;
    static const TUint kDecodedReservoirSize            = Jiffies::kPerMs * 2000;
    static const TUint kGorgerSizeDefault               = Jiffies::kPerMs * 1000;
    static const TUint kStarvationRamperSizeDefault     = Jiffies::kPerMs * 20;
    static const TUint kMaxReservoirStreamsDefault      = 10;
    static const TUint kLongRampDurationDefault         = Jiffies::kPerMs * 500;
    static const TUint kShortRampDurationDefault        = Jiffies::kPerMs * 50;
    static const TUint kEmergencyRampDurationDefault    = Jiffies::kPerMs * 20;
    static const TUint kSenderMinLatency                = Jiffies::kPerMs * 150;
    static const TUint kThreadPriorityMax               = kPriorityHighest - 1;
    static const TUint kMaxLatencyDefault               = Jiffies::kPerMs * 2000;
    static const MuterImpl kMuterDefault                = MuterImpl::eRampSamples;
    static const TBool kDsdSupportedDefault             = false;
};

namespace Codec {
    class ContainerController;
    class ContainerBase;
    class CodecController;
    class CodecBase;
}
class PipelineElementObserverThread;
class AudioDumper;
class EncodedAudioReservoir;
class Logger;
class DecodedAudioValidator;
class StreamValidator;
class DecodedAudioAggregator;
class DecodedAudioReservoir;
class Ramper;
class RampValidator;
class Seeker;
class VariableDelayLeft;
class TrackInspector;
class Skipper;
class Waiter;
class Stopper;
class Reporter;
class SpotifyReporter;
class Router;
class Attenuator;
class Drainer;
class VariableDelayRight;
class StarvationRamper;
class Muter;
class MuterVolume;
class IVolumeMuterStepped;
class PreDriver;
class ITrackObserver;
class ISpotifyReporter;
class ISpotifyTrackObserver;
class IMimeTypeList;
class VolumeRamper;
class IVolumeRamper;

class Pipeline : public IPipelineElementDownstream
               , public IPipeline
               , public IFlushIdProvider
               , public IWaiterObserver
               , public IStopper
               , public IMute
               , public IPostPipelineLatencyObserver
               , public IAttenuator
               , private IStopperObserver
               , private IPipelinePropertyObserver
               , private IStarvationRamperObserver
{
    friend class SuitePipeline; // test code

    static const TUint kReceiverMaxLatency      = Jiffies::kPerSecond;
    static const TUint kReservoirCount          = 5; // Encoded + Decoded + (optional) Songcast sender + StarvationRamper + spare
    static const TUint kSongcastFrameJiffies    = Jiffies::kPerMs * 5; // effectively hard-coded by volkano1
    static const TUint kRewinderMaxMsgs         = 100;

    static const TUint kMsgCountSilence         = 410; // 2secs @ 5ms per msg + 10 spare
    static const TUint kMsgCountPlayablePcm     = 10;
    static const TUint kMsgCountPlayableDsd     = 10;
    static const TUint kMsgCountPlayableSilence = 10;
    static const TUint kMsgCountFlush           = 16;
    static const TUint kMsgCountMode            = 20;
    static const TUint kMsgCountQuit            = 1;
    static const TUint kMsgCountDrain           = 5;
public:
    Pipeline(PipelineInitParams* aInitParams, IInfoAggregator& aInfoAggregator, TrackFactory& aTrackFactory, IPipelineObserver& aObserver,
             IStreamPlayObserver& aStreamPlayObserver, ISeekRestreamer& aSeekRestreamer, IUrlBlockWriter& aUrlBlockWriter);
    virtual ~Pipeline();
    void AddContainer(Codec::ContainerBase* aContainer);
    void AddCodec(Codec::CodecBase* aCodec);
    void Start(IVolumeRamper& aVolumeRamper, IVolumeMuterStepped& aVolumeMuter);
    void Quit();
    MsgFactory& Factory();
    void Play();
    void Pause();
    void Wait(TUint aFlushId);
    void FlushQuick(TUint aFlushId);
    void Stop(TUint aHaltId);
    void RemoveCurrentStream();
    void RemoveAll(TUint aHaltId);
    void Block(); // use before calls that pass flush or halt ids
    void Unblock(); // must be exactly one of these for each call to Block()
    void Seek(TUint aStreamId, TUint aSecondsAbsolute);
    void AddObserver(ITrackObserver& aObserver);
    ISpotifyReporter& SpotifyReporter() const;
    ISpotifyTrackObserver& SpotifyTrackObserver() const;
    IPipelineElementUpstream& InsertElements(IPipelineElementUpstream& aTail);
    TUint SenderMinLatencyMs() const;
    void GetThreadPriorityRange(TUint& aMin, TUint& aMax) const;
    void GetThreadPriorities(TUint& aFlywheelRamper, TUint& aStarvationRamper, TUint& aCodec, TUint& aEvent);
    void LogBuffers() const;
public: // from IPipelineElementDownstream
    void Push(Msg* aMsg) override;
public: // from IPipeline
    Msg* Pull() override;
    void SetAnimator(IPipelineAnimator& aAnimator) override;
private: // from IFlushIdProvider
    TUint NextFlushId() override;
private: // from IWaiterObserver
    void PipelineWaiting(TBool aWaiting) override;
private: // from IStopper
    void RemoveStream(TUint aStreamId) override;
private: // from IMute
    void Mute() override;
    void Unmute() override;
public: // from IPostPipelineLatencyObserver
    void PostPipelineLatencyChanged() override;
public: // from IAttenuator
    void SetAttenuation(TUint aAttenuation) override;
private:
    void DoPlay(TBool aQuit);
    void NotifyStatus();
private: // from IStopperObserver
    void PipelinePaused() override;
    void PipelineStopped() override;
    void PipelinePlaying() override;
private: // from IPipelinePropertyObserver
    void NotifyMode(const Brx& aMode, const ModeInfo& aInfo,
                    const ModeTransportControls& aTransportControls) override;
    void NotifyTrack(Track& aTrack, const Brx& aMode, TBool aStartOfStream) override;
    void NotifyMetaText(const Brx& aText) override;
    void NotifyTime(TUint aSeconds, TUint aTrackDurationSeconds) override;
    void NotifyStreamInfo(const DecodedStreamInfo& aStreamInfo) override;
private: // from IStarvationRamperObserver
    void NotifyStarvationRamperBuffering(TBool aBuffering) override;
private:
    enum EStatus
    {
        EPlaying
       ,EPaused
       ,EStopped
       ,EWaiting
    };
private:
    PipelineInitParams* iInitParams;
    IPipelineObserver& iObserver;
    Mutex iLock;
    MsgFactory* iMsgFactory;
    PipelineElementObserverThread* iEventThread;
    AudioDumper* iAudioDumper;
    EncodedAudioReservoir* iEncodedAudioReservoir;
    Logger* iLoggerEncodedAudioReservoir;
    Codec::ContainerController* iContainer;
    Logger* iLoggerContainer;
    Codec::CodecController* iCodecController;
    Logger* iLoggerCodecController;
    RampValidator* iRampValidatorCodec;
    StreamValidator* iStreamValidator;
    Logger* iLoggerStreamValidator;
    DecodedAudioAggregator* iDecodedAudioAggregator;
    Logger* iLoggerDecodedAudioAggregator;
    DecodedAudioReservoir* iDecodedAudioReservoir;
    Logger* iLoggerDecodedAudioReservoir;
    Ramper* iRamper;
    Logger* iLoggerRamper;
    RampValidator* iRampValidatorRamper;
    Seeker* iSeeker;
    Logger* iLoggerSeeker;
    RampValidator* iRampValidatorSeeker;
    DecodedAudioValidator* iDecodedAudioValidatorSeeker;
    VariableDelayLeft* iVariableDelay1;
    Logger* iLoggerVariableDelay1;
    RampValidator* iRampValidatorDelay1;
    DecodedAudioValidator* iDecodedAudioValidatorDelay1;
    TrackInspector* iTrackInspector;
    Logger* iLoggerTrackInspector;
    Skipper* iSkipper;
    Logger* iLoggerSkipper;
    RampValidator* iRampValidatorSkipper;
    DecodedAudioValidator* iDecodedAudioValidatorSkipper;
    Waiter* iWaiter;
    Logger* iLoggerWaiter;
    RampValidator* iRampValidatorWaiter;
    DecodedAudioValidator* iDecodedAudioValidatorWaiter;
    Stopper* iStopper;
    Logger* iLoggerStopper;
    RampValidator* iRampValidatorStopper;
    DecodedAudioValidator* iDecodedAudioValidatorStopper;
    Reporter* iReporter;
    Logger* iLoggerReporter;
    Media::SpotifyReporter* iSpotifyReporter;
    Logger* iLoggerSpotifyReporter;
    Router* iRouter;
    Logger* iLoggerRouter;
    Attenuator* iAttenuator;
    Logger* iLoggerAttenuator;
    DecodedAudioValidator* iDecodedAudioValidatorRouter;
    Drainer* iDrainer;
    Logger* iLoggerDrainer;
    VariableDelayRight* iVariableDelay2;
    Logger* iLoggerVariableDelay2;
    RampValidator* iRampValidatorDelay2;
    DecodedAudioValidator* iDecodedAudioValidatorDelay2;
    StarvationRamper* iStarvationRamper;
    Logger* iLoggerStarvationRamper;
    RampValidator* iRampValidatorStarvationRamper;
    DecodedAudioValidator* iDecodedAudioValidatorStarvationRamper;
    Muter* iMuterSamples;      // only one of iMuter or iMuterVolume will be instantiated
    MuterVolume* iMuterVolume; // only one of iMuter or iMuterVolume will be instantiated
    IMute* iMuter;
    Logger* iLoggerMuter;
    DecodedAudioValidator* iDecodedAudioValidatorMuter;
    VolumeRamper* iVolumeRamper;
    Logger* iLoggerVolumeRamper;
    PreDriver* iPreDriver;
    Logger* iLoggerPreDriver;
    IPipelineElementDownstream* iPipelineStart;
    IPipelineElementUpstream* iPipelineEnd;
    IMute* iMuteCounted;
    EStatus iState;
    EPipelineState iLastReportedState;
    TBool iBuffering;
    TBool iWaiting;
    TBool iQuitting;
    TUint iNextFlushId;
};

} // namespace Media
} // namespace OpenHome

