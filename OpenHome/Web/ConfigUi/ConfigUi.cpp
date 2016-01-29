#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Web/ConfigUi/ConfigUi.h>
#include <OpenHome/Private/Ascii.h>
#include <OpenHome/Private/NetworkAdapterList.h>
#include <OpenHome/Private/Parser.h>
#include <OpenHome/Av/Product.h>
#include <OpenHome/Av/Source.h>
#include <OpenHome/Av/Utils/Json.h>
#include <OpenHome/Private/Uri.h>
#include <OpenHome/Media/InfoProvider.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Av/RebootHandler.h>
#include <OpenHome/Private/Debug.h>

#include <limits>

using namespace OpenHome;
using namespace OpenHome::Av;
using namespace OpenHome::Configuration;
using namespace OpenHome::Media;
using namespace OpenHome::Web;


// WritableJsonEmpty

void WritableJsonEmpty::Write(IWriter& aWriter) const
{
    aWriter.Write(Brn("{}"));
}


// WritableJsonInfo

WritableJsonInfo::WritableJsonInfo(TBool aRebootRequired)
    : iRebootRequired(aRebootRequired)
{
}

void WritableJsonInfo::Write(IWriter& aWriter) const
{
    aWriter.Write(Brn("{\"reboot-required\":"));
    WriteBool(aWriter, iRebootRequired);
    aWriter.Write(Brn("}"));
}

void WritableJsonInfo::WriteBool(IWriter& aWriter, TBool aValue)
{
    if (aValue) {
        aWriter.Write(Brn("true"));
    }
    else {
        aWriter.Write(Brn("false"));
    }
}


// ConfigMessageBase

ConfigMessageBase::ConfigMessageBase(AllocatorBase& aAllocator)
    : Allocated(aAllocator)
    , iWriterAdditional(nullptr)
{
}

void ConfigMessageBase::Set(const IWritable& aJsonWriter) {
    iWriterAdditional = &aJsonWriter;
}

void ConfigMessageBase::Clear()
{
    iWriterAdditional = nullptr;
}

void ConfigMessageBase::Send(IWriter& aWriter)
{
    // {
    //     "key": aKey,
    //     "value": aValue,
    //     "type": aType,
    //     "meta": {}
    //     "info": {}
    // }

    aWriter.Write(Brn("{"));

    aWriter.Write(Brn("\"key\":"));
    WriteKey(aWriter);
    aWriter.Write(Brn(","));

    aWriter.Write(Brn("\"value\":"));
    WriteValue(aWriter);
    aWriter.Write(Brn(","));

    aWriter.Write(Brn("\"type\":"));
    WriteType(aWriter);
    aWriter.Write(Brn(","));

    aWriter.Write(Brn("\"meta\":{"));
    WriteMeta(aWriter);
    aWriter.Write(Brn("},"));

    aWriter.Write(Brn("\"info\":"));
    iWriterAdditional->Write(aWriter);

    aWriter.Write(Brn("}"));
}

void ConfigMessageBase::Destroy()
{
    RemoveRef();
}


// ConfigMessageNum

ConfigMessageNum::ConfigMessageNum(AllocatorBase& aAllocator)
    : ConfigMessageBase(aAllocator)
    , iNum(nullptr)
    , iValue(std::numeric_limits<TInt>::max())
{
}

void ConfigMessageNum::Set(ConfigNum& aNum, TInt aValue, const IWritable& aJsonWriter)
{
    ConfigMessageBase::Set(aJsonWriter);
    ASSERT(iNum == nullptr);
    iNum = &aNum;
    iValue = aValue;
}

void ConfigMessageNum::Clear()
{
    ConfigMessageBase::Clear();
    ASSERT(iNum != nullptr);
    iNum = nullptr;
    iValue = std::numeric_limits<TInt>::max();
}

void ConfigMessageNum::WriteKey(IWriter& aWriter)
{
    aWriter.Write(Brn("\""));
    Json::Escape(aWriter, iNum->Key());
    aWriter.Write(Brn("\""));
}

void ConfigMessageNum::WriteValue(IWriter& aWriter)
{
    Ascii::StreamWriteInt(aWriter, iValue);
}

void ConfigMessageNum::WriteType(IWriter& aWriter)
{
    aWriter.Write(Brn("\"numeric\""));
}

