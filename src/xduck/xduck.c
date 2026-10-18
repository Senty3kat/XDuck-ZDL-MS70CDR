/*
 * xduck.c -- XDuck: cross-sidechain ducker / gate, MS-70CDR.
 *
 * The level of one channel drives the gain of the other. Plug the instrument
 * into IN L and the control signal (drum machine, kick, loop) into IN R.
 * 9 knobs (UI value = params[slot] * 100, see manifest_pedal.json); output is
 * always 100% processed, there is no Mix:
 *   Type    (params[5])  - 0 Duck, 1 Gate, 2 Tex (texture: target level
 *                          follows the key envelope below Thresh, dB for dB
 *                          at Ratio 100; both outputs = L + R mono sum)
 *   Route   (params[6])  - 0 X (R keys L, L keys R), 1 R>L (R keys L, R
 *                          passes), 2 R<L (L keys R, L passes), 3 KeyR (R keys
 *                          L, R output silent), 4 KeyL (L keys R, L silent)
 *   Thresh  (params[8])  - key threshold, 0..60 -> 0..-60 dB
 *   Range   (params[9])  - max gain reduction, 0..60 dB
 *   Ratio   (params[10]) - duck: 0..1 compression slope (100 = limiter);
 *                          gate: expander slope up to 1:21 (100 = hard gate)
 *   Attack  (params[11]) - 0.1 .. 102 ms (duck: ducking speed, gate: opening)
 *   Hold    (params[12]) - 0 .. 500 ms key-level hold (duck stays down, gate
 *                          stays open); also stops gate chatter
 *   Release (params[13]) - 5 ms .. 2.5 s (duck: recovery, gate: closing)
 *   Freq    (params[7])  - key band-pass: 0 Full (no filter), 1..13 = centre
 *                          40 Hz .. 8 kHz in x1.555 steps. Only the detector
 *                          hears the filter; the audio stays full-band.
 *
 * The key is always the chain signal (ctx[5]). XProbe on hardware showed the
 * raw input buffer ctx[4] carries IN L only (its R half is silent), so a
 * pre-chain R key does not exist; put XDuck first in the patch instead.
 *
 * Gain computer works in dB through bit-trick log2/exp2 (no math lib, no
 * divide, no tables). Per channel: peak detector with hold on the key ->
 * target gain reduction -> attack/release smoothing in dB -> linear gain.
 *
 * Safe-DSP: no switch (all mode logic is float compares / blends), no static
 * arrays, tiny state in the ctx[3] arena, ctx[11]/ctx[12] shuttle preserved.
 */

#include <stdint.h>

#include "../../airwindows/common/zoom_params.h"
#include "xduck_params.h"

#ifndef XDUCK_AUDIO_FUNC
#define XDUCK_AUDIO_FUNC Fx_DLY_XDuck
#endif

#define XD_DO_PRAGMA(x) _Pragma(#x)
#define XD_EXPAND_PRAGMA(x) XD_DO_PRAGMA(x)
#define XD_CODE_SECTION(f) XD_EXPAND_PRAGMA(CODE_SECTION(f, ".audio"))

XD_CODE_SECTION(XDUCK_AUDIO_FUNC)

#define ZDL_PTR(type, word) ((type)(uintptr_t)(word))

#define XD_MAGIC   0x58444B31u   /* 'XDK1' */
#define XD_VERSION 3u

#define XD_DB_PER_LOG2  6.02059991f   /* 20*log10(2) */
#define XD_LOG2_PER_DB  0.16609640f   /* 1 / 6.0206 */
#define XD_LOG2E        1.44269504f
#define XD_ENV_FLOOR    0.000001f     /* -120 dB */
#define XD_DET_DECAY    0.99773502f   /* key peak detector, ~10 ms release @ 44.1k */
#define XD_ATK_X0       0.22675737f   /* 1 / (0.1 ms * 44100) */
#define XD_ATK_OCT      10.0f         /* 0.1 ms * 2^10 = 102 ms */
#define XD_REL_X0       0.00453515f   /* 1 / (5 ms * 44100) */
#define XD_REL_OCT      9.0f          /* 5 ms * 2^9 = 2.56 s */
#define XD_GATE_SLOPE   20.0f
#define XD_HOLD_SAMPLES 220.5f        /* per UI step: 100 -> 22050 = 500 ms */
#define XD_FREQ_TOP     8000.0f       /* Freq step 13 */
#define XD_FREQ_STEP    0.63698556f   /* log2(8000/40) / 12: one step = x1.555 */
#define XD_PI_SR        (3.14159265f / 44100.0f)
#define XD_SVF_K        0.8f          /* 1/Q of the key band-pass (Q = 1.25) */

