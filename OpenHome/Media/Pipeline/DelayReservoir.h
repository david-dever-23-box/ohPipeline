#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Private/Thread.h>

namespace OpenHome {
namespace Media {

class DelayReservoir : public MsgReservoir, public IPipelineElementUpstream
{
public:
    DelayReservoir(IPipelineElementUpstream& aUpstream, TUint aThreadPriority, TUint aMaxStreamCount);
    ~DelayReservoir();
    TUint SizeInJiffies() const;
private:
    inline TBool IsFull() const;
    void PullerThread();
private: // from IPipelineElementUpstream
    Msg* Pull() override;
private: // from MsgReservoir
    void ProcessMsgIn(MsgDelay* aMsg) override;
    void ProcessMsgIn(MsgQuit* aMsg) override;
private:
    IPipelineElementUpstream& iUpstream;
    const TUint iMaxStreamCount;
    TUint iMaxJiffies;
    Mutex iLock;
    Semaphore iSem;
    ThreadFunctor* iPullerThread;
    TBool iExit;
};

} //namespace Media
} // namespace OpenHome