/******************************************************************************
 * Includes for Audio + Compass Integration
 ******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <unistd.h>

#include "portaudio.h"
#include "pa_ringbuffer.h"
#include "pa_util.h"


/******************************************************************************
 * Macro / Constants
 ******************************************************************************/
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

typedef float SAMPLE;  // Audio sample type

#define NUM_CHANNELS       (2)      // 2 audio channels
#define SAMPLE_RATE        (48000)  // Audio sampling rate
#define FRAMES_PER_BUFFER  (512)    // Audio frames per callback
#define PA_SAMPLE_TYPE     paFloat32
#define SAMPLE_SIZE        (sizeof(float))
#define SAMPLE_SILENCE     (0.0f)
#define FILE_NAME          "some_audio_tests.raw"
#define GAIN_FACTOR        (2.0f)   // Demonstration output gain

// Noise Cancel Defaults
#define DEFAULT_FILTER_LENGTH 128
#define DEFAULT_MU            0.05f
#define EPSILON               1e-6f

// We'll skip the first 1 second of stats to avoid startup pops
#define STARTUP_SKIP_FRAMES  (SAMPLE_RATE)


/******************************************************************************
 * Global for CTRL+C
 ******************************************************************************/
volatile sig_atomic_t keepRunning = 1;

/******************************************************************************
 * NextPowerOf2 & Signal Handler
 ******************************************************************************/
static unsigned NextPowerOf2(unsigned val)
{
    val--;
    val = (val >> 1) | val;
    val = (val >> 2) | val;
    val = (val >> 4) | val;
    val = (val >> 8) | val;
    val = (val >> 16) | val;
    return ++val;
}

void sigint_handler(int sig)
{
    (void)sig;
    keepRunning = 0;
}


/******************************************************************************
 * Data Structure for Audio
 ******************************************************************************/
typedef struct
{
    // Ring buffer for passing processed audio from callback to main thread
    PaUtilRingBuffer ringBuffer;
    SAMPLE *sampleData;    // Memory block for ringBuffer
    FILE   *file;          // File pointer for raw audio output

    // Noise cancel flags & buffers
    int    doNoiseCancel;  // 1 => NLMS
    float  mu;
    int    filterLength;

    float *filterCoeffs;   // [filterLength]
    float *refHistory;     // [filterLength]
    int    refIndex;

    // Mono replication
    int    monoChannel;    // -1 => none, else 0 or 1

} AudioData;


/******************************************************************************
 * sampleSilence
 ******************************************************************************/
static SAMPLE sampleSilence[FRAMES_PER_BUFFER * NUM_CHANNELS] = { 0.0f };


/******************************************************************************
 * streamCallback for Audio
 ******************************************************************************/