void ConfigMessageNum::WriteMeta(IWriter& aWriter)
{
    aWriter.Write(Brn("\"default\":"));
    Ascii::StreamWriteInt(aWriter, iNum->Default());
    aWriter.Write(Brn(","));
    aWriter.Write(Brn("\"min\":"));
    Ascii::StreamWriteInt(aWriter, iNum->Min());
    aWriter.Write(Brn(","));
    aWriter.Write(Brn("\"max\":"));
    Ascii::StreamWriteInt(aWriter, iNum->Max());
}


// ConfigChoiceMappingWriterJson

ConfigChoiceMappingWriterJson::ConfigChoiceMappingWriterJson()
    : iStarted(false)
{
}

void ConfigChoiceMappingWriterJson::Write(IWriter& aWriter, TUint aChoice, const Brx& aMapping)
{
    if (!iStarted) {
        aWriter.Write(Brn("\"options\":["));
    }
    else {
        aWriter.Write(',');
    }

    aWriter.Write(Brn("{"));
    aWriter.Write(Brn("\"id\": "));
    Ascii::StreamWriteUint(aWriter, aChoice);
    aWriter.Write(Brn(",\"value\": \""));
    Json::Escape(aWriter, aMapping);
    aWriter.Write(Brn("\"}"));
    iStarted = true;
}

void ConfigChoiceMappingWriterJson::WriteComplete(IWriter& aWriter)
{
    aWriter.Write(Brn("]"));
}


// ConfigChoiceMapperResourceFile

ConfigChoiceMapperResourceFile::ConfigChoiceMapperResourceFile(const Brx& aKey, const std::vector<TUint>& aChoices, IWriter& aWriter, IConfigChoiceMappingWriter& aMappingWriter)
    : iKey(aKey)
    , iChoices(aChoices)
    , iWriter(aWriter)
    , iMappingWriter(aMappingWriter)
    , iChoicesIndex(0)
    , iFoundKey(false)
{
}

TBool ConfigChoiceMapperResourceFile::ProcessLine(const Brx& aLine)
{
    if (!iFoundKey) {
        iFoundKey = Ascii::Contains(aLine, iKey);
        return true;
    }
    Parser p(aLine);
    Brn idBuf = p.Next();
    Brn valueBuf = p.NextToEnd();
    ASSERT(valueBuf.Bytes() > 0);
    try {
        TUint id = Ascii::Uint(idBuf);
        ASSERT(id == iChoices[iChoicesIndex]);
    }
    catch (AsciiError&) {
        ASSERTS();
    }

    try {
        iMappingWriter.Write(iWriter, iChoices[iChoicesIndex], valueBuf);
        if (++iChoicesIndex == iChoices.size()) {
            iMappingWriter.WriteComplete(iWriter);
            return false;
        }
    }
    catch (WriterError&) {
        LOG(kHttp, "ConfigChoiceMapperResourceFile::ProcessLine WriterError");
        return false;
    }
    return true;
}


// ConfigMessageChoice

ConfigMessageChoice::ConfigMessageChoice(AllocatorBase& aAllocator)
    : ConfigMessageBase(aAllocator)
    , iLanguageResourceManager(nullptr)
    , iChoice(nullptr)
    , iValue(std::numeric_limits<TUint>::max())
{
}

void ConfigMessageChoice::Set(ConfigChoice& aChoice, TUint aValue, const IWritable& aJsonWriter, ILanguageResourceManager& aLanguageResourceManager, std::vector<Bws<10>>& aLanguageList)
{
    ConfigMessageBase::Set(aJsonWriter);
    ASSERT(iChoice == nullptr);
    iChoice = &aChoice;
    iValue = aValue;
    iLanguageResourceManager = &aLanguageResourceManager;
    iLanguageList = &aLanguageList;
}

void ConfigMessageChoice::Clear()
{
    ConfigMessageBase::Clear();
    ASSERT(iChoice != nullptr);
    iChoice = nullptr;
    iValue = std::numeric_limits<TUint>::max();
    iLanguageResourceManager = nullptr;
    iLanguageList = nullptr;
}

void ConfigMessageChoice::WriteKey(IWriter& aWriter)
{
    aWriter.Write(Brn("\""));
    Json::Escape(aWriter, iChoice->Key());
    aWriter.Write(Brn("\""));
}

