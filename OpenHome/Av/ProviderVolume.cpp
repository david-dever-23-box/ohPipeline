#include <OpenHome/Av/ProviderVolume.h>

#include <OpenHome/Buffer.h>
#include <OpenHome/Av/StringIds.h>
#include <OpenHome/Av/Product.h>
#include <OpenHome/Av/ProviderFactory.h>
#include <OpenHome/Media/MuteManager.h>
#include <OpenHome/Media/VolumeManager.h>

using namespace OpenHome;
using namespace OpenHome::Av;
using namespace OpenHome::Configuration;
using namespace OpenHome::Media;
using namespace OpenHome::Net;

// ProviderFactory

DvProvider* ProviderFactory::NewVolume(Product& aProduct, DvDevice& aDevice, IConfigManagerWriter& aConfigManager, IVolumeProfile& aVolumeProfile, IVolume& aVolume, IBalance& aBalance, IMute& aMute)
{ // static
    aProduct.AddAttribute("Volume");
    return new ProviderVolume(aDevice, aConfigManager, aVolumeProfile, aVolume, aBalance, aMute);;
}


// from older .../Preamp/ServiceVolume.cpp
const TInt kActionNotSupportedCode = 801;
const Brn  kActionNotSupportedMsg("Action not supported");

const TInt kInvalidVolumeCode = 811;
const Brn  kInvalidVolumeMsg("Volume invalid");

const TInt kInvalidBalanceCode = 812;
const Brn  kInvalidBalanceMsg("Balance invalid");


/**
 * This class is an implementer of the Linn-specific UPnP volume service. It is
 * the sole manipulator of volume and related values (e.g., balance and mute).
 *
 * All attempts to set the volume must come via this provider (i.e., over UPnP).
 * This class is also responsible for enforcing volume limits.
 *
 * The fade property is not currently applicable, so all fade-specific actions
 * will report that the action is not supported, and the values of
 * fade-specific evented variables have no meaning.
 */

const Brn ProviderVolume::kBalance("Volume.Balance");
const Brn ProviderVolume::kVolumeLimit("Volume.Limit");
const Brn ProviderVolume::kVolumeStartup("Volume.Startup");
const Brn ProviderVolume::kVolumeStartupEnabled("Volume.Startup.Enabled");