static int streamCallback(const void *inputBuffer,
                          void       *outputBuffer,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void *userData)
{
    AudioData *data = (AudioData *)userData;
    const SAMPLE *capturedData = (const SAMPLE *)inputBuffer;
    SAMPLE *playbackData = (SAMPLE *)outputBuffer;

    if (!keepRunning) return paComplete;
    if (capturedData == NULL)
        capturedData = sampleSilence;

    // 1) Mono replication?
    if (data->monoChannel >= 0)
    {
        static SAMPLE monoBlock[FRAMES_PER_BUFFER * NUM_CHANNELS];
        for (unsigned n = 0; n < framesPerBuffer; n++)
        {
            SAMPLE sel = capturedData[2*n + data->monoChannel];
            monoBlock[2*n + 0] = sel;
            monoBlock[2*n + 1] = sel;
        }
        if (data->sampleData)
        {
            ring_buffer_size_t writable = PaUtil_GetRingBufferWriteAvailable(&data->ringBuffer);
            ring_buffer_size_t toWrite  = min(writable, (ring_buffer_size_t)(framesPerBuffer * NUM_CHANNELS));
            PaUtil_WriteRingBuffer(&data->ringBuffer, monoBlock, toWrite);
        }
        for (unsigned i = 0; i < framesPerBuffer * NUM_CHANNELS; i++)
        {
            playbackData[i] = GAIN_FACTOR * monoBlock[i];
        }
    }
    // 2) Normal pass-through if doNoiseCancel=0
    else if (!data->doNoiseCancel)
    {
        if (data->sampleData)
        {
            ring_buffer_size_t writable = PaUtil_GetRingBufferWriteAvailable(&data->ringBuffer);
            ring_buffer_size_t toWrite  = min(writable, (ring_buffer_size_t)(framesPerBuffer * NUM_CHANNELS));
            PaUtil_WriteRingBuffer(&data->ringBuffer, capturedData, toWrite);
        }
        for (unsigned i = 0; i < framesPerBuffer * NUM_CHANNELS; i++)
        {
            playbackData[i] = GAIN_FACTOR * capturedData[i];
        }
    }
    // 3) Noise cancel mode
    else
    {
        static SAMPLE processedBlock[FRAMES_PER_BUFFER * NUM_CHANNELS];

        float *W          = data->filterCoeffs;
        float *refHistory = data->refHistory;
        int    L          = data->filterLength;
        float  mu         = data->mu;
        int    idx        = data->refIndex;

        for (unsigned n = 0; n < framesPerBuffer; n++)
        {
            float d = capturedData[2*n + 0]; // primary
            float x = capturedData[2*n + 1]; // reference

            refHistory[idx] = x;

            float y = 0.f;
            int histPos = idx;
            for (int k = 0; k < L; k++)
            {
                y += W[k] * refHistory[histPos];
                histPos = (histPos - 1 + L) % L;
            }

            float e = d - y;

            float power = 0.f;
            histPos = idx;
            for (int k = 0; k < L; k++)
            {
                float val = refHistory[histPos];
                power += val * val;
                histPos = (histPos - 1 + L) % L;
            }
            power += EPSILON;

            float normFactor = (mu * e) / power;
            histPos = idx;
            for (int k = 0; k < L; k++)
            {
                W[k] += normFactor * refHistory[histPos];
                histPos = (histPos - 1 + L) % L;
            }

            processedBlock[2*n + 0] = e;
            processedBlock[2*n + 1] = x;

            idx = (idx + 1) % L;
        }
        data->refIndex = idx;

        if (data->sampleData)
        {
            ring_buffer_size_t writable = PaUtil_GetRingBufferWriteAvailable(&data->ringBuffer);
            ring_buffer_size_t toWrite  = min(writable, (ring_buffer_size_t)(framesPerBuffer * NUM_CHANNELS));
            PaUtil_WriteRingBuffer(&data->ringBuffer, processedBlock, toWrite);
        }
        for (unsigned i = 0; i < framesPerBuffer * NUM_CHANNELS; i++)
        {
            playbackData[i] = GAIN_FACTOR * processedBlock[i];
        }
    }

    (void)timeInfo;
    (void)statusFlags;
    return paContinue;
}


/******************************************************************************
 * Compass / Pitch Reading
 * 
 * If --pitch is given, we open /dev/ttyUSB0 at 19200 8N1, read lines that start
 * with "$C...", parse "P<pitch>". We store pitch in a global or static
 * variable so that the bar code can use it.
 ******************************************************************************/
static int fdCompass = -1;       // file descriptor for /dev/ttyUSB0
static double pitchValue = 0.0;  // updated from compass
static double initialPitch = 0.0;
static int    pitchSet     = 0;  // 0 = not set, 1 = set

#define COMPASS_DEVICE  "/dev/ttyUSB0"
#define COMPASS_BAUD    B19200

static int configurePort(int fd)
{
    struct termios options;
    if (tcgetattr(fd, &options) < 0)
    {
        perror("tcgetattr");
        return -1;
    }

    cfsetispeed(&options, COMPASS_BAUD);
    cfsetospeed(&options, COMPASS_BAUD);

    // 8N1
    options.c_cflag &= ~PARENB; // no parity
    options.c_cflag &= ~CSTOPB; // 1 stop bit
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;     // 8 data bits
    options.c_cflag |= CREAD;   // enable receiver
    options.c_cflag |= CLOCAL;  // ignore modem controls

    // raw input
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_oflag &= ~OPOST;
    options.c_iflag &= ~(IXON | IXOFF | IXANY);

    if (tcsetattr(fd, TCSANOW, &options) < 0)
    {
        perror("tcsetattr");
        return -1;
    }
    return 0;
}