void ConfigMessageChoice::WriteValue(IWriter& aWriter)
{
    Ascii::StreamWriteUint(aWriter, iValue);
}

void ConfigMessageChoice::WriteType(IWriter& aWriter)
{
    aWriter.Write(Brn("\"choice\""));
}

void ConfigMessageChoice::WriteMeta(IWriter& aWriter)
{
    aWriter.Write(Brn("\"default\":"));
    Ascii::StreamWriteUint(aWriter, iChoice->Default());
    aWriter.Write(Brn(","));
    if (iChoice->HasInternalMapping()) {
        IConfigChoiceMapper& mapper = iChoice->Mapper();
        ConfigChoiceMappingWriterJson mappingWriter;
        mapper.Write(aWriter, mappingWriter);
    }
    else {
        // Read mapping from file.
        static const Brn kConfigOptionsFile("ConfigOptions.txt");
        const std::vector<TUint>& choices = iChoice->Choices();
        ConfigChoiceMappingWriterJson mappingWriter;
        ConfigChoiceMapperResourceFile mapper(iChoice->Key(), choices, aWriter, mappingWriter);
        ILanguageResourceReader* resourceHandler = &iLanguageResourceManager->CreateLanguageResourceHandler(kConfigOptionsFile, *iLanguageList);
        resourceHandler->Process(mapper);
    }
}


// ConfigMessageText

ConfigMessageText::ConfigMessageText(AllocatorBase& aAllocator)
    : ConfigMessageBase(aAllocator)
    , iText(nullptr)
    , iValue(kMaxBytes)
{
}

void ConfigMessageText::Set(ConfigText& aText, const OpenHome::Brx& aValue, const IWritable& aJsonWriter)
{
    ConfigMessageBase::Set(aJsonWriter);
    ASSERT(iText == nullptr);
    iText = &aText;
    iValue.Replace(aValue);
}

void ConfigMessageText::Clear()
{
    ConfigMessageBase::Clear();
    ASSERT(iText != nullptr);
    iText = nullptr;
    iValue.SetBytes(0);
}

void ConfigMessageText::WriteKey(IWriter& aWriter)
{
    aWriter.Write(Brn("\""));
    Json::Escape(aWriter, iText->Key());
    aWriter.Write(Brn("\""));
}

void ConfigMessageText::WriteValue(IWriter& aWriter)
{
    aWriter.Write(Brn("\""));
    Json::Escape(aWriter, iValue);
    aWriter.Write(Brn("\""));
}

void ConfigMessageText::WriteType(IWriter& aWriter)
{
    aWriter.Write(Brn("\"text\""));
}

void ConfigMessageText::WriteMeta(IWriter& aWriter)
{
    aWriter.Write(Brn("\"default\":"));
    aWriter.Write(Brn("\""));
    Json::Escape(aWriter, iText->Default());
    aWriter.Write(Brn("\""));
    aWriter.Write(Brn(","));
    aWriter.Write(Brn("\"maxlength\":"));
    Ascii::StreamWriteUint(aWriter, iText->MaxLength());
}


// ConfigMessageAllocator

ConfigMessageAllocator::ConfigMessageAllocator(IInfoAggregator& aInfoAggregator, TUint /*aMsgCountReadOnly*/, TUint aMsgCountNum, TUint aMsgCountChoice, TUint aMsgCountText, ILanguageResourceManager& aLanguageResourceManager)
    : /*iAllocatorMsgReadOnly("ConfigMessageReadOnly", aMsgCountReadOnly, aInfoAggregator)
    ,*/ iAllocatorMsgNum("ConfigMessageNum", aMsgCountNum, aInfoAggregator)
    , iAllocatorMsgChoice("ConfigMessageChoice", aMsgCountChoice, aInfoAggregator)
    , iAllocatorMsgText("ConfigMessageText", aMsgCountText, aInfoAggregator)
    , iLanguageResourceManager(aLanguageResourceManager)
{
}

ITabMessage* ConfigMessageAllocator::AllocateReadOnly(const Brx& /*aKey*/, const Brx& /*aValue*/)
{
    return nullptr;
}

