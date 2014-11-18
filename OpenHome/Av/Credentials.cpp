#include <OpenHome/Av/Credentials.h>
#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Exception.h>
#include <OpenHome/Private/Timer.h>
#include <OpenHome/Private/Thread.h>
#include <OpenHome/Private/Fifo.h>
#include <OpenHome/Av/Debug.h>
#include <OpenHome/Configuration/IStore.h>
#include <OpenHome/Configuration/ConfigManager.h>
#include <OpenHome/Av/ProviderCredentials.h>

#include <vector>
#include "openssl/bio.h"
#include "openssl/pem.h"
#include "openssl/rand.h"

namespace OpenHome {
namespace Av {

class Credential
{
    friend class Credentials;

    static const TUint kMaxEncryptedLen = 256;
    static const TUint kEventModerationMs = 500;
public:
    Credential(Environment& aEnv, ICredentialConsumer* aConsumer, ICredentialObserver& aObserver, Configuration::IConfigInitialiser& aConfigInitialiser, Fifo<Credential*>& aFifoCredentialsChanged);
    ~Credential();
    void SetKey(RSA* aKey);
    const Brx& Id() const;
    void Set(const Brx& aUsername, const Brx& aPassword);
    void Clear();
    void Enable(TBool aEnable);
    void Get(Bwx& aUsername, Bwx& aPassword, TBool& aEnabled, Bwx& aStatus);
    void SetStatus(const Brx& aStatus);
    void Login(Bwx& aToken);
    void Logout(const Brx& aToken);
private:
    void UsernameChanged(Configuration::KeyValuePair<const Brx&>& aKvp);
    void PasswordChanged(Configuration::KeyValuePair<const Brx&>& aKvp);
    void ModerationTimerCallback();
    void CheckStatus();
    void ReportChangesLocked();
private:
    Mutex iLock;
    ICredentialConsumer* iConsumer;
    ICredentialObserver& iObserver;
    RSA* iRsa;
    Fifo<Credential*>& iFifoCredentialsChanged;
    Configuration::ConfigText* iConfigUsername;
    Configuration::ConfigText* iConfigPassword;
    TUint iSubscriberIdUsername;
    TUint iSubscriberIdPassword;
    Timer* iModerationTimer;
    Bws<ICredentials::kMaxUsernameBytes> iUsername;
    Bws<ICredentials::kMaxPasswordBytes> iPassword;
    Bws<ICredentials::kMaxPasswordEncryptedBytes> iPasswordEncrypted;
    Bws<ICredentials::kMaxStatusBytes> iStatus;
    TBool iEnabled;
    TBool iModerationTimerStarted;
    TBool iPendingChange;
};

} // namespace Av
} //namespace OpenHome

using namespace OpenHome;
using namespace OpenHome::Av;
using namespace OpenHome::Configuration;

// Credential

Credential::Credential(Environment& aEnv, ICredentialConsumer* aConsumer, ICredentialObserver& aObserver, Configuration::IConfigInitialiser& aConfigInitialiser, Fifo<Credential*>& aFifoCredentialsChanged)
    : iLock("CRED")
    , iConsumer(aConsumer)
    , iObserver(aObserver)
    , iRsa(NULL)
    , iFifoCredentialsChanged(aFifoCredentialsChanged)
    , iEnabled(true)
    , iModerationTimerStarted(false)
{
    iModerationTimer = new Timer(aEnv, MakeFunctor(*this, &Credential::ModerationTimerCallback), "Credential");
    Bws<64> key(aConsumer->Id());
    key.Append('.');
    key.Append(Brn("Username"));
    iConfigUsername = new ConfigText(aConfigInitialiser, key, kMaxEncryptedLen, Brx::Empty());
    iSubscriberIdUsername = iConfigUsername->Subscribe(MakeFunctorConfigText(*this, &Credential::UsernameChanged));
    key.Replace(aConsumer->Id());
    key.Append('.');
    key.Append(Brn("Password"));
    iConfigPassword = new ConfigText(aConfigInitialiser, key, kMaxEncryptedLen, Brx::Empty());
    iSubscriberIdPassword = iConfigPassword->Subscribe(MakeFunctorConfigText(*this, &Credential::PasswordChanged));
}

Credential::~Credential()
{
    delete iModerationTimer;
    iConfigUsername->Unsubscribe(iSubscriberIdUsername);
    delete iConfigUsername;
    iConfigPassword->Unsubscribe(iSubscriberIdPassword);
    delete iConfigPassword;
    delete iConsumer;
}

void Credential::SetKey(RSA* aKey)
{
    iRsa = aKey;
}

const Brx& Credential::Id() const
{
    return iConsumer->Id();
}

void Credential::Set(const Brx& aUsername, const Brx& aPassword)
{
    iEnabled = true;
    iStatus.Replace(Brx::Empty());
    iConfigUsername->Set(aUsername);
    iConfigPassword->Set(aPassword);
}

