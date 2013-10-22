#include <OpenHome/PowerManager.h>
#include <OpenHome/Configuration/ConfigManager.h>
#include <OpenHome/Private/Arch.h>
#include <OpenHome/Private/Converter.h>

using namespace OpenHome;


// PowerManager

PowerManager::PowerManager()
    : iLock("PMLO")
{
}

PowerManager::~PowerManager() {}

void PowerManager::PowerDown()
{
    // This call can only be made ONCE as PriorityFunctors are deleted as they
    // are called and removed. Subsequent calls will call no functors.
    // If it must be possible to reuse the PriorityFunctors, two pointers to
    // priority_queues can be stored, with values copied into second queue
    // before being popped, and then swapping queue pointers.
    AutoMutex a(iLock);
    while (!iQueue.empty()) {
        const PriorityFunctor& functor = iQueue.top();
        functor.Callback();
        iQueue.pop(); // deletes PriorityFunctor
    }
}

void PowerManager::RegisterObserver(Functor aFunctor, TUint aPriority)
{
    ASSERT(aPriority <= kPowerPriorityHighest)
    ASSERT(aPriority >= kPowerPriorityLowest); // shouldn't matter as lowest is 0, and parameter type is TUint
    const PriorityFunctor functor(aFunctor, aPriority);
    AutoMutex a(iLock);
    iQueue.push(functor); // PriorityFunctor copied into queue
}


// PowerManager::PriorityFunctor

PowerManager::PriorityFunctor::PriorityFunctor(Functor aFunctor, TUint aPriority)
    : iFunctor(aFunctor)
    , iPriority(aPriority)
{
}

void PowerManager::PriorityFunctor::Callback() const
{
    iFunctor();
}

TUint PowerManager::PriorityFunctor::Priority() const
{
    return iPriority;
}


// PowerManager::PriorityFunctorCmp

TBool PowerManager::PriorityFunctorCmp::operator()(const PriorityFunctor& aFunc1, const PriorityFunctor& aFunc2) const
{
    return aFunc1.Priority() < aFunc2.Priority();
}


// StoreVal

StoreVal::StoreVal(Configuration::IStoreReadWrite& aStore, IPowerManager& aPowerManager, TUint aPriority, const Brx& aKey)
    : iStore(aStore)
    , iKey(aKey)
{
    // register with IPowerManager
    aPowerManager.RegisterObserver(MakeFunctor(*this, &StoreVal::Write), aPriority);
}


// StoreInt

StoreInt::StoreInt(Configuration::IStoreReadWrite& aStore, IPowerManager& aPowerManager, TUint aPriority, const Brx& aKey, TInt aDefault)
    : StoreVal(aStore, aPowerManager, aPriority, aKey)
    , iVal(aDefault)
{
    // read value from store (if it exists; otherwise write default)
    Bws<sizeof(TInt)> buf;
    try {
        iStore.Read(iKey, buf);
        iVal = Converter::BeUint32At(buf, 0);
    }
    catch (StoreKeyNotFound&) {
        Write();
    }
}

TInt StoreInt::Get() const
{
    return iVal;
}

void StoreInt::Set(TInt aValue)
{
    iVal = aValue;
}

void StoreInt::Write()
{
    Bws<sizeof(TInt)> buf;
    buf.Append(Arch::BigEndian4(iVal));
    iStore.Write(iKey, buf);
}


// StoreText

StoreText::StoreText(Configuration::IStoreReadWrite& aStore, IPowerManager& aPowerManager, TUint aPriority, const Brx& aKey, const Brx& aDefault, TUint aMaxLength)
    : StoreVal(aStore, aPowerManager, aPriority, aKey)
    , iVal(aMaxLength)
{
    try {
        iStore.Read(iKey, iVal);
    }
    catch (StoreKeyNotFound&) {
        iVal.Replace(aDefault);
        Write();
    }
}

const Brx& StoreText::Get() const
{
    return iVal;
}

void StoreText::Set(const Brx& aValue)
{
    iVal.Replace(aValue);
}

void StoreText::Write()
{
    iStore.Write(iKey, iVal);
}