ITabMessage* ConfigMessageAllocator::AllocateNum(ConfigNum& aNum, TInt aValue, const IWritable& aJsonWriter)
{
    ConfigMessageNum* msg = iAllocatorMsgNum.Allocate();
    msg->Set(aNum, aValue, aJsonWriter);
    return msg;
}

ITabMessage* ConfigMessageAllocator::AllocateChoice(ConfigChoice& aChoice, TUint aValue, const IWritable& aJsonWriter, std::vector<Bws<10>>& aLanguageList)
{
    ConfigMessageChoice* msg = iAllocatorMsgChoice.Allocate();
    msg->Set(aChoice, aValue, aJsonWriter, iLanguageResourceManager, aLanguageList);
    return msg;
}

ITabMessage* ConfigMessageAllocator::AllocateText(ConfigText& aText, const Brx& aValue, const IWritable& aJsonWriter)
{
    ConfigMessageText* msg = iAllocatorMsgText.Allocate();
    msg->Set(aText, aValue, aJsonWriter);
    return msg;
}


// JsonStringParser

Brn JsonStringParser::ParseString(const Brx& aBuffer, Brn& aRemaining)
{
    TUint offset = 0;

    // Skip any whitespace.
    for (TUint i=0; i<aBuffer.Bytes(); i++) {
        if (!Ascii::IsWhitespace(aBuffer[i])) {
            offset = i;
            break;
        }
    }

    if (aBuffer[offset] != '"') {
        THROW(JsonStringError);
    }
    offset++;   // Move past opening '"'.

    for (TUint i=offset; i<aBuffer.Bytes(); i++) {
        if (aBuffer[i] == '"') {
            if (aBuffer[i-1] != '\\') {
                const TUint bytes = i-offset;
                i++;
                ASSERT(aBuffer.Bytes() > i);
                if (aBuffer.Bytes()-i == 0) {
                    aRemaining.Set(Brn::Empty());
                }
                else {
                    aRemaining.Set(aBuffer.Ptr()+i, aBuffer.Bytes()-i);
                }

                if (bytes == 0) {
                    return Brn::Empty();
                }
                else {
                    return Brn(aBuffer.Ptr()+offset, bytes);
                }
            }
        }
    }

    THROW(JsonStringError);
}


// ConfigTabReceiver

ConfigTabReceiver::ConfigTabReceiver()
{
}

void ConfigTabReceiver::Receive(const Brx& aMessage)
{
    // FIXME - what if aMessage is malformed? - call some form of error handler?
    // FIXME - this should maybe also take an IWriter to allow writing out of a response (which could be none if successful, and an error description if unsuccessful/malformed request).

    // Parse JSON response.
    Bws<128> keyBuf;
    Bws<1024> valueBuf;
    Brn remaining(aMessage);

    LOG(kHttp, "ConfigTabReceiver::Receive:\n%.*s\n", PBUF(aMessage));

    try {
        Parser p(aMessage);
        (void)p.Next('{');
        Brn request = JsonStringParser::ParseString(p.Remaining(), remaining);

        if (request != Brn("request")) {
            LOG(kHttp, "ConfigTabReceiver::Receive Unknown response.\n");
            return;
        }

        p.Set(remaining);
        (void)p.Next('{');
        (void)JsonStringParser::ParseString(p.Remaining(), remaining);  // "type"
        p.Set(remaining);
        (void)p.Next(':');
        Brn type = JsonStringParser::ParseString(p.Remaining(), remaining);

        if (type == Brn("update")) {
            p.Set(remaining);
            (void)p.Next(',');
            (void)JsonStringParser::ParseString(p.Remaining(), remaining);  // "key"

            p.Set(remaining);
            (void)p.Next(':');
            Brn key = JsonStringParser::ParseString(p.Remaining(), remaining);

            p.Set(remaining);
            (void)p.Next(',');
            (void)JsonStringParser::ParseString(p.Remaining(), remaining);  // "value"

            p.Set(remaining);
            (void)p.Next(':');
            Brn value = JsonStringParser::ParseString(p.Remaining(), remaining);

            keyBuf.Replace(key);
            Json::Unescape(keyBuf);
            valueBuf.Replace(value);
            Json::Unescape(valueBuf);
            Receive(keyBuf, valueBuf);
        }
        else if (type == Brn("reboot")) {
            // FIXME - passing on reboot call here means that the DS may reboot before this call returns, so the WebAppFramework may not get chance to send a response to the UI (but does that matter, as the device is going to abruptly disappear at some point in the near future?).
            Reboot();
        }
    }
    catch (JsonStringError&) {
        LOG(kHttp, "ConfigTabReceiver::Receive caught JsonStringError: %.*s\n", PBUF(aMessage));
    }
}