ProviderVolume::ProviderVolume(DvDevice& aDevice, IConfigManagerWriter& aConfigManager, IVolumeProfile& aVolumeProfile, IVolume& aVolume, IBalance& aBalance, IMute& aMute)
    : DvProviderAvOpenhomeOrgVolume1(aDevice)
    , iConfigManager(aConfigManager)
    , iVolumeProfile(aVolumeProfile)
    , iVolumeSetter(aVolume)
    , iBalanceSetter(aBalance)
    , iMuteSetter(aMute)
    , iLock("PVOL")
{
    // Linn services are optional, but their actions are not.
    // i.e., they must be implemented all or nothing (so enable all actions,
    // but they can return errors if they are unused).
    EnablePropertyVolume();
    EnablePropertyMute();
    EnablePropertyBalance();
    EnablePropertyFade();
    EnablePropertyVolumeLimit();
    EnablePropertyVolumeMax();
    EnablePropertyVolumeUnity();
    EnablePropertyVolumeSteps();
    EnablePropertyVolumeMilliDbPerStep();
    EnablePropertyBalanceMax();
    EnablePropertyFadeMax();  // N/A to two-channel audio, but required by ohNet

    EnableActionCharacteristics();
    EnableActionSetVolume();
    EnableActionVolumeInc();
    EnableActionVolumeDec();
    EnableActionVolume();
    EnableActionSetBalance();
    EnableActionBalanceInc();
    EnableActionBalanceDec();
    EnableActionBalance();
    EnableActionSetFade();
    EnableActionFadeInc();
    EnableActionFadeDec();
    EnableActionFade();
    EnableActionSetMute();
    EnableActionMute();
    EnableActionVolumeLimit();

    TInt maxBalance = iVolumeProfile.MaxBalance();
    TUint maxVolume = iVolumeProfile.MaxVolume();
    iConfigBalance = new ConfigNum(iConfigManager, kBalance, -(maxBalance), maxBalance, 0);
    iListenerIdBalance = iConfigBalance->Subscribe(MakeFunctorConfigNum(*this, &ProviderVolume::ConfigBalanceChanged));
    iConfigVolumeLimit = new ConfigNum(iConfigManager, kVolumeLimit, 0, maxVolume, maxVolume);
    iListenerIdVolumeLimit = iConfigVolumeLimit->Subscribe(MakeFunctorConfigNum(*this, &ProviderVolume::ConfigVolumeLimitChanged));

    // Only care about startup volume and whether it is enabled when
    // creating this provider; it has no influence at any other time.
    iConfigVolumeStartup = new ConfigNum(iConfigManager, kVolumeStartup, 0, maxVolume, kVolumeStartupDefault);
    std::vector<TUint> choices;
    choices.push_back(eStringIdYes);
    choices.push_back(eStringIdNo);
    iConfigVolumeStartupEnabled = new ConfigChoice(iConfigManager, kVolumeStartupEnabled, choices, eStringIdYes);

    // When we subscribe to a ConfigVal, the initial callback is made from the
    // same thread that made the subscription.
    // - So, we know that the callbacks must have been called (at least) once
    // by the time the Subscribe() method returns.
    TUint iListenerIdVolumeStartup = iConfigVolumeStartup->Subscribe(MakeFunctorConfigNum(*this, &ProviderVolume::ConfigVolumeStartupChanged));
    TUint iListenerIdVolumeStartupEnabled = iConfigVolumeStartupEnabled->Subscribe(MakeFunctorConfigChoice(*this, &ProviderVolume::ConfigVolumeStartupEnabledChanged));

    // We don't care about any further changes to iConfigVolumeStartup or
    // iConfigVolumeStartupEnabled after we've made use of the initial value,
    // so we can unsubscribe. (But don't delete in case other elements wish to
    // make use of them.)
    iConfigVolumeStartup->Unsubscribe(iListenerIdVolumeStartup);
    iConfigVolumeStartupEnabled->Unsubscribe(iListenerIdVolumeStartupEnabled);

    // FIXME - Now, check if we should use a startup volume, or use a
    // previously stored volume (or a default volume).
    TUint volume = kVolumeStartupDefault;
    TBool mute = false;
    TUint volLimit = 0;
    GetPropertyVolumeLimit(volLimit);
    iLock.Wait();
    if (iVolumeStartupEnabled == eStringIdYes) {
        volume = iVolumeStartup;
    }
    iLock.Signal();

    // Adjust volume in case initial val is above limit.
    if (volume > volLimit) {
        volume = volLimit;
    }
    SetPropertyVolume(volume);
    SetPropertyMute(mute);

    iVolumeSetter.SetVolume(volume);
    if (mute) {
        iMuteSetter.Mute();
    }
    else {
        iMuteSetter.Unmute();
    }
    // iBalanceSetter is set via *BalanceChanged() callback

    //SetPropertyVolume(0);     // set above
    //SetPropertyMute(false);   // set above
    //SetPropertyBalance();     // set via BalanceChanged callback
    //SetPropertyVolumeLimit(); // set via VolumeLimitChanged callback
    SetPropertyVolumeMax(iVolumeProfile.MaxVolume());
    SetPropertyVolumeUnity(iVolumeProfile.VolumeUnity());
    SetPropertyVolumeSteps(iVolumeProfile.VolumeSteps());
    SetPropertyVolumeMilliDbPerStep(iVolumeProfile.VolumeMilliDbPerStep());
    SetPropertyBalanceMax(iVolumeProfile.MaxBalance());
    SetPropertyFade(0);       // unused
    SetPropertyFadeMax(0);    // unused
}

ProviderVolume::~ProviderVolume()
{
    iConfigBalance->Unsubscribe(iListenerIdBalance);
    iConfigVolumeLimit->Unsubscribe(iListenerIdVolumeLimit);
    delete iConfigVolumeStartupEnabled;
    delete iConfigVolumeStartup;
    delete iConfigVolumeLimit;
    delete iConfigBalance;
}