typedef struct XDuckState {
    uint32_t magic;
    uint32_t version;
    uint32_t initialized;
    uint32_t pad;
    float envL;     /* peak level of the L key (drives R) */
    float envR;     /* peak level of the R key (drives L) */
    float gdbL;     /* smoothed gain of L, dB (<= 0) */
    float gdbR;     /* smoothed gain of R, dB (<= 0) */
    float holdL;    /* samples left before envL may decay */
    float holdR;
    float s1L, s2L; /* key band-pass (TPT state-variable filter) state */
    float s1R, s2R;
} XDuckState;

typedef union { float f; uint32_t u; } XdBits;

XD_DO_PRAGMA(FUNC_ALWAYS_INLINE(xd_ui))
static inline float xd_ui(float raw, float def_ui)
{
    if (!(raw >= 0.0f && raw <= 100.0f)) return def_ui;
    if (raw <= 1.0f) return raw * 100.0f;
    return raw;
}

/* log2 for x > 0 (normal floats): exponent + 2nd-order mantissa fit. */
XD_DO_PRAGMA(FUNC_ALWAYS_INLINE(xd_log2))
static inline float xd_log2(float x)
{
    XdBits v;
    v.f = x;
    float e = (float)((int32_t)((v.u >> 23) & 255u) - 127);
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
    float m = v.f;
    return e + (-0.34484843f * m + 2.02466578f) * m - 1.67487759f;
}

/* 2^x for x <= 0, clamped at -60 (values we feed are never positive). */
XD_DO_PRAGMA(FUNC_ALWAYS_INLINE(xd_exp2))
static inline float xd_exp2(float x)
{
    if (x < -60.0f) x = -60.0f;
    if (x > 0.0f) x = 0.0f;
    int32_t i = (int32_t)x;               /* truncates toward zero */
    if ((float)i > x) i -= 1;             /* -> floor */
    float f = x - (float)i;               /* 0..1 */
    float p = 1.0f + f * (0.69606564f + f * (0.22449433f + f * 0.07944023f));
    XdBits v;
    v.u = ((uint32_t)(i + 127)) << 23;
    return v.f * p;
}

/* Target gain (dB, <= 0) for one channel from its key level. */
XD_DO_PRAGMA(FUNC_ALWAYS_INLINE(xd_target))
static inline float xd_target(float env, float thrDb, float range,
                              float slopeDuck, float slopeGate, float isGate)
{
    if (env < XD_ENV_FLOOR) env = XD_ENV_FLOOR;
    float keyDb = XD_DB_PER_LOG2 * xd_log2(env);
    float over = keyDb - thrDb;
    float red;
    if (isGate > 0.5f) red = (over < 0.0f) ? -over * slopeGate : 0.0f;
    else               red = (over > 0.0f) ?  over * slopeDuck : 0.0f;
    if (red > range) red = range;
    return -red;
}