// ConfigTab

const TUint ConfigTab::kInvalidSubscription = OpenHome::Configuration::IConfigManager::kSubscriptionIdInvalid;

ConfigTab::ConfigTab(TUint aId, IConfigMessageAllocator& aMessageAllocator, IConfigManager& aConfigManager, IJsonInfoProvider& aInfoProvider, IRebootHandler& aRebootHandler)
    : iId(aId)
    , iMsgAllocator(aMessageAllocator)
    , iConfigManager(aConfigManager)
    , iInfoProvider(aInfoProvider)
    , iRebootHandler(aRebootHandler)
    , iHandler(nullptr)
    , iStarted(false)
{
}

ConfigTab::~ConfigTab()
{
    if (iHandler != nullptr) {
        Destroy();
    }
}

void ConfigTab::AddKeyNum(const Brx& aKey)
{
    ASSERT(!iStarted);
    iConfigNums.push_back(SubscriptionPair(Brn(aKey),kInvalidSubscription));
}

void ConfigTab::AddKeyChoice(const Brx& aKey)
{
    ASSERT(!iStarted);
    iConfigChoices.push_back(SubscriptionPair(Brn(aKey),kInvalidSubscription));
}

void ConfigTab::AddKeyText(const Brx& aKey)
{
    ASSERT(!iStarted);
    iConfigTexts.push_back(SubscriptionPair(Brn(aKey),kInvalidSubscription));
}

// FIXME - remove?
void ConfigTab::Start()
{
    ASSERT(!iStarted);
    ASSERT(iConfigNums.size()>0 || iConfigChoices.size()>0 || iConfigTexts.size() > 0);
    iStarted = true;
}

TBool ConfigTab::Allocated() const
{
    TBool allocated = iHandler != nullptr;
    return allocated;
}

void ConfigTab::SetHandler(ITabHandler& aHandler, const std::vector<Bws<10>>& aLanguageList)
{
    LOG(kHttp, "ConfigTab::SetHandler iId: %u\n", iId);
    ASSERT(iHandler == nullptr);
    iLanguageList.clear();
    for (auto it=aLanguageList.begin(); it!=aLanguageList.end(); ++it) {
        iLanguageList.push_back(*it);
    }
    iHandler = &aHandler;
    for (TUint i=0; i<iConfigNums.size(); i++) {
        const Brx& key = iConfigNums[i].first;
        ConfigNum& num = iConfigManager.GetNum(key);
        TUint subscription = num.Subscribe(MakeFunctorConfigNum(*this, &ConfigTab::ConfigNumCallback));
        iConfigNums[i].second = subscription;
    }
    for (TUint i=0; i<iConfigChoices.size(); i++) {
        const Brx& key = iConfigChoices[i].first;
        ConfigChoice& choice = iConfigManager.GetChoice(key);
        TUint subscription = choice.Subscribe(MakeFunctorConfigChoice(*this, &ConfigTab::ConfigChoiceCallback));
        iConfigChoices[i].second = subscription;
    }
    for (TUint i=0; i<iConfigTexts.size(); i++) {
        const Brx& key = iConfigTexts[i].first;
        ConfigText& text = iConfigManager.GetText(key);
        TUint subscription = text.Subscribe(MakeFunctorConfigText(*this, &ConfigTab::ConfigTextCallback));
        iConfigTexts[i].second = subscription;
    }
}

void ConfigTab::Receive(const Brx& aKey, const Brx& aValue)
{
    // TabManager in HttpFramework should handle any race between Destroy() and
    // Receive() being called, as it will defer destruction of a tab until all
    // references are removed.
    if (iConfigManager.Has(aKey)) {
        ISerialisable& ser = iConfigManager.Get(aKey);
        try {
            ser.Deserialise(aValue);
        }
        // No exceptions should be thrown because all input verification should
        // be handled by browser side.
        catch (ConfigNotANumber&) {
            ASSERTS();
        }
        catch (ConfigValueOutOfRange&) {
            ASSERTS();
        }
        catch (ConfigValueTooLong&) {
            ASSERTS();
        }
        catch (ConfigInvalidSelection&) {
            ASSERTS();
        }
    }
    else {
        ASSERTS(); // Browser code tried to pass in an invalid ConfigVal. Developer error.
    }
}

