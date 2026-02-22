#include "VoiceEffects.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

const float VoiceEffects::PITCH_RATIOS[17] = {
    0.7071f,  // -6
    0.7491f,  // -5
    0.7937f,  // -4
    0.8409f,  // -3
    0.8909f,  // -2
    0.9439f,  // -1
    1.0000f,  //  0  unity
    1.0595f,  // +1
    1.1225f,  // +2
    1.1892f,  // +3
    1.2599f,  // +4
    1.3348f,  // +5
    1.4142f,  // +6
    1.4983f,  // +7
    1.5874f,  // +8
    1.6818f,  // +9
    1.7818f   // +10
};

VoiceEffects::VoiceEffects() {
    _pitchStep = 0;
    _effect    = EFFECT_NONE;
    _streaming = false;
    _sampleRate = 16000;
    _mixFreq1  = 0;
    _mixFreq2  = 0;
    _delayMs   = 0;
    _delayBuf  = nullptr;
    _delayLen  = 0;
    _delayPos  = 0;
    _userDelayBuf = nullptr;
    _userDelayLen = 0;
    _userDelayPos = 0;
    memset(_combBuf, 0, sizeof(_combBuf));
    memset(_apBuf,   0, sizeof(_apBuf));
}

// ============================================================
// Pitch controls
// ============================================================

void VoiceEffects::setPitch(int step) {
    if (step < PITCH_MIN) step = PITCH_MIN;
    if (step > PITCH_MAX) step = PITCH_MAX;
    _pitchStep = step;
    Serial.printf("Pitch: %+d semitones (ratio %.4f)\n", step, getPitchRatio());
}

void VoiceEffects::pitchUp()   { setPitch(_pitchStep + 1); }
void VoiceEffects::pitchDown() { setPitch(_pitchStep - 1); }

float VoiceEffects::getPitchRatio() {
    return PITCH_RATIOS[_pitchStep - PITCH_MIN];
}

void VoiceEffects::getPitchLabel(char* buf, int bufSize) {
    if (_pitchStep == 0) snprintf(buf, bufSize, "0");
    else                 snprintf(buf, bufSize, "%+d", _pitchStep);
}

// ============================================================
// Voice effect controls
// ============================================================

void VoiceEffects::cycleEffect() {
    switch (_effect) {
        case EFFECT_NONE:       _effect = EFFECT_ROBOT;      break;
        case EFFECT_ROBOT:      _effect = EFFECT_GHOST;      break;
        case EFFECT_GHOST:      _effect = EFFECT_ALIEN;      break;
        case EFFECT_ALIEN:      _effect = EFFECT_RADIO;      break;
        case EFFECT_RADIO:      _effect = EFFECT_CHIPMUNK;   break;
        case EFFECT_CHIPMUNK:   _effect = EFFECT_VILLAIN;    break;
        case EFFECT_VILLAIN:    _effect = EFFECT_CAVE;       break;
        case EFFECT_CAVE:       _effect = EFFECT_NONE;       break;
        default:                _effect = EFFECT_NONE;       break;
    }
    char label[16];
    getEffectLabel(label, sizeof(label));
    Serial.printf("Voice effect: %s\n", label);
}

void VoiceEffects::getEffectLabel(char* buf, int bufSize) {
    switch (_effect) {
        case EFFECT_NONE:       snprintf(buf, bufSize, "None");    break;
        case EFFECT_ROBOT:      snprintf(buf, bufSize, "Robot");   break;
        case EFFECT_GHOST:      snprintf(buf, bufSize, "Ghost");   break;
        case EFFECT_ALIEN:      snprintf(buf, bufSize, "Alien");   break;
        case EFFECT_RADIO:      snprintf(buf, bufSize, "Radio");   break;
        case EFFECT_CHIPMUNK:   snprintf(buf, bufSize, "Chipmk");  break;
        case EFFECT_VILLAIN:    snprintf(buf, bufSize, "Villain"); break;
        case EFFECT_CAVE:       snprintf(buf, bufSize, "Cave");    break;
        default:                snprintf(buf, bufSize, "None");    break;
    }
}

