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
#include <Functiondiscoverykeys_devpkey.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")

#define BUFFER_SIZE 1152
#define HEADER_SIZE 5

// Device change event handle
HANDLE g_deviceChangeEvent = NULL;

void Log(const std::string& message) {
    printf("%s\n", message.c_str());
}

void LogError(const std::string& message, HRESULT hr) {
    printf("%i : %s\n", hr, message.c_str());
}

// Device notification callback class
class CMMNotificationClient : public IMMNotificationClient {
private:
    LONG _cRef;
    IMMDeviceEnumerator* _pEnumerator;

public:
    CMMNotificationClient() : _cRef(1), _pEnumerator(NULL) {
    }

    ~CMMNotificationClient() {
        if (_pEnumerator) {
            _pEnumerator->Release();
        }
    }

    void SetEnumerator(IMMDeviceEnumerator* pEnumerator) {
        _pEnumerator = pEnumerator;
        _pEnumerator->AddRef();
    }

    // IUnknown methods
    ULONG STDMETHODCALLTYPE AddRef() {
        return InterlockedIncrement(&_cRef);
    }

    ULONG STDMETHODCALLTYPE Release() {
        ULONG ulRef = InterlockedDecrement(&_cRef);
        if (ulRef == 0) {
            delete this;
        }
        return ulRef;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppvInterface) {
        if (riid == IID_IUnknown) {
            AddRef();
            *ppvInterface = (IUnknown*)this;
        }
        else if (riid == __uuidof(IMMNotificationClient)) {
            AddRef();
            *ppvInterface = (IMMNotificationClient*)this;
        }
        else {
            *ppvInterface = NULL;
            return E_NOINTERFACE;
        }
        return S_OK;
    }

    // IMMNotificationClient methods
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) {
        if (flow == eRender && role == eConsole) {
            Log("Default audio device changed");
            SetEvent(g_deviceChangeEvent); // Signal that device has changed
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) {
        return S_OK;
    }
};

// Function to get device friendly name
std::string GetDeviceName(IMMDevice* pDevice) {
    if (!pDevice) return "Unknown Device";

    IPropertyStore* pProps = NULL;
    PROPVARIANT varName;
    PropVariantInit(&varName);
    std::string deviceName = "Unknown Device";

    HRESULT hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
    if (SUCCEEDED(hr)) {
        hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
            // Convert wide string to narrow string
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, NULL, 0, NULL, NULL);
            if (size_needed > 0) {
                std::vector<char> buffer(size_needed);
                WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, &buffer[0], size_needed, NULL, NULL);
                deviceName = &buffer[0];
            }
        }
        PropVariantClear(&varName);
        pProps->Release();
    }

    return deviceName;
}

