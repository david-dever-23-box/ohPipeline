#include <OpenHome/Types.h>
#include <OpenHome/Private/Standard.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Media/Tests/Cdecl.h>
#include <OpenHome/Media/Tests/GetCh.h>
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Private/OptionParser.h>
#include <OpenHome/Private/TestFramework.h>
#include <OpenHome/Media/Utils/AllocatorInfoLogger.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Media/Pipeline/Logger.h>
#include <OpenHome/Media/Utils/ProcessorPcmUtils.h>
#include <OpenHome/Media/Codec/CodecController.h>
#include <OpenHome/Media/Codec/CodecFactory.h>
#include <OpenHome/Media/Codec/Container.h>
#include <OpenHome/Media/Codec/Id3v2.h>
#include <OpenHome/Media/Codec/Mpeg4.h>
#include <OpenHome/Media/Codec/MpegTs.h>
#include <OpenHome/Private/File.h>

#include <stdlib.h>

#include <limits>

namespace OpenHome {
namespace Media {
namespace Codec {

class ElementFileReader : public IPipelineElementUpstream, public IUrlBlockWriter, private INonCopyable
{
private:
    static const TUint kInBufBytes = 4096;
public:
    ElementFileReader(IFileSystem& aFileSystem, TrackFactory& aTrackFactory, MsgFactory& aMsgFactory);
    ~ElementFileReader();
    void Open(const Brx& aFilename); // throws FileOpenError
public: // from IPipelineElementUpstream
    Msg* Pull() override;
public: // from IUrlBlockWriter
    TBool TryGet(IWriter& aWriter, const Brx& aUrl, TUint64 aOffset, TUint aBytes) override;
private:
    enum EMode {
        eMode,
        eSession,
        eTrack,
        eEncodedStream,
        eAudioEncoded,
        eQuit,
        eEnd
    };
private:
    IFileSystem& iFileSystem;
    TrackFactory& iTrackFactory;
    MsgFactory& iMsgFactory;
    IFile* iFile;
    EMode iMode;
    Bws<kInBufBytes> iInBuf;
};

class ElementFileWriter : public IPipelineElementDownstream, private IMsgProcessor, private INonCopyable
{
public:
    ElementFileWriter(IFileSystem& aFileSystem, Semaphore& aSem);
    ~ElementFileWriter();
    void Open(const Brx& aFilename); // throws FileOpenError
public: // from IPipelineElementDownstream
    void Push(Msg* aMsg) override;
private: // from IMsgProcessor
    Msg* ProcessMsg(MsgMode* aMsg) override;
    Msg* ProcessMsg(MsgSession* aMsg) override;
    Msg* ProcessMsg(MsgTrack* aMsg) override;
    Msg* ProcessMsg(MsgDelay* aMsg) override;
    Msg* ProcessMsg(MsgEncodedStream* aMsg) override;
    Msg* ProcessMsg(MsgAudioEncoded* aMsg) override;
    Msg* ProcessMsg(MsgMetaText* aMsg) override;
    Msg* ProcessMsg(MsgHalt* aMsg) override;
    Msg* ProcessMsg(MsgFlush* aMsg) override;
    Msg* ProcessMsg(MsgWait* aMsg) override;
    Msg* ProcessMsg(MsgDecodedStream* aMsg) override;
    Msg* ProcessMsg(MsgAudioPcm* aMsg) override;
    Msg* ProcessMsg(MsgSilence* aMsg) override;
    Msg* ProcessMsg(MsgPlayable* aMsg) override;
    Msg* ProcessMsg(MsgQuit* aMsg) override;
private:
    IFileSystem& iFileSystem;
    IFile* iFile;
    Semaphore& iSem;
};

class Decoder : private INonCopyable
{
private:
    static const TUint kThreadPriorityMax = kPriorityHighest - 1;
public:
    Decoder(MsgFactory& aMsgFactory, IPipelineElementUpstream& aUpstreamElement, IPipelineElementDownstream& aDownstreamElement, IUrlBlockWriter& aUrlBlockWriter);
    ~Decoder();
    void AddContainer(ContainerBase* aContainer);
    void AddCodec(CodecBase* aCodec);
    void Start();
private:
    Container* iContainer;
    Logger* iLoggerContainer;
    CodecController* iCodecController;
    Logger* iLoggerCodecController;
};

} // namespace Codec
} // namespace Media
} // namespace OpenHome


