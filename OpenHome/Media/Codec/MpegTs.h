#ifndef HEADER_PIPELINE_CODEC_MPEG_TS
#define HEADER_PIPELINE_CODEC_MPEG_TS

#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Media/Codec/Container.h>

namespace OpenHome {
namespace Media {
namespace Codec {

class MpegTs : public ContainerBase
{
public:
    MpegTs();
public: // from IRecogniser
    TBool Recognise(Brx& aBuf);
private: // from IMsgProcessor
    Msg* ProcessMsg(MsgEncodedStream* aMsg) override;
    Msg* ProcessMsg(MsgAudioEncoded* aMsg) override;
private: // from IStreamHandler
    TUint TrySeek(TUint aStreamId, TUint64 aOffset);
private:
    TBool RecogniseTableHeader(const Brx& aTableHeader);
    TBool RecogniseTableSyntax(const Brx& aTableSyntax);
    TBool RecogniseTableHeaderAndSyntax(const Brx& aTable, TUint aTableType);
    TBool RecognisePat(const Brx& aPat);
    TBool RecognisePmt(const Brx& aPmt);
private:
private:
    //static const TUint kRecogniseBytes = 4;
    static const TUint kRecogniseBytes = 188;
    static const TUint kPacketBytes = 188;
    static const TUint kMpegHeaderBytes = 4;
    static const TUint kTableHeaderBytes = 4;
    static const TUint kTableSyntaxFixedBytes = 5;
    static const TUint kPmtSpecificFixedBytes = 4;
    static const TUint kStreamSpecificFixedBytes = 5;

    static const TUint kTableTypePat = 0x00;
    static const TUint kTableTypePmt = 0x02;
    static const TUint kStreamTypeAdtsAac = 0x0f;   // stream type 15/0x0f is ISO/IEC 13818-7 ADTS AAC

    static const TUint kPesHeaderStartCodePrefixBytes = 3;
    static const TUint kPesHeaderFixedBytes = 6;
    static const TUint kPesHeaderOptionalFixedBytes = 3;

    TUint iSize;
    TUint iTotalSize;   // FIXME - this is important for seeking - ensure this is being incremented correctly
    TUint iPacketBytes;
    TUint iTableType;
    TUint iSectionLength;
    TUint iProgramMapPid;
    TUint iStreamPid;
    Bws<kRecogniseBytes> iBuf;
};

} // namespace Codec
} // namespace Media
} // namespace OpenHome

#endif  // HEADER_PIPELINE_CODEC_MPEG_TS