// We'll do a small buffer to accumulate lines
static char pitchBuf[256];
static int  pitchIndex = 0;

static void readCompassPitch(void)
{
    if (fdCompass < 0) return; // not open

    // Attempt to read all available bytes
    while (1)
    {
        char c;
        int n = read(fdCompass, &c, 1);
        if (n > 0)
        {
            if ((c == '\n') || (c == '\r') || (pitchIndex >= (int)(sizeof(pitchBuf)-1)))
            {
                // End of line => parse
                pitchBuf[pitchIndex] = '\0';
                pitchIndex = 0;

                // If it starts with $C, parse it
                if (pitchBuf[0] == '$' && pitchBuf[1] == 'C')
                {
                    double heading, pval, roll, temp;
                    unsigned int cs; // checksum
                    // Example: $C281.4P-9.7R-140.1T30.0*23
                    int parsed = sscanf(pitchBuf, "$C%lfP%lfR%lfT%lf*%x",
                                        &heading, &pval, &roll, &temp, &cs);
                    if (parsed == 5)
                    {
                        pitchValue = pval; // store global
                        // if not set yet, define initialPitch
                        if (!pitchSet)
                        {
                            initialPitch = pitchValue;
                            pitchSet = 1;
                        }
                    }
                }
                memset(pitchBuf, 0, sizeof(pitchBuf));
            }
            else
            {
                // accumulate
                pitchBuf[pitchIndex++] = c;
            }
        }
        else
        {
            // either no more data or error
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                perror("read compass");
            }
            break;
        }
    }
}


/******************************************************************************
 * moveCursorUp() - to avoid scrolling
 ******************************************************************************/
static void moveCursorUp(int lines)
{
    if (lines > 0)
        printf("\033[%dA", lines);
}


/******************************************************************************
 * 
 * Microphone Characteristic Storage (for downward pitch only)
 *
 * We'll store amplitude dB & power dB for angles up to 45° downward from
 * initialPitch. That is, if difference = (pitchValue - initialPitch) is negative
 * and >= -45, we accumulate the *average dB* for that 0.1 s block.
 *
 ******************************************************************************/
static double accumAmpDB[46];   // sums of amplitude dB
static double accumPowDB[46];   // sums of power dB
static unsigned accumCount[46]; // how many times we've added for each angle

/******************************************************************************
 * Main
 ******************************************************************************/
