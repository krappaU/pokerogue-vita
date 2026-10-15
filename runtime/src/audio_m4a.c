// SPDX-License-Identifier: AGPL-3.0-only
// M4A (AAC in an MP4 container) decoding for src/audio.c: minimp4 finds the AAC frames, faad2
// decodes them. A separate file because minimp4 and minimp3 define the same internal names.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP4_IMPLEMENTATION
#include "minimp3/minimp4.h"
#include <neaacdec.h>

typedef struct { const uint8_t *d; size_t n; } membuf;
static int mp4_read(int64_t off, void *buf, size_t size, void *token) {
    membuf *m = token;
    if (off < 0 || (size_t)off + size > m->n) return 1;
    memcpy(buf, m->d + off, size);
    return 0;
}

// Decodes an M4A (AAC in MP4) file to interleaved 16-bit PCM (mono or stereo). Returns non-NULL on success.
void *audio_decode_m4a(const uint8_t *d, size_t n, int16_t **pcm_out, int *channels, int *rate_out, int64_t *frames) {
    if (n < 12 || memcmp(d + 4, "ftyp", 4)) return NULL;
    membuf m = {d, n};
    MP4D_demux_t mp4;
    if (!MP4D_open(&mp4, mp4_read, &m, (int64_t)n)) return NULL;
    void *ok = NULL;
    int tr = -1;
    for (unsigned i = 0; i < mp4.track_count; i++)
        if (mp4.track[i].handler_type == MP4D_HANDLER_TYPE_SOUN && mp4.track[i].object_type_indication == MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3) {
            tr = (int)i;
            break;
        }
    NeAACDecHandle h = NULL;
    int16_t *pcm = NULL;
    size_t cap = 0, len = 0; // in samples (channels included)
    unsigned long rate = 0;
    unsigned char ch = 0;
    int oc = 1;
    if (tr < 0 || !mp4.track[tr].dsi || !(h = NeAACDecOpen())) goto out;
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(h);
    cfg->outputFormat = FAAD_FMT_16BIT;
    cfg->downMatrix = 1;
    NeAACDecSetConfiguration(h, cfg);
    if (NeAACDecInit2(h, mp4.track[tr].dsi, mp4.track[tr].dsi_bytes, &rate, &ch) < 0 || !rate || ch < 1) goto out;
    oc = ch > 2 ? 2 : ch; // channels kept (downMatrix folds surround into stereo anyway)
    for (unsigned s = 0; s < mp4.track[tr].sample_count; s++) {
        unsigned fb = 0, ts = 0, dur = 0;
        MP4D_file_offset_t off = MP4D_frame_offset(&mp4, tr, s, &fb, &ts, &dur);
        if (!fb || off + fb > n) break;
        NeAACDecFrameInfo fi;
        int16_t *out = NeAACDecDecode(h, &fi, (unsigned char *)d + off, fb);
        if (fi.error || !out || !fi.samples) continue;
        if (!fi.channels) continue;
        size_t nf = fi.samples / fi.channels;
        if (len + nf * oc > cap) {
            cap = (len + nf * oc) * 2;
            int16_t *np = realloc(pcm, cap * 2);
            if (!np) goto out;
            pcm = np;
        }
        for (size_t f = 0; f < nf; f++)
            for (int c = 0; c < oc; c++) pcm[len + f * oc + c] = out[f * fi.channels + (c < fi.channels ? c : 0)];
        len += nf * oc;
    }
    if (!len) goto out;
    *pcm_out = pcm, pcm = NULL;
    *channels = oc, *rate_out = (int)rate, *frames = (int64_t)(len / oc);
    ok = *pcm_out;
out:
    free(pcm);
    if (h) NeAACDecClose(h);
    MP4D_close(&mp4);
    return ok;
}

