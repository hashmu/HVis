#include "AudioCapture.h"
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ole32.lib")

const CLSID CLSID_MMDeviceEnumerator_Local = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator_Local = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient_Local = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient_Local = __uuidof(IAudioCaptureClient);
const IID IID_IAudioRenderClient_Local = __uuidof(IAudioRenderClient);

AudioCapture::AudioCapture() {}

AudioCapture::~AudioCapture() {
    Cleanup();
}

bool AudioCapture::Initialize(bool loopback) {
    Cleanup();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        printf("[Audio] CoInitializeEx failed: 0x%08X\n", hr);
        return false;
    }

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_Local, nullptr, CLSCTX_ALL,
        IID_IMMDeviceEnumerator_Local, (void**)&m_deviceEnumerator);
    if (FAILED(hr)) {
        printf("[Audio] CoCreateInstance failed: 0x%08X\n", hr);
        return false;
    }

    EDataFlow dataFlow = loopback ? eRender : eCapture;
    hr = m_deviceEnumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &m_device);
    if (FAILED(hr)) {
        printf("[Audio] GetDefaultAudioEndpoint failed: 0x%08X\n", hr);
        Cleanup();
        return false;
    }

    hr = m_device->Activate(IID_IAudioClient_Local, CLSCTX_ALL, nullptr, (void**)&m_audioClient);
    if (FAILED(hr)) {
        printf("[Audio] Activate IAudioClient failed: 0x%08X\n", hr);
        Cleanup();
        return false;
    }

    hr = m_audioClient->GetMixFormat(&m_mixFormat);
    if (FAILED(hr)) {
        printf("[Audio] GetMixFormat failed: 0x%08X\n", hr);
        Cleanup();
        return false;
    }

    m_sampleRate = m_mixFormat->nSamplesPerSec;
    m_channels = m_mixFormat->nChannels;
    printf("[Audio] Format: %d Hz, %d channels\n", m_sampleRate, m_channels);

    DWORD streamFlags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
        10000000, 0, m_mixFormat, nullptr);
    if (FAILED(hr)) {
        printf("[Audio] IAudioClient::Initialize failed: 0x%08X\n", hr);
        Cleanup();
        return false;
    }

    hr = m_audioClient->GetService(IID_IAudioCaptureClient_Local, (void**)&m_captureClient);
    if (FAILED(hr)) {
        printf("[Audio] GetService IAudioCaptureClient failed: 0x%08X\n", hr);
        Cleanup();
        return false;
    }

    // Render silence to keep loopback device active even when nothing is playing
    if (loopback) {
        hr = m_device->Activate(IID_IAudioClient_Local, CLSCTX_ALL, nullptr, (void**)&m_renderAudioClient);
        if (FAILED(hr)) { Cleanup(); return false; }

        hr = m_renderAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
            10000000, 0, m_mixFormat, nullptr);
        if (FAILED(hr)) { Cleanup(); return false; }

        hr = m_renderAudioClient->GetBufferSize(&m_renderBufferFrameCount);
        if (FAILED(hr)) { Cleanup(); return false; }

        hr = m_renderAudioClient->GetService(IID_IAudioRenderClient_Local, (void**)&m_renderClient);
        if (FAILED(hr)) { Cleanup(); return false; }

        BYTE* buffer = nullptr;
        hr = m_renderClient->GetBuffer(m_renderBufferFrameCount, &buffer);
        if (SUCCEEDED(hr)) {
            memset(buffer, 0, m_renderBufferFrameCount * m_mixFormat->nBlockAlign);
            m_renderClient->ReleaseBuffer(m_renderBufferFrameCount, 0);
        }

        m_renderAudioClient->Start();
    }

    m_initialized = true;
    printf("[Audio] Initialized successfully\n");
    return true;
}

bool AudioCapture::Start() {
    if (!m_initialized || !m_audioClient) return false;
    HRESULT hr = m_audioClient->Start();
    if (FAILED(hr)) return false;
    m_capturing = true;
    return true;
}

bool AudioCapture::Stop() {
    if (!m_capturing || !m_audioClient) return false;
    m_audioClient->Stop();
    m_audioClient->Reset();
    m_capturing = false;
    return true;
}

UINT32 AudioCapture::GetAudioData(float* buffer, UINT32 bufferSizeInSamples) {
    if (!m_capturing || !m_captureClient) return 0;

    // Feed silence to keep loopback active
    if (m_renderAudioClient && m_renderClient) {
        UINT32 padding = 0;
        if (SUCCEEDED(m_renderAudioClient->GetCurrentPadding(&padding))) {
            UINT32 available = m_renderBufferFrameCount - padding;
            if (available > 0) {
                BYTE* renderBuf = nullptr;
                if (SUCCEEDED(m_renderClient->GetBuffer(available, &renderBuf))) {
                    memset(renderBuf, 0, available * m_mixFormat->nBlockAlign);
                    m_renderClient->ReleaseBuffer(available, 0);
                }
            }
        }
    }

    UINT32 totalRead = 0;
    UINT32 packetLength = 0;

    while (SUCCEEDED(m_captureClient->GetNextPacketSize(&packetLength)) && packetLength > 0) {
        BYTE* data = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;

        if (FAILED(m_captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr)))
            break;

        UINT32 samples = numFrames * m_channels;
        UINT32 canRead = (bufferSizeInSamples - totalRead);
        if (samples > canRead) samples = canRead;

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            memset(buffer + totalRead, 0, samples * sizeof(float));
        } else {
            memcpy(buffer + totalRead, data, samples * sizeof(float));
        }

        m_captureClient->ReleaseBuffer(numFrames);
        totalRead += samples;

        if (totalRead >= bufferSizeInSamples) break;
    }

    return totalRead;
}

void AudioCapture::Cleanup() {
    if (m_capturing) Stop();

    if (m_renderClient) { m_renderClient->Release(); m_renderClient = nullptr; }
    if (m_renderAudioClient) { m_renderAudioClient->Release(); m_renderAudioClient = nullptr; }
    if (m_mixFormat) { CoTaskMemFree(m_mixFormat); m_mixFormat = nullptr; }
    if (m_captureClient) { m_captureClient->Release(); m_captureClient = nullptr; }
    if (m_audioClient) { m_audioClient->Release(); m_audioClient = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }
    if (m_deviceEnumerator) { m_deviceEnumerator->Release(); m_deviceEnumerator = nullptr; }

    m_initialized = false;
    m_capturing = false;
    m_sampleRate = 0;
    m_channels = 0;
    m_renderBufferFrameCount = 0;
}