void ProviderVolume::Characteristics(IDvInvocation& aInvocation, IDvInvocationResponseUint& aVolumeMax, IDvInvocationResponseUint& aVolumeUnity, IDvInvocationResponseUint& aVolumeSteps, IDvInvocationResponseUint& aVolumeMilliDbPerStep, IDvInvocationResponseUint& aBalanceMax, IDvInvocationResponseUint& aFadeMax)
{
    TUint maxVol = 0;
    TUint unityVol = 0;
    TUint volSteps = 0;
    TUint milliDbPerVolStep = 0;
    TUint maxBalance = 0;
    TUint maxFade = 0;

    GetPropertyVolumeMax(maxVol);
    GetPropertyVolumeUnity(unityVol);
    GetPropertyVolumeSteps(volSteps);
    GetPropertyVolumeMilliDbPerStep(milliDbPerVolStep);
    GetPropertyBalanceMax(maxBalance);
    GetPropertyFadeMax(maxFade);

    aInvocation.StartResponse();
    aVolumeMax.Write(maxVol);
    aVolumeUnity.Write(unityVol);
    aVolumeSteps.Write(volSteps);
    aVolumeMilliDbPerStep.Write(milliDbPerVolStep);
    aBalanceMax.Write(maxBalance);
    aFadeMax.Write(maxFade);
    aInvocation.EndResponse();
}

void ProviderVolume::SetVolume(IDvInvocation& aInvocation, TUint aValue)
{
    TUint volCurrent = 0;
    GetPropertyVolume(volCurrent);
    HelperSetVolume(aInvocation, volCurrent, aValue);
}

void ProviderVolume::VolumeInc(IDvInvocation& aInvocation)
{
    TUint volCurrent = 0;
    GetPropertyVolume(volCurrent);
    TUint volNew = volCurrent+1;
    HelperSetVolume(aInvocation, volCurrent, volNew);
}

void ProviderVolume::VolumeDec(IDvInvocation& aInvocation)
{
    TUint volCurrent = 0;
    GetPropertyVolume(volCurrent);
    TUint volNew = volCurrent-1;
    HelperSetVolume(aInvocation, volCurrent, volNew);
}

void ProviderVolume::Volume(IDvInvocation& aInvocation, IDvInvocationResponseUint& aValue)
{
    TUint vol = 0;
    GetPropertyVolume(vol);
    aInvocation.StartResponse();
    aValue.Write(vol);
    aInvocation.EndResponse();
}

void ProviderVolume::SetBalance(IDvInvocation& aInvocation, TInt aValue)
{
    // FIXME - we should update iConfigBalance for consistency.
    // When we do that, its callback will be made; we should let that be the
    // sole setter of balance, as it automatically does bounds checking for us
    // (and only notifies if the value has actually been changed).
    TInt balCurrent = 0;
    GetPropertyBalance(balCurrent);
    HelperSetBalance(aInvocation, balCurrent, aValue);
}

void ProviderVolume::BalanceInc(IDvInvocation& aInvocation)
{
    TInt balCurrent = 0;
    GetPropertyBalance(balCurrent);
    TUint balNew = balCurrent+1;
    HelperSetBalance(aInvocation, balCurrent, balNew);
}

void ProviderVolume::BalanceDec(IDvInvocation& aInvocation)
{
    TInt balCurrent = 0;
    GetPropertyBalance(balCurrent);
    TInt balNew = balCurrent-1;
    HelperSetBalance(aInvocation, balCurrent, balNew);
}

void ProviderVolume::Balance(IDvInvocation& aInvocation, IDvInvocationResponseInt& aValue)
{
    TInt bal = 0;
    GetPropertyBalance(bal);
    aInvocation.StartResponse();
    aValue.Write(bal);
    aInvocation.EndResponse();
}

void ProviderVolume::SetFade(IDvInvocation& aInvocation, TInt /*aValue*/)
{
    aInvocation.Error(kActionNotSupportedCode, kActionNotSupportedMsg);
}

void ProviderVolume::FadeInc(IDvInvocation& aInvocation)
{
    aInvocation.Error(kActionNotSupportedCode, kActionNotSupportedMsg);
}

void ProviderVolume::FadeDec(IDvInvocation& aInvocation)
{
    aInvocation.Error(kActionNotSupportedCode, kActionNotSupportedMsg);
}

