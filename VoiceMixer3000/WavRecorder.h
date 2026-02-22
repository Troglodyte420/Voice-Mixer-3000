#ifndef WAV_RECORDER_H
#define WAV_RECORDER_H

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

class ES8311Audio;
class VoiceEffects;

class WavRecorder {
public:
    WavRecorder();
    
    bool begin(ES8311Audio* audio, VoiceEffects* effects, uint8_t csPin = 12);
    
    bool startRecording(uint32_t sampleRate = 16000);
    bool stopRecording();
    bool stopRecordingTrimmed(uint32_t trimMs = 80);  // trim N ms from end to remove button click
    
    bool startPlayback();
    void stopPlayback();
    
    // MUST call in loop()
    void update();
    
    bool isRecording() { return _state == STATE_RECORDING; }
    bool isPlaying()   { return _state == STATE_PLAYING; }
    bool isBusy()      { return _state != STATE_IDLE; }
    
    float getRecordedSeconds();
    float getPlaybackSeconds();
    float getMaxSeconds() { return (float)MAX_RECORD_SECONDS; }
    
    bool hasRecording();
    bool sdReady() { return _sdOk; }
    
private:
    enum State { STATE_IDLE, STATE_RECORDING, STATE_PLAYING };
    
    ES8311Audio*  _audio;
    VoiceEffects* _effects;
    State _state;
    File _file;
    bool _sdOk;
    SPIClass _sdSpi;
    
    uint32_t _sampleRate;
    uint32_t _totalSamplesWritten;
    uint32_t _totalSamplesToPlay;
    uint32_t _samplesPlayed;
    unsigned long _recordStartMs;
    
    bool _i2sRxInstalled;
    bool _i2sTxInstalled;

    // ---- Recording signal conditioning ----
    // Three-stage pipeline applied per chunk before SD write:
    //   1. Slew limiter  — kills sudden wind/impact spikes
    //   2. IIR high-pass — removes DC offset and low-frequency rumble/breath
    //   3. Soft limiter  — catches anything still too loud
    //
    // HPF cutoff frequency is set by REC_HPF_CUTOFF_HZ (default 1000 Hz).
    // To change it, edit that constant at the top of WavRecorder.cpp.
    float   _recPrevSample;   // slew limiter state
    float   _recHpfX1;        // HPF input  delay (x[n-1])
    float   _recHpfY1;        // HPF output delay (y[n-1])
    
    static const uint32_t MAX_RECORD_SECONDS = 60;
    static const char* FOLDER_PATH;
    static const char* FILE_PATH;
    
    // SD card pins (Cardputer-Adv)
    static const int PIN_SD_CS   = 12;
    static const int PIN_SD_MOSI = 14;
    static const int PIN_SD_CLK  = 40;
    static const int PIN_SD_MISO = 39;
    
    // I2S pins
    static const int PIN_I2S_SCLK = 41;
    static const int PIN_I2S_LRCK = 43;
    static const int PIN_I2S_DOUT = 42;
    static const int PIN_I2S_DIN  = 46;
    
    bool installI2sRx(uint32_t sampleRate);
    void uninstallI2sRx();
    bool installI2sTx(uint32_t sampleRate);
    void uninstallI2sTx();
    
    void writeWavHeader(uint32_t sampleRate, uint32_t dataSize);
    void updateWavHeader();
    bool readWavHeader(uint32_t* sampleRate, uint32_t* numSamples);
    
    void ensureFolder();
};

#endif
