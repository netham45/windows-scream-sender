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
#include <ws2ipdef.h>
#include <windows.h>
#include <shellapi.h>
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

HRESULT CaptureAudio(IAudioClient* pAudioClient, IAudioCaptureClient* pCaptureClient, WAVEFORMATEXTENSIBLE* pwfex, SOCKET sock, sockaddr_in remoteAddr) {
    UINT32 packetLength = 0;
    BYTE* pData;
    UINT32 numFramesAvailable;
    DWORD flags;
    char buffer[BUFFER_SIZE + HEADER_SIZE] = { 0 };

    Log("Starting audio capture loop");

    char pcmBuffer[BUFFER_SIZE * 8] = { 0 };
    uint32_t pcmBufferHead = 0;
    while (true) {
        Sleep(3);
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
                UINT32 bytesToCopy = numFramesAvailable * pwfex->Format.nBlockAlign;

                memcpy(pcmBuffer + pcmBufferHead, pData, bytesToCopy);
                pcmBufferHead += bytesToCopy;
                while (pcmBufferHead >= BUFFER_SIZE) {
                    bool is_44100 = pwfex->Format.nSamplesPerSec % 44100 == 0;
                    int sample_mask = pwfex->Format.nSamplesPerSec / (is_44100 ? 44100 : 48000);
                    sample_mask |= is_44100 << 7;
                    buffer[0] = sample_mask;
                    buffer[1] = pwfex->Format.wBitsPerSample;
                    buffer[2] = pwfex->Format.nChannels;
                    buffer[3] = static_cast<BYTE>((pwfex->dwChannelMask >> 8) & 0xFF);
                    buffer[4] = static_cast<BYTE>(pwfex->dwChannelMask & 0xFF);

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
            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                LogError("Failed to get next packet size", hr);
                return hr;
            }
        }
    }

    return S_OK;
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    Log("Application started");

    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2) {
        Log("Usage: program.exe <IP> [port] [-m]");
        Log(" Default port 16401");
        Log(" -m Enable Multicast");
        LocalFree(argv);
        return 1;
    }

    std::wstring wRemoteIP(argv[1]);
    std::string REMOTE_IP(wRemoteIP.begin(), wRemoteIP.end());
    int REMOTE_PORT = 16401;
    bool multicast = false;

    for (int i = 2; i < argc; i++) {
        if (wcscmp(argv[i], L"-m") == 0) {
            multicast = true;
        } else {
            try {
                REMOTE_PORT = std::stoi(std::wstring(argv[i]));
            } catch (const std::exception&) {
                Log("Invalid port number. Using default port 16401.");
            }
        }
    }

    LocalFree(argv);

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

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Log("WSAStartup failed");
        CoUninitialize();
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        Log("Socket creation failed");
        WSACleanup();
        CoUninitialize();
        return 1;
    }

    sockaddr_in remoteAddr;
    remoteAddr.sin_family = AF_INET;
    remoteAddr.sin_port = htons(REMOTE_PORT);
    remoteAddr.sin_addr.s_addr = inet_addr(REMOTE_IP.c_str());

    if (multicast) {
        // Set up multicast socket options
        int ttl = 32;  // Adjust TTL as needed
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (char*)&ttl, sizeof(ttl)) < 0) {
            Log("Failed to set multicast TTL");
            closesocket(sock);
            WSACleanup();
            CoUninitialize();
            return 1;
        }

        // Set IP_MULTICAST_IF to INADDR_ANY to send on all interfaces
        in_addr addr;
        addr.s_addr = INADDR_ANY;
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (char*)&addr, sizeof(addr)) < 0) {
            Log("Failed to set IP_MULTICAST_IF");
            closesocket(sock);
            WSACleanup();
            CoUninitialize();
            return 1;
        }

        // Join the multicast group on all interfaces
        ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(REMOTE_IP.c_str());
        mreq.imr_interface.s_addr = INADDR_ANY;
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq)) < 0) {
            Log("Failed to join multicast group");
            closesocket(sock);
            WSACleanup();
            CoUninitialize();
            return 1;
        }
        Log("Multicast setup completed successfully");
    }
    while (true) {
        IMMDeviceEnumerator* pEnumerator = NULL;
        IMMDevice* pDevice = NULL;
        IAudioClient* pAudioClient = NULL;
        IAudioCaptureClient* pCaptureClient = NULL;
        WAVEFORMATEX* pwfx = NULL;

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
        if (FAILED(hr)) {
            LogError("Failed to create device enumerator", hr);
            continue;
        }

        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (FAILED(hr)) {
            LogError("Failed to get default audio endpoint", hr);
            pEnumerator->Release();
            continue;
        }

        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
        if (FAILED(hr)) {
            LogError("Failed to activate audio client", hr);
            pDevice->Release();
            pEnumerator->Release();
            continue;
        }

        hr = pAudioClient->GetMixFormat(&pwfx);
        if (FAILED(hr)) {
            LogError("Failed to get mix format", hr);
            pAudioClient->Release();
            pDevice->Release();
            pEnumerator->Release();
            continue;
        }

        pwfx->wFormatTag = WAVE_FORMAT_PCM;
        pwfx->cbSize = 0;

        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, pwfx, NULL);
        if (FAILED(hr)) {
            LogError("Failed to initialize audio client", hr);
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            pEnumerator->Release();
            continue;
        }

        hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
        if (FAILED(hr)) {
            LogError("Failed to get audio capture client", hr);
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            pEnumerator->Release();
            continue;
        }

        hr = pAudioClient->Start();
        if (FAILED(hr)) {
            LogError("Failed to start audio client", hr);
            pCaptureClient->Release();
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            pEnumerator->Release();
            continue;
        }

        Log("Starting audio capture");
        hr = CaptureAudio(pAudioClient, pCaptureClient, reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx), sock, remoteAddr);
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

        Log("Restarting audio capture...");
    }

    // This part will never be reached in the current implementation
    closesocket(sock);
    WSACleanup();
    CoUninitialize();

    Log("Application ended");

    return 0;
}