int main(int argc, char *argv[])
{
    // parse arguments
    int doNoiseCancel = 0;
    float mu          = DEFAULT_MU;
    int filterLength  = DEFAULT_FILTER_LENGTH;
    int monoChannel   = -1;
    int doBar         = 0;
    int doPitch       = 0;  // new flag

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--noise") == 0)
        {
            doNoiseCancel = 1;
        }
        else if (strncmp(argv[i], "--mu=", 5) == 0)
        {
            mu = (float)atof(argv[i] + 5);
        }
        else if (strncmp(argv[i], "--filterlen=", 12) == 0)
        {
            filterLength = atoi(argv[i] + 12);
        }
        else if (strncmp(argv[i], "--mono=", 7) == 0)
        {
            int ch = atoi(argv[i] + 7);
            if (ch < 0 || ch >= NUM_CHANNELS)
            {
                fprintf(stderr, "Error: --mono channel must be 0 or 1.\n");
                return 1;
            }
            monoChannel = ch;
        }
        else if (strcmp(argv[i], "--bar") == 0)
        {
            doBar = 1;
        }
        else if (strcmp(argv[i], "--pitch") == 0)
        {
            doPitch = 1;
        }
        else
        {
            fprintf(stderr, "Unrecognized parameter: %s\n", argv[i]);
            return 1;
        }
    }

    // check incompatibilities
    if (monoChannel >= 0 && doNoiseCancel)
    {
        fprintf(stderr, "Error: --mono and --noise are incompatible.\n");
        return 1;
    }

    // set up signals
    signal(SIGINT, sigint_handler);

    // pitch setup if doPitch
    if (doPitch)
    {
        fdCompass = open(COMPASS_DEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fdCompass < 0)
        {
            perror("open compass");
            fprintf(stderr, "Cannot open %s\n", COMPASS_DEVICE);
            // not fatal if you want to run anyway, but let's just exit
            return 1;
        }
        // configure
        if (configurePort(fdCompass) < 0)
        {
            close(fdCompass);
            fdCompass = -1;
            return 1;
        }
    }

    // Print info
    if (monoChannel >= 0)
        printf("Mono replication: channel %d => both.\n", monoChannel);
    else
        printf("Noise cancellation: %s\n", (doNoiseCancel ? "ENABLED" : "DISABLED"));
    if (doBar)   printf("ASCII bar display: ENABLED (no scroll)\n");
    if (doPitch) printf("Pitch reading from %s (downward mic characteristic)\n", COMPASS_DEVICE);

    // set up ring buffer & portaudio
    PaStreamParameters inputParameters, outputParameters;
    PaStream *stream = NULL;
    PaError err;

    AudioData audioData;

    unsigned long numSamples = NextPowerOf2((unsigned long)(SAMPLE_RATE * 0.5f * NUM_CHANNELS));
    unsigned long numBytes   = numSamples * sizeof(SAMPLE);

    audioData.sampleData = (SAMPLE *)malloc(numBytes);
    if (!audioData.sampleData)
    {
        perror("malloc ring buffer");
        return 1;
    }

    SAMPLE *file_buffer = (SAMPLE *)malloc(numBytes);
    if (!file_buffer)
    {
        perror("malloc file_buffer");
        free(audioData.sampleData);
        return 1;
    }

    if (PaUtil_InitializeRingBuffer(&audioData.ringBuffer,
                                    sizeof(SAMPLE),
                                    numSamples,
                                    audioData.sampleData) < 0)
    {
        fprintf(stderr, "Failed to init ring buffer\n");
        free(audioData.sampleData);
        free(file_buffer);
        return 1;
    }

    err = Pa_Initialize();
    if (err != paNoError)
    {
        fprintf(stderr, "Pa_Initialize error\n");
        goto error2;
    }

    inputParameters.device = Pa_GetDefaultInputDevice();
    inputParameters.channelCount              = NUM_CHANNELS;
    inputParameters.sampleFormat              = PA_SAMPLE_TYPE;
    inputParameters.suggestedLatency          = Pa_GetDeviceInfo(inputParameters.device)->defaultHighInputLatency;
    inputParameters.hostApiSpecificStreamInfo = NULL;

    // Output device index, e.g. 2
    outputParameters.device = 2;
    outputParameters.channelCount              = NUM_CHANNELS;
    outputParameters.sampleFormat              = PA_SAMPLE_TYPE;
    outputParameters.suggestedLatency          = Pa_GetDeviceInfo(outputParameters.device)->defaultHighOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;

    err = Pa_OpenStream(&stream,
                        &inputParameters,
                        &outputParameters,
                        SAMPLE_RATE,
                        FRAMES_PER_BUFFER,
                        paClipOff,
                        streamCallback,
                        &audioData);
    if (err != paNoError)
        goto error2;

    audioData.file = fopen(FILE_NAME, "wb");
    if (!audioData.file)
        goto error2;

    // init noise/mono
    audioData.doNoiseCancel = doNoiseCancel;
    audioData.mu            = mu;
    audioData.filterLength  = filterLength;
    audioData.refIndex      = 0;
    audioData.monoChannel   = monoChannel;

    if (doNoiseCancel)
    {
        audioData.filterCoeffs = (float *)calloc(filterLength, sizeof(float));
        audioData.refHistory   = (float *)calloc(filterLength, sizeof(float));
        if (!audioData.filterCoeffs || !audioData.refHistory)
        {
            fprintf(stderr, "NoiseCancel mem fail\n");
            goto error2;
        }
    }
    else
    {
        audioData.filterCoeffs = NULL;
        audioData.refHistory   = NULL;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError)
        goto error1;

    printf("\n=== Started stream... Press CTRL+C to stop. ===\n");

    /***************************************************************************
     * MAIN LOOP:
     *  - read from ring buffer
     *  - write to file
     *  - optionally read pitch from compass
     *  - optionally display bars
     *  - optionally accumulate mic characteristic for downward angles
     ***************************************************************************/

    // skip stats for first 1 second
    unsigned long skipStatsRemaining = STARTUP_SKIP_FRAMES;

    // doBar -> how many channels?
    int numBarChannels = (audioData.doNoiseCancel || audioData.monoChannel >= 0) ? 1 : 2;

    // We have 4 lines per channel normally (Amplitude, Amp dB, Power, Power dB).
    // If doPitch=1, we add 1 more line => 5 lines. Then we also print
    // 1 final blank line, and if numBarChannels=2 we print "Channel x:" lines.
    // We'll do a small fix by counting them exactly:

    // Base lines for amplitude/power: 4
    int linesBase = 4;
    // if pitch => +1
    if (doPitch) linesBase += 1;
    // plus final blank line => +1
    linesBase += 1;
    // if 2 channels => we also have a "Channel X:" line => +1 for that
    // but we do that once per channel
    // => linesBase += 1;
    // We'll do it inside the loop below.

    // We'll just do it simpler: we'll handle "Channel X:" as part of the lines
    // so for each channel if (numBarChannels==2) => +1 line for heading
    // That means total lines = (linesBase + maybeHeading) * numBarChannels
    // We'll just define a function to count them each iteration. For minimal fix, do:

    // We'll define:
    int headingLines = (numBarChannels == 2) ? 1 : 0;
    int linesPerChannel = linesBase + headingLines; // if 2 channels => 4 + 1 + 1 + 1=7, if doPitch => 8
    int totalLines      = linesPerChannel * numBarChannels;

    // stats accum
    float sumAbs[2]        = {0.f, 0.f};
    float sumSq[2]         = {0.f, 0.f};
    unsigned long count[2] = {0, 0};
    float maxAvgAbsSoFar[2] = {1e-9f, 1e-9f};
    float maxAvgSqSoFar[2]  = {1e-9f, 1e-9f};

    unsigned long framesAccumulated = 0;
    const unsigned long FRAMES_PER_UPDATE = (unsigned long)(0.1f * SAMPLE_RATE); // ~4800 => 100ms

    int firstBarDraw = 1;

    #define BAR_WIDTH 40

    // Zero out accum arrays for mic characteristic
    memset(accumAmpDB, 0, sizeof(accumAmpDB));
    memset(accumPowDB, 0, sizeof(accumPowDB));
    memset(accumCount, 0, sizeof(accumCount));

    while (keepRunning)
    {
        // 1) If doPitch, read from compass
        if (doPitch)
        {
            readCompassPitch();
        }

        // 2) Check ring buffer
        size_t itemsToRead = PaUtil_GetRingBufferReadAvailable(&audioData.ringBuffer);
        if (itemsToRead > 0)
        {
            ring_buffer_size_t itemsRead =
                PaUtil_ReadRingBuffer(&audioData.ringBuffer, file_buffer, itemsToRead);

            // write raw data to file
            fwrite(file_buffer, SAMPLE_SIZE, itemsRead, audioData.file);

            if (!doBar)
            {
                // no bar => just sleep
                Pa_Sleep(80);
                continue;
            }

            unsigned long chunkFrames = (unsigned long)(itemsRead / NUM_CHANNELS);

            // skip stats if needed
            if (skipStatsRemaining > 0)
            {
                if (skipStatsRemaining >= chunkFrames)
                {
                    skipStatsRemaining -= chunkFrames;
                    Pa_Sleep(80);
                    continue;
                }
                else
                {
                    // partial skip
                    unsigned long partial = skipStatsRemaining;
                    skipStatsRemaining = 0;
                    unsigned long usedFrames = chunkFrames - partial;
                    memmove(file_buffer, file_buffer + (partial*NUM_CHANNELS),
                            (usedFrames*NUM_CHANNELS)*sizeof(SAMPLE));
                    chunkFrames = usedFrames;
                }
            }

            // accumulate stats
            for (unsigned long f = 0; f < chunkFrames; f++)
            {
                float s0 = file_buffer[2*f + 0];
                float s1 = file_buffer[2*f + 1];

                if (numBarChannels == 1)
                {
                    sumAbs[0] += fabsf(s0);
                    sumSq[0]  += (s0*s0);
                    count[0]  += 1;
                }
                else
                {
                    sumAbs[0] += fabsf(s0);
                    sumSq[0]  += (s0*s0);
                    count[0]  += 1;

                    sumAbs[1] += fabsf(s1);
                    sumSq[1]  += (s1*s1);
                    count[1]  += 1;
                }
            }

            framesAccumulated += chunkFrames;

            // every 100ms => print bars
            if (framesAccumulated >= FRAMES_PER_UPDATE)
            {
                // move cursor up if not first
                if (!firstBarDraw)
                {
                    moveCursorUp(totalLines);
                }
                else
                {
                    firstBarDraw = 0;
                }

                for (int ch = 0; ch < numBarChannels; ch++)
                {
                    if (numBarChannels == 2)
                    {
                        printf("Channel %d:\n", ch);  // heading line
                    }

                    if (count[ch] == 0)
                    {
                        // blank lines
                        printf("Amplitude           \n");
                        printf("Amplitude (dB)      \n");
                        printf("Power               \n");
                        printf("Power (dB)          \n");
                        if (doPitch) printf("Pitch angle         \n");
                        printf("\n"); // final blank line
                        continue;
                    }

                    float avgAmp = sumAbs[ch] / (float)count[ch];
                    float avgPow = sumSq[ch]  / (float)count[ch];

                    if (avgAmp > maxAvgAbsSoFar[ch]) maxAvgAbsSoFar[ch] = avgAmp;
                    if (avgPow > maxAvgSqSoFar[ch])  maxAvgSqSoFar[ch]  = avgPow;

                    float ratioAmp = avgAmp / maxAvgAbsSoFar[ch];
                    float ratioPow = avgPow / maxAvgSqSoFar[ch];

                    float ampDB = 20.f * log10f(ratioAmp + 1e-12f);
                    if (ampDB < -40.f) ampDB = -40.f;
                    float powDB = 10.f * log10f(ratioPow + 1e-12f);
                    if (powDB < -40.f) powDB = -40.f;

                    int barAmp    = (int)(ratioAmp * BAR_WIDTH + 0.5f);
                    int barAmpdB  = (int)(((ampDB + 40.f)/40.f)*(float)BAR_WIDTH + 0.5f);
                    int barPow    = (int)(ratioPow * BAR_WIDTH + 0.5f);
                    int barPowdB  = (int)(((powDB + 40.f)/40.f)*(float)BAR_WIDTH + 0.5f);

                    #define CLAMP_B(v) if(v<0) v=0; else if(v>BAR_WIDTH)v=BAR_WIDTH
                    CLAMP_B(barAmp);
                    CLAMP_B(barAmpdB);
                    CLAMP_B(barPow);
                    CLAMP_B(barPowdB);

                    char barStrAmp[BAR_WIDTH+1];   memset(barStrAmp,   '*', barAmp);   barStrAmp[barAmp]   = '\0';
                    char barStrAmpdB[BAR_WIDTH+1]; memset(barStrAmpdB, '*', barAmpdB); barStrAmpdB[barAmpdB] = '\0';
                    char barStrPow[BAR_WIDTH+1];   memset(barStrPow,   '*', barPow);   barStrPow[barPow]   = '\0';
                    char barStrPowdB[BAR_WIDTH+1]; memset(barStrPowdB, '*', barPowdB); barStrPowdB[barPowdB] = '\0';

                    printf("Amplitude           %-*.*s (%.2f)\n",
                           BAR_WIDTH, BAR_WIDTH, barStrAmp,   ratioAmp);
                    printf("Amplitude (dB)      %-*.*s (%.1f dB)\n",
                           BAR_WIDTH, BAR_WIDTH, barStrAmpdB, ampDB);
                    printf("Power               %-*.*s (%.2f)\n",
                           BAR_WIDTH, BAR_WIDTH, barStrPow,   ratioPow);
                    printf("Power (dB)          %-*.*s (%.1f dB)\n",
                           BAR_WIDTH, BAR_WIDTH, barStrPowdB, powDB);

                    // If doPitch => 5th bar for pitch
                    if (doPitch)
                    {
                        double pval = pitchValue;
                        if (!pitchSet) pval = 0.0; // if never set, skip
                        double diff = pval - initialPitch; 
                        // pitch difference: negative => downward
                        // We'll do bar from -90..0 => that is 0..90 in ratio. 
                        // But we only show a bar if it's in [-90..+90].
                        // Then map to [0..1].
                        double ratioP = 0.0;
                        if (pval < -90.0) pval = -90.0;
                        if (pval >  90.0) pval =  90.0;

                        // Build pitch bar
                        ratioP = (pval + 90.0)/180.0; // => [0..1]
                        int barPitch = (int)(ratioP * BAR_WIDTH + 0.5f);
                        if (barPitch<0) barPitch=0; else if(barPitch>BAR_WIDTH) barPitch=BAR_WIDTH;

                        char barStrPitch[BAR_WIDTH+1];
                        memset(barStrPitch, '*', barPitch);
                        barStrPitch[barPitch] = '\0';

                        printf("Pitch angle         %-*.*s (%.2f deg)\n",
                               BAR_WIDTH, BAR_WIDTH, barStrPitch, pitchValue);

                        // If downward: difference < 0, up to -45
                        if (diff < 0 && diff >= -45.0)
                        {
                            // round to integer
                            int angleIndex = -(int)round(diff); // e.g. diff=-10.2 => angleIndex=10
                            if (angleIndex >= 0 && angleIndex <= 45)
                            {
                                // Accumulate amplitude/power dB
                                accumAmpDB[angleIndex] += ampDB;
                                accumPowDB[angleIndex] += powDB;
                                accumCount[angleIndex] += 1;
                            }
                        }
                    }

                    printf("\n");  // final blank line

                    // reset accum for next block
                    sumAbs[ch] = 0.f;
                    sumSq[ch]  = 0.f;
                    count[ch]  = 0;
                }
                framesAccumulated = 0;
            }
        }

        // small sleep
        Pa_Sleep(80);
    }

    // Stop stream
    err = Pa_StopStream(stream);
    if (err != paNoError) goto error1;
    printf("Stopped stream.\n");

    // If doPitch => write mic_characteristics.tsv
    // We do angle from 0..45 => angle means how many degrees downward from the initial pitch
    // accumCount[i], accumAmpDB[i], accumPowDB[i]
    if (doPitch)
    {
        FILE *fchar = fopen("mic_characteristics.tsv", "w");
        if (fchar)
        {
            for (int i = 0; i <= 45; i++)
            {
                if (accumCount[i] > 0)
                {
                    double meanAmp = accumAmpDB[i]/accumCount[i];
                    double meanPow = accumPowDB[i]/accumCount[i];
                    // i is the angle in degrees
                    // no header, just tab separated
                    // angle, amplitude dB, power dB
                    fprintf(fchar, "%d\t%.2f\t%.2f\n", i, meanAmp, meanPow);
                }
            }
            fclose(fchar);
            printf("Wrote mic_characteristics.tsv\n");
        }
        else
        {
            perror("fopen mic_characteristics.tsv");
        }
    }

    // Cleanup
    if (audioData.sampleData) free(audioData.sampleData);
    if (file_buffer)          free(file_buffer);
    if (audioData.file)       fclose(audioData.file);
    if (audioData.filterCoeffs) free(audioData.filterCoeffs);
    if (audioData.refHistory)   free(audioData.refHistory);

    if (fdCompass >= 0) close(fdCompass);

    Pa_Terminate();
    printf("Terminated.\n");
    return 0;

error1:
    if (audioData.sampleData) free(audioData.sampleData);
    if (file_buffer) free(file_buffer);
    Pa_Terminate();
    if (audioData.file) fclose(audioData.file);
    if (audioData.filterCoeffs) free(audioData.filterCoeffs);
    if (audioData.refHistory)   free(audioData.refHistory);
    if (fdCompass >= 0) close(fdCompass);
    return -3;

error2:
    if (stream)
    {
        Pa_AbortStream(stream);
        Pa_CloseStream(stream);
    }
    Pa_Terminate();
    if (audioData.file) fclose(audioData.file);
    if (file_buffer) free(file_buffer);
    if (audioData.sampleData) free(audioData.sampleData);
    if (audioData.filterCoeffs) free(audioData.filterCoeffs);
    if (audioData.refHistory)   free(audioData.refHistory);
    if (fdCompass >= 0) close(fdCompass);
    fprintf(stderr, "An error occurred while using the PortAudio stream.\n");
    return -1;
}