using namespace OpenHome;
using namespace OpenHome::Media;
using namespace OpenHome::Media::Codec;
using namespace OpenHome::Net;
using namespace OpenHome::TestFramework;


// ElementFileReader

ElementFileReader::ElementFileReader(IFileSystem& aFileSystem, TrackFactory& aTrackFactory, MsgFactory& aMsgFactory)
    : iFileSystem(aFileSystem)
    , iTrackFactory(aTrackFactory)
    , iMsgFactory(aMsgFactory)
    , iFile(NULL)
    , iMode(eMode)
{
}

ElementFileReader::~ElementFileReader()
{
    delete iFile;
}

void ElementFileReader::Open(const Brx& aFilename)
{
    Bwh filename(aFilename.Bytes()+1);  // space for '\0'
    filename.Replace(aFilename);
    iFile = iFileSystem.Open(filename.PtrZ(), eFileReadOnly);
}

Msg* ElementFileReader::Pull()
{
    // Container/CodecController expect msgs in a particular sequence, so adhere to that.

    // MsgMode (mode: Playlist, supportsLatency: 0, realTime: 0)
    // MsgSession
    // MsgTrack
    // MsgEncodedStream
    // MsgAudioEncoded - repeated while audio should still be output
    // MsgFlush (asked for from upstream/required?)
    // MsgHalt
    // MsgQuit

    ASSERT(iFile != NULL);
    Msg* msg = NULL;
    if (iMode == eMode) {
        const Brn mode("Playlist");
        const TBool supportsLatency = false;
        const TBool realTime = false;
        IClockPuller* clockPuller = NULL;
        msg = iMsgFactory.CreateMsgMode(mode, supportsLatency, realTime, clockPuller);
        iMode = eSession;
    }
    else if (iMode == eSession) {
        msg = iMsgFactory.CreateMsgSession();
        iMode = eTrack;
    }
    else if (iMode == eTrack) {
        const Brn uri("file://dummy/interactive/decoder/track/uri");
        const Brn metaData(Brx::Empty());
        Track* track = iTrackFactory.CreateTrack(uri, metaData);
        const TBool startOfStream = true;
        msg = iMsgFactory.CreateMsgTrack(*track, startOfStream);
        track->RemoveRef();
        iMode = eEncodedStream;
    }
    else if (iMode == eEncodedStream) {
        const Brn uri("file://dummy/interactive/decoder/stream/uri");
        const Brn metaText(Brx::Empty());
        const TUint64 totalBytes = iFile->Bytes();
        const TUint streamId = IPipelineIdProvider::kStreamIdInvalid+1;
        const TBool seekable = true;    // Streams such as those within MPEG TS containers are currently non-seekable.
        const TBool live = false;
        IStreamHandler* streamHandler = NULL;
        msg = iMsgFactory.CreateMsgEncodedStream(uri, metaText, totalBytes, streamId, seekable, live, streamHandler);
        iMode = eAudioEncoded;
    }
    else if (iMode == eAudioEncoded) {
        Log::Print("ElementFileReader::Pull iFile->Tell(): %u, iFile->Bytes(): %u\n", iFile->Tell(), iFile->Bytes());
        try {
            iFile->Read(iInBuf);
            msg = iMsgFactory.CreateMsgAudioEncoded(iInBuf);
            iInBuf.SetBytes(0);
        }
        catch (FileReadError&) {
            Log::Print("ElementFileReader::Pull reached end of input file\n");
            // Reached end of file.
            // Must output something here to avoid sending a NULL msg down pipeline.
            // Next msg should be MsgHalt, so output that.

            //const TUint id = MsgHalt::kIdNone+1;
            msg = iMsgFactory.CreateMsgHalt();
            iMode = eQuit;
        }
    }
    else if (iMode == eQuit) {
        msg = iMsgFactory.CreateMsgQuit();
        iMode = eEnd;
    }
    else {
        ASSERTS();
    }
    ASSERT(msg != NULL);
    return msg;
}

