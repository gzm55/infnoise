/* Driver for the Infinite Noise Multiplier USB stick */

// Required to include clock_gettime
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ftdi.h>
#include "infnoise.h"
#include "KeccakF-1600-interface.h"

// Extract the INM output from the data received.  Basically, either COMP1 or COMP2
// changes, not both, so alternate reading bits from them.  We get 1 INM bit of output
// per byte read.  Feed bits from the INM to the health checker.  Return the expected
// bits of entropy.
static uint32_t extractBytes(uint8_t *bytes, uint8_t *inBuf) {
    inmClearEntropyLevel();
    //printf("New batch\n");
    uint32_t i;
    for(i = 0u; i < BUFLEN/8u; i++) {
        uint32_t j;
        uint8_t byte = 0u;
        for(j = 0u; j < 8u; j++) {
            //printf("%x ", inBuf[i*8u + j] & ~MASK);
            uint8_t val = inBuf[i*8u + j];
            uint8_t evenBit = (val >> COMP2) & 1u;
            uint8_t oddBit = (val >> COMP1) & 1u;
            bool even = j & 1u; // Use the even bit if j is odd
            uint8_t bit = even? evenBit : oddBit;
            byte = (byte << 1u) | bit;
            // This is a good place to feed the bit from the INM to the health checker.
            //printf("Address: %u, adding evenBit:%u oddBit:%u even:%u\n", addr, evenBit, oddBit, even);
            if(!inmHealthCheckAddBit(evenBit, oddBit, even)) {
                fputs("Health check of Infinite Noise Multiplier failed!\n", stderr);
                exit(1);
            }
        }
        //printf("extracted byte:%x\n", byte);
        bytes[i] = byte;
    }
    return inmGetEntropyLevel();
}

// Write the bytes to either stdout, or /dev/random.
static void outputBytes(uint8_t *bytes, uint32_t length, uint32_t entropy, bool writeDevRandom) {
    if(!writeDevRandom) {
        if(fwrite(bytes, 1, length, stdout) != length) {
            fputs("Unable to write output from Infinite Noise Multiplier\n", stderr);
            exit(1);
        }
    } else {
        inmWaitForPoolToHaveRoom();
        inmWriteEntropyToPool(bytes, length, entropy);
    }
}

// Whiten the output, if requested, with a Keccak sponge.  Output bytes only if the health
// checker says it's OK.  Using outputMultiplier > 1 is a nice way to generate a lot more
// cryptographically secure pseudo-random data than the INM generates.  If
// outputMultiplier is 0, we output only as many bits as we measure in entropy.
// This allows a user to generate hundreds of MiB per second if needed, for use
// as cryptogrpahic keys.
static uint32_t processBytes(uint8_t *keccakState, uint8_t *bytes, uint32_t entropy, bool raw,
        bool writeDevRandom, uint32_t outputMultiplier) {
    //Use the lower of the measured entropy and the provable lower bound on
    //average entropy.
    if(entropy > inmExpectedEntropyPerBit*BUFLEN/INM_ACCURACY) {
        entropy = inmExpectedEntropyPerBit*BUFLEN/INM_ACCURACY;
    }
    if(raw) {
        // In raw mode, we just output raw data from the INM.
        outputBytes(bytes, BUFLEN/8u, entropy, writeDevRandom);
        return BUFLEN/8u;
    }
    // Note that BUFLEN has to be less than 1600 by enough to make the sponge secure,
    // since outputing all 1600 bits would tell an attacker the Keccak state, allowing
    // him to predict any further output, when outputMultiplier > 1, until the next call
    // to processBytes.  All 512 bits are absorbed before sqeezing data out to insure that
    // we instantly recover (reseed) from a state compromise, which is when an attacker
    // gets a snapshot of the keccak state.  BUFLEN must be a multiple of 64, since
    // Keccak-1600 uses 64-bit "lanes".
    KeccakAbsorb(keccakState, bytes, BUFLEN/64u);
    uint8_t dataOut[16u*8u];
    if(outputMultiplier == 0u) {
        // Output all the bytes of entropy we have
        KeccakExtract(keccakState, dataOut, (entropy + 63u)/64u);
        outputBytes(dataOut, entropy/8u, entropy & 0x7u, writeDevRandom);
        return entropy/8u;
    }
    // Output 256*outputMultipler bytes.
    uint32_t numBits = outputMultiplier*256u;
    uint32_t bytesWritten = 0u;
    while(numBits > 0u) {
        // Write up to 1024 bits at a time.
        uint32_t bytesToWrite = 1024u/8u;
        if(bytesToWrite > numBits/8u) {
            bytesToWrite = numBits/8u;
        }
        KeccakExtract(keccakState, dataOut, bytesToWrite/8u);
        uint32_t entropyThisTime = entropy;
        if(entropyThisTime > 8u*bytesToWrite) {
            entropyThisTime = 8u*bytesToWrite;
        }
        outputBytes(dataOut, bytesToWrite, entropyThisTime, writeDevRandom);
        bytesWritten += bytesToWrite;
        numBits -= bytesToWrite*8u;
        entropy -= entropyThisTime;
        if(numBits > 0u) {
            KeccakPermutation(keccakState);
        }
    }
    if(bytesWritten != outputMultiplier*(256u/8u)) {
        fprintf(stderr, "Internal error outputing bytes\n");
        exit(1);
    }
    return bytesWritten;
}