// ============================================================
// Circular delay line helpers
// ============================================================

void VoiceEffects::delayWrite(float sample) {
    if (!_delayBuf) return;
    _delayBuf[_delayPos] = sample;
    _delayPos++;
    if (_delayPos >= _delayLen) _delayPos = 0;
}

float VoiceEffects::delayRead(int32_t offset) {
    if (!_delayBuf || offset <= 0 || offset > _delayLen) return 0.0f;
    int32_t idx = _delayPos - offset;
    if (idx < 0) idx += _delayLen;
    return _delayBuf[idx];
}

void VoiceEffects::freeDelayLines() {
    if (_delayBuf) { free(_delayBuf); _delayBuf = nullptr; }
    if (_userDelayBuf) { free(_userDelayBuf); _userDelayBuf = nullptr; }
    for (int k = 0; k < 4; k++) {
        if (_combBuf[k]) { free(_combBuf[k]); _combBuf[k] = nullptr; }
    }
    for (int k = 0; k < 2; k++) {
        if (_apBuf[k]) { free(_apBuf[k]); _apBuf[k] = nullptr; }
    }
    _delayLen = 0;
    _delayPos = 0;
    _userDelayLen = 0;
    _userDelayPos = 0;
}

// ============================================================
// beginStream - allocate delay lines, reset state
// ============================================================

bool VoiceEffects::beginStream(uint32_t sampleRate) {
    if (_streaming) endStream();

    _sampleRate = sampleRate;
    _srcPhase   = 0.0f;
    _prevSample = 0;
    _globalIdx  = 0;

    _hp_x1 = _hp_y1 = 0.0f;
    _lp_y1 = _lp2_y1 = _lp3_y1 = 0.0f;
    _bq_x1 = _bq_x2 = _bq_y1 = _bq_y2 = 0.0f;
    _bq2_x1 = _bq2_x2 = _bq2_y1 = _bq2_y2 = 0.0f;
    _ap_x1 = _ap_y1 = 0.0f;
    _noiseSeed  = 12345;
    _tailRemaining = 0;

    // De-esser state — removed
    // HPF playback state — removed

    VoiceEffect eff = _effect;

    // Allocate delay lines based on which effect is active
    if (eff == EFFECT_GHOST) {
        // Ghost needs delay for chorus taps: up to 240ms + chorus depth 8ms
        _delayLen = (int32_t)(sampleRate * 0.260f) + 64;
        _delayBuf = (float*)calloc(_delayLen, sizeof(float));
        if (!_delayBuf) {
            Serial.println("Ghost: delay alloc failed");
            return false;
        }
        _delayPos = 0;
        _tailRemaining = (int32_t)(sampleRate * 0.25f);
        Serial.printf("Ghost: delay %d samples (%d bytes)\n",
                      _delayLen, (int)(_delayLen * sizeof(float)));
    }
    else if (eff == EFFECT_CAVE) {
        float srScale = (float)sampleRate / 16000.0f;
        _combLen[0] = (int32_t)(1499 * srScale);
        _combLen[1] = (int32_t)(1637 * srScale);
        _combLen[2] = (int32_t)(1861 * srScale);
        _combLen[3] = (int32_t)(2053 * srScale);
        _apLen[0]   = (int32_t)(241  * srScale);
        _apLen[1]   = (int32_t)(83   * srScale);

        bool ok = true;
        for (int k = 0; k < 4 && ok; k++) {
            _combBuf[k] = (float*)calloc(_combLen[k], sizeof(float));
            _combPos[k] = 0;
            if (!_combBuf[k]) ok = false;
        }
        for (int k = 0; k < 2 && ok; k++) {
            _apBuf[k] = (float*)calloc(_apLen[k], sizeof(float));
            _apPos[k] = 0;
            if (!_apBuf[k]) ok = false;
        }
        if (!ok) {
            Serial.println("Cave: delay alloc failed");
            freeDelayLines();
            return false;
        }
        _tailRemaining = _combLen[3] + _apLen[0] + _apLen[1];

        int totalBytes = 0;
        for (int k = 0; k < 4; k++) totalBytes += _combLen[k] * sizeof(float);
        for (int k = 0; k < 2; k++) totalBytes += _apLen[k] * sizeof(float);
        Serial.printf("Cave: %d bytes allocated for delay lines\n", totalBytes);
    }
    else if (eff == EFFECT_VILLAIN) {
        // Villain uses sub-harmonic comb at ~80Hz period
        int32_t subDelay = (int32_t)(sampleRate / 80.0f);
        _delayLen = subDelay + 64;
        _delayBuf = (float*)calloc(_delayLen, sizeof(float));
        if (!_delayBuf) {
            Serial.println("Villain: delay alloc failed");
            return false;
        }
        _delayPos = 0;
    }

    // Allocate user delay buffer when in NONE mode with delay active
    if (eff == EFFECT_NONE && _delayMs > 0) {
        _userDelayLen = (int32_t)(sampleRate * (float)_delayMs / 1000.0f) + 64;
        _userDelayBuf = (float*)calloc(_userDelayLen, sizeof(float));
        if (!_userDelayBuf) {
            Serial.println("UserDelay: alloc failed");
            _userDelayLen = 0;
        } else {
            _userDelayPos = 0;
            _tailRemaining = _userDelayLen;  // drain the reverb tail
            Serial.printf("UserDelay: %dms = %d samples (%d bytes)\n",
                          _delayMs, _userDelayLen,
                          (int)(_userDelayLen * sizeof(float)));
        }
    }

    _streaming = true;
    Serial.printf("Stream started: sr=%d effect=%d pitch=%d\n",
                  sampleRate, (int)_effect, _pitchStep);
    return true;
}