TBool ElementFileReader::TryGet(IWriter& aWriter, const Brx& aUrl, TUint64 aOffset, TUint aBytes)
{
    ASSERT(iFile != NULL);
    Log::Print("ElementFileReader::TryGet aUrl: ");
    Log::Print(aUrl);
    Log::Print(", aOffset: %llu, iFile->Bytes(): %u, aBytes: %u, iFile bytes remaining: %u\n", aOffset, iFile->Bytes(), aBytes, iFile->Bytes()-iFile->Tell());

    const TUint current = iFile->Tell();

    //ASSERT(aOffset <= iFile->Bytes());
    //ASSERT(aBytes <= iFile->Bytes()-iFile->Tell());
    if (aOffset > iFile->Bytes() || aBytes > iFile->Bytes()-iFile->Tell()) {
        Log::Print("ElementFileReader::TryGet seek parameters out of bounds\n");
        return false;
    }

    try {
        iFile->Seek(aBytes);
        ASSERT(iInBuf.Bytes() == 0);
        TUint remaining = aBytes;
        while (remaining > 0) {
            try {
                iFile->Read(iInBuf, remaining);
                aWriter.Write(iInBuf);
                remaining -= iInBuf.Bytes();
                iInBuf.SetBytes(0);
            }
            catch (FileReadError&) {
                // Reached end of file.
                Log::Print("ElementFileReader::TryGet reached end of file unexpectedly\n");
                ASSERTS();
            }
        }
    }
    catch (FileSeekError&) {
        Log::Print("ElementFileReader::TryGet failed to seek to pos %u\n", aBytes);
        ASSERTS();
    }

    try {
        iFile->Seek(current);
    }
    catch (FileSeekError&) {
        Log::Print("ElementFileReader::TryGet failed to seek back to pos %u\n", current);
        ASSERTS();
    }

    Log::Print("ElementFileReader::TryGet successfully retrieved all bytes\n");
    return true;
}


// ElementFileWriter

ElementFileWriter::ElementFileWriter(IFileSystem& aFileSystem, Semaphore& aSem)
    : iFileSystem(aFileSystem)
    , iFile(NULL)
    , iSem(aSem)
{
}

ElementFileWriter::~ElementFileWriter()
{
    delete iFile;
}

void ElementFileWriter::Open(const Brx& aFilename)
{
    Bwh filename(aFilename.Bytes()+1);  // space for '\0'
    filename.Replace(aFilename);
    iFile = iFileSystem.Open(filename.PtrZ(), eFileReadWrite);
}


void ElementFileWriter::Push(Msg* aMsg)
{
    ASSERT(iFile != NULL);
    ASSERT(aMsg != NULL);
    Msg* msg = aMsg->Process(*this);
    ASSERT(msg == NULL);
}

Msg* ElementFileWriter::ProcessMsg(MsgMode* aMsg)
{
    Log::Print("ElementFileWriter::ProcessMsg MsgMode\n");
    aMsg->RemoveRef();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgSession* aMsg)
{
    Log::Print("ElementFileWriter::ProcessMsg MsgSession\n");
    aMsg->RemoveRef();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgTrack* aMsg)
{
    Log::Print("ElementFileWriter::ProcessMsg MsgTrack\n");
    aMsg->RemoveRef();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgDelay* /*aMsg*/)
{
    ASSERTS();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgEncodedStream* aMsg)
{
    Log::Print("ElementFileWriter::ProcessMsg MsgEncodedStream\n");
    aMsg->RemoveRef();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgAudioEncoded* /*aMsg*/)
{
    ASSERTS();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgMetaText* aMsg)
{
    aMsg->RemoveRef();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgHalt* aMsg)
{
    Log::Print("ElementFileWriter::ProcessMsg MsgHalt\n");
    aMsg->RemoveRef();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgFlush* /*aMsg*/)
{
    ASSERTS();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgWait* /*aMsg*/)
{
    ASSERTS();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgDecodedStream* aMsg)
{
    // FIXME - output (an optional) WAV header here?
    Log::Print("ElementFileWriter::ProcessMsg MsgDecodedStream\n");
    aMsg->RemoveRef();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgAudioPcm* aMsg)
{
    // Write audio out to file.
    const TUint64 trackOffset = aMsg->TrackOffset();
    ProcessorPcmBufPacked pcmProcessor;
    MsgPlayable* playable = aMsg->CreatePlayable();
    playable->Read(pcmProcessor);
    Brn buf(pcmProcessor.Buf());

    try {
        iFile->Write(buf);
    }
    catch (FileWriteError&) {
        Log::Print("ElementFileWriter::ProcessMsg MsgAudioPcm Error while writing output file at audio offset %llu\n", trackOffset);
        ASSERTS();
    }

    playable->RemoveRef();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgSilence* /*aMsg*/)
{
    ASSERTS();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgPlayable* /*aMsg*/)
{
    ASSERTS();
    return NULL;
}

