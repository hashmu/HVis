#pragma once
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    bool Initialize(bool loopback = true);
    bool Start();
    bool Stop();

    // Returns interleaved float32 samples (-1.0 to 1.0)
    UINT32 GetAudioData(float* buffer, UINT32 bufferSizeInSamples);

    UINT32 GetSampleRate() const { return m_sampleRate; }
    UINT32 GetChannels() const { return m_channels; }

    void Cleanup();

private:
    IMMDeviceEnumerator* m_deviceEnumerator = nullptr;
    IMMDevice* m_device = nullptr;
    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    WAVEFORMATEX* m_mixFormat = nullptr;

    IAudioClient* m_renderAudioClient = nullptr;
    IAudioRenderClient* m_renderClient = nullptr;
    UINT32 m_renderBufferFrameCount = 0;

    UINT32 m_sampleRate = 0;
    UINT32 m_channels = 0;
    bool m_initialized = false;
    bool m_capturing = false;
};