void VoiceEffects::endStream() {
    freeDelayLines();
    _streaming = false;
    _tailRemaining = 0;
}

bool VoiceEffects::hasTail() {
    return _tailRemaining > 0;
}

// ============================================================
// Pitch resampler - shared by multiple effects
//
// Consumes input at rate 'ratio' and produces interpolated output.
// Maintains _srcPhase and _prevSample across calls.
// ratio > 1 = pitch up (fewer output samples per input)
// ratio < 1 = pitch down (more output samples per input)
// ============================================================

int32_t VoiceEffects::resampleChunk(const int16_t* in, int32_t inCount,
                                     int16_t* out, int32_t outSize,
                                     float ratio) {
    int32_t outIdx = 0;
    int32_t inIdx  = 0;

    while (outIdx < outSize) {
        // _srcPhase is the fractional offset within the current
        // pair of input samples (_prevSample, in[inIdx])
        if (_srcPhase >= 1.0f) {
            // Advance through input samples
            while (_srcPhase >= 1.0f && inIdx < inCount) {
                _prevSample = in[inIdx];
                inIdx++;
                _srcPhase -= 1.0f;
            }
            if (inIdx >= inCount) break;  // consumed all input
        }

        // Interpolate between _prevSample and in[inIdx]
        float v = (float)_prevSample +
                  _srcPhase * ((float)in[inIdx] - (float)_prevSample);
        out[outIdx++] = clamp16(v);
        _srcPhase += ratio;
    }

    // If we haven't consumed all input, save the remaining position
    // Advance past any remaining input samples
    while (inIdx < inCount && _srcPhase >= 1.0f) {
        _prevSample = in[inIdx];
        inIdx++;
        _srcPhase -= 1.0f;
    }

    return outIdx;
}

// ============================================================
// processChunk - main entry point for streaming
// ============================================================