void ProviderVolume::Fade(IDvInvocation& aInvocation, IDvInvocationResponseInt& /*aValue*/)
{
    aInvocation.Error(kActionNotSupportedCode, kActionNotSupportedMsg);
}

void ProviderVolume::SetMute(IDvInvocation& aInvocation, TBool aValue)
{
    TBool mute = false;
    GetPropertyMute(mute);

    if (aValue != mute) {
        SetPropertyMute(aValue);

        if (aValue) {
            iMuteSetter.Mute();
        }
        else {
            iMuteSetter.Unmute();
        }
    }

    aInvocation.StartResponse();
    aInvocation.EndResponse();
}

void ProviderVolume::Mute(IDvInvocation& aInvocation, IDvInvocationResponseBool& aValue)
{
    TBool muted = false;
    GetPropertyMute(muted);
    aInvocation.StartResponse();
    aValue.Write(muted);
    aInvocation.EndResponse();
}

void ProviderVolume::VolumeLimit(IDvInvocation& aInvocation, IDvInvocationResponseUint& aValue)
{
    TUint volLimit = 0;
    GetPropertyVolumeLimit(volLimit);
    aInvocation.StartResponse();
    aValue.Write(volLimit);
    aInvocation.EndResponse();
}

void ProviderVolume::HelperSetVolume(IDvInvocation& aInvocation, TUint aVolumeCurrent, TUint aVolumeNew)
{
    TUint volLimit = 0;
    GetPropertyVolumeLimit(volLimit);

    if (aVolumeNew != aVolumeCurrent) {
        if (aVolumeNew > volLimit) {
            // IDvInvocation::Error() throws an exception, so this method returns immediately.
            aInvocation.Error(kInvalidVolumeCode, kInvalidVolumeMsg);
        }
        SetPropertyVolume(aVolumeNew);
        iVolumeSetter.SetVolume(aVolumeNew);
    }
    aInvocation.StartResponse();
    aInvocation.EndResponse();
}

void ProviderVolume::HelperSetBalance(IDvInvocation& aInvocation, TInt aBalanceCurrent, TInt aBalanceNew)
{
    TUint balLimit = 0;
    GetPropertyBalanceMax(balLimit);

    // Get absolute value of balance.
    TUint balAbs = 0;
    if (aBalanceNew < 0) {
        balAbs = aBalanceNew * -1;
    }
    else {
        balAbs = aBalanceNew;
    }

    if (aBalanceNew != aBalanceCurrent) {
        if (balAbs > balLimit) {
            aInvocation.Error(kInvalidBalanceCode, kInvalidBalanceMsg);
        }
        SetPropertyBalance(aBalanceNew);
        iBalanceSetter.SetBalance(aBalanceNew);
    }
    aInvocation.StartResponse();
    aInvocation.EndResponse();
}

void ProviderVolume::ConfigBalanceChanged(ConfigNum::KvpNum& aKvp)
{
    // Balance limits are enforced by iConfigBalance, and this callback is only
    // made when the value has changed.
    TInt balance = aKvp.Value();
    SetPropertyBalance(balance);
    iBalanceSetter.SetBalance(balance);
}

void ProviderVolume::ConfigVolumeLimitChanged(ConfigNum::KvpNum& aKvp)
{
    // Volume limit is enforced by limits in iConfigVolumeLimit.
    TUint volumeLimit = aKvp.Value();
    SetPropertyVolumeLimit(volumeLimit);
    // FIXME - legacy devices can receive volume limit cmds (and send them
    // back) so, although we enforce volume limit here, still send it to
    // device?
}

void ProviderVolume::ConfigVolumeStartupChanged(ConfigNum::KvpNum& aKvp)
{
    AutoMutex a(iLock);
    iVolumeStartup = aKvp.Value();  // shouldn't be set outwith c'tor
}

void ProviderVolume::ConfigVolumeStartupEnabledChanged(ConfigChoice::KvpChoice& aKvp)
{
    AutoMutex a(iLock);
    iVolumeStartupEnabled = aKvp.Value();  // shouldn't be set outwith c'tor
}
