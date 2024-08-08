#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <iostream>
#include <winsock2.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <chrono>
#include <string>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")

#define BUFFER_SIZE 1152
#define HEADER_SIZE 5

void Log(const std::string& message) {
    printf("%s\n", message.c_str());
}

void LogError(const std::string& message, HRESULT hr) {
    printf("%i : %s\n", hr, message.c_str());
}

HRESULT CaptureAudio(IAudioClient* pAudioClient, IAudioCaptureClient* pCaptureClient, WAVEFORMATEX* pwfx, SOCKET sock, sockaddr_in remoteAddr) {
    UINT32 packetLength = 0;
    BYTE* pData;
    UINT32 numFramesAvailable;
    DWORD flags;
    char buffer[BUFFER_SIZE + HEADER_SIZE] = { 0 };

    Log("Starting audio capture loop");

    char pcmBuffer[BUFFER_SIZE * 8] = { 0 };
    uint32_t pcmBufferHead = 0;
    while (true) {
        HRESULT hr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            LogError("Failed to get next packet size", hr);
            return hr;
        }

        while (packetLength != 0) {
            numFramesAvailable = 0;
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            if (FAILED(hr)) {
                LogError("Failed to get buffer", hr);
                return hr;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                pData = NULL;
                Log("Silent buffer detected");
            }
            if (pData) {
                UINT32 bytesToCopy = numFramesAvailable * pwfx->nBlockAlign;

                memcpy(pcmBuffer + pcmBufferHead, pData, bytesToCopy);
                pcmBufferHead += bytesToCopy;
                while (pcmBufferHead >= BUFFER_SIZE) {
                    bool is_44100 = pwfx->nSamplesPerSec % 44100 == 0;
                    int sample_mask = pwfx->nSamplesPerSec / (is_44100 ? 44100 : 48000);
                    sample_mask |= is_44100 << 7;
                    buffer[0] = sample_mask;
                    buffer[1] = pwfx->wBitsPerSample;
                    buffer[2] = pwfx->nChannels;
                    buffer[3] = static_cast<BYTE>((pwfx->wFormatTag >> 8) & 0xFF);
                    buffer[4] = static_cast<BYTE>(pwfx->wFormatTag & 0xFF);

                    memcpy(buffer + HEADER_SIZE, pcmBuffer, BUFFER_SIZE);
                    int sendResult = sendto(sock, buffer, BUFFER_SIZE + HEADER_SIZE, 0, (sockaddr*)&remoteAddr, sizeof(remoteAddr));
                    if (sendResult == SOCKET_ERROR) {
                        Log("Failed to send data over UDP");
                    }

                    for (int i = 0; i < pcmBufferHead - BUFFER_SIZE; i++)
                        pcmBuffer[i] = pcmBuffer[i + BUFFER_SIZE];
                    pcmBufferHead -= BUFFER_SIZE;
                }
            }
            hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr)) {
                LogError("Failed to release buffer", hr);
                return hr;
            }
        }
    }

    return S_OK;
}

int main(int argc, char* argv[]) {
    Log("Application started");

    if (argc < 2) {
        Log("Usage: program.exe <IP> [port]");
        return 1;
    }

    const char* REMOTE_IP = argv[1];
    int REMOTE_PORT = (argc > 2) ? std::stoi(argv[2]) : 16401;

    // Set the highest process priority possible
    if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
        DWORD error = GetLastError();
        LogError("Failed to set process priority", error);
    }
    else {
        Log("Process priority set to REALTIME_PRIORITY_CLASS");
    }

    // Optionally, set the thread priority as well
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        DWORD error = GetLastError();
        LogError("Failed to set thread priority", error);
    }
    else {
        Log("Thread priority set to THREAD_PRIORITY_TIME_CRITICAL");
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogError("Failed to initialize COM", hr);
        return hr;
    }

    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDevice = NULL;
    IAudioClient* pAudioClient = NULL;
    IAudioCaptureClient* pCaptureClient = NULL;
    WAVEFORMATEX* pwfx = NULL;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) {
        LogError("Failed to create device enumerator", hr);
        return hr;
    }

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        LogError("Failed to get default audio endpoint", hr);
        return hr;
    }

    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
    if (FAILED(hr)) {
        LogError("Failed to activate audio client", hr);
        return hr;
    }

    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        LogError("Failed to get mix format", hr);
        return hr;
    }

    pwfx->wFormatTag = WAVE_FORMAT_PCM;
    pwfx->cbSize = 0;

    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, pwfx, NULL);
    if (FAILED(hr)) {
        LogError("Failed to initialize audio client", hr);
        return hr;
    }

    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
    if (FAILED(hr)) {
        LogError("Failed to get audio capture client", hr);
        return hr;
    }

    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        LogError("Failed to start audio client", hr);
        return hr;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Log("WSAStartup failed");
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        Log("Socket creation failed");
        WSACleanup();
        return 1;
    }

    sockaddr_in remoteAddr;
    remoteAddr.sin_family = AF_INET;
    remoteAddr.sin_port = htons(REMOTE_PORT);
    remoteAddr.sin_addr.s_addr = inet_addr(REMOTE_IP);

    Log("Starting audio capture");
    hr = CaptureAudio(pAudioClient, pCaptureClient, pwfx, sock, remoteAddr);
    if (FAILED(hr)) {
        LogError("Audio capture failed", hr);
    }

    Log("Cleaning up resources");
    pAudioClient->Stop();
    CoTaskMemFree(pwfx);
    pCaptureClient->Release();
    pAudioClient->Release();
    pDevice->Release();
    pEnumerator->Release();
    CoUninitialize();
    closesocket(sock);
    WSACleanup();

    Log("Application ended");

    return 0;
}