int32_t VoiceEffects::processChunk(const int16_t* inBuffer, int32_t inCount,
                                    int16_t* outBuffer, int32_t outBufferSize) {
    if (!_streaming) return 0;

    if (_effect != EFFECT_NONE) {
        int32_t produced = 0;
        switch (_effect) {
            case EFFECT_ROBOT:      produced = streamRobot(inBuffer, inCount, outBuffer, outBufferSize); break;
            case EFFECT_GHOST:      produced = streamGhost(inBuffer, inCount, outBuffer, outBufferSize); break;
            case EFFECT_ALIEN:      produced = streamAlien(inBuffer, inCount, outBuffer, outBufferSize); break;
            case EFFECT_RADIO:      produced = streamRadio(inBuffer, inCount, outBuffer, outBufferSize); break;
            case EFFECT_CHIPMUNK:   produced = streamChipmunk(inBuffer, inCount, outBuffer, outBufferSize); break;
            case EFFECT_VILLAIN:    produced = streamVillain(inBuffer, inCount, outBuffer, outBufferSize); break;
            case EFFECT_CAVE:       produced = streamCave(inBuffer, inCount, outBuffer, outBufferSize); break;
            default: break;
        }
        return produced;
    }

    if (isPitchActive()) {
        int32_t produced = streamPitch(inBuffer, inCount, outBuffer, outBufferSize);
        applyMixFreqs(outBuffer, produced);
        applyUserDelay(outBuffer, produced);
        return produced;
    }

    // Passthrough
    int32_t n = min(inCount, outBufferSize);
    memcpy(outBuffer, inBuffer, n * sizeof(int16_t));
    applyMixFreqs(outBuffer, n);
    applyUserDelay(outBuffer, n);
    _globalIdx += n;
    return n;
}

// ============================================================
// flushTail - drain echo/reverb tail after input ends
// ============================================================

int32_t VoiceEffects::flushTail(int16_t* outBuffer, int32_t outBufferSize) {
    if (_tailRemaining <= 0) return 0;

    // Feed silence into the effect to let tails ring out
    int32_t chunkSize = min(_tailRemaining, (int32_t)MAX_CHUNK);
    chunkSize = min(chunkSize, outBufferSize);

    int16_t silence[MAX_CHUNK] = {0};
    int32_t produced = processChunk(silence, chunkSize, outBuffer, outBufferSize);

    _tailRemaining -= chunkSize;
    if (_tailRemaining < 0) _tailRemaining = 0;
    return produced;
}

// ============================================================
// streamPitch - pitch-only (no voice effect)
// ============================================================

int32_t VoiceEffects::streamPitch(const int16_t* in, int32_t n,
                                   int16_t* out, int32_t outSz) {
    float ratio = getPitchRatio();
    int32_t produced = resampleChunk(in, n, out, outSz, ratio);
    _globalIdx += produced;
    return produced;
}

// ============================================================
// applyMixFreqs - ring-modulate voice with two carriers
//
// Each frequency (0-200 Hz) acts as a ring modulation carrier
// that MULTIPLIES the voice signal, just like Robot and Alien.
// This creates metallic sidebands — the voice is heard through
// the modulation rather than being drowned out by a tone.
//
// When both carriers are active, they are applied in series:
//   voice × carrier1 × carrier2
// Each stage gets a 2x gain boost to compensate for the
// amplitude loss inherent in ring modulation.
// freq = 0 means that carrier is off (bypass).
// ============================================================

void VoiceEffects::applyMixFreqs(int16_t* buf, int32_t n) {
    if (_mixFreq1 == 0 && _mixFreq2 == 0) return;

    const float twoPiOverSr = 2.0f * M_PI / (float)_sampleRate;

    for (int32_t i = 0; i < n; i++) {
        float s = (float)buf[i];
        int64_t idx = _globalIdx - n + i;

        if (_mixFreq1 > 0) {
            float carrier1 = sinf(twoPiOverSr * (float)_mixFreq1 * (float)idx);
            s = s * carrier1 * 2.0f;
        }
        if (_mixFreq2 > 0) {
            float carrier2 = sinf(twoPiOverSr * (float)_mixFreq2 * (float)idx);
            s = s * carrier2 * 2.0f;
        }

        buf[i] = clamp16(s);
    }
}

// ============================================================
// streamRobot - ring modulation at 80 Hz
// ============================================================

int32_t VoiceEffects::streamRobot(const int16_t* in, int32_t n,
                                   int16_t* out, int32_t outSz) {
    int32_t count = min(n, outSz);
    const float carrierHz   = 80.0f;
    const float twoPiOverSr = 2.0f * M_PI / (float)_sampleRate;

    for (int32_t i = 0; i < count; i++) {
        float modulated = (float)in[i] *
                          sinf(twoPiOverSr * carrierHz * (float)(_globalIdx + i));
        modulated *= 2.0f;
        out[i] = clamp16(modulated);
    }
    _globalIdx += count;
    return count;
}

