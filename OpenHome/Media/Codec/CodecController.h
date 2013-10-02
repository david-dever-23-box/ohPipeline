#ifndef HEADER_PIPELINE_CODEC
#define HEADER_PIPELINE_CODEC

#include <OpenHome/OhNetTypes.h>
#include <OpenHome/Exception.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Private/Standard.h>
#include <OpenHome/Media/Msg.h>
#include <OpenHome/Media/Rewinder.h>

#include <vector>

EXCEPTION(CodecStreamStart);
EXCEPTION(CodecStreamEnded);
EXCEPTION(CodecStreamFlush);
EXCEPTION(CodecStreamSeek);
EXCEPTION(CodecStreamFeatureUnsupported);

namespace OpenHome {
namespace Media {
namespace Codec {

/*
Element which holds all audio codecs.
Recognises the format of audio data then decodes it.
Accepts MsgAudioEncoded; outputs MsgAudioPcm.
MsgTrack and MsgStartOfStream are expected and passed through unchanged.
FIXME - no reporting of corrupt/unrecognised file errors.
*/

class ICodecController
{
public:
    virtual void Read(Bwx& aBuf, TUint aBytes) = 0;
    virtual TBool TrySeek(TUint aStreamId, TUint64 aBytePos) = 0;
    virtual TUint64 StreamLength() const = 0;
    virtual TUint64 StreamPos() const = 0;
    virtual void OutputDecodedStream(TUint aBitRate, TUint aBitDepth, TUint aSampleRate, TUint aNumChannels, const Brx& aCodecName, TUint64 aTrackLength, TUint64 aSampleStart, TBool aLossless) = 0;
    virtual TUint64 OutputAudioPcm(const Brx& aData, TUint aChannels, TUint aSampleRate, TUint aBitDepth, EMediaDataEndian aEndian, TUint64 aTrackOffset) = 0; // returns jiffy size of data
};
    
class CodecBase
{
    friend class CodecController;
public:
    virtual ~CodecBase();
public:
    virtual TBool SupportsMimeType(const Brx& aMimeType) = 0;
    virtual TBool Recognise() = 0;
    virtual void StreamInitialise();
    virtual void Process() = 0;
    virtual TBool TrySeek(TUint aStreamId, TUint64 aSample) = 0;
    virtual void StreamCompleted();
protected:
    CodecBase();
private:
    void Construct(ICodecController& aController);
protected:
    ICodecController* iController;
};

class CodecController : private ICodecController, private IMsgProcessor, private INonCopyable
{
public:
    CodecController(MsgFactory& aMsgFactory, IPipelineElementUpstream& aUpstreamElement, IPipelineElementDownstream& aDownstreamElement);
    virtual ~CodecController();
    void AddCodec(CodecBase* aCodec);
    void Start();
    TBool Seek(TUint aTrackId, TUint aStreamId, TUint aSecondsAbsolute);
    TBool SupportsMimeType(const Brx& aMimeType);
private:
    void CodecThread();
    void Rewind();
    void PullMsg();
    void Queue(Msg* aMsg);
    TBool QueueTrackData() const;
    void ReleaseAudioEncoded();
    TBool DoRead(Bwx& aBuf, TUint aBytes);
private: // ICodecController
    void Read(Bwx& aBuf, TUint aBytes);
    TBool TrySeek(TUint aStreamId, TUint64 aBytePos);
    TUint64 StreamLength() const;
    TUint64 StreamPos() const;
    void OutputDecodedStream(TUint aBitRate, TUint aBitDepth, TUint aSampleRate, TUint aNumChannels, const Brx& aCodecName, TUint64 aTrackLength, TUint64 aSampleStart, TBool aLossless);
    TUint64 OutputAudioPcm(const Brx& aData, TUint aChannels, TUint aSampleRate, TUint aBitDepth, EMediaDataEndian aEndian, TUint64 aTrackOffset);
private: // IMsgProcessor
    Msg* ProcessMsg(MsgAudioEncoded* aMsg);
    Msg* ProcessMsg(MsgAudioPcm* aMsg);
    Msg* ProcessMsg(MsgSilence* aMsg);
    Msg* ProcessMsg(MsgPlayable* aMsg);
    Msg* ProcessMsg(MsgDecodedStream* aMsg);
    Msg* ProcessMsg(MsgTrack* aMsg);
    Msg* ProcessMsg(MsgEncodedStream* aMsg);
    Msg* ProcessMsg(MsgMetaText* aMsg);
    Msg* ProcessMsg(MsgHalt* aMsg);
    Msg* ProcessMsg(MsgFlush* aMsg);
    Msg* ProcessMsg(MsgQuit* aMsg);
private:
    static const TUint kMaxRecogniseBytes = 6 * 1024;
    MsgFactory& iMsgFactory;
    Rewinder iRewinder;
    IPipelineElementUpstream& iUpstreamElement;
    IPipelineElementDownstream& iDownstreamElement;
    Mutex iLock;
    std::vector<CodecBase*> iCodecs;
    ThreadFunctor* iDecoderThread;
    CodecBase* iActiveCodec;
    MsgQueue iQueue;
    Msg* iPendingMsg;
    TBool iQueueTrackData;
    TBool iStreamStarted;
    TBool iStreamEnded;
    TBool iQuit;
    TBool iSeek;
    TUint iSeekSeconds;
    TUint iExpectedFlushId;
    TBool iConsumeExpectedFlush;
    MsgDecodedStream* iPostSeekStreamInfo;
    MsgAudioEncoded* iAudioEncoded;
    TByte iReadBuf[kMaxRecogniseBytes];
    TBool iSeekable;
    TBool iLive;
    IStreamHandler* iStreamHandler;
    TUint iStreamId;
    TUint iSampleRate;
    TUint64 iStreamLength;
    TUint64 iStreamPos;
    TUint iTrackId;
};

} // namespace Codec
} // namespace Media
} // namespace OpenHome

#endif // HEADER_PIPELINE_CODEC
