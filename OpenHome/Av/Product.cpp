#include <OpenHome/Av/Product.h>
#include <OpenHome/OhNetTypes.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Private/Thread.h>
#include <OpenHome/Av/ProviderProduct.h>
#include <OpenHome/Av/Source.h>
#include <OpenHome/Av/KvpStore.h>
#include <OpenHome/Configuration/ConfigManager.h>
#include <OpenHome/Private/Printer.h>

#include <limits.h>

using namespace OpenHome;
using namespace OpenHome::Net;
using namespace OpenHome::Av;
using namespace OpenHome::Configuration;

const Brn Product::kStartupSourceBase("Startup.Source");
const Brn Product::kConfigIdRoomBase("Product.Room");
const Brn Product::kConfigIdNameBase("Product.Name");

Product::Product(Net::DvDevice& aDevice, IReadStore& aReadStore, IStoreReadWrite& aReadWriteStore,
                 IConfigManagerReader& aConfigReader, Configuration::IConfigManagerWriter& aConfigWriter,
                 IPowerManager& aPowerManager, const Brx& aConfigPrefix)
    : iDevice(aDevice)
    , iReadStore(aReadStore)
    , iConfigWriter(aConfigWriter)
    , iLock("PRDM")
    , iLockDetails("PRDD")
    , iStarted(false)
    , iStandby(false)
    , iCurrentSource(UINT_MAX)
    , iSourceXmlChangeCount(0)
    , iConfigPrefix(aConfigPrefix)
{
    if (iConfigPrefix.Bytes() > 0) {
        iConfigPrefix.Append('.');
    }
    Bws<32> key(iConfigPrefix);
    key.Append(kStartupSourceBase);
    iStartupSource = new StoreText(aReadWriteStore, aPowerManager, kPowerPriorityHighest, key, Brn("Playlist"), ISource::kMaxSourceTypeBytes);
    key.Replace(iConfigPrefix);
    key.Append(kConfigIdRoomBase);
    iConfigProductRoom = &aConfigReader.GetText(key);
    iListenerIdProductRoom = iConfigProductRoom->Subscribe(MakeFunctorGeneric<KeyValuePair<const Brx&>&>(*this, &Product::ProductRoomChanged));
    key.Replace(iConfigPrefix);
    key.Append(kConfigIdNameBase);
    iConfigProductName = &aConfigReader.GetText(key);
    iListenerIdProductName = iConfigProductName->Subscribe(MakeFunctorGeneric<KeyValuePair<const Brx&>&>(*this, &Product::ProductNameChanged));
    iProviderProduct = new ProviderProduct(aDevice, *this);
}

Product::~Product()
{
    for (TUint i=0; i<(TUint)iSources.size(); i++) {
        delete iSources[i];
    }
    iSources.clear();
    delete iProviderProduct;
    iConfigProductName->Unsubscribe(iListenerIdProductName);
    iConfigProductRoom->Unsubscribe(iListenerIdProductRoom);
    delete iStartupSource;
}

void Product::AddObserver(IProductObserver& aObserver)
{
    iObservers.push_back(&aObserver);
}

void Product::Start()
{
    Bws<ISource::kMaxSystemNameBytes> startupSource;
    iStartupSource->Get(startupSource);
    SetCurrentSource(startupSource);
    iStarted = true;
    iSourceXmlChangeCount++;
    for (TUint i=0; i<iObservers.size(); i++) {
        iObservers[i]->Started();
    }
}

void Product::AddSource(ISource* aSource)
{
    ASSERT(!iStarted);
    iSources.push_back(aSource);
    aSource->Initialise(*this, iConfigWriter, iConfigPrefix);
}

void Product::AddAttribute(const TChar* aAttribute)
{
    ASSERT(!iStarted);
    Brn attr(aAttribute);
    AddAttribute(attr);
}

void Product::AddAttribute(const Brx& aAttribute)
{
    if (iAttributes.Bytes() > 0) {
        iAttributes.Append(' ');
    }
    iAttributes.Append(aAttribute);
}

void Product::GetManufacturerDetails(Brn& aName, Brn& aInfo, Brn& aUrl, Brn& aImageUri)
{
    ASSERT(iReadStore.TryReadStoreStaticItem(StaticDataKey::kBufManufacturerName, aName));
    ASSERT(iReadStore.TryReadStoreStaticItem(StaticDataKey::kBufManufacturerInfo, aInfo));
    ASSERT(iReadStore.TryReadStoreStaticItem(StaticDataKey::kBufManufacturerUrl, aUrl));
    ASSERT(iReadStore.TryReadStoreStaticItem(StaticDataKey::kBufManufacturerImageUrl, aImageUri)); // FIXME - generate (at least partially) dynamically
}

void Product::GetModelDetails(Brn& aName, Brn& aInfo, Brn& aUrl, Brn& aImageUri)
{
    ASSERT(iReadStore.TryReadStoreStaticItem(StaticDataKey::kBufModelName, aName));
    ASSERT(iReadStore.TryReadStoreStaticItem(StaticDataKey::kBufModelInfo, aInfo));
    ASSERT(iReadStore.TryReadStoreStaticItem(StaticDataKey::kBufModelUrl, aUrl));
    ASSERT(iReadStore.TryReadStoreStaticItem(StaticDataKey::kBufModelImageUrl, aImageUri)); // FIXME - generate (at least partially) dynamically
}

