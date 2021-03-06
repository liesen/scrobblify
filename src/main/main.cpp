#include "stdafx.h"
#include "resource.h"
#include "scrobblify.h"
#include <shellapi.h>
#include <wchar.h>
#include <vector>


// Current instance
HINSTANCE hInst;
HWND hwnd;

// Application level mutex
HANDLE application_mutex;

const LPCWSTR kApplicationName = _T("Scrobblify");

NOTIFYICONDATA tray_icon;

// Window class name -- use MsnMsgrUIManager to receive messages from Spotify
const wchar_t kMsnMessengerClassName[] = _T("MsnMsgrUIManager");

// Number identifying an MSN "play this-notification"
const unsigned long kMsnMagicNumber = 0x547;

// The magic
Scrobblify scrobblify;


void InitTray(HWND hWnd) {
  ZeroMemory(&tray_icon, NOTIFYICONDATA_V2_SIZE);
  tray_icon.cbSize = NOTIFYICONDATA_V2_SIZE;
  tray_icon.uID = IDM_TRAYICON;
  tray_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  tray_icon.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SCROBBLIFY));
  tray_icon.hWnd = hWnd;
  tray_icon.uCallbackMessage = WM_USER;
  wcscpy(tray_icon.szTip, kApplicationName); // Tooltip text
  Shell_NotifyIcon(NIM_ADD, &tray_icon);
}

/**
 * Forwards a message to MSN Live Messenger.
 */
void NotifyMsnMessenger(PCOPYDATASTRUCT data) {
  for (HWND w = NULL;
       (w = FindWindowEx(NULL, w, kMsnMessengerClassName, NULL)) != NULL;
       ) {
    // Do not send to Scrobblify
    if (w != hwnd) {
      // SendNotifyMessage doesn't block like SendMessage
      SendNotifyMessage(w, 
                        WM_COPYDATA,
                        (WPARAM) kMsnMagicNumber,
                        (LPARAM) data);
    }
  }
}

/**
 * Handles the WM_COPYDATA event if the target is an MsnMsgrUIManager-window.
 * Parses the input data and scrobbles the song.
 */
LRESULT CALLBACK OnCopyData(HWND hWnd, WPARAM w, LPARAM lParam) {
  PCOPYDATASTRUCT cds = (PCOPYDATASTRUCT) lParam;
  
  // TODO(liesen): uncomment when this works
  // NotifyMsnMessenger(cds);
  
  // Input is of the form: \0Music\0<status>\0<format>\0<song>\0<performer>\0
  std::wstring data = (WCHAR *) cds->lpData; // Null terminated
  std::wstring delimiter = _T("\\0"); // Field delimiter
  size_t delimiter_size = delimiter.size();
  std::wstring music = delimiter + _T("Music") + delimiter;
  size_t p = music.size(); // Index of next field
  
  // Data must begin with \0Music\0
  if (data.compare(0, p, music) != 0) {
    return 0;
  }

  // The second field contains some kind of status: 
  //   0    paused (or stopped)
  //   1    playing
  BOOL play = data.compare(p, 1 + delimiter_size, _T("1") + delimiter) == 0;
  p += 1 + delimiter_size;

  if (!play) {
    // Can't distinguish between pause and stop
    scrobblify.Stop();
	return 0;
  }

  std::wstring format = data.substr(p, data.find(delimiter, p) - p); 
  p += format.size() + delimiter_size;
  
  std::wstring title = data.substr(p, data.find(delimiter, p) - p);
  p += title.size() + delimiter_size;
  
  std::wstring artist = data.substr(p, data.find(delimiter, p) - p);
  p += artist.size();
  
  // Ensure that the string also ends with the delimiter
  if (data.compare(p, delimiter_size, delimiter) != 0) {
     // Skip anyway...
  }

  try {
    scrobblify.Start(artist, title);
  } catch (...) {
    return 1;
  }

  return 0;
}


/**
 * Initializes Scrobblify. Exits on any problem.
 */
void InitScrobbler() {
  try {
    scrobblify.Init();
  } catch (const std::exception& e) {
    MessageBoxA(hwnd,
                e.what(),
                "Failure during initialization",
                MB_ICONERROR | MB_TOPMOST | MB_OK);
    exit(EXIT_FAILURE);
  }
}

