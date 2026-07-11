#include "Audio/FilePlayer.h"

namespace xoa
{

// Read-ahead buffer (samples per channel). ~32k covers well over half a second
// at any supported rate - a large cushion against disk latency.
static constexpr int kReadAheadSamples = 32768;

FilePlayer::FilePlayer()
{
    formatManager.registerBasicFormats();   // WAV + AIFF + FLAC + Ogg (+ CAF/MP3 on Apple)
    readAheadThread.startThread();
}

FilePlayer::~FilePlayer()
{
    transport.setSource (nullptr);
    readAheadThread.stopThread (2000);
}

int FilePlayer::detectAmbiOrder (int channelCount) noexcept
{
    for (int n = xoa::kAmbisonicOrder; n >= 0; --n)
        if ((n + 1) * (n + 1) == channelCount)
            return n;
    return 0;
}

//==============================================================================
FilePlayer::OpenResult FilePlayer::open (const juce::File& file)
{
    OpenResult r;

    if (! file.existsAsFile())
    {
        r.error = "File does not exist: " + file.getFullPathName();
        return r;
    }

    // CAF is only readable where CoreAudioFormat is available (macOS).
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        if (file.hasFileExtension ("caf"))
            r.error = "CAF is only supported on macOS in v1; please convert to WAV.";
        else
            r.error = "Unsupported or unreadable audio file: " + file.getFileName();
        return r;
    }

    r.numChannels    = (int) reader->numChannels;
    r.fileSampleRate = reader->sampleRate;
    r.lengthSamples  = reader->lengthInSamples;
    r.detectedOrder  = detectAmbiOrder (r.numChannels);

    if (r.numChannels > xoa::kMaxFileChannels)
        r.warnings.add ("File has " + juce::String (r.numChannels) + " channels; only the first "
                        + juce::String (xoa::kMaxFileChannels) + " will be played.");
    if (r.detectedOrder == 0)
        r.warnings.add ("Channel count " + juce::String (r.numChannels)
                        + " is not a perfect square; set the content order manually.");

    const int playChannels = juce::jmin (r.numChannels, xoa::kMaxFileChannels);

    // AudioFormatReaderSource takes ownership of the reader.
    auto newSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
    newSource->setLooping (looping);

    transport.setSource (newSource.get(), kReadAheadSamples, &readAheadThread,
                         r.fileSampleRate, playChannels);
    readerSource = std::move (newSource);
    numChannels = playChannels;

    if (deviceSampleRate > 0.0 && blockSize > 0)
        transport.prepareToPlay (blockSize, deviceSampleRate);

    r.ok = true;
    return r;
}

void FilePlayer::close()
{
    transport.stop();
    transport.setSource (nullptr);
    readerSource.reset();
    numChannels = 0;
}

//==============================================================================
void FilePlayer::play()               { if (readerSource != nullptr) transport.start(); }
void FilePlayer::stop()               { transport.stop(); }
bool FilePlayer::isPlaying() const    { return transport.isPlaying(); }

void FilePlayer::setLooping (bool shouldLoop)
{
    looping = shouldLoop;
    if (readerSource != nullptr)
        readerSource->setLooping (shouldLoop);
}

void FilePlayer::seekSeconds (double seconds)  { transport.setPosition (seconds); }
double FilePlayer::getPositionSeconds() const  { return transport.getCurrentPosition(); }
double FilePlayer::getLengthSeconds() const    { return transport.getLengthInSeconds(); }

//==============================================================================
void FilePlayer::prepareToPlay (double sr, int block)
{
    deviceSampleRate = sr;
    blockSize = block;
    transport.prepareToPlay (block, sr);
}

void FilePlayer::renderNextBlock (juce::AudioBuffer<float>& dest, int numSamples) noexcept
{
    // Channels the file does not provide stay silent.
    dest.clear();

    if (readerSource == nullptr || ! transport.isPlaying())
        return;

    juce::AudioSourceChannelInfo info (&dest, 0, numSamples);
    transport.getNextAudioBlock (info);
}

void FilePlayer::releaseResources()
{
    transport.releaseResources();
    deviceSampleRate = 0.0;
    blockSize = 0;
}

} // namespace xoa