void Product::GetProductDetails(Bwx& aRoom, Bwx& aName, Brn& aInfo, Brn& aImageUri)
{
    iLockDetails.Wait();
    aRoom.Append(iProductRoom);
    aName.Append(iProductName);
    iLockDetails.Signal();
    ASSERT(iReadStore.TryReadStoreStaticItem(StaticDataKey::kBufModelInfo, aInfo));
    // presentation url
    ASSERT(iReadStore.TryReadStoreStaticItem(StaticDataKey::kBufModelImageUrl, aImageUri));
}

TBool Product::StandbyEnabled() const
{
    AutoMutex a(iLock);
    return iStandby;
}

void Product::SetStandby(TBool aStandby)
{
    iLock.Wait();
    const TBool changed = ((aStandby && !iStandby) || (!aStandby && iStandby));
    if (changed) {
        iStandby = aStandby;
    }
    iLock.Signal();
    if (changed) {
        for (TUint i=0; i<iObservers.size(); i++) {
            iObservers[i]->StandbyChanged();
            // FIXME - other observers to notify. (e.g. to disable any hardware)
        }
    }
}

TUint Product::SourceCount() const
{
    return (TUint)iSources.size();
}

TUint Product::CurrentSourceIndex() const
{
    return iCurrentSource;
}

void Product::GetSourceXml(Bwx& aXml)
{
    aXml.Append("<SourceList>");
    iLock.Wait();
    for (TUint i=0; i<iSources.size(); i++) {
        ISource* src = iSources[i];
        aXml.Append("<Source>");
        AppendTag(aXml, "Name", src->Name());
        AppendTag(aXml, "Type", src->Type());
        AppendTag(aXml, "Visible", src->IsVisible()? Brn("true") : Brn("false"));
        aXml.Append("</Source>");
    }
    iLock.Signal();
    aXml.Append("</SourceList>");
}

void Product::AppendTag(Bwx& aXml, const TChar* aTag, const Brx& aValue)
{
    aXml.Append('<');
    aXml.Append(aTag);
    aXml.Append('>');
    aXml.Append(aValue);
    aXml.Append("</");
    aXml.Append(aTag);
    aXml.Append('>');
}

void Product::ProductRoomChanged(KeyValuePair<const Brx&>& aKvp)
{
    AutoMutex a(iLockDetails);
    iProductRoom.Replace(aKvp.Value());
}

void Product::ProductNameChanged(KeyValuePair<const Brx&>& aKvp)
{
    AutoMutex a(iLockDetails);
    iProductName.Replace(aKvp.Value());
}

void Product::SetCurrentSource(TUint aIndex)
{
    AutoMutex a(iLock);
    if (aIndex >= (TUint)iSources.size()) {
        THROW(AvSourceNotFound);
    }
    if (iCurrentSource == aIndex) {
        return;
    }
    if (iCurrentSource != UINT_MAX) {
        iSources[iCurrentSource]->Deactivate();
    }
    iCurrentSource = aIndex;
    iStartupSource->Set(iSources[iCurrentSource]->SystemName());
    iSources[iCurrentSource]->Activate();

    for (TUint i=0; i<iObservers.size(); i++) {
        iObservers[i]->SourceIndexChanged();
    }
}

void Product::SetCurrentSource(const Brx& aName)
{
    AutoMutex a(iLock);
    // volkano treats [name] as a system name and anything else as a user-defined name.  Do we need to do the same?
    for (TUint i=0; i<(TUint)iSources.size(); i++) {
        if (iSources[i]->Name() == aName) {
            iCurrentSource = i;
            iStartupSource->Set(iSources[iCurrentSource]->SystemName());
            iSources[iCurrentSource]->Activate();
            for (TUint i=0; i<iObservers.size(); i++) {
                iObservers[i]->SourceIndexChanged();
            }
            return;
        }
    }
    THROW(AvSourceNotFound);
}

void Product::GetSourceDetails(TUint aIndex, Bwx& aSystemName, Bwx& aType, Bwx& aName, TBool& aVisible)
{
    AutoMutex a(iLock);
    if (aIndex >= (TUint)iSources.size()) {
        THROW(AvSourceNotFound);
    }
    ISource* source = iSources[aIndex];
    aSystemName.Append(source->SystemName());
    aType.Append(source->Type());
    aName.Append(source->Name());
    aVisible = source->IsVisible();
}

const Brx& Product::Attributes() const
{
    return iAttributes;
}

TUint Product::SourceXmlChangeCount()
{
    return iSourceXmlChangeCount;
}

void Product::Activate(ISource& aSource)
{
    ISource* srcNew = NULL;
    ISource* srcOld = NULL;

    AutoMutex a(iLock);
    // deactivate current (old) source, if one exists
    if (iCurrentSource != UINT_MAX) {
        srcOld = iSources[iCurrentSource];
        srcOld->Deactivate();
    }

    // find and activate new source
    for (TUint i=0; i<(TUint)iSources.size(); i++) {
        if (iSources[i]->Name() == aSource.Name()) {
            iCurrentSource = i;
            iStartupSource->Set(iSources[iCurrentSource]->SystemName());
            srcNew = iSources[i];
            srcNew->Activate();
            for (TUint i=0; i<iObservers.size(); i++) {
                iObservers[i]->SourceIndexChanged();
            }
            return;
        }
    }
    THROW(AvSourceNotFound);
}

void Product::NotifySourceNameChanged(ISource& /*aSource*/)
{
    iLock.Wait();
    iSourceXmlChangeCount++;
    iLock.Signal();
    for (TUint i=0; i<iObservers.size(); i++) {
        iObservers[i]->SourceXmlChanged();
    }
}

void Product::QueryInfo(const Brx& /*aQuery*/, IWriter& /*aWriter*/)
{
}
