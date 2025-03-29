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
#include <sstream>
#include <ws2ipdef.h>
#include <windows.h>
#include <shellapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")

// Global variables
static std::ofstream g_logFile;
static bool g_logging = false;

std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    std::ostringstream ss;
    ss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void LogFormat(const std::string& format, const std::vector<std::string>& values) {
    if (!g_logging) return;
    std::ostringstream ss;
    for (const auto& value : values) {
        ss << "  " << value << "\n";
    }
    std::string logMessage = format + ":\n" + ss.str();
    printf("%s", logMessage.c_str());
    if (g_logFile.is_open()) {
        g_logFile << logMessage;
        g_logFile.flush();
    }
}

void Log(const std::string& message) {
    if (!g_logging) return;
    std::string timestamp = GetTimestamp();
    std::string logMessage = timestamp + " | " + message;
    printf("%s\n", logMessage.c_str());
    if (g_logFile.is_open()) {
        g_logFile << logMessage << std::endl;
        g_logFile.flush();
    }
}

void LogError(const std::string& message, HRESULT hr) {
    if (!g_logging) return;
    std::string timestamp = GetTimestamp();
    std::string logMessage = timestamp + " | ERROR " + std::to_string(hr) + " : " + message;
    printf("%s\n", logMessage.c_str());
    if (g_logFile.is_open()) {
        g_logFile << logMessage << std::endl;
        g_logFile.flush();
    }
}

// Device change event handle
HANDLE g_deviceChangeEvent = NULL;

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

#define BUFFER_SIZE 1152
#define HEADER_SIZE 5