// Modified to handle device disconnections
HRESULT CaptureAudio(IAudioClient* pAudioClient, IAudioCaptureClient* pCaptureClient, WAVEFORMATEXTENSIBLE* pwfex, SOCKET sock, sockaddr_in remoteAddr) {
    UINT32 packetLength = 0;
    BYTE* pData;
    UINT32 numFramesAvailable;
    DWORD flags;
    char buffer[BUFFER_SIZE + HEADER_SIZE] = { 0 };

    Log("Starting audio capture loop");

    char pcmBuffer[BUFFER_SIZE * 8] = { 0 };
    uint32_t pcmBufferHead = 0;
    
    HANDLE events[2] = { g_deviceChangeEvent, NULL };
    HANDLE waitEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (waitEvent == NULL) {
        LogError("Failed to create wait event", GetLastError());
        return E_FAIL;
    }
    events[1] = waitEvent;

    // Loop until told to stop or device changes
    while (true) {
        // Wait for audio data (small timeout)
        Sleep(3);

        // Check if device change event was signaled
        DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, 0);
        if (waitResult == WAIT_OBJECT_0) {
            // Default audio device has changed
            Log("Audio device change detected, reconnecting...");
            ResetEvent(g_deviceChangeEvent);
            CloseHandle(waitEvent);
            return S_FALSE; // Special return code to signal device change
        }

        HRESULT hr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            // Check if this is a device disconnect error
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == E_INVALIDARG) {
                LogError("Audio device disconnected", hr);
                CloseHandle(waitEvent);
                return S_FALSE; // Special return code to signal device change
            }
            LogError("Failed to get next packet size", hr);
            CloseHandle(waitEvent);
            return hr;
        }

        while (packetLength != 0) {
            numFramesAvailable = 0;
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            if (FAILED(hr)) {
                // Check if this is a device disconnect error
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == E_INVALIDARG) {
                    LogError("Audio device disconnected while getting buffer", hr);
                    CloseHandle(waitEvent);
                    return S_FALSE; // Special return code to signal device change
                }
                LogError("Failed to get buffer", hr);
                CloseHandle(waitEvent);
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
                // Check if this is a device disconnect error
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == E_INVALIDARG) {
                    LogError("Audio device disconnected while releasing buffer", hr);
                    CloseHandle(waitEvent);
                    return S_FALSE; // Special return code to signal device change
                }
                LogError("Failed to release buffer", hr);
                CloseHandle(waitEvent);
                return hr;
            }
            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                // Check if this is a device disconnect error
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == E_INVALIDARG) {
                    LogError("Audio device disconnected while getting next packet size", hr);
                    CloseHandle(waitEvent);
                    return S_FALSE; // Special return code to signal device change
                }
                LogError("Failed to get next packet size", hr);
                CloseHandle(waitEvent);
                return hr;
            }
        }
    }

    CloseHandle(waitEvent);
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

    // Create device change event
    g_deviceChangeEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_deviceChangeEvent == NULL) {
        LogError("Failed to create device change event", GetLastError());
        closesocket(sock);
        WSACleanup();
        CoUninitialize();
        return 1;
    }

    // Create and set up the notification client
    CMMNotificationClient* pNotificationClient = new CMMNotificationClient();
    if (!pNotificationClient) {
        Log("Failed to create notification client");
        CloseHandle(g_deviceChangeEvent);
        closesocket(sock);
        WSACleanup();
        CoUninitialize();
        return 1;
    }

    IMMDeviceEnumerator* pEnumerator = NULL;

    // Create device enumerator
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) {
        LogError("Failed to create device enumerator", hr);
        delete pNotificationClient;
        CloseHandle(g_deviceChangeEvent);
        closesocket(sock);
        WSACleanup();
        CoUninitialize();
        return 1;
    }

    // Set enumerator for the notification client
    pNotificationClient->SetEnumerator(pEnumerator);

    // Register for notifications
    hr = pEnumerator->RegisterEndpointNotificationCallback(pNotificationClient);
    if (FAILED(hr)) {
        LogError("Failed to register for endpoint notifications", hr);
        pEnumerator->Release();
        delete pNotificationClient;
        CloseHandle(g_deviceChangeEvent);
        closesocket(sock);
        WSACleanup();
        CoUninitialize();
        return 1;
    }

    while (true) {
        IMMDevice* pDevice = NULL;
        IAudioClient* pAudioClient = NULL;
        IAudioCaptureClient* pCaptureClient = NULL;
        WAVEFORMATEX* pwfx = NULL;

        // Get the default audio endpoint
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (FAILED(hr)) {
            LogError("Failed to get default audio endpoint", hr);
            Sleep(1000); // Wait before retrying
            continue;
        }

        // Get device name for logging
        std::string deviceName = GetDeviceName(pDevice);
        Log("Using audio device: " + deviceName);

        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
        if (FAILED(hr)) {
            LogError("Failed to activate audio client", hr);
            pDevice->Release();
            Sleep(1000); // Wait before retrying
            continue;
        }

        hr = pAudioClient->GetMixFormat(&pwfx);
        if (FAILED(hr)) {
            LogError("Failed to get mix format", hr);
            pAudioClient->Release();
            pDevice->Release();
            Sleep(1000); // Wait before retrying
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
            Sleep(1000); // Wait before retrying
            continue;
        }

        hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
        if (FAILED(hr)) {
            LogError("Failed to get audio capture client", hr);
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            Sleep(1000); // Wait before retrying
            continue;
        }

        hr = pAudioClient->Start();
        if (FAILED(hr)) {
            LogError("Failed to start audio client", hr);
            pCaptureClient->Release();
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            Sleep(1000); // Wait before retrying
            continue;
        }

        Log("Starting audio capture");
        // The return value from CaptureAudio now has special meaning:
        // S_OK = Normal exit, should not happen unless we add a way to exit gracefully
        // S_FALSE = Device change detected, should reconnect
        // Failed HRESULT = Error occurred, may need to delay before retrying
        hr = CaptureAudio(pAudioClient, pCaptureClient, reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx), sock, remoteAddr);
        
        Log("Cleaning up resources");
        pAudioClient->Stop();
        CoTaskMemFree(pwfx);
        pCaptureClient->Release();
        pAudioClient->Release();
        pDevice->Release();

        if (FAILED(hr)) {
            LogError("Audio capture failed with error", hr);
            Sleep(1000); // Wait before retrying on hard errors
        } else if (hr == S_FALSE) {
            Log("Audio device changed, reconnecting immediately");
            // No delay, reconnect immediately for device changes
        } else {
            Log("Audio capture completed normally");
            // This should not happen in current implementation
        }
    }

    // This part will never be reached in the current implementation
    pEnumerator->UnregisterEndpointNotificationCallback(pNotificationClient);
    pEnumerator->Release();
    delete pNotificationClient;
    CloseHandle(g_deviceChangeEvent);
    closesocket(sock);
    WSACleanup();
    CoUninitialize();

    Log("Application ended");

    return 0;
}