// ============================================================
// streamGhost - detune + chorus + echo tails
//
// Pitch down 1 semitone, then multi-tap chorus/echo using
// a circular delay buffer. Same algorithm as original, but
// processes chunk-by-chunk.
// ============================================================

int32_t VoiceEffects::streamGhost(const int16_t* in, int32_t n,
                                   int16_t* out, int32_t outSz) {
    // Step 1: pitch-shift into a temp buffer
    const float detuneRatio = 0.9439f;
    int16_t pitched[MAX_CHUNK * 2];  // worst case: ratio < 1 means more output
    int32_t maxPitched = min((int32_t)(sizeof(pitched) / sizeof(int16_t)), outSz);
    int32_t pCount = resampleChunk(in, n, pitched, maxPitched, detuneRatio);

    // Step 2: write pitched samples into delay line and apply chorus
    int32_t tap1 = (int32_t)(_sampleRate * 0.080f);
    int32_t tap2 = (int32_t)(_sampleRate * 0.160f);
    int32_t tap3 = (int32_t)(_sampleRate * 0.240f);
    const float dryGain  = 0.50f;
    const float gain1    = 0.55f;
    const float gain2    = 0.35f;
    const float gain3    = 0.20f;
    const float chorusHz    = 0.5f;
    const float chorusDepth = (float)(_sampleRate * 0.008f);
    const float twoPiOverSr = 2.0f * M_PI / (float)_sampleRate;

    int32_t outCount = min(pCount, outSz);
    for (int32_t i = 0; i < outCount; i++) {
        float dry = (float)pitched[i];

        // Write to delay line
        delayWrite(dry);

        // Chorus modulation
        float chorusMod   = chorusDepth * sinf(twoPiOverSr * chorusHz * (float)(_globalIdx + i));
        int32_t chorusOff = (int32_t)chorusMod;

        float result = dryGain * dry;

        // Read taps from delay line (offset = tap - chorusOff, clamped)
        int32_t r1 = tap1 - chorusOff;
        if (r1 > 0 && r1 < _delayLen) result += gain1 * delayRead(r1);

        int32_t r2 = tap2 - chorusOff;
        if (r2 > 0 && r2 < _delayLen) result += gain2 * delayRead(r2);

        int32_t r3 = tap3 - chorusOff;
        if (r3 > 0 && r3 < _delayLen) result += gain3 * delayRead(r3);

        out[i] = clamp16(result);
    }
    _globalIdx += outCount;
    return outCount;
}

// ============================================================
// streamAlien - ring mod with wobble + metallic resonator
// ============================================================

int32_t VoiceEffects::streamAlien(const int16_t* in, int32_t n,
                                   int16_t* out, int32_t outSz) {
    // Step 1: pitch down ~1.4 semitones
    const float detuneRatio = 0.92f;
    int16_t pitched[MAX_CHUNK * 2];
    int32_t maxP = min((int32_t)(sizeof(pitched) / sizeof(int16_t)), outSz);
    int32_t pCount = resampleChunk(in, n, pitched, maxP, detuneRatio);

    int32_t count = min(pCount, outSz);

    // Step 2: ring mod with wobbling carrier
    const float carrierHz   = 28.0f;
    const float lfoHz       = 0.3f;
    const float twoPiOverSr = 2.0f * M_PI / (float)_sampleRate;

    for (int32_t i = 0; i < count; i++) {
        float t = (float)(_globalIdx + i);
        float wobble   = carrierHz + 8.0f * sinf(twoPiOverSr * lfoHz * t);
        float carrier  = sinf(twoPiOverSr * wobble * t);
        float modulated = (float)pitched[i] * carrier * 2.2f;
        // Soft clip
        if (modulated >  20000.0f) modulated =  20000.0f + (modulated - 20000.0f) * 0.3f;
        if (modulated < -20000.0f) modulated = -20000.0f + (modulated + 20000.0f) * 0.3f;
        pitched[i] = clamp16(modulated);
    }

    // Step 3: biquad bandpass resonator at 1200 Hz, Q=8
    // Uses persistent state: _bq_x1, _bq_x2, _bq_y1, _bq_y2
    float omega = 2.0f * M_PI * 1200.0f / (float)_sampleRate;
    float alpha = sinf(omega) / (2.0f * 8.0f);
    float cosw  = cosf(omega);
    float a0inv = 1.0f / (1.0f + alpha);
    float b0 = alpha * a0inv;
    float b2 = -alpha * a0inv;
    float a1 = -2.0f * cosw * a0inv;
    float a2 = (1.0f - alpha) * a0inv;

    for (int32_t i = 0; i < count; i++) {
        float x0 = (float)pitched[i];
        float y0 = b0*x0 + 0.0f*_bq_x1 + b2*_bq_x2 - a1*_bq_y1 - a2*_bq_y2;
        float mixed = 0.70f * x0 + 0.30f * y0;
        _bq_x2 = _bq_x1; _bq_x1 = x0;
        _bq_y2 = _bq_y1; _bq_y1 = y0;
        out[i] = clamp16(mixed);
    }

    _globalIdx += count;
    return count;
}

