#include <OpenHome/Media/Protocol/Protocol.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Private/Stream.h>
#include <OpenHome/Private/Ascii.h>
#include <OpenHome/Private/Parser.h>
#include <OpenHome/Av/Debug.h>
#include <OpenHome/Av/Radio/ContentProcessorFactory.h>

/* Example pls file

[playlist]
NumberOfEntries=3

File1=http://streamexample.com:80
Title1=My Favorite Online Radio
Length1=-1

File2=http://example.com/song.mp3
Title2=Remote MP3
Length2=286

File3=/home/myaccount/album.flac
Title3=Local album
Length3=3487

Version=2

*/

namespace OpenHome {
namespace Av {

class ContentPls : public Media::ContentProcessor
{
private: // from ContentProcessor
    TBool Recognise(const Brx& aUri, const Brx& aMimeType, const Brx& aData);
    Media::ProtocolStreamResult Stream(Media::IProtocolReader& aReader, TUint64 aTotalBytes);
    void Reset();
private:
    TBool iIsPlaylist;
};

} // namespace Av
} // namespace OpenHome

using namespace OpenHome;
using namespace OpenHome::Media;
using namespace OpenHome::Av;


ContentProcessor* ContentProcessorFactory::NewPls()
{ // static
    return new ContentPls();
}


// ContentPls

TBool ContentPls::Recognise(const Brx& /*aUri*/, const Brx& aMimeType, const Brx& aData)
{
    if (Ascii::CaseInsensitiveEquals(aMimeType, Brn("audio/x-scpls"))) {
        return true;
    }
    Brn plsId("[playlist]");
    if (aData.Bytes() >= plsId.Bytes()) {
        Brn plsLine(aData.Ptr(), plsId.Bytes());
        if (Ascii::CaseInsensitiveEquals(plsLine, plsId)) {
            return true;
        }
    }
    return false;
}

ProtocolStreamResult ContentPls::Stream(IProtocolReader& aReader, TUint64 aTotalBytes)
{
    LOG(kMedia, "ContentPls::Stream\n");

    TUint64 bytesRemaining = aTotalBytes;
    TBool stopped = false;
    TBool streamSucceeded = false;
    try {
        // Find [playlist]
        while (!iIsPlaylist) {
            Brn line = ReadLine(aReader, bytesRemaining);
            if (Ascii::CaseInsensitiveEquals(line, Brn("[playlist]"))) {
                iIsPlaylist = true;
            }
        }

        while (!stopped) {
            Brn line = ReadLine(aReader, bytesRemaining);
            Parser parser(line);
            Brn key = parser.Next('=');
            if (key.BeginsWith(Brn("File"))) {
                Brn value = parser.Next();
                ProtocolStreamResult res = iProtocolSet->Stream(value);
                if (res == EProtocolStreamStopped) {
                    stopped = true;
                }
                else if (res == EProtocolStreamSuccess) {
                    streamSucceeded = true;
                }
            }
        }
    }
    catch (ReaderError&) {
    }

    if (stopped) {
        return EProtocolStreamStopped;
    }
    else if (bytesRemaining > 0 && bytesRemaining < aTotalBytes) {
        // break in stream.  Return an error and let caller attempt to re-establish connection
        return EProtocolStreamErrorRecoverable;
    }
    else if (streamSucceeded) {
        return EProtocolStreamSuccess;
    }
    return EProtocolStreamErrorUnrecoverable;
}

void ContentPls::Reset()
{
    ContentProcessor::Reset();
    iIsPlaylist = false;
}
