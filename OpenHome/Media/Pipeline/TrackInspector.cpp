#include <OpenHome/Media/Pipeline/TrackInspector.h>
#include <OpenHome/OhNetTypes.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Media/Pipeline/Msg.h>

using namespace OpenHome;
using namespace OpenHome::Media;

// TrackInspector

TrackInspector::TrackInspector(IPipelineElementUpstream& aUpstreamElement)
    : iUpstreamElement(aUpstreamElement)
    , iTrack(NULL)
{
}

TrackInspector::~TrackInspector()
{
    if (iTrack != NULL) {
        iTrack->RemoveRef();
    }
}

void TrackInspector::AddObserver(ITrackObserver& aObserver)
{
    iObservers.push_back(&aObserver);
}

Msg* TrackInspector::Pull()
{
    Msg* msg = iUpstreamElement.Pull();
    (void)msg->Process(*this);
    return msg;
}

void TrackInspector::NotifyTrackPlaying()
{
    ASSERT(iTrack!=NULL);
    for (TUint i=0; i<iObservers.size(); i++) {
        iObservers[i]->NotifyTrackPlay(*iTrack);
    }
    iTrack->RemoveRef();
    iTrack = NULL;
}

void TrackInspector::NotifyTrackFailed()
{
    ASSERT(iTrack!=NULL);
    for (TUint i=0; i<iObservers.size(); i++) {
        iObservers[i]->NotifyTrackFail(*iTrack);
    }
    iTrack->RemoveRef();
    iTrack = NULL;
}

Msg* TrackInspector::ProcessMsg(MsgMode* aMsg)
{
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgSession* aMsg)
{
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgTrack* aMsg)
{
    if (iTrack != NULL) {
        NotifyTrackFailed();
    }
    iTrack = &aMsg->Track();
    iTrack->AddRef();
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgDelay* aMsg)
{
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgEncodedStream* aMsg)
{
    if (iTrack != NULL && aMsg->Live()) {
        /* Live streams wait for the stopper to tell them to start generating audio.
           Its possible we'll see this msg but no audio for a perfectly good stream
           ...so assume that generation of MsgEncodedStream implies the track is good
           ...(more accurately, not utterly bad) */
        NotifyTrackPlaying();
    }
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgAudioEncoded* aMsg)
{
    ASSERTS();
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgMetaText* aMsg)
{
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgHalt* aMsg)
{
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgFlush* aMsg)
{
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgWait* aMsg)
{
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgDecodedStream* aMsg)
{
    if (iTrack != NULL) {
        NotifyTrackPlaying();
    }
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgAudioPcm* aMsg)
{
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgSilence* aMsg)
{
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgPlayable* aMsg)
{
    ASSERTS();
    return aMsg;
}

Msg* TrackInspector::ProcessMsg(MsgQuit* aMsg)
{
    return aMsg;
}