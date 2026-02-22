#include "WavRecorder.h"
#include "ES8311Audio.h"
#include "VoiceEffects.h"
#include <driver/i2s.h>

const char* WavRecorder::FOLDER_PATH = "/VoiceMemos";
const char* WavRecorder::FILE_PATH   = "/VoiceMemos/current.wav";

// ---- Recording HPF cutoff frequency ----
// Change this value to adjust the high-pass filter cutoff on the recording path.
// At 16 kHz sample rate:
//   80  Hz — removes only DC/subsonic rumble, keeps everything else
//   200 Hz — removes breath noise, keeps full voice
//   500 Hz — cuts low voice fundamentals, cleaner but slightly thin
//   1000 Hz — removes all low-end, focuses on mid/upper voice clarity  ← default
//   2000 Hz — aggressive, only upper voice/consonants remain
static const float REC_HPF_CUTOFF_HZ = 500.0f;

WavRecorder::WavRecorder() : _sdSpi(FSPI) {
    _audio   = nullptr;
    _effects = nullptr;
    _state = STATE_IDLE;
    _sdOk = false;
    _sampleRate = 16000;
    _totalSamplesWritten = 0;
    _totalSamplesToPlay = 0;
    _samplesPlayed = 0;
    _recordStartMs = 0;
    _i2sRxInstalled = false;
    _i2sTxInstalled = false;
    _recPrevSample  = 0.0f;
    _recHpfX1       = 0.0f;
    _recHpfY1       = 0.0f;
}

bool WavRecorder::begin(ES8311Audio* audio, VoiceEffects* effects, uint8_t csPin) {
    _audio   = audio;
    _effects = effects;
    
    _sdSpi.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, csPin);
    
    if (!SD.begin(csPin, _sdSpi, 4000000)) {
        Serial.println("SD card init failed!");
        _sdOk = false;
        return false;
    }
    
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD card: %llu MB\n", cardSize);
    
    ensureFolder();
    _sdOk = true;
    return true;
}

void WavRecorder::ensureFolder() {
    if (!SD.exists(FOLDER_PATH)) {
        SD.mkdir(FOLDER_PATH);
        Serial.printf("Created: %s\n", FOLDER_PATH);
    }
}

// ============================================================
// WAV header helpers (16-bit mono PCM)
// ============================================================

void WavRecorder::writeWavHeader(uint32_t sampleRate, uint32_t dataSize) {
    uint16_t numCh = 1, bits = 16, fmt = 1;
    uint32_t byteRate = sampleRate * 2;
    uint16_t blockAlign = 2;
    uint32_t chunkSize = 36 + dataSize;
    uint32_t sub1 = 16;
    
    _file.write((const uint8_t*)"RIFF", 4);
    _file.write((const uint8_t*)&chunkSize, 4);
    _file.write((const uint8_t*)"WAVE", 4);
    _file.write((const uint8_t*)"fmt ", 4);
    _file.write((const uint8_t*)&sub1, 4);
    _file.write((const uint8_t*)&fmt, 2);
    _file.write((const uint8_t*)&numCh, 2);
    _file.write((const uint8_t*)&sampleRate, 4);
    _file.write((const uint8_t*)&byteRate, 4);
    _file.write((const uint8_t*)&blockAlign, 2);
    _file.write((const uint8_t*)&bits, 2);
    _file.write((const uint8_t*)"data", 4);
    _file.write((const uint8_t*)&dataSize, 4);
}

void WavRecorder::updateWavHeader() {
    if (!_file) return;
    uint32_t dataSize = _totalSamplesWritten * 2;
    uint32_t chunkSize = 36 + dataSize;
    _file.seek(4);
    _file.write((const uint8_t*)&chunkSize, 4);
    _file.seek(40);
    _file.write((const uint8_t*)&dataSize, 4);
}