// Initialize the Infinite Noise Multiplier USB ineterface.
static bool initializeUSB(struct ftdi_context *ftdic, char **message) {
    *message = NULL;

    // Initialize FTDI context
    ftdi_init(ftdic);
    // Open FTDI device based on FT240X vendor & product IDs
    if(ftdi_usb_open(ftdic, 0x0403, 0x6015) < 0) {
        if(!isSuperUser()) {
            *message = "Can't find Infinite Noise Multiplier.  Try running as super user?\n";
        } else {
            *message = "Can't find Infinite Noise Multiplier\n";
        }
        return false;
    }

    // Set high baud rate
    int rc = ftdi_set_baudrate(ftdic, 30000);
    if(rc == -1) {
        *message = "Invalid baud rate\n";
        return false;
    } else if(rc == -2) {
        *message = "Setting baud rate failed\n";
        return false;
    } else if(rc == -3) {
        *message = "Infinite Noise Multiplier unavailable\n";
        return false;
    }

    // Enable syncrhonous bitbang mode
    rc = ftdi_set_bitmode(ftdic, MASK, BITMODE_SYNCBB);
    if(rc == -1) {
        *message = "Can't enable bit-bang mode\n";
        return false;
    } else if(rc == -2) {
        *message = "Infinite Noise Multiplier unavailable\n";
        return false;
    }
    
    // Just test to see that we can write and read.
    uint8_t buf[64u] = {0u,};
    if(ftdi_write_data(ftdic, buf, 64) != 64) {
        *message = "USB write failed\n";
        return false;
    }
    if(ftdi_read_data(ftdic, buf, 64) != 64) {
        *message = "USB read failed\n";
        return false;
    }
    return true;
}

// Return the differnece in the times as a double in microseconds.
static double diffTime(struct timespec *start, struct timespec *end) {
    uint32_t seconds = end->tv_sec - start->tv_sec;
    int32_t nanoseconds = end->tv_nsec - start->tv_nsec;
    return seconds*1.0e6 + nanoseconds/1000.0;
}

