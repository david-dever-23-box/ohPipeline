#include <OpenHome/Web/ConfigUi/FileResourceHandler.h>
#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Web/ResourceHandler.h>
#include <OpenHome/Web/WebAppFramework.h>
#include <OpenHome/Web/ConfigUi/ConfigUi.h>
#include <OpenHome/Private/Debug.h>
#include <OpenHome/Private/Printer.h>

using namespace OpenHome;
using namespace OpenHome::Web;


// FileResourceHandler

FileResourceHandler::FileResourceHandler(const Brx& aRootDir, IResourceHandlerDeallocator& aDeallocator)
    : ResourceHandlerBase(aRootDir, aDeallocator)
    , iFile(nullptr)
{
}

TUint FileResourceHandler::Bytes()
{
    ASSERT(iFile != nullptr);
    return iFile->Bytes();
}

void FileResourceHandler::Write(IWriter& aWriter)
{
    ASSERT(iFile != nullptr);

    TUint bytes = Bytes();
    Bws<1024> buf;
    try {
        while (bytes > 0) {
            iFile->Read(buf);
            aWriter.Write(buf);
            bytes -= buf.Bytes();
            buf.SetBytes(0);
        }
    }
    catch (FileReadError&) {
        // FIXME - this behaviour seems undesirable
        // iFile->Read() should EITHER do what we ask, OR throw an exception
        // It shouldn't do both (either partially or fully)!
        if (buf.Bytes() > 0) {
            aWriter.Write(buf);
        }
        THROW(WriterError);
    }
}

void FileResourceHandler::SetResource(const Brx& aResourceTail)
{
    ASSERT(iFile == nullptr);
    Bwh filename(iRootDir.Bytes()+aResourceTail.Bytes()+1);
    filename.Replace(iRootDir);
    filename.Append(aResourceTail);

    try {
        // FIXME - dynamic allocation!
        iFile = new FileAnsi(filename.PtrZ(), eFileReadOnly); // asserts if a file is already open
    }
    catch (FileOpenError&) {
        LOG(kHttp, "FileResourceHandler::SetResource failed to open resource: %.*s\n", PBUF(filename));
        THROW(ResourceInvalid);
    }
}

void FileResourceHandler::Clear()
{
    ASSERT(iFile != nullptr);
    delete iFile;
    iFile = nullptr;
}


// LanguageResourceFileReader

LanguageResourceFileReader::LanguageResourceFileReader(const Brx& aRootDir)
    : iRootDir(aRootDir)
    , iReadBuffer(iFileStream)
    , iReaderText(iReadBuffer)
    , iAllocated(false)
    , iLock("LRRL")
{
    ASSERT(iRootDir[iRootDir.Bytes()-1] == '/');    // expect trailing '/'
}

void LanguageResourceFileReader::SetResource(const Brx& aUriTail)
{
    AutoMutex a(iLock);
    Bws<Uri::kMaxUriBytes+1> filename(iRootDir);
    filename.Append(aUriTail);

    try {
        // FIXME - dynamic allocation!
        IFile* file = new FileAnsi(filename.PtrZ(), eFileReadOnly); // asserts if a file is already open
        iFileStream.SetFile(file);
        iAllocated = true;
    }
    catch (FileOpenError&) {
        LOG(kHttp, "LanguageResourceFileReader::SetResource failed to open resource: %.*s\n", PBUF(filename));
        THROW(LanguageResourceInvalid);
    }
}

TBool LanguageResourceFileReader::Allocated() const
{
    AutoMutex a(iLock);
    return iAllocated;
}

void LanguageResourceFileReader::Process(const Brx& aKey, IResourceFileConsumer& aResourceConsumer)
{
    TBool entryProcessed = false;
    AutoMutex _(iLock);
    try {
        for (;;) {
            Brn line = iReaderText.ReadLine();
            if (!aResourceConsumer.ProcessLine(line)) {
                entryProcessed = true;
                break;
            }
        }
    }
    catch (ReaderError&) {
        LOG(kHttp, "LanguageResourceFileReader::Run ReaderError\n");
    }
    if (!entryProcessed) {
        Log::Print("LanguageResourceFileReader::Process Failed to process key: %.*s\n", PBUF(aKey));
        ASSERTS();
    }
    iReaderText.ReadFlush();
    iFileStream.CloseFile();
    iAllocated = false;
}


// FileResourceHandlerFactory

ResourceHandlerBase* FileResourceHandlerFactory::NewResourceHandler(const Brx& aRootDir, IResourceHandlerDeallocator& aDeallocator)
{
    return new FileResourceHandler(aRootDir, aDeallocator);
}

ILanguageResourceReader* FileResourceHandlerFactory::NewLanguageReader(const Brx& aResourceDir)
{
    return new LanguageResourceFileReader(aResourceDir);
}