void ConfigTab::Reboot()
{
    iRebootHandler.Reboot(Brn("ConfigTab::Reboot"));
}

void ConfigTab::Destroy()
{
    LOG(kHttp, "ConfigTab::Destroy iId: %u\n", iId);
    ASSERT(iHandler != nullptr);
    iHandler = nullptr;

    for (TUint i=0; i<iConfigNums.size(); i++) {
        const Brx& key = iConfigNums[i].first;
        ConfigNum& num = iConfigManager.GetNum(key);
        num.Unsubscribe(iConfigNums[i].second);
        iConfigNums[i].second = kInvalidSubscription;
    }
    for (TUint i=0; i<iConfigChoices.size(); i++) {
        const Brx& key = iConfigChoices[i].first;
        ConfigChoice& choice = iConfigManager.GetChoice(key);
        choice.Unsubscribe(iConfigChoices[i].second);
        iConfigChoices[i].second = kInvalidSubscription;
    }
    for (TUint i=0; i<iConfigTexts.size(); i++) {
        const Brx& key = iConfigTexts[i].first;
        ConfigText& text = iConfigManager.GetText(key);
        text.Unsubscribe(iConfigTexts[i].second);
        iConfigTexts[i].second = kInvalidSubscription;
    }
}

void ConfigTab::ConfigNumCallback(ConfigNum::KvpNum& aKvp)
{
    ASSERT(iHandler != nullptr);
    ConfigNum& num = iConfigManager.GetNum(aKvp.Key());
    const WritableJsonInfo& info = iInfoProvider.GetInfo(aKvp.Key());
    // FIXME - because JSON is static and now stored in ConfigApp, it means
    // that ConfigMessages can also now just take a reference to the JSON
    // instead of copying it.
    ITabMessage* msg = iMsgAllocator.AllocateNum(num, aKvp.Value(), info);
    iHandler->Send(*msg);
}

void ConfigTab::ConfigChoiceCallback(ConfigChoice::KvpChoice& aKvp)
{
    ASSERT(iHandler != nullptr);
    ConfigChoice& choice = iConfigManager.GetChoice(aKvp.Key());
    const WritableJsonInfo& info = iInfoProvider.GetInfo(aKvp.Key());
    ITabMessage* msg = iMsgAllocator.AllocateChoice(choice, aKvp.Value(), info, iLanguageList);
    iHandler->Send(*msg);
}

void ConfigTab::ConfigTextCallback(ConfigText::KvpText& aKvp)
{
    ASSERT(iHandler != nullptr);
    ConfigText& text = iConfigManager.GetText(aKvp.Key());
    const WritableJsonInfo& json = iInfoProvider.GetInfo(aKvp.Key());
    ITabMessage* msg = iMsgAllocator.AllocateText(text, aKvp.Value(), json);
    iHandler->Send(*msg);
}


// ConfigAppBase

const Brn ConfigAppBase::kLangRoot("lang");
const Brn ConfigAppBase::kDefaultLanguage("en-gb");

