// Deterministic ring-stream test. Build raylib with a null audio backend for CI:
// -DMA_ENABLE_ONLY_SPECIFIC_BACKENDS -DMA_ENABLE_NULL

#include "raylib.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int16_t ExpectedSample(size_t frame, unsigned int channel)
{
    return (int16_t)(uint16_t)(frame*31 + channel*17 + 7);
}

static void FillFrames(int16_t *frames, size_t firstFrame, size_t frameCount, unsigned int channels)
{
    for (size_t frame = 0; frame < frameCount; frame++)
    {
        for (unsigned int channel = 0; channel < channels; channel++)
        {
            frames[frame*channels + channel] = ExpectedSample(firstFrame + frame, channel);
        }
    }
}

static int CheckFrames(const int16_t *frames, size_t firstFrame, size_t frameCount, unsigned int channels)
{
    for (size_t frame = 0; frame < frameCount; frame++)
    {
        for (unsigned int channel = 0; channel < channels; channel++)
        {
            if (frames[frame*channels + channel] != ExpectedSample(firstFrame + frame, channel))
            {
                fprintf(stderr, "sample mismatch at frame %zu channel %u: got %d, expected %d\n",
                        firstFrame + frame, channel, frames[frame*channels + channel],
                        ExpectedSample(firstFrame + frame, channel));
                return 0;
            }
        }
    }

    return 1;
}

static int TestInvalidArguments(void)
{
    AudioStream invalidSize = LoadAudioStreamRing(48000, 24, 2, 1024);
    AudioStream invalidCapacity = LoadAudioStreamRing(48000, 16, 2, 0);
    CHECK(!IsAudioStreamValid(invalidSize));
    CHECK(!IsAudioStreamValid(invalidCapacity));

    AudioStream stream = LoadAudioStreamRing(48000, 16, 1, 16);
    CHECK(IsAudioStreamValid(stream));

    int transferred = -1;
    CHECK(WriteAudioStream(stream, NULL, 1, &transferred) == AUDIO_STREAM_STATUS_INVALID);
    CHECK(transferred == 0);
    CHECK(WriteAudioStream(stream, NULL, 0, &transferred) == AUDIO_STREAM_STATUS_OK);
    CHECK(transferred == 0);
    CHECK(ReadAudioStream(stream, NULL, 1, &transferred) == AUDIO_STREAM_STATUS_INVALID);
    CHECK(ReadAudioStream(stream, NULL, 0, &transferred) == AUDIO_STREAM_STATUS_OK);

    UnloadAudioStream(stream);
    return 0;
}