Msg* ElementFileWriter::ProcessMsg(MsgQuit* aMsg)
{
    Log::Print("ElementFileWriter::ProcessMsg MsgQuit\n");
    aMsg->RemoveRef();
    iSem.Signal();
    return NULL;
}


// Decoder

Decoder::Decoder(MsgFactory& aMsgFactory, IPipelineElementUpstream& aUpstreamElement, IPipelineElementDownstream& aDownstreamElement, IUrlBlockWriter& aUrlBlockWriter)
{
    iContainer = new Container(aMsgFactory, aUpstreamElement);
    iLoggerContainer = new Logger(*iContainer, "Codec Container");

    // Construct push logger slightly out of sequence.
    iLoggerCodecController = new Logger("Codec Controller", aDownstreamElement);
    iCodecController = new Codec::CodecController(aMsgFactory, *iLoggerContainer, *iLoggerCodecController, aUrlBlockWriter, kThreadPriorityMax);

    //iLoggerContainer->SetEnabled(true);
    //iLoggerCodecController->SetEnabled(true);

    //iLoggerContainer->SetFilter(Logger::EMsgAll);
    //iLoggerCodecController->SetFilter(Logger::EMsgAll);
}

Decoder::~Decoder()
{
    delete iCodecController;
    delete iLoggerCodecController;
    delete iLoggerContainer;
    delete iContainer;
}

void Decoder::AddContainer(ContainerBase* aContainer)
{
    iContainer->AddContainer(aContainer);
}

void Decoder::AddCodec(CodecBase* aCodec)
{
    iCodecController->AddCodec(aCodec);
}

void Decoder::Start()
{
    iCodecController->Start();
}