bool WavRecorder::readWavHeader(uint32_t* sampleRate, uint32_t* numSamples) {
    if (!_file || _file.size() < 44) return false;
    _file.seek(0);
    uint8_t h[44];
    if (_file.read(h, 44) != 44) return false;
    if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return false;
    
    uint16_t fmt = *(uint16_t*)(h + 20);
    uint16_t ch  = *(uint16_t*)(h + 22);
    *sampleRate  = *(uint32_t*)(h + 24);
    uint16_t bits = *(uint16_t*)(h + 34);
    uint32_t dataSize = *(uint32_t*)(h + 40);
    
    if (fmt != 1 || ch != 1 || bits != 16) {
        Serial.println("WAV: only 16-bit mono PCM supported");
        return false;
    }
    *numSamples = dataSize / 2;
    Serial.printf("WAV: %dHz, %ds\n", *sampleRate, *numSamples / *sampleRate);
    return true;
}

// ============================================================
// I2S install/uninstall
// ============================================================

bool WavRecorder::installI2sRx(uint32_t sampleRate) {
    if (_i2sRxInstalled) return true;
    
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = sampleRate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
    
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_I2S_SCLK;
    pins.ws_io_num  = PIN_I2S_LRCK;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = PIN_I2S_DIN;
    
    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return false;
    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_zero_dma_buffer(I2S_NUM_0);
    _i2sRxInstalled = true;
    return true;
}

void WavRecorder::uninstallI2sRx() {
    if (_i2sRxInstalled) {
        i2s_driver_uninstall(I2S_NUM_0);
        _i2sRxInstalled = false;
    }
}

bool WavRecorder::installI2sTx(uint32_t sampleRate) {
    if (_i2sTxInstalled) return true;
    
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = sampleRate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
    
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_I2S_SCLK;
    pins.ws_io_num  = PIN_I2S_LRCK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    
    if (i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL) != ESP_OK) return false;
    i2s_set_pin(I2S_NUM_1, &pins);
    i2s_zero_dma_buffer(I2S_NUM_1);
    _i2sTxInstalled = true;
    return true;
}

void WavRecorder::uninstallI2sTx() {
    if (_i2sTxInstalled) {
        i2s_driver_uninstall(I2S_NUM_1);
        _i2sTxInstalled = false;
    }
}

// ============================================================
// Recording
// ============================================================

bool WavRecorder::startRecording(uint32_t sampleRate) {
    if (_state != STATE_IDLE || !_audio) return false;
    
    _sampleRate = sampleRate;
    _totalSamplesWritten = 0;
    _recordStartMs = millis();
    
    _file = SD.open(FILE_PATH, FILE_WRITE);
    if (!_file) {
        Serial.printf("Failed to open %s\n", FILE_PATH);
        return false;
    }
    
    writeWavHeader(sampleRate, 0);
    _audio->enableMicForStream();
    
    if (!installI2sRx(sampleRate)) {
        _file.close();
        return false;
    }
    
    delay(100);
    
    // Discard initial noisy samples
    int16_t discard[512];
    size_t br;
    for (int i = 0; i < 4; i++)
        i2s_read(I2S_NUM_0, discard, sizeof(discard), &br, portMAX_DELAY);
    
    _state = STATE_RECORDING;
    _recPrevSample = 0.0f;
    _recHpfX1      = 0.0f;
    _recHpfY1      = 0.0f;
    Serial.println("Recording started");
    return true;
}

bool WavRecorder::stopRecording() {
    if (_state != STATE_RECORDING) return false;

    uninstallI2sRx();
    updateWavHeader();
    _file.close();
    _state = STATE_IDLE;

    float secs = (float)_totalSamplesWritten / _sampleRate;
    Serial.printf("Saved: %.1fs (%d samples)\n", secs, _totalSamplesWritten);
    return true;
}