ConfigAppBase::ConfigAppBase(IInfoAggregator& aInfoAggregator, IConfigManager& aConfigManager, IConfigAppResourceHandlerFactory& aResourceHandlerFactory, const Brx& aResourcePrefix, const Brx& aResourceDir, TUint aMaxTabs, TUint aSendQueueSize, IRebootHandler& aRebootHandler)
    : iConfigManager(aConfigManager)
    , iLangResourceDir(aResourceDir.Bytes()+1+kLangRoot.Bytes()+1)  // "<aResourceDir>/<kLangRoot>/"
    , iResourcePrefix(aResourcePrefix)
    , iLock("COAL")
{
    Log::Print("ConfigAppBase::ConfigAppBase iResourcePrefix: ");
    Log::Print(iResourcePrefix);
    Log::Print("\n");

    iLangResourceDir.Replace(aResourceDir);
    if (iLangResourceDir.Bytes() == 0 || iLangResourceDir[iLangResourceDir.Bytes()-1] != '/') {
        iLangResourceDir.Append('/');
    }
    iLangResourceDir.Append(kLangRoot);
    iLangResourceDir.Append('/');

    iMsgAllocator = new ConfigMessageAllocator(aInfoAggregator, aSendQueueSize, aSendQueueSize, aSendQueueSize, aSendQueueSize, *this);

    for (TUint i=0; i<aMaxTabs; i++) {
        iResourceHandlers.push_back(aResourceHandlerFactory.NewResourceHandler(aResourceDir));
        iTabs.push_back(new ConfigTab(i, *iMsgAllocator, iConfigManager, *this, aRebootHandler));
    }

    for (TUint i=0; i<aMaxTabs; i++) {
        iLanguageResourceHandlers.push_back(aResourceHandlerFactory.NewLanguageReader(iLangResourceDir));
    }
}

ConfigAppBase::~ConfigAppBase()
{
    for (TUint i=0; i<iLanguageResourceHandlers.size(); i++) {
        delete iLanguageResourceHandlers[i];
    }

    for (TUint i=0; i<iTabs.size(); i++) {
        delete iTabs[i];
        delete iResourceHandlers[i];
    }
    for (TUint i=0; i<iKeysNums.size(); i++) {
        delete iKeysNums[i];
    }
    for (TUint i=0; i<iKeysChoices.size(); i++) {
        delete iKeysChoices[i];
    }
    for (TUint i=0; i<iKeysTexts.size(); i++) {
        delete iKeysTexts[i];
    }

    InfoMap::iterator it;
    for (it = iInfoMap.begin(); it != iInfoMap.end(); ++it) {
        delete it->second;
    }

    delete iMsgAllocator;
}

// FIXME - is this really required? If so, app framework should call it when it is started
//void ConfigAppBase::Start()
//{
//    for (TUint i=0; i<iTabs.size(); i++) {
//        iTabs[i]->Start();
//    }
//}

ITab& ConfigAppBase::Create(ITabHandler& aHandler, const std::vector<Bws<10>>& aLanguageList)
{
    AutoMutex a(iLock);
    for (TUint i=0; i<iTabs.size(); i++) {
        if (!iTabs[i]->Allocated()) {
            // FIXME - won't be cleared until a new handler is set.
            // Shouldn't matter as only thing that can call tab handler is the
            // tab, which gets destroyed when it is no longer in use.
            iTabs[i]->SetHandler(aHandler, aLanguageList);
            return *iTabs[i];
        }
    }
    THROW(TabAllocatorFull);
}

const Brx& ConfigAppBase::ResourcePrefix() const
{
    return iResourcePrefix;
}

IResourceHandler& ConfigAppBase::CreateResourceHandler(const OpenHome::Brx& aResource)
{
    AutoMutex a(iLock);
    for (TUint i=0; i<iResourceHandlers.size(); i++) {
        if (!iResourceHandlers[i]->Allocated()) {
            IResourceHandler& handler = *iResourceHandlers[i];
            handler.SetResource(aResource);
            return handler;
        }
    }
    ASSERTS();  // FIXME - throw exception instead?
    // Could throw a ResourceHandlerFull if temporarily unavailable, and send an appropriate error response to browser.
    // However, in most cases, this should never happen. If it does (repeatedly) it likely means resource handlers aren't being returned/Destroy()ed.
    return *iResourceHandlers[0];   // unreachable
}

const WritableJsonInfo& ConfigAppBase::GetInfo(const OpenHome::Brx& aKey)
{
    InfoMap::iterator it = iInfoMap.find(Brn(aKey));
    ASSERT(it != iInfoMap.end());
    return *it->second;
}