// ============================================================
// streamRadio - telephone bandpass + distortion + noise
//
// Note: original had AGC over the entire buffer. We replace
// it with a per-chunk approximation (running envelope).
// ============================================================

int32_t VoiceEffects::streamRadio(const int16_t* in, int32_t n,
                                   int16_t* out, int32_t outSz) {
    int32_t count = min(n, outSz);

    // High-pass at 300 Hz (persistent state: _hp_x1, _hp_y1)
    float hpAlpha = 1.0f / (1.0f + 2.0f * M_PI * 300.0f / (float)_sampleRate);

    for (int32_t i = 0; i < count; i++) {
        float x0 = (float)in[i];
        float y0 = hpAlpha * (_hp_y1 + x0 - _hp_x1);
        _hp_x1 = x0; _hp_y1 = y0;
        out[i] = clamp16(y0);
    }

    // Low-pass at 3400 Hz (persistent state: _lp_y1)
    float lpAlpha = 2.0f * M_PI * 3400.0f /
                    ((float)_sampleRate + 2.0f * M_PI * 3400.0f);
    for (int32_t i = 0; i < count; i++) {
        float lp = lpAlpha * (float)out[i] + (1.0f - lpAlpha) * _lp_y1;
        _lp_y1 = lp;
        out[i] = clamp16(lp);
    }

    // Soft-clip distortion
    for (int32_t i = 0; i < count; i++) {
        float s = (float)out[i] * 1.8f;
        if (s >  16000.0f) s =  16000.0f + (s - 16000.0f) * 0.2f;
        if (s < -16000.0f) s = -16000.0f + (s + 16000.0f) * 0.2f;
        if (s >  24000.0f) s =  24000.0f;
        if (s < -24000.0f) s = -24000.0f;
        out[i] = (int16_t)s;
    }

    // Static noise
    for (int32_t i = 0; i < count; i++) {
        _noiseSeed = _noiseSeed * 1103515245 + 12345;
        int16_t noise = (int16_t)((_noiseSeed >> 16) & 0xFFFF) - 32768;
        float mixed = (float)out[i] * 0.9f + (float)noise * 0.0005f;
        out[i] = clamp16(mixed);
    }

    // Streaming AGC: track envelope with smoothing, apply gain
    // _lp2_y1 used as the running RMS envelope
    const float targetRMS = 12000.0f;
    const float envAttack = 0.01f;   // fast attack
    const float envRelease = 0.0002f; // slow release
    for (int32_t i = 0; i < count; i++) {
        float absVal = fabsf((float)out[i]);
        float envCoeff = (absVal > _lp2_y1) ? envAttack : envRelease;
        _lp2_y1 += envCoeff * (absVal - _lp2_y1);

        float gain = (_lp2_y1 > 100.0f) ? (targetRMS / _lp2_y1) : 1.0f;
        if (gain > 3.0f) gain = 3.0f;
        if (gain < 0.5f) gain = 0.5f;
        out[i] = clamp16((float)out[i] * gain);
    }

    _globalIdx += count;
    return count;
}