// Modified to handle device disconnections
HRESULT CaptureAudio(IAudioClient* pAudioClient, IAudioCaptureClient* pCaptureClient, WAVEFORMATEXTENSIBLE* pwfex, SOCKET sock, sockaddr_in remoteAddr) {
    if (!pCaptureClient) {
        return E_POINTER;
    }

    UINT32 packetLength = 0;
    BYTE* pData;
    UINT32 numFramesAvailable;
    DWORD flags;
    char buffer[BUFFER_SIZE + HEADER_SIZE] = { 0 };

    Log("Starting audio capture loop");

    char pcmBuffer[BUFFER_SIZE * 512] = { 0 };
    uint32_t pcmBufferHead = 0;
    
    HANDLE events[2] = { g_deviceChangeEvent, NULL };
    HANDLE waitEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (waitEvent == NULL) {
        LogError("Failed to create wait event", GetLastError());
        return E_FAIL;
    }
    events[1] = waitEvent;

    while (true) {
        // Wait for audio data (small timeout)
        Sleep(1);

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
                
                // Check if we have enough space
                if (pcmBufferHead + bytesToCopy > sizeof(pcmBuffer)) {
                    LogError("Buffer overflow prevented", E_BOUNDS);
                    hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
                    if (FAILED(hr)) {
                        LogError("Failed to release buffer", hr);
                        CloseHandle(waitEvent);
                        return hr;
                    }
                    continue;
                }

                memcpy(pcmBuffer + pcmBufferHead, pData, bytesToCopy);
                pcmBufferHead += bytesToCopy;
                while (pcmBufferHead >= BUFFER_SIZE) {
                    bool is_44100 = pwfex->Format.nSamplesPerSec % 44100 == 0;
                    int sample_mask = pwfex->Format.nSamplesPerSec / (is_44100 ? 44100 : 48000);
                    sample_mask |= is_44100 << 7;
                    buffer[0] = static_cast<char>(sample_mask);
                    buffer[1] = static_cast<char>(pwfex->Format.wBitsPerSample);
                    buffer[2] = static_cast<char>(pwfex->Format.nChannels);
                    buffer[3] = static_cast<BYTE>((pwfex->dwChannelMask >> 8) & 0xFF);
                    buffer[4] = static_cast<BYTE>(pwfex->dwChannelMask & 0xFF);

                    memcpy(buffer + HEADER_SIZE, pcmBuffer, BUFFER_SIZE);
                    int sendResult = sendto(sock, buffer, BUFFER_SIZE + HEADER_SIZE, 0, (sockaddr*)&remoteAddr, sizeof(remoteAddr));
                    if (sendResult == SOCKET_ERROR) {
                        Log("Failed to send data over UDP");
                    }

                    for (size_t i = 0; i < pcmBufferHead - BUFFER_SIZE; i++)
                        pcmBuffer[i] = pcmBuffer[i + BUFFER_SIZE];
                    pcmBufferHead -= BUFFER_SIZE;
                }
            }
            hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr)) {
                LogError("Failed to release buffer", hr);
                CloseHandle(waitEvent);
                return hr;
            }
            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
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
    printf("Application starting...\n"); // Keep this one as printf since Log isn't initialized yet

    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2) {
    printf("Usage: program.exe <IP> [port] [-m] [-l]\n"); // Keep as printf since Log isn't initialized
    printf(" Default port 16401\n");
    printf(" -m Enable Multicast\n");
    printf(" -l Enable Logging\n");
        LocalFree(argv);
        return 1;
    }

    std::wstring wRemoteIP(argv[1]);
    std::string REMOTE_IP;
    REMOTE_IP.reserve(wRemoteIP.length());
    for (wchar_t wc : wRemoteIP) {
        REMOTE_IP.push_back(static_cast<char>(wc & 0xFF));
    }
    int REMOTE_PORT = 16401;
    bool multicast = false;
    for (int i = 2; i < argc; i++) {
        if (wcscmp(argv[i], L"-m") == 0) {
            multicast = true;
        }
        else if (wcscmp(argv[i], L"-l") == 0) {
            g_logging = true;
        }
        else {
            try {
                REMOTE_PORT = std::stoi(std::wstring(argv[i]));
            }
            catch (const std::exception&) {
                printf("Invalid port number. Using default port 16401.\n");
            }
        }
    }

    LocalFree(argv);

    // Only open log file if logging is enabled
    if (g_logging) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        struct tm timeinfo;
        localtime_s(&timeinfo, &time);
        std::ostringstream ss;
        ss << "scream_sender_" << REMOTE_IP << "_" << REMOTE_PORT << "_" 
           << std::put_time(&timeinfo, "%Y%m%d_%H%M%S") << ".log";
        g_logFile.open(ss.str(), std::ios::out | std::ios::app);
        if (!g_logFile.is_open()) {
            printf("Failed to open log file\n");
            return 1;
        }

        Log("Application started");
        Log("Connecting to " + REMOTE_IP + ":" + std::to_string(REMOTE_PORT) + (multicast ? " (multicast)" : ""));
    }

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

        // Create a WAVEFORMATEXTENSIBLE format for multichannel PCM
        WAVEFORMATEX *pwfxClosest = NULL;
        WAVEFORMATEXTENSIBLE wfxExt = {0};
        
        // Copy the original format exactly
        memcpy(&wfxExt, pwfx, sizeof(WAVEFORMATEXTENSIBLE));
        
        // Just change the SubFormat to PCM
        wfxExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

        // Log original mix format
        LogFormat("Original Mix Format", {
            "wFormatTag: " + std::to_string(pwfx->wFormatTag),
            "nChannels: " + std::to_string(pwfx->nChannels),
            "nSamplesPerSec: " + std::to_string(pwfx->nSamplesPerSec),
            "nAvgBytesPerSec: " + std::to_string(pwfx->nAvgBytesPerSec),
            "nBlockAlign: " + std::to_string(pwfx->nBlockAlign),
            "wBitsPerSample: " + std::to_string(pwfx->wBitsPerSample),
            "cbSize: " + std::to_string(pwfx->cbSize)
        });

        // Log requested format
        LogFormat("Requested Format", {
            "wFormatTag: " + std::to_string(wfxExt.Format.wFormatTag),
            "nChannels: " + std::to_string(wfxExt.Format.nChannels),
            "nSamplesPerSec: " + std::to_string(wfxExt.Format.nSamplesPerSec),
            "nAvgBytesPerSec: " + std::to_string(wfxExt.Format.nAvgBytesPerSec),
            "nBlockAlign: " + std::to_string(wfxExt.Format.nBlockAlign),
            "wBitsPerSample: " + std::to_string(wfxExt.Format.wBitsPerSample),
            "cbSize: " + std::to_string(wfxExt.Format.cbSize)
        });

        // Check if the format is supported
        hr = pAudioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, reinterpret_cast<WAVEFORMATEX*>(&wfxExt), &pwfxClosest);
        if (hr == S_FALSE && pwfxClosest) {
            LogFormat("Using closest supported format", {
                "wFormatTag: " + std::to_string(pwfxClosest->wFormatTag),
                "nChannels: " + std::to_string(pwfxClosest->nChannels),
                "nSamplesPerSec: " + std::to_string(pwfxClosest->nSamplesPerSec),
                "nAvgBytesPerSec: " + std::to_string(pwfxClosest->nAvgBytesPerSec),
                "nBlockAlign: " + std::to_string(pwfxClosest->nBlockAlign),
                "wBitsPerSample: " + std::to_string(pwfxClosest->wBitsPerSample),
                "cbSize: " + std::to_string(pwfxClosest->cbSize)
            });

            // Use the closest supported format
            memcpy(&wfxExt, pwfxClosest, sizeof(WAVEFORMATEX) + pwfxClosest->cbSize);
            CoTaskMemFree(pwfxClosest);
        } else if (FAILED(hr)) {
            LogError("Format not supported", hr);
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            continue;
        }

        // Initialize with the format
        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, reinterpret_cast<WAVEFORMATEX*>(&wfxExt), NULL);
        if (FAILED(hr)) {
            LogError("Failed to initialize audio client", hr);
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            continue;
        }

        Log("Attempting to get capture service");
        hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
        if (FAILED(hr)) {
            LogError("Failed to get audio capture client", hr);
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            continue;
        }
        Log("Successfully got capture service");
        if (!pCaptureClient) {
            LogError("pCaptureClient is null after successful GetService", hr);
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            continue;
        }

        // Get device name for logging
        std::string deviceName = GetDeviceName(pDevice);
        Log("Using audio device: " + deviceName);

        hr = pAudioClient->Start();
        if (FAILED(hr)) {
            LogError("Failed to start audio client", hr);
            pCaptureClient->Release();
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            continue;
        }

        Log("Starting audio capture");
        hr = CaptureAudio(pAudioClient, pCaptureClient, &wfxExt, sock, remoteAddr);
        if (FAILED(hr)) {
            LogError("Audio capture failed", hr);
            Sleep(1000); // Wait before retrying on hard errors
        }
        else if (hr == S_FALSE) {
            Log("Audio device changed, reconnecting immediately");
            // No delay, reconnect immediately for device changes
        }
        else {
            Log("Audio capture completed normally");
            // This should not happen in current implementation
        }

        Log("Cleaning up resources");
        pAudioClient->Stop();
        CoTaskMemFree(pwfx);
        pCaptureClient->Release();
        pAudioClient->Release();
        pDevice->Release();

        Log("Restarting audio capture...");
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

    // Close log file
    if (g_logFile.is_open()) {
        g_logFile.close();
    }

    return 0;
}
