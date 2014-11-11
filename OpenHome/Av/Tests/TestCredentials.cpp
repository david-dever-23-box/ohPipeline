#include <OpenHome/Private/TestFramework.h>
#include <OpenHome/Av/Credentials.h>
#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Exception.h>
#include <OpenHome/Net/Core/DvDevice.h>
#include <OpenHome/Net/Core/CpDeviceDv.h>
#include <OpenHome/Net/Private/DviStack.h>
#include <OpenHome/Net/Core/OhNet.h>
#include <OpenHome/Configuration/Tests/ConfigRamStore.h>
#include <OpenHome/Configuration/ConfigManager.h>
#include <Generated/CpAvOpenhomeOrgCredentials1.h>
#include <OpenHome/Net/Private/Error.h>
#include <OpenHome/Private/Thread.h>

#include "openssl/bio.h"
#include "openssl/pem.h"

using namespace OpenHome;
using namespace OpenHome::TestFramework;
using namespace OpenHome::Av;
using namespace OpenHome::Net;
using namespace OpenHome::Configuration;

#define TEST_THROWS_PROXYERROR(aExp, aCode) \
    do { \
        const TChar* file = __FILE__; \
        try { \
            aExp; \
            OpenHome::TestFramework::Fail(file, __LINE__, #aExp, "ProxyError expected but not thrown"); \
        } \
        catch(ProxyError& aPe) { \
            if (aPe.Level() != Error::eUpnp) { \
                OpenHome::TestFramework::Fail(file, __LINE__, #aExp, "Wrong error level"); \
            } \
            else if (aPe.Code() == aCode) { \
                OpenHome::TestFramework::Succeed(file, __LINE__); \
            } \
            else { \
                char str[128]; \
                (void)sprintf(str, "Expected error code %d, got %d", aCode, (int)aPe.Code()); \
                OpenHome::TestFramework::Fail(file, __LINE__, #aExp, str); \
            } \
        } \
    } while(0)

namespace OpenHome {
namespace Av {
namespace TestCredentials {

class DummyAsyncOutput : private IAsyncOutput
{
public:
    void LogError(IAsync& aAsync);
    virtual ~DummyAsyncOutput() {}
private:
    void Output(const TChar* aKey, const TChar* aValue);
};

class DummyCredential : public ICredentialConsumer
{
public:
    static const Brn kUsernameInvalid;
    static const Brn kStatusUsernameInvalid;
public:
    DummyCredential(Credentials& aCredentials);
    const Brx& Username() const { return iUsername; }
    const Brx& Password() const { return iPassword; }
    const Brx& Status() const { return iStatus; }
public: // from ICredentialConsumer
    const Brx& Id() const;
    void CredentialsChanged(const Brx& aUsername, const Brx& aPassword);
    void Login(Bwx& aToken);
    void Logout(const Brx& aToken);
private:
    Credentials& iCredentials;
    Bws<ICredentials::kMaxUsernameBytes> iUsername;
    Bws<ICredentials::kMaxPasswordBytes> iPassword;
    Bws<ICredentials::kMaxStatusBytes> iStatus;
};


class SuiteCredentials : public Suite
{
    static const TUint kKeyBits = 1024; // smaller than production to avoid delays in frequently run tests
public:
    SuiteCredentials(CpStack& aCpStack, DvStack& aDvStack);
    ~SuiteCredentials();
private: // from Suite
    void Test();
private:
    void SeqChanged();
private:
    DvDevice* iDvDevice;
    CpDeviceDv* iCpDevice;
    ConfigRamStore* iConfigRamStore;
    ConfigManager* iConfigManager;
    Credentials* iCredentials;
    DummyCredential* iDummy;
    CpProxyAvOpenhomeOrgCredentials1* iProxy;
    Semaphore iSeqChanged;
};

} // namespace TestCredentials
} // namespace Av
} // namespace OpenHome

using namespace OpenHome::Av::TestCredentials;

// DummyAsyncOutput

void DummyAsyncOutput::LogError(IAsync& aAsync)
{
    aAsync.Output(*this);
}

void DummyAsyncOutput::Output(const TChar* /*aKey*/, const TChar* /*aValue*/)
{
}


// DummyCredential

const Brn DummyCredential::kUsernameInvalid("UsernameInvalid");
const Brn DummyCredential::kStatusUsernameInvalid("Invalid username");

DummyCredential::DummyCredential(Credentials& aCredentials)
    : iCredentials(aCredentials)
{
}

const Brx& DummyCredential::Id() const
{
    static const Brn kId("DummyCredential");
    return kId;
}

void DummyCredential::CredentialsChanged(const Brx& aUsername, const Brx& aPassword)
{
    iUsername.Replace(aUsername);
    iPassword.Replace(aPassword);
    if (aUsername == kUsernameInvalid) {
        iCredentials.SetStatusLocked(Id(), kStatusUsernameInvalid);
    }
}

void DummyCredential::Login(Bwx& /*aToken*/)
{
    THROW(CredentialsLoginFailed);
}

void DummyCredential::Logout(const Brx& /*aToken*/)
{
    THROW(CredentialsLogoutFailed);
}

    
// SuiteCredentials

SuiteCredentials::SuiteCredentials(CpStack& aCpStack, DvStack& aDvStack)
    : Suite("Credentials tests")
    , iSeqChanged("TCRD", 0)
{
    Bwh udn("TestCredentials");
    RandomiseUdn(aDvStack.Env(), udn);
    iDvDevice = new DvDevice(aDvStack, udn);
    iCpDevice = CpDeviceDv::New(aCpStack, *iDvDevice);
    iConfigRamStore = new ConfigRamStore();
    iConfigManager = new ConfigManager(*iConfigRamStore);
    iCredentials = new Credentials(aDvStack.Env(), *iDvDevice, iConfigManager->Store(), udn, *iConfigManager, kKeyBits);
    iDummy = new DummyCredential(*iCredentials);
    iCredentials->Add(iDummy);
    iDvDevice->SetEnabled();
    iProxy = new CpProxyAvOpenhomeOrgCredentials1(*iCpDevice);
    Functor f = MakeFunctor(*this, &SuiteCredentials::SeqChanged);
    iProxy->SetPropertySequenceNumberChanged(f);
    iProxy->Subscribe();
    iSeqChanged.Wait();
}

SuiteCredentials::~SuiteCredentials()
{
    delete iProxy;
    delete iCredentials;
    delete iConfigManager;
    delete iConfigRamStore;
    iCpDevice->RemoveRef();
    delete iDvDevice;
}

void SuiteCredentials::Test()
{
    // check that 800 (IdNotFound) is returned from all actions when we pass an invalid id
    Brn id("bogus");
    TEST_THROWS_PROXYERROR(iProxy->SyncSet(id, Brn("username"), Brn("password")), 800);
    TEST_THROWS_PROXYERROR(iProxy->SyncClear(id), 800);
    TEST_THROWS_PROXYERROR(iProxy->SyncSetEnabled(id, false), 800);
    Brh username, password, status;
    TBool enabled;
    TEST_THROWS_PROXYERROR(iProxy->SyncGet(id, username, password, enabled, status), 800);
    Brh token;
    TEST_THROWS_PROXYERROR(iProxy->SyncLogin(id, token), 800);
    TEST_THROWS_PROXYERROR(iProxy->SyncLogout(id, Brn("token")), 800);

    // check getters for state variables
    Brh ids;
    iProxy->SyncGetIds(ids);
    TEST(ids == iDummy->Id());
    Brh key;
    iProxy->SyncGetPublicKey(key);
    TEST(key.BeginsWith(Brn("-----BEGIN RSA PUBLIC KEY")));
    TUint seq = UINT_MAX;
    iProxy->SyncGetSequenceNumber(seq);
    TEST(seq == 0);

    // check that Set/Clear/SetEnabled are passed through
    iProxy->SyncGet(iDummy->Id(), username, password, enabled, status);
    TEST(username.Bytes() == 0);
    TEST(password.Bytes() == 0);
    TEST(enabled);
    TEST(status.Bytes() == 0);

    const Brn kUsername("username");
    const Brn kPasswordDec("password");
    BIO *bio = BIO_new_mem_buf((void*)key.Ptr(), key.Bytes());
    RSA* rsa = PEM_read_bio_RSAPublicKey (bio, NULL, NULL, NULL);
    BIO_free(bio);
    Bws<19> src(kPasswordDec);
    src.PtrZ();
    Bws<256> passwordEnc;
    int encryptedLen = RSA_public_encrypt(src.Bytes(), src.Ptr(), const_cast<TByte*>(passwordEnc.Ptr()), rsa, RSA_PKCS1_OAEP_PADDING);
    TEST(encryptedLen > 0);
    passwordEnc.SetBytes(encryptedLen);
    RSA_free(rsa);

    iProxy->SyncSet(iDummy->Id(), kUsername, passwordEnc);
    iSeqChanged.Wait();
    TUint oldSeq = seq;
    iProxy->SyncGetSequenceNumber(seq);
    TEST(seq > oldSeq);
    iProxy->SyncGet(iDummy->Id(), username, password, enabled, status);
    TEST(username == kUsername);
    TEST(password == passwordEnc);
    TEST(enabled);
    TEST(status.Bytes() == 0);
    TEST(iDummy->Username() == kUsername);
    TEST(iDummy->Password() == kPasswordDec);

    iProxy->SyncSetEnabled(iDummy->Id(), false);
    iSeqChanged.Wait();
    iProxy->SyncGet(iDummy->Id(), username, password, enabled, status);
    TEST(username == kUsername);
    TEST(password == passwordEnc);
    TEST(!enabled);
    TEST(status.Bytes() == 0);
    TEST(iDummy->Username().Bytes() == 0);
    TEST(iDummy->Password().Bytes() == 0);

    iProxy->SyncSetEnabled(iDummy->Id(), true);
    iSeqChanged.Wait();
    iProxy->SyncGet(iDummy->Id(), username, password, enabled, status);
    TEST(username == kUsername);
    TEST(password == passwordEnc);
    TEST(enabled);
    TEST(status.Bytes() == 0);
    TEST(iDummy->Username() == kUsername);
    TEST(iDummy->Password() == kPasswordDec);

    iProxy->SyncSet(iDummy->Id(), DummyCredential::kUsernameInvalid, passwordEnc);
    iSeqChanged.Wait();
    iProxy->SyncGet(iDummy->Id(), username, password, enabled, status);
    TEST(username == DummyCredential::kUsernameInvalid);
    TEST(password == passwordEnc);
    TEST(enabled);
    TEST(status == DummyCredential::kStatusUsernameInvalid);
    TEST(iDummy->Username() == DummyCredential::kUsernameInvalid);
    TEST(iDummy->Password() == kPasswordDec);

    iProxy->SyncClear(iDummy->Id());
    iSeqChanged.Wait();
    iProxy->SyncGet(iDummy->Id(), username, password, enabled, status);
    TEST(username.Bytes() == 0);
    TEST(password.Bytes() == 0);
    TEST(enabled);
    TEST(status.Bytes() == 0);
    TEST(iDummy->Username().Bytes() == 0);
    TEST(iDummy->Password().Bytes() == 0);

    // check error reporting from Login/Logout
    TEST_THROWS_PROXYERROR(iProxy->SyncLogin(iDummy->Id(), token), 801);
    TEST_THROWS_PROXYERROR(iProxy->SyncLogout(iDummy->Id(), Brn("token")), 802);
}

void SuiteCredentials::SeqChanged()
{
    iSeqChanged.Signal();
}


void TestCredentials(CpStack& aCpStack, DvStack& aDvStack)
{
    DummyAsyncOutput errorSuppressor;
    InitialisationParams* initParams = aDvStack.Env().InitParams();
    FunctorAsync oldAsyncErrorHandler = initParams->AsyncErrorHandler();
    initParams->SetAsyncErrorHandler(MakeFunctorAsync(errorSuppressor, &DummyAsyncOutput::LogError));
    
    Runner runner("Credentials service tests\n");
    runner.Add(new SuiteCredentials(aCpStack, aDvStack));
    runner.Run();

    initParams->SetAsyncErrorHandler(oldAsyncErrorHandler);
}