// ============================================================
// streamChipmunk - 2x resample (octave up) + HF shelf boost
// ============================================================

int32_t VoiceEffects::streamChipmunk(const int16_t* in, int32_t n,
                                      int16_t* out, int32_t outSz) {
    // Pitch up 1 octave
    const float ratio = 2.0f;
    int32_t pCount = resampleChunk(in, n, out, outSz, ratio);

    // HF shelf boost at 1500 Hz (persistent state: _lp_y1)
    float lpAlpha = 2.0f * M_PI * 1500.0f /
                    ((float)_sampleRate + 2.0f * M_PI * 1500.0f);
    for (int32_t i = 0; i < pCount; i++) {
        float x0 = (float)out[i];
        float lp = lpAlpha * x0 + (1.0f - lpAlpha) * _lp_y1;
        _lp_y1 = lp;
        float boosted = x0 + 1.8f * (x0 - lp);
        out[i] = clamp16(boosted);
    }

    // Mild soft-clip
    for (int32_t i = 0; i < pCount; i++) {
        float s = (float)out[i];
        if (s >  28000.0f) s =  28000.0f + (s - 28000.0f) * 0.15f;
        if (s < -28000.0f) s = -28000.0f + (s + 28000.0f) * 0.15f;
        out[i] = clamp16(s);
    }

    _globalIdx += pCount;
    return pCount;
}

// ============================================================
// streamVillain - pitch down 5 semi + sub-bass + EQ + tube sat
// ============================================================

int32_t VoiceEffects::streamVillain(const int16_t* in, int32_t n,
                                     int16_t* out, int32_t outSz) {
    // Pitch down 5 semitones
    const float ratio = 0.7491f;
    int16_t pitched[MAX_CHUNK * 2];
    int32_t maxP = min((int32_t)(sizeof(pitched) / sizeof(int16_t)), outSz);
    int32_t pCount = resampleChunk(in, n, pitched, maxP, ratio);

    int32_t count = min(pCount, outSz);

    // Sub-harmonic comb using delay line
    int32_t subDelay = (int32_t)(_sampleRate / 80.0f);
    for (int32_t i = 0; i < count; i++) {
        float dry = (float)pitched[i];
        float delayed = delayRead(subDelay);
        float m = dry + 0.30f * delayed;
        delayWrite(m);  // write the mixed signal for feedback path
        pitched[i] = clamp16(m);
    }

    // Peaking EQ at 100 Hz, +8 dB, Q=1.5
    // Uses persistent biquad state: _bq_x1/x2/y1/y2
    float A     = powf(10.0f, 8.0f / 40.0f);
    float omega = 2.0f * M_PI * 100.0f / (float)_sampleRate;
    float sinW  = sinf(omega);
    float cosW  = cosf(omega);
    float alpha = sinW / (2.0f * 1.5f);
    float a0inv = 1.0f / (1.0f + alpha / A);
    float pb0   = (1.0f + alpha * A) * a0inv;
    float pb1   = (-2.0f * cosW)     * a0inv;
    float pb2   = (1.0f - alpha * A) * a0inv;
    float pa1   = (-2.0f * cosW)     * a0inv;
    float pa2   = (1.0f - alpha / A) * a0inv;

    for (int32_t i = 0; i < count; i++) {
        float x0 = (float)pitched[i];
        float y0 = pb0*x0 + pb1*_bq_x1 + pb2*_bq_x2 - pa1*_bq_y1 - pa2*_bq_y2;
        _bq_x2 = _bq_x1; _bq_x1 = x0;
        _bq_y2 = _bq_y1; _bq_y1 = y0;
        pitched[i] = clamp16(y0);
    }

    // Asymmetric tube saturation
    for (int32_t i = 0; i < count; i++) {
        float s = (float)pitched[i] * 1.4f;
        if (s >  22000.0f) s =  22000.0f + (s - 22000.0f) * 0.12f;
        if (s < -20000.0f) s = -20000.0f + (s + 20000.0f) * 0.12f;
        pitched[i] = clamp16(s);
    }

    // LP at 4000 Hz (persistent state: _lp_y1)
    float lpAlpha = 2.0f * M_PI * 4000.0f /
                    ((float)_sampleRate + 2.0f * M_PI * 4000.0f);
    for (int32_t i = 0; i < count; i++) {
        float lp = lpAlpha * (float)pitched[i] + (1.0f - lpAlpha) * _lp_y1;
        _lp_y1 = lp;
        out[i] = clamp16(lp);
    }

    _globalIdx += count;
    return count;
}