int main(int argc, char **argv)
{
    struct ftdi_context ftdic;
    bool raw = false;
    bool debug = false;
    bool writeDevRandom = false;
    bool noOutput = false;
    uint32_t outputMultiplier = 0u; // We output all the entropy when outputMultiplier == 0
    int xArg;
    bool multiplierAssigned = false;
    bool pidFile = false;
    char *pidFileName = NULL;
    bool runDaemon = false;

    // Process arguments
    for(xArg = 1; xArg < argc; xArg++) {
        if(!strcmp(argv[xArg], "--raw")) {
            raw = true;
        } else if(!strcmp(argv[xArg], "--debug")) {
            debug = true;
        } else if(!strcmp(argv[xArg], "--dev-random")) {
            writeDevRandom = true;
        } else if(!strcmp(argv[xArg], "--no-output")) {
            noOutput = true;
        } else if(!strcmp(argv[xArg], "--multiplier") && xArg+1 < argc) {
            xArg++;
            multiplierAssigned = true;
            int tmpOutputMult = atoi(argv[xArg]);
            if(tmpOutputMult < 0) {
                fputs("Multiplier must be >= 0\n", stderr);
                return 1;
            }
            outputMultiplier = tmpOutputMult;
        } else if(!strcmp(argv[xArg], "--pidfile")) {
            xArg++;
            pidFileName = argv[xArg];
            pidFile = true;
            if(pidFileName == NULL || !strcmp("",pidFileName)) {
                fputs("--pidfile without file name\n", stderr);
                return 1;
            }
        } else if(!strcmp(argv[xArg], "--daemon")) {
            runDaemon = true;
        } else {
            fputs("Usage: infnoise [options]\n"
                            "Options are:\n"
                            "    --debug - turn on some debug output\n"
                            "    --dev-random - write entropy to /dev/random instead of stdout\n"
                            "    --raw - do not whiten the output\n"
                            "    --multiplier <value> - write 256 bits * value for each 512 bits written to\n"
                            "      the Keccak sponge.  Default of 0 means write all the entropy.\n"
                            "    --no-output - do not write random output data\n"
                            "    --pidfile <file> - write process ID to file\n"
                            "    --daemon - run in the background\n", stderr);
            return 1;
        }
    }

    if(!multiplierAssigned && writeDevRandom) {
        outputMultiplier = 2u; // Don't throw away entropy when writing to /dev/random unless told to do so
    }

    // Optionally run in the background and optionally write a PID-file
    startDaemon(runDaemon, pidFile, pidFileName);

    if(writeDevRandom) {
        inmWriteEntropyStart(BUFLEN/8u, debug);
    }
    if(!inmHealthCheckStart(PREDICTION_BITS, DESIGN_K, debug)) {
        fputs("Can't intialize health checker\n", stderr);
        return 1;
    }
    KeccakInitialize();
    uint8_t keccakState[KeccakPermutationSizeInBytes];
    KeccakInitializeState(keccakState);

    char *message;
    if(!initializeUSB(&ftdic, &message)) {
        // Sometimes have to do it twice - not sure why
        if(!initializeUSB(&ftdic, &message)) {
            fputs(message, stderr);
            return 1;
        }
    }

    // Endless loop: set SW1EN and SW2EN alternately
    uint32_t i;
    uint8_t outBuf[BUFLEN], inBuf[BUFLEN];
    for(i = 0u; i < BUFLEN; i++) {
        // Alternate Ph1 and Ph2
        outBuf[i] = i & 1?  (1 << SWEN2) : (1 << SWEN1);
    }

    uint64_t totalBytesWritten = 0u;
    while(true) {
        struct timespec start;
        clock_gettime(CLOCK_REALTIME, &start);

        if(ftdi_write_data(&ftdic, outBuf, BUFLEN) != BUFLEN) {
            fputs("USB write failed\n", stderr);
            return 1;
        }
        if(ftdi_read_data(&ftdic, inBuf, BUFLEN) != BUFLEN) {
            fputs("USB read failed\n", stderr);
            return 1;
        }
        struct timespec end;
        clock_gettime(CLOCK_REALTIME, &end);
        uint32_t us = diffTime(&start, &end);
        //printf("diffTime:%u us\n", us);
        if(us <= MAX_MICROSEC_FOR_SAMPLES) {
            uint8_t bytes[BUFLEN/8u];
            uint32_t entropy = extractBytes(bytes, inBuf);
            if(!noOutput && inmHealthCheckOkToUseData() && inmEntropyOnTarget(entropy, BUFLEN)) {
                uint64_t prevTotalBytesWritten = totalBytesWritten;
                totalBytesWritten += processBytes(keccakState, bytes, entropy, raw, writeDevRandom, outputMultiplier);
                if(debug && (1u << 20u)*(totalBytesWritten/(1u << 20u)) > (1u << 20u)*(prevTotalBytesWritten/(1u << 20u))) {
                    fprintf(stderr, "Output %lu bytes\n", (unsigned long)totalBytesWritten);
                }
            }
        }
    }
    return 0;
}