void ShowContextMenu(HWND hWnd) {
  POINT pt;
  GetCursorPos(&pt);
  HMENU tray_menu = CreatePopupMenu();

  if (tray_menu) {
    AppendMenu(tray_menu, MF_STRING, WM_DESTROY, _T("Exit"));
    SetForegroundWindow(hWnd);
    TrackPopupMenu(tray_menu, TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(tray_menu);
  }
}

LRESULT CALLBACK WndProc(HWND hWnd,
                         UINT message,
                         WPARAM wParam,
                         LPARAM lParam) {
  int wm_id, wm_event;

  switch (message)
  {
  case WM_CREATE:
    InitTray(hWnd);
    InitScrobbler();
    break;
  case WM_DESTROY:
    Shell_NotifyIcon(NIM_DELETE, &tray_icon);

    // Release application level lock
    if (application_mutex) {
      CloseHandle(application_mutex);
      application_mutex = NULL;
    }

    PostQuitMessage(0);
    break;
  case WM_USER:
    switch (lParam) {
    case WM_RBUTTONDOWN:
    case WM_CONTEXTMENU:
      ShowContextMenu(hWnd);
      break;
    }
    break;
  case WM_COMMAND:
    wm_id = LOWORD(wParam);
    wm_event = HIWORD(wParam);

    switch (wm_id) {
      case WM_DESTROY:
        DestroyWindow(hWnd);
        break;
    }
    break;
  case WM_COPYDATA:
    if (((PCOPYDATASTRUCT) lParam)->dwData == kMsnMagicNumber) {
      OnCopyData(hWnd, wParam, lParam);
    }
    break;
  }

  return DefWindowProc(hWnd, message, wParam, lParam);
}




/**
 * Saves instance handle and creates main window.
 */
bool InitInstance(HINSTANCE hInstance, int nCmdShow) {
  // Only allow one instance of the program
  application_mutex = CreateMutex(NULL, FALSE, kApplicationName);

  if (ERROR_ALREADY_EXISTS == GetLastError()) {
    return false;
  }
  
  // Check if MSN is running
  if (FindWindowEx(NULL, NULL, kMsnMessengerClassName, NULL) != NULL) {
    MessageBox(hwnd,
               _T("It seems like MSN Live Messenger is running. Scrobblify \
                  will not receive Spotify notifications and scrobbling \
                  will not work. Exit MSN Live Messenger in order to \
                  scrobble."),
               _T("Can't scrobble when MSN Live Messenger is running"),
               MB_ICONWARNING | MB_TOPMOST | MB_OK);
  }

  hInst = hInstance;
  hwnd = CreateWindow(kMsnMessengerClassName,
                      kApplicationName,
                      WS_MINIMIZE,
                      CW_USEDEFAULT,
                      CW_USEDEFAULT,
                      CW_USEDEFAULT,
                      CW_USEDEFAULT,
                      HWND_MESSAGE,
                      NULL,
                      hInstance,
                      NULL);

  if (!hwnd) {
    return false;
  }

  // ShowWindow(hWnd, nCmdShow);
  UpdateWindow(hwnd);
  return true;
}


int APIENTRY _tWinMain(HINSTANCE hInstance, 
                       HINSTANCE hPrevInstance, 
                       LPTSTR lpCmdLine, 
                       int nCmdShow) {
  UNREFERENCED_PARAMETER(hPrevInstance);
  UNREFERENCED_PARAMETER(lpCmdLine);

  // Register class
  WNDCLASSEX wcex;
  wcex.cbSize = sizeof(wcex);
  wcex.style = 0;
  wcex.lpfnWndProc = WndProc;
  wcex.cbClsExtra = 0;
  wcex.cbWndExtra = 0;
  wcex.hInstance = hInstance;
  wcex.hIcon = LoadIcon(hInstance, (LPCTSTR) IDI_SCROBBLIFY);
  wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
  wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
  wcex.lpszMenuName = NULL;
  wcex.lpszClassName = kMsnMessengerClassName;
  wcex.hIconSm = NULL;
  RegisterClassEx(&wcex);

  if (!InitInstance(hInstance, nCmdShow)) {
    return FALSE;
  }

  MSG msg;

  while (GetMessage(&msg, NULL, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  return (int) msg.wParam;
}