static int TestCapacityAndUnderrun(void)
{
    AudioStream stream = LoadAudioStreamRing(48000, 16, 1, 7);
    CHECK(IsAudioStreamValid(stream));
    CHECK(GetAudioStreamCapacityFrames(stream) == 7);
    CHECK(GetAudioStreamBufferedFrames(stream) == 0);
    CHECK(GetAudioStreamFreeFrames(stream) == 7);

    int16_t input[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    int written = -1;
    CHECK(WriteAudioStream(stream, input, 5, &written) == AUDIO_STREAM_STATUS_OK);
    CHECK(written == 5);
    CHECK(GetAudioStreamFreeFrames(stream) == 2);

    CHECK(WriteAudioStream(stream, input + 5, 4, &written) == AUDIO_STREAM_STATUS_PARTIAL);
    CHECK(written == 2);
    CHECK(GetAudioStreamFreeFrames(stream) == 0);
    CHECK(GetAudioStreamBufferedFrames(stream) == 7);

    CHECK(WriteAudioStream(stream, input, 1, &written) == AUDIO_STREAM_STATUS_FULL);
    CHECK(written == 0);

    int16_t output[16];
    int read = -1;
    CHECK(ReadAudioStream(stream, output, 3, &read) == AUDIO_STREAM_STATUS_OK);
    CHECK(read == 3);
    CHECK(output[0] == 0 && output[1] == 1 && output[2] == 2);

    CHECK(ReadAudioStream(stream, output, 10, &read) == AUDIO_STREAM_STATUS_UNDERRUN);
    CHECK(read == 4);
    CHECK(output[0] == 3 && output[1] == 4 && output[2] == 5 && output[3] == 6);
    for (int i = 4; i < 10; i++) CHECK(output[i] == 0);
    CHECK(GetAudioStreamUnderrunCount(stream) == 1);
    CHECK(GetAudioStreamStatus(stream) == AUDIO_STREAM_STATUS_UNDERRUN);

    CHECK(ReadAudioStream(stream, output, 1, &read) == AUDIO_STREAM_STATUS_UNDERRUN);
    CHECK(read == 0);
    CHECK(GetAudioStreamUnderrunCount(stream) == 1);

    CHECK(WriteAudioStream(stream, input, 2, &written) == AUDIO_STREAM_STATUS_OK);
    CHECK(GetAudioStreamStatus(stream) == AUDIO_STREAM_STATUS_OK);
    CHECK(ReadAudioStream(stream, output, 2, &read) == AUDIO_STREAM_STATUS_OK);
    CHECK(read == 2 && output[0] == 0 && output[1] == 1);
    CHECK(GetAudioStreamUnderrunCount(stream) == 1);

    FlushAudioStream(stream);
    CHECK(GetAudioStreamBufferedFrames(stream) == 0);
    CHECK(GetAudioStreamFreeFrames(stream) == 7);
    CHECK(GetAudioStreamUnderrunCount(stream) == 0);
    CHECK(GetAudioStreamStatus(stream) == AUDIO_STREAM_STATUS_OK);

    UnloadAudioStream(stream);
    return 0;
}

static int TestWrapAndRecovery(void)
{
    AudioStream stream = LoadAudioStreamRing(44100, 16, 2, 13);
    CHECK(IsAudioStreamValid(stream));

    size_t produced = 0;
    size_t consumed = 0;
    const unsigned int channels = 2;
    int16_t input[64*2];
    int16_t output[64*2];

    for (int step = 0; step < 200; step++)
    {
        size_t writeCount = (size_t)(1 + ((step*7)%29));
        FillFrames(input, produced, writeCount, channels);

        int written = 0;
        AudioStreamStatus status = WriteAudioStream(stream, input, (int)writeCount, &written);
        if (status != AUDIO_STREAM_STATUS_OK)
        {
            CHECK(status == AUDIO_STREAM_STATUS_PARTIAL);
            CHECK(written > 0);
        }
        produced += (size_t)written;

        size_t readCount = (size_t)(1 + ((step*11)%17));
        if (readCount > (produced - consumed)) readCount = produced - consumed;

        int read = 0;
        CHECK(ReadAudioStream(stream, output, (int)readCount, &read) == AUDIO_STREAM_STATUS_OK);
        CHECK((size_t)read == readCount);
        CHECK(CheckFrames(output, consumed, readCount, channels));
        consumed += readCount;
    }

    while (consumed < produced)
    {
        size_t readCount = produced - consumed;
        if (readCount > 64) readCount = 64;

        int read = 0;
        CHECK(ReadAudioStream(stream, output, (int)readCount, &read) == AUDIO_STREAM_STATUS_OK);
        CHECK((size_t)read == readCount);
        CHECK(CheckFrames(output, consumed, readCount, channels));
        consumed += readCount;
    }

    int read = 0;
    CHECK(ReadAudioStream(stream, output, 1, &read) == AUDIO_STREAM_STATUS_UNDERRUN);
    CHECK(read == 0);
    CHECK(GetAudioStreamUnderrunCount(stream) == 1);

    FillFrames(input, produced, 1, channels);
    int written = 0;
    CHECK(WriteAudioStream(stream, input, 1, &written) == AUDIO_STREAM_STATUS_OK);
    CHECK(ReadAudioStream(stream, output, 1, &read) == AUDIO_STREAM_STATUS_OK);
    CHECK(read == 1);
    CHECK(CheckFrames(output, produced, 1, channels));
    CHECK(GetAudioStreamUnderrunCount(stream) == 1);

    UnloadAudioStream(stream);
    return 0;
}

static int TestOneHour(void)
{
    const unsigned int sampleRate = 48000;
    const unsigned int channels = 2;
    const unsigned int capacityFrames = 4096;
    const size_t totalFrames = (size_t)sampleRate*60*60;

    AudioStream stream = LoadAudioStreamRing(sampleRate, 16, channels, capacityFrames);
    CHECK(IsAudioStreamValid(stream));
    CHECK(GetAudioStreamCapacityFrames(stream) == (int)capacityFrames);

    unsigned int memoryUsage = GetAudioStreamMemoryUsage(stream);
    CHECK(memoryUsage >= (unsigned int)(capacityFrames*channels*sizeof(int16_t)));

    int16_t input[2048*2];
    int16_t output[2048*2];
    size_t produced = 0;
    size_t consumed = 0;
    size_t chunk = 1024;

    while (produced < capacityFrames)
    {
        size_t writeCount = capacityFrames - produced;
        if (writeCount > 1024) writeCount = 1024;

        FillFrames(input, produced, writeCount, channels);
        int written = 0;
        CHECK(WriteAudioStream(stream, input, (int)writeCount, &written) == AUDIO_STREAM_STATUS_OK);
        CHECK((size_t)written == writeCount);
        produced += writeCount;
    }

    while (consumed < totalFrames)
    {
        size_t readCount = produced - consumed;
        if (readCount > chunk) readCount = chunk;
        if (readCount > totalFrames - consumed) readCount = totalFrames - consumed;

        int read = 0;
        CHECK(ReadAudioStream(stream, output, (int)readCount, &read) == AUDIO_STREAM_STATUS_OK);
        CHECK((size_t)read == readCount);
        CHECK(CheckFrames(output, consumed, readCount, channels));
        consumed += readCount;

        if (produced < totalFrames)
        {
            size_t writeCount = readCount;
            if (writeCount > totalFrames - produced) writeCount = totalFrames - produced;

            FillFrames(input, produced, writeCount, channels);
            int written = 0;
            CHECK(WriteAudioStream(stream, input, (int)writeCount, &written) == AUDIO_STREAM_STATUS_OK);
            CHECK((size_t)written == writeCount);
            produced += writeCount;
        }

        chunk = 333 + ((chunk*17)%1715);
    }

    CHECK(produced == totalFrames);
    CHECK(consumed == totalFrames);
    CHECK(GetAudioStreamUnderrunCount(stream) == 0);
    CHECK(GetAudioStreamMemoryUsage(stream) == memoryUsage);

    int16_t nextInput[channels];
    int16_t nextOutput[channels];
    int transferred = 0;

    CHECK(ReadAudioStream(stream, nextOutput, 1, &transferred) == AUDIO_STREAM_STATUS_UNDERRUN);
    CHECK(transferred == 0);
    CHECK(GetAudioStreamUnderrunCount(stream) == 1);

    FillFrames(nextInput, totalFrames, 1, channels);
    CHECK(WriteAudioStream(stream, nextInput, 1, &transferred) == AUDIO_STREAM_STATUS_OK);
    CHECK(GetAudioStreamStatus(stream) == AUDIO_STREAM_STATUS_OK);
    CHECK(ReadAudioStream(stream, nextOutput, 1, &transferred) == AUDIO_STREAM_STATUS_OK);
    CHECK(transferred == 1);
    CHECK(CheckFrames(nextOutput, totalFrames, 1, channels));
    CHECK(GetAudioStreamUnderrunCount(stream) == 1);
    CHECK(GetAudioStreamMemoryUsage(stream) == memoryUsage);

    printf("one_hour_frames=%zu\n", totalFrames);
    printf("capacity_frames=%u\n", capacityFrames);
    printf("capacity_bytes=%u\n", capacityFrames*channels*(unsigned int)sizeof(int16_t));
    printf("stream_memory_bytes=%u\n", memoryUsage);

    UnloadAudioStream(stream);
    return 0;
}

static int TestPlaybackUnderrunRecovery(void)
{
    const unsigned int channels = 2;
    AudioStream stream = LoadAudioStreamRing(48000, 16, channels, 1024);
    CHECK(IsAudioStreamValid(stream));

    PlayAudioStream(stream);
    WaitTime(0.08);

    unsigned int underrunCount = GetAudioStreamUnderrunCount(stream);
    CHECK(underrunCount >= 1);
    CHECK(GetAudioStreamStatus(stream) == AUDIO_STREAM_STATUS_UNDERRUN);

    int16_t input[1024*2];
    FillFrames(input, 0, 1024, channels);

    int written = 0;
    CHECK(WriteAudioStream(stream, input, 1024, &written) == AUDIO_STREAM_STATUS_OK);
    CHECK(written == 1024);
    CHECK(GetAudioStreamStatus(stream) == AUDIO_STREAM_STATUS_OK);

    WaitTime(0.005);
    CHECK(GetAudioStreamUnderrunCount(stream) == underrunCount);

    StopAudioStream(stream);
    CHECK(GetAudioStreamBufferedFrames(stream) == 0);

    UnloadAudioStream(stream);
    return 0;
}

int main(void)
{
    SetTraceLogLevel(LOG_NONE);
    InitAudioDevice();
    CHECK(IsAudioDeviceReady());

    CHECK(TestInvalidArguments() == 0);
    CHECK(TestCapacityAndUnderrun() == 0);
    CHECK(TestWrapAndRecovery() == 0);
    CHECK(TestOneHour() == 0);
    CHECK(TestPlaybackUnderrunRecovery() == 0);

    CloseAudioDevice();
    printf("audio_stream_ring_test: PASS\n");
    return 0;
}
