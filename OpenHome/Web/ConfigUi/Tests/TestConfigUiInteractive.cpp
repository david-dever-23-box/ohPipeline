#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Media/Tests/Cdecl.h>
#include <OpenHome/Media/Tests/GetCh.h>
#include <OpenHome/Web/ConfigUi/ConfigUi.h>
#include <OpenHome/Net/Private/DviStack.h>
#include <OpenHome/Private/OptionParser.h>
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Private/TestFramework.h>
#include <OpenHome/Configuration/Tests/ConfigRamStore.h>
#include <OpenHome/Av/Product.h>
#include <OpenHome/Web/ConfigUi/FileResourceHandler.h>
#include <OpenHome/Media/Utils/AllocatorInfoLogger.h>
#include <OpenHome/Av/RebootHandler.h>
#include <OpenHome/Av/Tests/RamStore.h>
#include <OpenHome/Av/KvpStore.h>

#include <stdlib.h>

namespace OpenHome {
namespace Web {
namespace Test {

class TestPresentationUrlHandler
{
public:
    void PresentationUrlChanged(const Brx& aUrl);
};

class MockRebootHandler : public Av::IRebootHandler
{
public: // from IRebootHandler
    void Reboot(const Brx& aReason) override;
};

} // namespace Test
} // namespace Web
} // namespace OpenHome

using namespace OpenHome;
using namespace OpenHome::Configuration;
using namespace OpenHome::Net;
using namespace OpenHome::TestFramework;
using namespace OpenHome::Web;
using namespace OpenHome::Web::Test;


// TestPresentationUrlHandler

void TestPresentationUrlHandler::PresentationUrlChanged(const Brx& aUrl)
{
    Log::Print("Presentation URL changed: ");
    Log::Print(aUrl);
    Log::Print("\n");
}


// MockRebootHandler

void MockRebootHandler::Reboot(const Brx& aReason)
{
    Log::Print("\n\n\nRebootLogger::Reboot. Reason:\n%.*s\n\n\n", PBUF(aReason));
}



int CDECL main(int aArgc, char* aArgv[])
{
#ifdef _WIN32
    char* noErrDlgs = getenv("NO_ERROR_DIALOGS");
    if (noErrDlgs != nullptr && strcmp(noErrDlgs, "1") == 0) {
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
#endif // _WIN32

    // Parse command line args.
    OptionParser parser;
    OptionString optionDir("-d", "--root-dir", Brn(""), "Root directory for serving static files");
    parser.AddOption(&optionDir);

    if (!parser.Parse(aArgc, aArgv)) {
        return 1;
    }

    // Initialise ohNet.
    InitialisationParams* initParams = InitialisationParams::Create();
    //initParams->SetUseLoopbackNetworkAdapter();
    Library* lib = new Library(initParams);
    DvStack* dvStack = lib->StartDv();
    Environment& env = dvStack->Env();
    Media::AllocatorInfoLogger infoAggregator;
    MockRebootHandler rebootHandler;

    // Set up the server.
    Debug::SetLevel(Debug::kHttp);
    const TUint kMinResourceHandlers = 1;
    const TUint kMaxSessions = 1;
    const TUint kSendQueueSize = 32;
    ConfigRamStore* configRamStore = new ConfigRamStore();
    ConfigManager* confMgr = new ConfigManager(*configRamStore);

    // ConfigAppBasic expects Product.Name and Room.Name ConfigVals.
    ConfigText* productName = new ConfigText(*confMgr, Av::Product::kConfigIdNameBase, Av::Product::kMinNameBytes, Av::Product::kMaxNameBytes, Brn("Product Name"));
    ConfigText* productRoom = new ConfigText(*confMgr, Av::Product::kConfigIdRoomBase, Av::Product::kMinRoomBytes, Av::Product::kMaxRoomBytes, Brn("Product Room"));

    confMgr->Print();
    confMgr->Open();


    WebAppFrameworkInitParams* wafInitParams = new WebAppFrameworkInitParams();
    wafInitParams->SetServerPort(0);  // bind to OS-assigned port
    wafInitParams->SetMinServerThreadsResources(kMinResourceHandlers);
    wafInitParams->SetMaxServerThreadsLongPoll(kMaxSessions);
    wafInitParams->SetSendQueueSize(1024);

    WebAppFramework* server = new WebAppFramework(env, wafInitParams);

     // Web App should only be initialised once ConfigManager is opened (i.e.,
     // once ALL ConfigVals have been registered).
    FileResourceHandlerFactory resourceHandlerFactory;
    Brn resourcePrefix("SoftPlayerBasic");
    ConfigAppBasic* app = new ConfigAppBasic(infoAggregator, *confMgr, resourceHandlerFactory, resourcePrefix, optionDir.Value(), kMinResourceHandlers, kMaxSessions, kSendQueueSize, rebootHandler);

    TestPresentationUrlHandler* urlHandler = new TestPresentationUrlHandler();
    server->Add(app, MakeFunctorGeneric(*urlHandler, &TestPresentationUrlHandler::PresentationUrlChanged));   // takes ownership
    server->Start();

    Log::Print("\nTest Http server for Config Web UI\n");
    Log::Print("Root dir for static resources: ");
    Log::Print(optionDir.Value());
    Log::Print("\n");

    Log::Print("Press <q> followed by <enter> to quit:\n");
    Log::Print("\n");
    while (getchar() != 'q') {
        ;
    }

    // Shutdown.
    delete server;
    delete urlHandler;
    delete confMgr;
    delete productRoom;
    delete productName;
    delete configRamStore;
    delete lib;

    return 0;
}