ILanguageResourceReader& ConfigAppBase::CreateLanguageResourceHandler(const Brx& aResourceUriTail, std::vector<Bws<10>>& aLanguageList)
{
    // If no desired language can be found, should default to English.
    // Developer error if English mappings don't exist.
    std::vector<Bws<10>> languages(aLanguageList);
    Bws<10> def(kDefaultLanguage);
    languages.push_back(def);

    AutoMutex a(iLock);
    for (TUint i=0; i<iLanguageResourceHandlers.size(); i++) {
        if (!iLanguageResourceHandlers[i]->Allocated()) {
            for (TUint j=0; j<languages.size(); j++) {
                Bws<Uri::kMaxUriBytes> resource(languages[j]);
                resource.Append("/");
                resource.Append(aResourceUriTail);
                try {
                    ILanguageResourceReader& handler = *iLanguageResourceHandlers[i];
                    handler.SetResource(resource);
                    return handler;
                }
                catch (LanguageResourceInvalid&) {
                    LOG(kHttp, "ConfigAppBase::CreateLanguageResourceHandler no mapping found for: %.*s\n", PBUF(resource));
                }
            }
            ASSERTS();  // No mapping found; should have been able to find kDefaultLanguage
        }
    }
    ASSERTS();  // No free handler available. // FIXME - throw exception instead?
    return *iLanguageResourceHandlers[0];   // unreachable
}

void ConfigAppBase::AddReadOnly(const Brx& /*aKey*/)
{

}

void ConfigAppBase::AddNum(const OpenHome::Brx& aKey, TBool aRebootRequired)
{
    Brh* key = new Brh(aKey);
    iKeysNums.push_back(key);
    AddInfo(*key, aRebootRequired);

    for (TUint i=0; i<iTabs.size(); i++) {
        iTabs[i]->AddKeyNum(*key);
    }
}

void ConfigAppBase::AddChoice(const OpenHome::Brx& aKey, TBool aRebootRequired)
{
    Brh* key = new Brh(aKey);
    iKeysChoices.push_back(key);
    AddInfo(*key, aRebootRequired);

    for (TUint i=0; i<iTabs.size(); i++) {
        iTabs[i]->AddKeyChoice(*key);
    }
}

void ConfigAppBase::AddText(const OpenHome::Brx& aKey, TBool aRebootRequired)
{
    Brh* key = new Brh(aKey);
    iKeysTexts.push_back(key);
    AddInfo(*key, aRebootRequired);

    for (TUint i=0; i<iTabs.size(); i++) {
        iTabs[i]->AddKeyText(*key);
    }
}

void ConfigAppBase::AddInfo(const Brx& aKey, TBool aRebootRequired)
{
    //ASSERT(!iStarted);
    const WritableJsonInfo* info = new WritableJsonInfo(aRebootRequired);
    iInfoMap.insert(InfoPair(Brn(aKey), info));
}


// ConfigAppBasic

ConfigAppBasic::ConfigAppBasic(IInfoAggregator& aInfoAggregator, IConfigManager& aConfigManager, IConfigAppResourceHandlerFactory& aResourceHandlerFactory, const Brx& aResourcePrefix, const Brx& aResourceDir, TUint aMaxTabs, TUint aSendQueueSize, IRebootHandler& aRebootHandler)
    : ConfigAppBase(aInfoAggregator, aConfigManager, aResourceHandlerFactory, aResourcePrefix, aResourceDir, aMaxTabs, aSendQueueSize, aRebootHandler)
{
    AddText(Brn("Product.Name"));
    AddText(Brn("Product.Room"));
}


// ConfigAppSources

ConfigAppSources::ConfigAppSources(IInfoAggregator& aInfoAggregator, IConfigManager& aConfigManager, IConfigAppResourceHandlerFactory& aResourceHandlerFactory, const std::vector<const Brx*>& aSources, const Brx& aResourcePrefix, const Brx& aResourceDir, TUint aMaxTabs, TUint aSendQueueSize, IRebootHandler& aRebootHandler)
    : ConfigAppBasic(aInfoAggregator, aConfigManager, aResourceHandlerFactory, aResourcePrefix, aResourceDir, aMaxTabs, aSendQueueSize, aRebootHandler)
{
    // Get all product names.
    for (TUint i=0; i<aSources.size(); i++) {

        Bws<Av::Source::kKeySourceNameMaxBytes> key;
        Av::Source::GetSourceNameKey(*aSources[i], key);
        AddText(key);

        Av::Source::GetSourceVisibleKey(*aSources[i], key);
        AddNum(key);   // FIXME - why not a ConfigChoice?
        //AddChoice(key);
    }

    AddChoice(ConfigStartupSource::kKeySource);
}