// Stop recording and remove the last trimMs milliseconds of audio.
// This eliminates the button-press click captured at the moment of stopping.
// The WAV header is updated to reflect the new (shorter) data size.
bool WavRecorder::stopRecordingTrimmed(uint32_t trimMs) {
    if (_state != STATE_RECORDING) return false;

    uninstallI2sRx();

    // Calculate how many bytes to remove.
    // 16-bit mono: 2 bytes per sample, sampleRate samples per second.
    uint32_t trimSamples  = (_sampleRate * trimMs) / 1000;

    // Don't trim more than we actually recorded (leave at least 0.1s)
    uint32_t minSamples   = _sampleRate / 10;
    if (trimSamples >= _totalSamplesWritten - minSamples) {
        trimSamples = 0;
    }

    if (trimSamples > 0 && _totalSamplesWritten > trimSamples) {
        _totalSamplesWritten -= trimSamples;
        // No file truncation needed — updateWavHeader() below will write
        // the shorter dataSize into the header. WAV players read exactly
        // dataSize bytes and ignore anything beyond, so the tail is harmless.
        Serial.printf("Trimmed %u ms (%u samples) from end\n", trimMs, trimSamples);
    }

    updateWavHeader();
    _file.close();
    _state = STATE_IDLE;

    float secs = (float)_totalSamplesWritten / _sampleRate;
    Serial.printf("Saved (trimmed): %.1fs (%d samples)\n", secs, _totalSamplesWritten);
    return true;
}

// ============================================================
// Playback
// ============================================================

bool WavRecorder::startPlayback() {
    if (_state != STATE_IDLE || !_audio) return false;
    
    _file = SD.open(FILE_PATH, FILE_READ);
    if (!_file) {
        Serial.println("No recording found");
        return false;
    }
    
    uint32_t sr, ns;
    if (!readWavHeader(&sr, &ns)) {
        _file.close();
        return false;
    }
    
    _sampleRate = sr;
    _totalSamplesToPlay = ns;
    _samplesPlayed = 0;
    
    _audio->enableSpkForStream();
    
    if (!installI2sTx(sr)) {
        _file.close();
        return false;
    }
    
    delay(100);
    _state = STATE_PLAYING;
    Serial.printf("Playing: %.1fs at %dHz\n", (float)ns / sr, sr);
    return true;
}

void WavRecorder::stopPlayback() {
    if (_state != STATE_PLAYING) return;
    
    // Flush silence to avoid pop
    int16_t silence[512] = {0};
    size_t bw;
    for (int i = 0; i < 4; i++)
        i2s_write(I2S_NUM_1, silence, sizeof(silence), &bw, 10);
    
    uninstallI2sTx();
    _file.close();
    _state = STATE_IDLE;
    Serial.println("Playback stopped");
}

// Soft limiter — prevents speaker distortion on playback.
// Below 22000: unchanged. Above: smooth asymptotic compression to 28000.
static inline int16_t softLimit(float v) {
    const float THRESH = 22000.0f;
    const float KNEE   = 6000.0f;
    float a = fabsf(v);
    if (a <= THRESH) return (int16_t)v;
    float excess = (a - THRESH) / THRESH;
    float compressed = THRESH + KNEE * (excess / (1.0f + excess));
    return (v > 0) ? (int16_t)compressed : (int16_t)(-compressed);
}

// ============================================================
// update() — call every loop
// ============================================================

