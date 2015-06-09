#ifndef HEADER_TESTMEDIAPLAYER
#define HEADER_TESTMEDIAPLAYER

#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>

#include <OpenHome/Av/MediaPlayer.h>
#include <OpenHome/Net/Core/DvDevice.h>
#include <OpenHome/Av/Utils/DriverSongcastSender.h>
#include <OpenHome/Media/PipelineManager.h>
#include <OpenHome/Media/UriProviderSingleTrack.h>
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Private/Standard.h>
#include <OpenHome/Private/Debug.h>
#include <OpenHome/Private/OptionParser.h>
#include <OpenHome/Media/Codec/CodecFactory.h>
#include <OpenHome/Media/Protocol/ProtocolFactory.h>
#include <OpenHome/Av/SourceFactory.h>
#include <OpenHome/Av/KvpStore.h>
#include <OpenHome/Av/Raop/Raop.h>
#include <OpenHome/Av/Songcast/OhmTimestamp.h>
#include "RamStore.h"
#include <OpenHome/PowerManager.h>
#include <OpenHome/Av/VolumeManager.h>
#include <OpenHome/Web/WebAppFramework.h>

namespace OpenHome {
    class PowerManager;
namespace Net {
    class DviServerUpnp;
    class DvStack;
    class DvDevice;
    class Shell;
    class ShellCommandDebug;
}
namespace Media {
    class PipelineManager;
    class DriverSongcastSender;
    class IPullableClock;
}
namespace Configuration {
    class ConfigRamStore;
    class ConfigManager;
}
namespace Web {
    class ConfigAppMediaPlayer;
}
namespace Av {
    class RamStore;
namespace Test {

class VolumeProfile : public IVolumeProfile
{
    static const TUint kVolumeMax = 100;
    static const TUint kVolumeDefault = 45;
    static const TUint kVolumeUnity = 80;
    static const TUint kVolumeDefaultLimit = 85;
    static const TUint kVolumeStep = 1;
    static const TUint kVolumeMilliDbPerStep = 1024;
    static const TUint kBalanceMax = 12;
    static const TUint kFadeMax = 10;
private: // from IVolumeProfile
    TUint VolumeMax() const override;
    TUint VolumeDefault() const override;
    TUint VolumeUnity() const override;
    TUint VolumeDefaultLimit() const override;
    TUint VolumeStep() const override;
    TUint VolumeMilliDbPerStep() const override;
    TUint BalanceMax() const override;
    TUint FadeMax() const override;
};
    
class VolumeSinkLogger : public IVolume, public IBalance, public IFade
{
private: // from IVolume
    void SetVolume(TUint aVolume) override;
private: // from IBalance
    void SetBalance(TInt aBalance) override;
private: // from IFade
    void SetFade(TInt aFade) override;
};

class TestMediaPlayer : private Net::IResourceManager, public IPowerHandler/*, public Web::IWebAppFramework*/
{
private:
    static const Brn kSongcastSenderIconFileName;
    static const TUint kTrackCount = 1200;
    // FIXME - make at least kMaxUiTabs a parameter?
    static const TUint kMaxUiTabs = 4;
    static const TUint kUiSendQueueSize = 32;
public:
    TestMediaPlayer(Net::DvStack& aDvStack, const Brx& aUdn, const TChar* aRoom, const TChar* aProductName,
                    const Brx& aTuneInPartnerId, const Brx& aTidalId, const Brx& aQobuzIdSecret, const Brx& aUserAgent);
    virtual ~TestMediaPlayer();
    void SetPullableClock(Media::IPullableClock& aPullableClock);
    void SetSongcastRxTimestamper(IOhmTimestamper& aTimestamper);
    void StopPipeline();
    void AddAttribute(const TChar* aAttribute); // FIXME - only required by Songcasting driver
    virtual void Run();
    virtual void RunWithSemaphore();
    Media::PipelineManager& Pipeline();
    Net::DvDeviceStandard* Device();
protected:
    virtual void RegisterPlugins(Environment& aEnv);
    void DoRegisterPlugins(Environment& aEnv, const Brx& aSupportedProtocols);
private: // from Net::IResourceManager
    void WriteResource(const Brx& aUriTail, TIpAddress aInterface, std::vector<char*>& aLanguageList, Net::IResourceWriter& aResourceWriter) override;
private: // from IPowerHandler
    void PowerUp() override;
    void PowerDown() override;
//private: // from IWebAppFramework
//    void Add(Web::IWebApp* aWebApp, FunctorPresentationUrl aFunctor) override;
private:
    static TUint Hash(const Brx& aBuf);
    static void GenerateMacAddr(Environment& aEnv, TUint aSeed, Bwx& aMacAddr);
    void MacAddrFromUdn(Environment& aEnv, Bwx& aMacAddr);
    void PresentationUrlChanged(const Brx& aUrl);
    void PowerDownUpnp();
    void PowerDownDisable(Net::DvDevice& aDevice);
    void PowerDownUpnpCallback();
    TBool TryDisable(Net::DvDevice& aDevice);
    void Disabled();
protected:
    MediaPlayer* iMediaPlayer;
    Media::LoggingPipelineObserver* iPipelineObserver;
    Net::DvDeviceStandard* iDevice;
    Net::DvDevice* iDeviceUpnpAv;
    RamStore* iRamStore;
    Configuration::ConfigRamStore* iConfigRamStore;
    Semaphore iSemShutdown;
    Net::Shell* iShell;
    Web::WebAppFramework* iAppFramework;
private:
    Semaphore iDisabled;
    IPowerManagerObserver* iPowerObserver;
    Net::ShellCommandDebug* iShellDebug;
    const Brx& iTuneInPartnerId;
    const Brx& iTidalId;
    const Brx& iQobuzIdSecret;
    const Brx& iUserAgent;
    Media::IPullableClock* iPullableClock;
    ObservableBrx iObservableFriendlyName;
    Web::ConfigAppMediaPlayer* iConfigApp;
    Av::IOhmTimestamper* iRxTimestamper;
    VolumeSinkLogger iVolumeLogger;
};

class TestMediaPlayerOptions
{
public:
    TestMediaPlayerOptions();
    void AddOption(TestFramework::Option* aOption);
    TBool Parse(int aArgc, char* aArgv[]);
    TestFramework::OptionString& Room();
    TestFramework::OptionString& Name();
    TestFramework::OptionString& Udn();
    TestFramework::OptionUint& Channel();
    TestFramework::OptionUint& Adapter();
    TestFramework::OptionBool& Loopback();
    TestFramework::OptionString& TuneIn();
    TestFramework::OptionString& Tidal();
    TestFramework::OptionString& Qobuz();
    TestFramework::OptionString& UserAgent();
private:
    TestFramework::OptionParser iParser;
    TestFramework::OptionString iOptionRoom;
    TestFramework::OptionString iOptionName;
    TestFramework::OptionString iOptionUdn;
    TestFramework::OptionUint iOptionChannel;
    TestFramework::OptionUint iOptionAdapter;
    TestFramework::OptionBool iOptionLoopback;
    TestFramework::OptionString iOptionTuneIn;
    TestFramework::OptionString iOptionTidal;
    TestFramework::OptionString iOptionQobuz;
    TestFramework::OptionString iOptionUserAgent;
};

// Not very nice, but only to allow reusable test functions.
class TestMediaPlayerInit
{
public:
    static OpenHome::Net::Library* CreateLibrary(TBool aLoopback, TUint aAdapter);  // creates lib; client must start appropriate stacks
    static void SeedRandomNumberGenerator(Environment& aEnv, const Brx& aRoom, TIpAddress aAddress, Net::DviServerUpnp& aServer);    // seed from room + server port
    static void AppendUniqueId(Environment& aEnv, const Brx& aUserUdn, const Brx& aDefaultUdn, Bwh& aOutput);
};

} // namespace Test
} // namespace Av
} // namespace OpenHome

#endif // HEADER_TESTMEDIAPLAYER