void XDUCK_AUDIO_FUNC(unsigned int *ctx)
{
    float *params = ZDL_PTR(float *, ctx[1]);
    float *fxBuf  = ZDL_PTR(float *, ctx[5]);

    unsigned int *magicSrc = ZDL_PTR(unsigned int *, ctx[12]);
    unsigned int *magicDst = ZDL_PTR(unsigned int *, *(unsigned int *)ZDL_PTR(unsigned int *, ctx[11]));
    *magicDst = *magicSrc;

    if (params[0] < 0.5f) return;

    volatile unsigned int *desc = ZDL_PTR(volatile unsigned int *, ctx[3]);
    if (!desc) return;
    uintptr_t base = (uintptr_t)desc[0];
    uintptr_t end  = (uintptr_t)desc[1];
    if (base == 0u || end <= base) return;
    if ((base & 3u) != 0u) return;
    if (end - base < (uintptr_t)sizeof(XDuckState)) return;

    XDuckState *st = (XDuckState *)base;
    if (st->magic != XD_MAGIC || st->version != XD_VERSION || !st->initialized) {
        st->magic = XD_MAGIC;
        st->version = XD_VERSION;
        st->envL = 0.0f;
        st->envR = 0.0f;
        st->gdbL = 0.0f;
        st->gdbR = 0.0f;
        st->holdL = 0.0f;
        st->holdR = 0.0f;
        st->s1L = 0.0f; st->s2L = 0.0f;
        st->s1R = 0.0f; st->s2R = 0.0f;
        st->initialized = 1u;
    }

    float thrUi  = xd_ui(params[XDUCK_THRESH_SLOT],  (float)XDUCK_THRESH_UI_DEFAULT);
    float range  = xd_ui(params[XDUCK_RANGE_SLOT],   (float)XDUCK_RANGE_UI_DEFAULT);
    float ratio  = xd_ui(params[XDUCK_RATIO_SLOT],   (float)XDUCK_RATIO_UI_DEFAULT) * 0.01f;
    float atk    = xd_ui(params[XDUCK_ATTACK_SLOT],  (float)XDUCK_ATTACK_UI_DEFAULT) * 0.01f;
    float holdUi = xd_ui(params[XDUCK_HOLD_SLOT],    (float)XDUCK_HOLD_UI_DEFAULT);
    float rel    = xd_ui(params[XDUCK_RELEASE_SLOT], (float)XDUCK_RELEASE_UI_DEFAULT) * 0.01f;
    float typeUi = xd_ui(params[XDUCK_TYPE_SLOT],    (float)XDUCK_TYPE_UI_DEFAULT);
    float routUi = xd_ui(params[XDUCK_ROUTE_SLOT],   (float)XDUCK_ROUTE_UI_DEFAULT);
    float freqUi = xd_ui(params[XDUCK_FREQ_SLOT],    (float)XDUCK_FREQ_UI_DEFAULT);

    if (range > 60.0f) range = 60.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    if (atk > 1.0f) atk = 1.0f;
    if (rel > 1.0f) rel = 1.0f;
    if (holdUi > 100.0f) holdUi = 100.0f;
    float holdLen = holdUi * XD_HOLD_SAMPLES;

    float thrDb = -thrUi;
    /* Gate and Tex both act below the threshold; Tex follows the key 1:1
     * in dB (expander slope = Ratio), Gate is steep (up to 1:21). */
    float isGate = (typeUi > 0.5f) ? 1.0f : 0.0f;
    float isTex  = (typeUi > 1.5f) ? 1.0f : 0.0f;
    float slopeDuck = ratio;
    float slopeGate = ratio * ratio * XD_GATE_SLOPE;
    slopeGate = slopeGate + isTex * (ratio - slopeGate);

    /* Route as float flags (no switch): which channels get their gain
     * applied, and which one is muted because it is only the key. */
    float rX  = (routUi < 0.5f) ? 1.0f : 0.0f;
    float rRL = (routUi > 0.5f && routUi < 1.5f) ? 1.0f : 0.0f;
    float rLR = (routUi > 1.5f && routUi < 2.5f) ? 1.0f : 0.0f;
    float rKR = (routUi > 2.5f && routUi < 3.5f) ? 1.0f : 0.0f;
    float rKL = (routUi > 3.5f) ? 1.0f : 0.0f;
    float procL = rX + rRL + rKR;
    float procR = rX + rLR + rKL;
    float keepL = 1.0f - rKL;
    float keepR = 1.0f - rKR;

    /* One-pole coefficients: a = 1 - exp(-1/(t*fs)), t exponential in knob. */
    float xA = XD_ATK_X0 * xd_exp2(-XD_ATK_OCT * atk);
    float xR = XD_REL_X0 * xd_exp2(-XD_REL_OCT * rel);
    float aAtk = 1.0f - xd_exp2(-xA * XD_LOG2E);
    float aRel = 1.0f - xd_exp2(-xR * XD_LOG2E);
    /* Attack follows the key's onset: ducking down, or a gate opening up. */
    float aDown = (isGate > 0.5f) ? aRel : aAtk;
    float aUp   = (isGate > 0.5f) ? aAtk : aRel;

    /* Key band-pass, TPT state-variable filter. Step n (1..13) is centred at
     * 8 kHz * 1.555^-(13-n); step 0 bypasses it (the filter still runs so it
     * is warm when switched in). g = tan(pi*f/fs) by series, 1/d by Newton. */
    float fStep = freqUi;
    if (fStep > 13.0f) fStep = 13.0f;
    float useBp = (fStep > 0.5f) ? 1.0f : 0.0f;
    float fc = XD_FREQ_TOP * xd_exp2(-(13.0f - fStep) * XD_FREQ_STEP);
    float w = fc * XD_PI_SR;
    float w2 = w * w;
    float g = w * (1.0f + w2 * (0.33333333f + w2 * 0.13333333f));
    float d = 1.0f + g * (g + XD_SVF_K);
    float r = 0.7f;                           /* d is in 1..1.93: converges */
    r = r * (2.0f - d * r);
    r = r * (2.0f - d * r);
    r = r * (2.0f - d * r);
    r = r * (2.0f - d * r);
    float a1 = r, a2 = g * r, a3 = g * g * r;

    float envL = st->envL, envR = st->envR;
    float gdbL = st->gdbL, gdbR = st->gdbR;
    float hL = st->holdL, hR = st->holdR;
    float s1L = st->s1L, s2L = st->s2L, s1R = st->s1R, s2R = st->s2R;
    int i;
    for (i = 0; i < 8; i++) {
        float inL = fxBuf[i];
        float inR = fxBuf[i + 8];

        /* Band-passed keys, unity gain at the centre (k * v1). */
        float v3 = inL - s2L;
        float v1 = a1 * s1L + a2 * v3;
        float v2 = s2L + a2 * s1L + a3 * v3;
        s1L = 2.0f * v1 - s1L;
        s2L = 2.0f * v2 - s2L;
        float kL = inL + useBp * (XD_SVF_K * v1 - inL);
        v3 = inR - s2R;
        v1 = a1 * s1R + a2 * v3;
        v2 = s2R + a2 * s1R + a3 * v3;
        s1R = 2.0f * v1 - s1R;
        s2R = 2.0f * v2 - s2R;
        float kR = inR + useBp * (XD_SVF_K * v1 - inR);

        /* Peak detector with hold: a new peak restarts the hold; the level
         * only starts to decay once the hold has run out. */
        float aL = kL < 0.0f ? -kL : kL;
        float aR = kR < 0.0f ? -kR : kR;
        if (aL > envL)      { envL = aL; hL = holdLen; }
        else if (hL > 0.0f) { hL -= 1.0f; }
        else                { envL *= XD_DET_DECAY; }
        if (aR > envR)      { envR = aR; hR = holdLen; }
        else if (hR > 0.0f) { hR -= 1.0f; }
        else                { envR *= XD_DET_DECAY; }

        /* L is driven by the R key, R by the L key. */
        float tL = xd_target(envR, thrDb, range, slopeDuck, slopeGate, isGate);
        float tR = xd_target(envL, thrDb, range, slopeDuck, slopeGate, isGate);
        gdbL += ((tL < gdbL) ? aDown : aUp) * (tL - gdbL);
        gdbR += ((tR < gdbR) ? aDown : aUp) * (tR - gdbR);

        float gL = xd_exp2(gdbL * XD_LOG2_PER_DB);
        float gR = xd_exp2(gdbR * XD_LOG2_PER_DB);

        float wetL = (inL + procL * (inL * gL - inL)) * keepL;
        float wetR = (inR + procR * (inR * gR - inR)) * keepR;

        /* Tex: the two routed channels are mixed, same sum on both outputs. */
        float mono = wetL + wetR;
        fxBuf[i]     = wetL + isTex * (mono - wetL);
        fxBuf[i + 8] = wetR + isTex * (mono - wetR);
    }

    st->envL = envL;
    st->envR = envR;
    st->gdbL = gdbL;
    st->gdbR = gdbR;
    st->holdL = hL;
    st->holdR = hR;
    st->s1L = s1L; st->s2L = s2L;
    st->s1R = s1R; st->s2R = s2R;
}
