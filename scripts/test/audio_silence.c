/* Exercise the actual jitter-buffer predicate, without a device or game. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
#define CELL_AUDIO_BLOCK_SAMPLES 256
#include "audio_silence_helper.inc"

int main(void)
{
    float block[CELL_AUDIO_BLOCK_SAMPLES * 8] = {0};
    unsigned cases = 0;
    for (u32 channels = 2; channels <= 8; channels += 6) {
        const u32 samples = CELL_AUDIO_BLOCK_SAMPLES * channels;
        assert(!audio_block_has_data(block, channels));
        /* A single sample anywhere in any channel must keep the block alive.
         * This covers right/center/surround-only audio and short transients. */
        for (u32 i = 0; i < samples; ++i) {
            for (int sign = -1; sign <= 1; sign += 2) {
                block[i] = sign * 0.25f;
                if (!audio_block_has_data(block, channels)) {
                    fprintf(stderr, "Missed %u-channel frame %u channel %u sign %d\n",
                            channels, i / channels, i % channels, sign);
                    return 1;
                }
                assert(block[i] == sign * 0.25f);
                block[i] = 0;
                ++cases;
            }
        }
        assert(!audio_block_has_data(block, channels));
        /* Data beyond the requested channel count must not count. */
        if (channels == 2) {
            block[samples] = 0.25f;
            assert(!audio_block_has_data(block, channels));
            block[samples] = 0;
        }
    }
    printf("PASS: %u single-sample signals, silent blocks and channel bounds\n", cases);
    return 0;
}