void Credential::Clear()
{
    iStatus.Replace(Brx::Empty());
    iConfigUsername->Set(Brx::Empty());
    iConfigPassword->Set(Brx::Empty());
}

void Credential::Enable(TBool aEnable)
{
    AutoMutex _(iLock);
    if (iEnabled == aEnable) {
        return;
    }
    iEnabled = aEnable;
    iObserver.CredentialChanged();
    ReportChangesLocked();
}

void Credential::Get(Bwx& aUsername, Bwx& aPassword, TBool& aEnabled, Bwx& aStatus)
{
    AutoMutex _(iLock);
    aUsername.Replace(iUsername);
    aPassword.Replace(iPasswordEncrypted);
    aEnabled = iEnabled;
    aStatus.Replace(iStatus);
}

void Credential::SetStatus(const Brx& aStatus)
{
    AutoMutex _(iLock);
    if (iStatus == aStatus) {
        return;
    }
    iStatus.Replace(aStatus);
    iObserver.CredentialChanged();
}

void Credential::Login(Bwx& aToken)
{
    iConsumer->Login(aToken);
}

void Credential::Logout(const Brx& aToken)
{
    iConsumer->Logout(aToken);
}

void Credential::UsernameChanged(Configuration::KeyValuePair<const Brx&>& aKvp)
{
    iLock.Wait();
    iUsername.Replace(aKvp.Value());
    iObserver.CredentialChanged();
    if (!iModerationTimerStarted) {
        iModerationTimer->FireIn(kEventModerationMs);
        iModerationTimerStarted = true;
    }
    iLock.Signal();
}

void Credential::PasswordChanged(Configuration::KeyValuePair<const Brx&>& aKvp)
{
    AutoMutex _(iLock);
    iPasswordEncrypted.Replace(aKvp.Value());
    if (iPasswordEncrypted.Bytes() == 0) {
        iPassword.SetBytes(0);
    }
    else {
        ASSERT(iRsa != NULL);
        const int decryptedLen = RSA_private_decrypt(iPasswordEncrypted.Bytes(), iPasswordEncrypted.Ptr(), const_cast<TByte*>(iPassword.Ptr()), iRsa, RSA_PKCS1_OAEP_PADDING);
        if (decryptedLen < 0) {
            LOG(kError, "Failed to decrypt password for ");
            LOG(kError, Id());
            LOG(kError, "\n");
            iPassword.SetBytes(0);
        }
        else {
            iPassword.SetBytes((TUint)decryptedLen);
        }
    }
    iObserver.CredentialChanged();
    if (!iModerationTimerStarted) {
        iModerationTimer->FireIn(kEventModerationMs);
        iModerationTimerStarted = true;
    }
}

void Credential::ModerationTimerCallback()
{
    AutoMutex _(iLock);
    iModerationTimerStarted = false;
    ReportChangesLocked();
    if (iEnabled) {
        iFifoCredentialsChanged.Write(this);
    }
}

void Credential::CheckStatus()
{
    iConsumer->UpdateStatus();
}

void Credential::ReportChangesLocked()
{
    if (iEnabled) {
        iConsumer->CredentialsChanged(iUsername, iPassword);
    }
    else {
        iConsumer->CredentialsChanged(Brx::Empty(), Brx::Empty());
    }
}


// Credentials

const Brn Credentials::kKeyRsaPrivate("RsaPrivateKey");
const Brn Credentials::kKeyRsaPublic("RsaPublicKey");

Credentials::Credentials(Environment& aEnv, Net::DvDevice& aDevice, IStoreReadWrite& aStore, const Brx& aEntropy, Configuration::IConfigInitialiser& aConfigInitialiser, TUint aKeyBits)
    : iEnv(aEnv)
    , iConfigInitialiser(aConfigInitialiser)
    , iModerationTimerStarted(false)
    , iKeyParams(aStore, aEntropy, aKeyBits)
    , iFifo(kNumFifoElements)
    , iStarted(false)
{
    iProvider = new ProviderCredentials(aDevice, *this);
    iModerationTimer = new Timer(aEnv, MakeFunctor(*this, &Credentials::ModerationTimerCallback), "Credentials");
    iThread = new ThreadFunctor("Credentials", MakeFunctor(*this, &Credentials::CredentialsThread), kPriorityLow);
    iThread->Start();
}

Credentials::~Credentials()
{
    delete iModerationTimer;
    iFifo.ReadInterrupt();
    delete iThread;
    delete iProvider;
    for (auto it=iCredentials.begin(); it!=iCredentials.end(); ++it) {
        delete *it;
    }
    RSA_free((RSA*)iKey);
}

void Credentials::Add(ICredentialConsumer* aConsumer)
{
    ASSERT(!iStarted);
    Credential* credential = new Credential(iEnv, aConsumer, *this, iConfigInitialiser, iFifo);
    iCredentials.push_back(credential);
    iProvider->AddId(credential->Id());
}

void Credentials::SetStatus(const Brx& aId, const Brx& aState)
{
    Credential* credential = Find(aId);
    credential->SetStatus(aState);
}

