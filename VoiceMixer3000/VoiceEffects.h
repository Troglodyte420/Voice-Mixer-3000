#ifndef VOICE_EFFECTS_H
#define VOICE_EFFECTS_H

#include <Arduino.h>

// ============================================================
// VoiceEffects - STREAMING audio processing for Cardputer-Adv
//
// STREAMING ARCHITECTURE:
//   Instead of loading the entire WAV into RAM, effects are
//   applied chunk-by-chunk (~256 samples at a time).
//   Peak RAM: ~30-40 KB (delay lines) vs. megabytes before.
//
//   Usage:
//     effects.beginStream(sampleRate);
//     while (moreData) {
//         read chunk from SD;
//         int out = effects.processChunk(in, inN, out, outSz);
//         write out to I2S;
//     }
//     while (effects.hasTail()) {
//         int out = effects.flushTail(out, outSz);
//         write out to I2S;
//     }
//     effects.endStream();
// ============================================================

class VoiceEffects {
public:
    enum VoiceEffect {
        EFFECT_NONE       = 0,
        EFFECT_ROBOT      = 1,
        EFFECT_GHOST      = 2,
        EFFECT_ALIEN      = 3,
        EFFECT_RADIO      = 4,
        EFFECT_CHIPMUNK   = 5,
        EFFECT_VILLAIN    = 6,
        EFFECT_CAVE       = 7
    };

    VoiceEffects();

    // ---- Pitch ----
    void  setPitch(int step);
    int   getPitch()       { return _pitchStep; }
    void  pitchUp();
    void  pitchDown();
    bool  isPitchActive()  { return _pitchStep != 0; }
    float getPitchRatio();
    void  getPitchLabel(char* buf, int bufSize);

    // ---- Voice effect ----
    void        cycleEffect();
    void        setEffect(VoiceEffect e) { _effect = e; }
    VoiceEffect getEffect()              { return _effect; }
    bool        isEffectActive()         { return _effect != EFFECT_NONE; }
    void        getEffectLabel(char* buf, int bufSize);

    bool isAnyEffectActive() { return isPitchActive() || isEffectActive() || isMixActive() || isDelayActive(); }

    // ---- Mix frequencies (active when effect = NONE) ----
    void  setMixFreq1(uint16_t hz)  { _mixFreq1 = (hz > 200) ? 200 : hz; }
    void  setMixFreq2(uint16_t hz)  { _mixFreq2 = (hz > 15) ? 15 : hz; }
    uint16_t getMixFreq1()          { return _mixFreq1; }
    uint16_t getMixFreq2()          { return _mixFreq2; }
    bool  isMixActive()             { return _mixFreq1 > 0 || _mixFreq2 > 0; }

    // ---- Delay / reverb (active when effect = NONE) ----
    // 0 = off, 10-500 ms in 10ms steps
    void     setDelayMs(uint16_t ms) { _delayMs = (ms > 1000) ? 1000 : ms; }
    uint16_t getDelayMs()            { return _delayMs; }
    bool     isDelayActive()         { return _delayMs > 0; }

    static const uint16_t DELAY_MAX_MS = 1000;

    // ---- Streaming API ----
    bool    beginStream(uint32_t sampleRate);
    int32_t processChunk(const int16_t* inBuffer, int32_t inCount,
                         int16_t* outBuffer, int32_t outBufferSize);
    int32_t flushTail(int16_t* outBuffer, int32_t outBufferSize);
    bool    hasTail();
    void    endStream();

    static const int PITCH_MIN  = -6;
    static const int PITCH_MAX  = 10;
    static const int MAX_CHUNK  = 256;

private:
    int         _pitchStep;
    VoiceEffect _effect;
    uint32_t    _sampleRate;
    bool        _streaming;
    uint16_t    _mixFreq1;
    uint16_t    _mixFreq2;
    uint16_t    _delayMs;

    static const float PITCH_RATIOS[17];

    // ---- Pitch resampler state ----
    float   _srcPhase;
    int16_t _prevSample;

    // ---- Global sample counter ----
    int64_t _globalIdx;

    // ---- Filter states ----
    float _hp_x1, _hp_y1;
    float _lp_y1, _lp2_y1, _lp3_y1;
    float _bq_x1, _bq_x2, _bq_y1, _bq_y2;
    float _bq2_x1, _bq2_x2, _bq2_y1, _bq2_y2;
    float _ap_x1, _ap_y1;
    uint32_t _noiseSeed;

    // ---- Delay line (Ghost, Villain, user delay) ----
    float*  _delayBuf;
    int32_t _delayLen;
    int32_t _delayPos;

    // ---- Second delay line for user delay in NONE mode ----
    float*  _userDelayBuf;
    int32_t _userDelayLen;
    int32_t _userDelayPos;

    // ---- Cave reverb ----
    float*  _combBuf[4];
    int32_t _combLen[4];
    int32_t _combPos[4];
    float*  _apBuf[2];
    int32_t _apLen[2];
    int32_t _apPos[2];

    // ---- Tail flushing ----
    int32_t _tailRemaining;

    // ---- Per-effect streaming processors ----
    int32_t streamPitch     (const int16_t* in, int32_t n, int16_t* out, int32_t outSz);
    int32_t streamRobot     (const int16_t* in, int32_t n, int16_t* out, int32_t outSz);
    int32_t streamGhost     (const int16_t* in, int32_t n, int16_t* out, int32_t outSz);
    int32_t streamAlien     (const int16_t* in, int32_t n, int16_t* out, int32_t outSz);
    int32_t streamRadio     (const int16_t* in, int32_t n, int16_t* out, int32_t outSz);
    int32_t streamChipmunk  (const int16_t* in, int32_t n, int16_t* out, int32_t outSz);
    int32_t streamVillain   (const int16_t* in, int32_t n, int16_t* out, int32_t outSz);
    int32_t streamCave      (const int16_t* in, int32_t n, int16_t* out, int32_t outSz);

    // Pitch-shift helper: resamples 'in' at given ratio.
    // Returns output sample count. Updates _srcPhase/_prevSample.
    int32_t resampleChunk(const int16_t* in, int32_t inCount,
                          int16_t* out, int32_t outSize, float ratio);

    // Ring-modulate with mix frequency carriers (when effect = NONE)
    void applyMixFreqs(int16_t* buf, int32_t n);

    // Apply user-controlled delay/reverb (when effect = NONE)
    void applyUserDelay(int16_t* buf, int32_t n);

    // Write a sample into the circular delay line and advance
    void delayWrite(float sample);
    // Read from delay line at 'offset' samples ago
    float delayRead(int32_t offset);

    void freeDelayLines();

    // Clamp helper
    static inline int16_t clamp16(float v) {
        if (v >  32767.0f) return  32767;
        if (v < -32768.0f) return -32768;
        return (int16_t)v;
    }
};

#endif