int CDECL main(int aArgc, char* aArgv[])
{
#ifdef _WIN32
    char* noErrDlgs = getenv("NO_ERROR_DIALOGS");
    if (noErrDlgs != NULL && strcmp(noErrDlgs, "1") == 0) {
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
#endif // _WIN32


    // Parse options.
    OptionParser options;
    OptionString optionFileIn("-i", "--in", Brn("C:\\work\\ohMediaPlayerTmp\\in.txt"), "Input file");
    OptionString optionFileOut("-o", "--out", Brn("C:\\work\\ohMediaPlayerTmp\\out.txt"), "Output file");
    //OptionBool optionSeek("-s", "--seek", "Perform a seek during decoding");
    //OptionUint optionSeekFrom("-sf", "--seekfrom", 0, "Seek start pos (in seconds)");
    //OptionUint optionSeekTo("-st", "--seekto", 0, "Seek end pos (in seconds)");
    //OptionString optionSeekFile("-os", "--fileseek", Brn(""), "File to output raw data to from seek position");

    options.AddOption(&optionFileIn);
    options.AddOption(&optionFileOut);
    //options.AddOption(&optionSeek);
    //options.AddOption(&optionSeekFrom);
    //options.AddOption(&optionSeekTo);
    //options.AddOption(&optionSeekFile);

    if (!options.Parse(aArgc, aArgv)) {
        Log::Print("Error: failure parsing input parameters.\n");
        return 1;
    }

    if (optionFileIn.Value().Bytes() == 0) {
        Log::Print("Error: input file [-i|--in <file>] must be specified.\n");
        return 1;
    }

    if (optionFileOut.Value().Bytes() == 0) {
        Log::Print("Error: output file [-o|--out <file>] must be specified.\n");
        return 1;
    }

    static const TUint kTrackCount = 1;

    static const TUint kEncodedAudioCount = 100;
    static const TUint kMsgEncodedAudioCount = 200;    // Allow for Split().
    static const TUint kDecodedAudioCount = 100;
    static const TUint kMsgAudioPcmCount = 200;        // Allow for Split().
    static const TUint kMsgSilenceCount = 0;
    static const TUint kMsgPlayablePcmCount = 10;
    static const TUint kMsgPlayableSilenceCount = 0;
    static const TUint kMsgDecodedStreamCount = 2;
    static const TUint kMsgTrackCount = 1;
    static const TUint kMsgEncodedStreamCount = 2;
    static const TUint kMsgMetaTextCount = 1;
    static const TUint kMsgHaltCount = 1;
    static const TUint kMsgFlushCount = 1;
    static const TUint kMsgWaitCount = 0;
    static const TUint kMsgModeCount = 1;
    static const TUint kMsgSessionCount = 1;
    static const TUint kMsgDelayCount = 0;
    static const TUint kMsgQuitCount = 1;

    InitialisationParams* initParams = InitialisationParams::Create();
    Net::UpnpLibrary::InitialiseMinimal(initParams);

    AllocatorInfoLogger* infoAggregator = new AllocatorInfoLogger();
    TrackFactory* trackFactory = new TrackFactory(*infoAggregator, kTrackCount);
    MsgFactory* msgFactory = new MsgFactory(*infoAggregator,
                                           kEncodedAudioCount,
                                           kMsgEncodedAudioCount,
                                           kDecodedAudioCount,
                                           kMsgAudioPcmCount,
                                           kMsgSilenceCount,
                                           kMsgPlayablePcmCount,
                                           kMsgPlayableSilenceCount,
                                           kMsgDecodedStreamCount,
                                           kMsgTrackCount,
                                           kMsgEncodedStreamCount,
                                           kMsgMetaTextCount,
                                           kMsgHaltCount,
                                           kMsgFlushCount,
                                           kMsgWaitCount,
                                           kMsgModeCount,
                                           kMsgSessionCount,
                                           kMsgDelayCount,
                                           kMsgQuitCount);
    Semaphore* sem = new Semaphore("TCIS", 0);
    FileSystemAnsii fileSystem;
    ElementFileReader fileReader(fileSystem, *trackFactory, *msgFactory);
    ElementFileWriter fileWriter(fileSystem, *sem);
    Decoder* decoder = new Decoder(*msgFactory, fileReader, fileWriter, fileReader);

    decoder->AddContainer(new Id3v2());
    //decoder->AddContainer(new Mpeg4Container());
    decoder->AddContainer(new MpegTs());

    decoder->AddCodec(CodecFactory::NewAac());
    decoder->AddCodec(CodecFactory::NewAifc());
    decoder->AddCodec(CodecFactory::NewAiff());
    decoder->AddCodec(CodecFactory::NewAlac());
    decoder->AddCodec(CodecFactory::NewAdts());
    decoder->AddCodec(CodecFactory::NewFlac());
    decoder->AddCodec(CodecFactory::NewPcm());
    decoder->AddCodec(CodecFactory::NewVorbis());
    decoder->AddCodec(CodecFactory::NewWav());

    // Try open input file.
    try {
        fileReader.Open(optionFileIn.Value());
    }
    catch (FileOpenError&) {
        Log::Print("Error: failed to open input file <");
        Log::Print(optionFileIn.Value());
        Log::Print(">\n");
        return 1;
    }

    // Try open output file.
    try {
        fileWriter.Open(optionFileOut.Value());
    }
    catch (FileOpenError&) {
        Log::Print("Error: failed to open output file <");
        Log::Print(optionFileOut.Value());
        Log::Print(">\n");
        return 1;
    }

    decoder->Start();

    sem->Wait();

    delete decoder;
    delete sem;
    delete msgFactory;
    delete trackFactory;
    delete infoAggregator;

    Net::UpnpLibrary::Close();
    delete initParams;

    return 0;
}