// ============================================================
// streamCave - Schroeder reverb (4 comb + 2 all-pass)
// ============================================================

int32_t VoiceEffects::streamCave(const int16_t* in, int32_t n,
                                  int16_t* out, int32_t outSz) {
    if (!_combBuf[0]) {
        // Fallback: passthrough if alloc failed
        int32_t c = min(n, outSz);
        memcpy(out, in, c * sizeof(int16_t));
        return c;
    }

    const float combDecay[4] = { 0.805f, 0.827f, 0.783f, 0.764f };
    const float apDecay = 0.7f;

    int32_t count = min(n, outSz);

    for (int32_t i = 0; i < count; i++) {
        float inSample = (float)in[i];
        float wet = 0.0f;

        // 4 parallel comb filters
        for (int k = 0; k < 4; k++) {
            float delayed = _combBuf[k][_combPos[k]];
            _combBuf[k][_combPos[k]] = inSample + combDecay[k] * delayed;
            _combPos[k]++;
            if (_combPos[k] >= _combLen[k]) _combPos[k] = 0;
            wet += delayed;
        }
        wet *= 0.25f;

        // All-pass 1
        {
            float delayed = _apBuf[0][_apPos[0]];
            float written = wet + apDecay * delayed;
            _apBuf[0][_apPos[0]] = written;
            _apPos[0]++;
            if (_apPos[0] >= _apLen[0]) _apPos[0] = 0;
            wet = -apDecay * written + delayed;
        }
        // All-pass 2
        {
            float delayed = _apBuf[1][_apPos[1]];
            float written = wet + apDecay * delayed;
            _apBuf[1][_apPos[1]] = written;
            _apPos[1]++;
            if (_apPos[1] >= _apLen[1]) _apPos[1] = 0;
            wet = -apDecay * written + delayed;
        }

        float mixed = 0.35f * inSample + 0.65f * wet;
        out[i] = clamp16(mixed);
    }

    _globalIdx += count;
    return count;
}

// ============================================================
// applyUserDelay - user-controlled delay/reverb for NONE mode
//
// Simple feedback delay using a circular buffer:
//   output = dry + feedback * delayed
//   delayed signal written back with decay.
// Creates a cave/echo effect whose length is controlled by Q/W.
// Feedback is 0.45 — gives 4-5 audible repeats before dying out.
// Uses _userDelayBuf (separate from _delayBuf used by effects).
// ============================================================

void VoiceEffects::applyUserDelay(int16_t* buf, int32_t n) {
    if (!_userDelayBuf || _userDelayLen <= 0 || _delayMs == 0) return;

    const float feedback = 0.45f;
    const float wetMix   = 0.50f;

    int32_t delaySamples = (int32_t)(_sampleRate * (float)_delayMs / 1000.0f);
    if (delaySamples > _userDelayLen - 1) delaySamples = _userDelayLen - 1;
    if (delaySamples <= 0) return;

    for (int32_t i = 0; i < n; i++) {
        int32_t readPos = _userDelayPos - delaySamples;
        if (readPos < 0) readPos += _userDelayLen;
        float delayed = _userDelayBuf[readPos];

        float dry = (float)buf[i];
        float out = dry + wetMix * delayed;

        _userDelayBuf[_userDelayPos] = dry + feedback * delayed;
        _userDelayPos++;
        if (_userDelayPos >= _userDelayLen) _userDelayPos = 0;

        buf[i] = clamp16(out);
    }
}