void Credentials::GetPublicKey(Bwx& aKey)
{
    aKey.Replace(iKeyBuf);
}

void Credentials::Set(const Brx& aId, const Brx& aUsername, const Brx& aPassword)
{
    Credential* credential = Find(aId);
    credential->Set(aUsername, aPassword);
}

void Credentials::Clear(const Brx& aId)
{
    Credential* credential = Find(aId);
    credential->Clear();
}

void Credentials::Enable(const Brx& aId, TBool aEnable)
{
    Credential* credential = Find(aId);
    credential->Enable(aEnable);
}

void Credentials::Get(const Brx& aId, Bwx& aUsername, Bwx& aPassword, TBool& aEnabled, Bwx& aStatus)
{
    Credential* credential = Find(aId);
    credential->Get(aUsername, aPassword, aEnabled, aStatus);
}

void Credentials::Login(const Brx& aId, Bwx& aToken)
{
    Credential* credential = Find(aId);
    credential->Login(aToken);
}

void Credentials::Logout(const Brx& aId, const Brx& aToken)
{
    Credential* credential = Find(aId);
    credential->Logout(aToken);
}

void Credentials::CredentialChanged()
{
    if (!iModerationTimerStarted) {
        iModerationTimer->FireIn(kModerationTimeMs);
        iModerationTimerStarted = true;
    }
}

Credential* Credentials::Find(const Brx& aId) const
{
    for (auto it=iCredentials.begin(); it!=iCredentials.end(); ++it) {
        if ((*it)->Id() == aId) {
            return *it;
        }
    }
    THROW(CredentialsIdNotFound);
}

static void WriteToStore(IStoreReadWrite& aStore, const Brx& aKey, BIO* aBio)
{
    const int len = BIO_pending(aBio);
    char* val = (char*)calloc(len+1, 1); // +1 for nul terminator
    ASSERT(val != NULL);
    BIO_read(aBio, val, len);
    Brn valBuf(val);
    aStore.Write(aKey, valBuf);
    free(val);
}

void Credentials::CreateKey(IStoreReadWrite& aStore, const Brx& aEntropy, TUint aKeyBits)
{
    try {
        aStore.Read(kKeyRsaPrivate, iKeyBuf);
        BIO *bio = BIO_new_mem_buf((void*)iKeyBuf.Ptr(), iKeyBuf.Bytes());
        iKey = (void*)PEM_read_bio_RSAPrivateKey(bio, NULL, 0, NULL);
        BIO_free(bio);
        return;
    }
    catch (StoreKeyNotFound&) {
    }

    RAND_seed(aEntropy.Ptr(), aEntropy.Bytes());
    BIGNUM *bn = BN_new();
    ASSERT(BN_set_word(bn, RSA_F4));
    RSA* rsa = RSA_new();
    ASSERT(rsa != NULL);
    ASSERT(RSA_generate_key_ex(rsa, aKeyBits, bn, NULL));
    BN_free(bn);

    BIO* bio = BIO_new(BIO_s_mem());
    ASSERT(bio != NULL);
    ASSERT(1 == PEM_write_bio_RSAPrivateKey(bio, rsa, NULL, NULL, 0, NULL, NULL));
    WriteToStore(aStore, kKeyRsaPrivate, bio);
    ASSERT(1 == PEM_write_bio_RSAPublicKey(bio, rsa));
    WriteToStore(aStore, kKeyRsaPublic, bio);
    BIO_free(bio);
    iKey = (void*)rsa;
}

void Credentials::ModerationTimerCallback()
{
    iModerationTimerStarted = false;
    iProvider->NotifyCredentialsChanged();
}

void Credentials::CredentialsThread()
{
    // create private key
    CreateKey(iKeyParams.Store(), iKeyParams.Entropy(), iKeyParams.KeyBits());
    iStarted = true;
    for (auto it=iCredentials.begin(); it!=iCredentials.end(); ++it) {
        (*it)->SetKey((RSA*)iKey);
    }
    iKeyParams.Store().Read(kKeyRsaPublic, iKeyBuf);
    iProvider->SetPublicKey(iKeyBuf);

    // run any NotifyCredentialsChanged() callbacks
    // these are potentially slow so can't be run directly from the timer thread
    try {
        for (;;) {
            Credential* c = iFifo.Read();
            c->CheckStatus();
        }
    }
    catch (FifoReadError&) {
    }
}


// Credentials::KeyParams

Credentials::KeyParams::KeyParams(Configuration::IStoreReadWrite& aStore, const Brx& aEntropy, TUint aKeyBits)
    : iStore(aStore)
    , iEntropy(aEntropy)
    , iKeyBits(aKeyBits)
{
}

Configuration::IStoreReadWrite& Credentials::KeyParams::Store() const
{
    return iStore;
}

const Brx& Credentials::KeyParams::Entropy() const
{
    return iEntropy;
}

TUint Credentials::KeyParams::KeyBits() const
{
    return iKeyBits;
}