void WavRecorder::update() {
    if (_state == STATE_RECORDING) {
        unsigned long elapsed = millis() - _recordStartMs;
        if (elapsed >= MAX_RECORD_SECONDS * 1000UL) {
            Serial.println("Max recording time reached");
            stopRecording();
            return;
        }
        
        const int RF = 256;
        int16_t stereoBuf[RF * 2];
        int16_t monoBuf[RF];
        size_t bytesRead = 0;
        
        i2s_read(I2S_NUM_0, stereoBuf, RF * 4, &bytesRead, 10);
        
        if (bytesRead > 0) {
            int frames = bytesRead / 4;

            // ---- Three-stage recording signal conditioning ----
            //
            // Stage 1: Slew rate limiter
            //   Prevents the signal from jumping more than SLEW_MAX counts
            //   per sample. Wind blasts and mic impacts cause huge
            //   inter-sample jumps (often > 10000 in one step). Normal
            //   voice at 16kHz rarely exceeds 6000/sample. Clamping here
            //   kills the spike before it reaches the other stages.
            //
            // Stage 2: Single-pole IIR high-pass filter
            //   y[n] = alpha * (y[n-1] + x[n] - x[n-1])
            //   where alpha = RC / (RC + 1/Fs),  RC = 1 / (2*pi*fc)
            //   Cutoff set by REC_HPF_CUTOFF_HZ at top of this file.
            //   At 1000 Hz / 16 kHz: removes breath, mic handling noise,
            //   low-frequency room rumble, and DC offset while keeping
            //   all voice clarity (fundamentals 300Hz+ pass through).
            //   Lower the cutoff to keep more bass, raise to cut more.
            //
            // Stage 3: Soft asymptotic limiter
            //   Below THRESH (22000): signal passes unchanged.
            //   Above THRESH: compressed toward ceiling (28000) using
            //   x/(1+x) curve — never hard-clips, always smooth.
            //   Catches anything that survived stages 1 & 2.
            //
            // State variables (_recPrevSample, _recHpfX1, _recHpfY1)
            // are reset to 0 at the start of every recording session.

            const float SLEW_MAX   = 6000.0f;
            const float LIM_THRESH = 22000.0f;
            const float LIM_KNEE   =  6000.0f;

            // Pre-compute HPF coefficient from cutoff (done once per chunk,
            // same value every sample so the compiler will hoist it out).
            const float RC    = 1.0f / (2.0f * 3.14159265f * REC_HPF_CUTOFF_HZ);
            const float dt    = 1.0f / (float)_sampleRate;
            const float alpha = RC / (RC + dt);   // closer to 1.0 = higher cutoff

            for (int i = 0; i < frames; i++) {
                float s = (float)(stereoBuf[i * 2 + 1]);  // Right channel

                // Stage 1 — slew limiter
                float delta = s - _recPrevSample;
                if (delta >  SLEW_MAX) s = _recPrevSample + SLEW_MAX;
                if (delta < -SLEW_MAX) s = _recPrevSample - SLEW_MAX;
                _recPrevSample = s;

                // Stage 2 — IIR high-pass
                // y[n] = alpha * (y[n-1] + x[n] - x[n-1])
                float hpOut = alpha * (_recHpfY1 + s - _recHpfX1);
                _recHpfX1 = s;
                _recHpfY1 = hpOut;
                s = hpOut;

                // Stage 3 — soft limiter
                float a = fabsf(s);
                if (a > LIM_THRESH) {
                    float excess = (a - LIM_THRESH) / LIM_THRESH;
                    float compressed = LIM_THRESH + LIM_KNEE * (excess / (1.0f + excess));
                    s = (s > 0.0f) ? compressed : -compressed;
                }

                monoBuf[i] = (int16_t)s;
            }

            size_t written = _file.write((const uint8_t*)monoBuf, frames * 2);
            if (written != (size_t)(frames * 2)) {
                Serial.println("SD write error!");
                stopRecording();
                return;
            }
            _totalSamplesWritten += frames;
        }
        
    } else if (_state == STATE_PLAYING) {
        const int PF = 256;
        int16_t monoBuf[PF];
        int16_t stereoBuf[PF * 2];
        
        int32_t remaining = _totalSamplesToPlay - _samplesPlayed;
        if (remaining <= 0) {
            memset(stereoBuf, 0, sizeof(stereoBuf));
            size_t bw;
            for (int i = 0; i < 4; i++)
                i2s_write(I2S_NUM_1, stereoBuf, sizeof(stereoBuf), &bw, 10);
            stopPlayback();
            Serial.println("Playback finished");
            return;
        }
        
        int toRead = min((int32_t)PF, remaining);
        size_t bytesRead = _file.read((uint8_t*)monoBuf, toRead * 2);
        int got = bytesRead / 2;
        
        if (got <= 0) { stopPlayback(); return; }
        
        for (int i = 0; i < got; i++) {
            int16_t s = softLimit((float)monoBuf[i]);
            stereoBuf[i * 2]     = s;
            stereoBuf[i * 2 + 1] = s;
        }
        
        size_t bw;
        i2s_write(I2S_NUM_1, stereoBuf, got * 4, &bw, portMAX_DELAY);
        _samplesPlayed += got;
    }
}

// ============================================================
// Utility
// ============================================================

float WavRecorder::getRecordedSeconds() {
    if (_sampleRate == 0) return 0;
    return (float)_totalSamplesWritten / _sampleRate;
}

float WavRecorder::getPlaybackSeconds() {
    if (_sampleRate == 0) return 0;
    return (float)_samplesPlayed / _sampleRate;
}

bool WavRecorder::hasRecording() {
    return SD.exists(FILE_PATH);
}
