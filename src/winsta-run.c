// winsta-run.c — run a program on a private desktop of the calling window station.
//
// DeepSeek Harness uses this launcher to keep agent-run GUI windows, tests and
// automation off the user's visible desktop. The launcher:
//   1. creates (or opens) a desktop by name in the *current* window station
//      (WinSta0 for an interactive session),
//   2. starts the requested program with STARTUPINFO.lpDesktop = "<name>",
//   3. forwards the caller's standard handles (bytes flow straight through),
//   4. waits for the child and mirrors its exit code.
//
// The child inherits the isolated desktop, so every process it starts stays
// there. Nothing is displayed on the user's desktop and no input is taken.
//
// This program is deliberately small and side-effect free apart from the
// desktop object it creates: no files (except --probe/--selftest), no
// services, no scheduled tasks, no ACL or integrity-label changes.
//
// Modes:
//   winsta-run.exe --diag                 print capability facts as KEY=VALUE
//   winsta-run.exe --selftest             end-to-end isolation proof
//   winsta-run.exe --list                 enumerate window stations/desktops
//   winsta-run.exe --probe <file>         internal: report this process desktop
//   winsta-run.exe --desktop <name> -- <program> [args...]

#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winuser.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

// This SDK's winuser.h ships the individual DESKTOP_* rights but not the
// combined mask, and does not declare DeleteDesktopW. Provide both.
#ifndef DESKTOP_ALL_ACCESS
#define DESKTOP_ALL_ACCESS                              \
    (STANDARD_RIGHTS_REQUIRED | DESKTOP_READOBJECTS |    \
     DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU |         \
     DESKTOP_HOOKCONTROL | DESKTOP_JOURNALRECORD |       \
     DESKTOP_JOURNALPLAYBACK | DESKTOP_ENUMERATE |       \
     DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP)
#endif

#define EXIT_LAUNCHER_FAILURE 1

typedef BOOL(WINAPI *PFN_DeleteDesktopW)(HDESK);
static PFN_DeleteDesktopW pDeleteDesktopW;

static void ResolveOptionalApis(void)
{
    pDeleteDesktopW = (PFN_DeleteDesktopW)(void *)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "DeleteDesktopW");
}

// --- output ----------------------------------------------------------------

// Write one wide string to a stream as UTF-8. The child's own bytes are never
// touched; this only formats launcher diagnostics.
static void PutUtf8(FILE *stream, const WCHAR *text)
{
    char buffer[2048];
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, (int)sizeof(buffer), NULL, NULL);
    if (length > 0) fwrite(buffer, 1, (size_t)length - 1, stream);
}

static void Err(const WCHAR *what)
{
    DWORD error = GetLastError();
    WCHAR *message = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, error, 0, (LPWSTR)&message, 0, NULL);
    fputs("dsh-winsta-run: ", stderr);
    PutUtf8(stderr, what);
    fprintf(stderr, " failed win32=%lu ", (unsigned long)error);
    if (message != NULL) {
        WCHAR *cursor = message;
        while (*cursor != L'\0' && *cursor != L'\r' && *cursor != L'\n') cursor++;
        *cursor = L'\0';
        PutUtf8(stderr, message);
        LocalFree(message);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

// --- names -----------------------------------------------------------------

static void UserObjectName(HANDLE object, WCHAR *buffer, DWORD capacity)
{
    DWORD needed = 0;
    buffer[0] = L'\0';
    GetUserObjectInformationW(object, UOI_NAME, buffer, capacity, &needed);
}

static int IntegrityRid(void)
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return -1;
    BYTE buffer[2048];
    DWORD size = 0;
    int rid = -1;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
        TOKEN_MANDATORY_LABEL *label = (TOKEN_MANDATORY_LABEL *)buffer;
        rid = (int)*GetSidSubAuthority(label->Label.Sid,
                                       *GetSidSubAuthorityCount(label->Label.Sid) - 1);
    }
    CloseHandle(token);
    return rid;
}

// --- desktop creation ------------------------------------------------------

// Create the named desktop in the calling process's window station, or open
// it when another launcher already created it. Never deletes a shared desktop.
static HDESK OpenOrCreateDesktop(const WCHAR *name)
{
    HDESK desktop = CreateDesktopW(name, NULL, NULL, 0, DESKTOP_ALL_ACCESS, NULL);
    if (desktop != NULL) return desktop;
    DWORD createError = GetLastError();
    desktop = OpenDesktopW(name, 0, FALSE, DESKTOP_ALL_ACCESS);
    if (desktop == NULL) SetLastError(createError);
    return desktop;
}

// --- argument quoting ------------------------------------------------------

// Quote one argument using the CommandLineToArgvW parsing rules so that the
// child's CRT parses back exactly the same argv the caller passed.
static void AppendQuoted(WCHAR *buffer, size_t capacity, const WCHAR *argument)
{
    size_t length = wcslen(buffer);
    size_t count = wcslen(argument);

#define APPEND_CHAR(ch) do { if (length + 1 < capacity) buffer[length++] = (ch); } while (0)

    if (count == 0) {
        APPEND_CHAR(L'"');
        APPEND_CHAR(L'"');
    } else if (wcspbrk(argument, L" \t\"") == NULL) {
        for (size_t index = 0; index < count; index++) APPEND_CHAR(argument[index]);
    } else {
        APPEND_CHAR(L'"');
        size_t index = 0;
        while (index < count) {
            size_t backslashes = 0;
            while (index < count && argument[index] == L'\\') {
                backslashes++;
                index++;
            }
            if (index == count) {
                for (size_t i = 0; i < backslashes * 2; i++) APPEND_CHAR(L'\\');
            } else if (argument[index] == L'"') {
                for (size_t i = 0; i < backslashes * 2 + 1; i++) APPEND_CHAR(L'\\');
                APPEND_CHAR(L'"');
                index++;
            } else {
                for (size_t i = 0; i < backslashes; i++) APPEND_CHAR(L'\\');
                APPEND_CHAR(argument[index]);
                index++;
            }
        }
        APPEND_CHAR(L'"');
    }
#undef APPEND_CHAR
    buffer[length] = L'\0';
}

// --- child launch ----------------------------------------------------------

// Start one argv on the named desktop with the caller's standard handles.
// The environment and working directory are inherited from the launcher, which
// the caller already prepared (cwd, TMP/TEMP rewrites, sandbox environment).
static int LaunchOnDesktop(const WCHAR *desktopName, int childArgc, WCHAR **childArgv)
{
    size_t commandCapacity = 32768;
    WCHAR *commandLine = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                            commandCapacity * sizeof(WCHAR));
    if (commandLine == NULL) {
        fputs("dsh-winsta-run: out of memory building the command line\n", stderr);
        return EXIT_LAUNCHER_FAILURE;
    }
    commandLine[0] = L'\0';
    for (int index = 0; index < childArgc; index++) {
        AppendQuoted(commandLine, commandCapacity, childArgv[index]);
        if (index + 1 < childArgc) wcsncat_s(commandLine, commandCapacity, L" ", _TRUNCATE);
    }

    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.lpDesktop = (LPWSTR)desktopName;
    startup.dwFlags = 0;

    HANDLE standard[3];
    standard[0] = GetStdHandle(STD_INPUT_HANDLE);
    standard[1] = GetStdHandle(STD_OUTPUT_HANDLE);
    standard[2] = GetStdHandle(STD_ERROR_HANDLE);
    BOOL inherit = FALSE;
    for (int index = 0; index < 3; index++) {
        if (standard[index] != NULL && standard[index] != INVALID_HANDLE_VALUE) {
            SetHandleInformation(standard[index], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            inherit = TRUE;
        }
    }
    if (inherit) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = standard[0];
        startup.hStdOutput = standard[1];
        startup.hStdError = standard[2];
    }

    PROCESS_INFORMATION process;
    ZeroMemory(&process, sizeof(process));
    BOOL created = CreateProcessW(NULL, commandLine, NULL, NULL, inherit ? TRUE : FALSE,
                                  0, NULL, NULL, &startup, &process);
    HeapFree(GetProcessHeap(), 0, commandLine);
    if (!created) {
        Err(L"CreateProcessW(lpDesktop)");
        return EXIT_LAUNCHER_FAILURE;
    }

    CloseHandle(process.hThread);
    DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
    if (waited != WAIT_OBJECT_0) {
        Err(L"WaitForSingleObject");
        CloseHandle(process.hProcess);
        return EXIT_LAUNCHER_FAILURE;
    }
    DWORD exitCode = EXIT_LAUNCHER_FAILURE;
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
        Err(L"GetExitCodeProcess");
        CloseHandle(process.hProcess);
        return EXIT_LAUNCHER_FAILURE;
    }
    CloseHandle(process.hProcess);
    return (int)exitCode;
}

// --- probe -----------------------------------------------------------------

static BOOL CALLBACK CollectWindowProc(HWND window, LPARAM parameter)
{
    (void)parameter;
    (void)window;
    int *count = (int *)parameter;
    if (count != NULL) (*count)++;
    return TRUE;
}

typedef struct {
    int count;
    int found;
    WCHAR needle[128];
} WindowSearch;

static BOOL CALLBACK SearchWindowProc(HWND window, LPARAM parameter)
{
    WindowSearch *search = (WindowSearch *)parameter;
    WCHAR title[256] = L"";
    GetWindowTextW(window, title, 256);
    search->count++;
    if (wcsstr(title, search->needle) != NULL) search->found = 1;
    return TRUE;
}

static void WriteUtf8File(const WCHAR *path, const WCHAR *text)
{
    char buffer[8192];
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, (int)sizeof(buffer), NULL, NULL);
    if (length <= 0) return;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, buffer, (DWORD)(length - 1), &written, NULL);
    CloseHandle(file);
}

static BOOL ReadUtf8File(const WCHAR *path, WCHAR *buffer, DWORD capacity)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(file, NULL);
    BOOL ok = FALSE;
    if (size < 65536) {
        char *raw = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
        DWORD got = 0;
        if (raw != NULL && ReadFile(file, raw, size, &got, NULL)) {
            raw[got] = '\0';
            int length = MultiByteToWideChar(CP_UTF8, 0, raw, (int)got, buffer, (int)capacity - 1);
            if (length >= 0) {
                buffer[length] = L'\0';
                ok = TRUE;
            }
        }
        if (raw != NULL) HeapFree(GetProcessHeap(), 0, raw);
    }
    CloseHandle(file);
    return ok;
}

static int Probe(const WCHAR *outputPath)
{
    WCHAR station[256] = L"?";
    WCHAR desktop[256] = L"?";
    UserObjectName(GetProcessWindowStation(), station, sizeof(station));
    UserObjectName(GetThreadDesktop(GetCurrentThreadId()), desktop, sizeof(desktop));

    WNDCLASSEXW windowClass;
    ZeroMemory(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(NULL);
    windowClass.lpszClassName = L"DshIsoProbeClass";
    BOOL windowOk = FALSE;
    HWND window = NULL;
    if (RegisterClassExW(&windowClass)) {
        window = CreateWindowExW(0, windowClass.lpszClassName, L"DSH-ISO-PROBE",
                                 WS_OVERLAPPEDWINDOW, 60, 60, 420, 300,
                                 NULL, NULL, windowClass.hInstance, NULL);
        if (window != NULL) {
            ShowWindow(window, SW_SHOWNORMAL);
            windowOk = TRUE;
        }
    }

    WCHAR text[2048];
    _snwprintf_s(text, 2048, _TRUNCATE,
                 L"STATION=%ls\nDESKTOP=%ls\nWINDOW_OK=%d\nHWND=%p\n",
                 station, desktop, windowOk ? 1 : 0, (void *)window);
    WriteUtf8File(outputPath, text);

    if (window != NULL) {
        MSG message;
        DWORD end = GetTickCount() + 2500;
        while (GetTickCount() < end) {
            while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(20);
        }
        DestroyWindow(window);
    }
    return 0;
}

// --- diagnostics -----------------------------------------------------------

static int Diag(void)
{
    int rid = IntegrityRid();
    WCHAR station[256] = L"?";
    UserObjectName(GetProcessWindowStation(), station, sizeof(station));

    // Prove the capability this launcher relies on: create and remove one
    // private desktop in the current station.
    WCHAR probeName[128];
    _snwprintf_s(probeName, 128, _TRUNCATE, L"DSH-DIAG-%lu", GetCurrentProcessId());
    BOOL desktopOk = FALSE;
    DWORD desktopError = 0;
    HDESK desktop = OpenOrCreateDesktop(probeName);
    if (desktop != NULL) {
        desktopOk = TRUE;
        if (pDeleteDesktopW != NULL) pDeleteDesktopW(desktop);
        CloseDesktop(desktop);
    } else {
        desktopError = GetLastError();
    }

    // Report whether this process is on the session's input desktop: synthesized
    // input only ever reaches that one, so a 0 here is expected and important.
    int inputDesktop = 0;
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (input != NULL) {
        WCHAR inputName[256] = L"?";
        WCHAR threadName[256] = L"?";
        UserObjectName(input, inputName, sizeof(inputName));
        UserObjectName(GetThreadDesktop(GetCurrentThreadId()), threadName, sizeof(threadName));
        inputDesktop = _wcsicmp(inputName, threadName) == 0 ? 1 : 0;
        CloseDesktop(input);
    }

    fputs("INTEGRITY_RID=", stdout);
    fprintf(stdout, "%d", rid);
    fputs("\nSTATION=", stdout);
    PutUtf8(stdout, station);
    fputs("\nDESKTOP_OK=", stdout);
    fprintf(stdout, "%d", desktopOk ? 1 : 0);
    fputs("\nWIN32=", stdout);
    fprintf(stdout, "%lu", (unsigned long)desktopError);
    fputs("\nINPUT_DESKTOP=", stdout);
    fprintf(stdout, "%d", inputDesktop);
    fputc('\n', stdout);
    fflush(stdout);
    return desktopOk ? 0 : EXIT_LAUNCHER_FAILURE;
}

// --- self test -------------------------------------------------------------

static int SelfTest(void)
{
    int failures = 0;
    WCHAR desktopName[128];
    WCHAR probeFile[MAX_PATH] = L"";
    WCHAR tempPath[MAX_PATH];
    WCHAR exe[MAX_PATH];

    GetModuleFileNameW(NULL, exe, MAX_PATH);
    DWORD tempLength = GetTempPathW(MAX_PATH, tempPath);
    if (tempLength == 0 || tempLength > MAX_PATH) tempPath[0] = L'\0';
    _snwprintf_s(probeFile, MAX_PATH, _TRUNCATE, L"%lsdsh-winsta-probe-%lu.txt", tempPath, GetCurrentProcessId());
    DeleteFileW(probeFile);

    _snwprintf_s(desktopName, 128, _TRUNCATE, L"DSH-SELFTEST-%lu", GetCurrentProcessId());
    HDESK desktop = OpenOrCreateDesktop(desktopName);
    if (desktop == NULL) {
        fputs("FAIL  create desktop\n", stdout);
        Err(L"CreateDesktopW");
        return EXIT_LAUNCHER_FAILURE;
    }
    fputs("PASS  desktop  ", stdout);
    PutUtf8(stdout, desktopName);
    fputc('\n', stdout);

    // Launch ourselves as the probe on the isolated desktop.
    WCHAR *childArgv[4];
    childArgv[0] = exe;
    childArgv[1] = L"--probe";
    childArgv[2] = probeFile;
    childArgv[3] = NULL;

    WCHAR commandLine[4096] = L"";
    AppendQuoted(commandLine, 4096, exe);
    wcsncat_s(commandLine, 4096, L" --probe ", _TRUNCATE);
    AppendQuoted(commandLine, 4096, probeFile);

    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.lpDesktop = desktopName;
    PROCESS_INFORMATION process;
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(NULL, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        fputs("FAIL  CreateProcessW(lpDesktop)\n", stdout);
        Err(L"CreateProcessW(lpDesktop)");
        if (pDeleteDesktopW != NULL) pDeleteDesktopW(desktop);
        CloseDesktop(desktop);
        return EXIT_LAUNCHER_FAILURE;
    }
    CloseHandle(process.hThread);
    fputs("PASS  child started\n", stdout);

    WCHAR report[2048];
    BOOL haveReport = FALSE;
    for (int attempt = 0; attempt < 100 && !haveReport; attempt++) {
        haveReport = ReadUtf8File(probeFile, report, 2048);
        if (!haveReport) Sleep(50);
    }
    if (!haveReport) {
        fputs("FAIL  probe did not report\n", stdout);
        failures++;
        report[0] = L'\0';
    } else {
        WCHAR expected[160];
        _snwprintf_s(expected, 160, _TRUNCATE, L"DESKTOP=%ls", desktopName);
        if (wcsstr(report, expected) != NULL) {
            fputs("PASS  child desktop is the isolated desktop\n", stdout);
        } else {
            fputs("FAIL  child desktop mismatch\n", stdout);
            failures++;
        }
        if (wcsstr(report, L"WINDOW_OK=1") != NULL) {
            fputs("PASS  child created a window on that desktop\n", stdout);
        } else {
            fputs("FAIL  child could not create a window\n", stdout);
            failures++;
        }
    }

    // The probe window must be enumerable on the isolated desktop...
    WindowSearch onIsolated;
    ZeroMemory(&onIsolated, sizeof(onIsolated));
    wcsncpy_s(onIsolated.needle, 128, L"DSH-ISO-PROBE", _TRUNCATE);
    EnumDesktopWindows(desktop, SearchWindowProc, (LPARAM)&onIsolated);
    if (onIsolated.found) {
        fputs("PASS  probe window is on the isolated desktop\n", stdout);
    } else {
        fputs("FAIL  probe window not found on the isolated desktop\n", stdout);
        failures++;
    }

    // ...and absent from the user's Default desktop in the same station.
    HDESK defaultDesktop = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_ENUMERATE);
    if (defaultDesktop == NULL) {
        fputs("WARN  could not open Default desktop for comparison\n", stdout);
    } else {
        WindowSearch onDefault;
        ZeroMemory(&onDefault, sizeof(onDefault));
        wcsncpy_s(onDefault.needle, 128, L"DSH-ISO-PROBE", _TRUNCATE);
        EnumDesktopWindows(defaultDesktop, SearchWindowProc, (LPARAM)&onDefault);
        CloseDesktop(defaultDesktop);
        if (!onDefault.found) {
            fputs("PASS  probe window is invisible on Default\n", stdout);
        } else {
            fputs("FAIL  probe window leaked onto Default\n", stdout);
            failures++;
        }
    }

    if (WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);
        fputs("PASS  probe exited, code=", stdout);
        fprintf(stdout, "%lu\n", (unsigned long)exitCode);
    } else {
        fputs("FAIL  probe did not exit\n", stdout);
        failures++;
    }
    CloseHandle(process.hProcess);
    DeleteFileW(probeFile);

    if (pDeleteDesktopW != NULL) pDeleteDesktopW(desktop);
    CloseDesktop(desktop);

    fprintf(stdout, "\n%s (%d failure(s))\n", failures ? "RESULT: FAIL" : "RESULT: PASS", failures);
    fflush(stdout);
    (void)childArgv;
    (void)CollectWindowProc;
    return failures ? EXIT_LAUNCHER_FAILURE : 0;
}

// --- enumeration -----------------------------------------------------------

static BOOL CALLBACK OnDesktop(WCHAR *name, LPARAM parameter)
{
    (void)parameter;
    fputs("  desktop: ", stdout);
    PutUtf8(stdout, name);
    fputc('\n', stdout);
    return TRUE;
}

static BOOL CALLBACK OnStation(WCHAR *name, LPARAM parameter)
{
    (void)parameter;
    PutUtf8(stdout, name);
    fputc('\n', stdout);
    HWINSTA station = OpenWindowStationW(name, FALSE, WINSTA_ENUMDESKTOPS | WINSTA_READATTRIBUTES);
    if (station == NULL) return TRUE;
    EnumDesktopsW(station, OnDesktop, 0);
    CloseWindowStation(station);
    return TRUE;
}

static int ListObjects(void)
{
    fputs("=== window stations in this session ===\n", stdout);
    EnumWindowStationsW(OnStation, 0);
    fflush(stdout);
    return 0;
}

// --- entry -----------------------------------------------------------------

static int Usage(void)
{
    fputs("usage:\n"
          "  dsh-winsta-run.exe --diag\n"
          "  dsh-winsta-run.exe --selftest\n"
          "  dsh-winsta-run.exe --list\n"
          "  dsh-winsta-run.exe --probe <file>\n"
          "  dsh-winsta-run.exe [--allow-low] --desktop <name> -- <program> [args...]\n",
          stderr);
    return 2;
}

int wmain(int argc, WCHAR **argv)
{
    ResolveOptionalApis();
    if (argc < 2) return Usage();

    if (wcscmp(argv[1], L"--diag") == 0) return Diag();
    if (wcscmp(argv[1], L"--selftest") == 0) return SelfTest();
    if (wcscmp(argv[1], L"--list") == 0) return ListObjects();
    if (wcscmp(argv[1], L"--probe") == 0 && argc >= 3) return Probe(argv[2]);

    int index = 1;
    // A confined sandbox starts this launcher with a lowered token on purpose:
    // the isolated desktop is then created at that integrity, which is exactly
    // the same token the shell command runs under. Unconfined launches must not
    // be lowered silently, so running Low needs this explicit opt-in.
    BOOL allowLowIntegrity = FALSE;
    if (index < argc && wcscmp(argv[index], L"--allow-low") == 0) {
        allowLowIntegrity = TRUE;
        index++;
    }

    if (index < argc && wcscmp(argv[index], L"--desktop") == 0) {
        if (argc - index < 4) {
            fputs("dsh-winsta-run: --desktop requires a name and '-- <program>'\n", stderr);
            return 2;
        }
        const WCHAR *desktopName = argv[index + 1];
        if (desktopName[0] == L'\0' || wcslen(desktopName) > 200 || wcschr(desktopName, L'\\') != NULL) {
            fputs("dsh-winsta-run: invalid desktop name\n", stderr);
            return 2;
        }
        if (wcscmp(argv[index + 2], L"--") != 0) {
            fputs("dsh-winsta-run: expected '--' before the program\n", stderr);
            return 2;
        }
        int childArgc = argc - (index + 3);
        WCHAR **childArgv = argv + index + 3;
        if (childArgc <= 0) {
            fputs("dsh-winsta-run: no program given\n", stderr);
            return 2;
        }
        // A Low-labeled launcher would run every isolated command at Low
        // integrity, silently changing how the user's shell behaves. Fail
        // loudly instead; the fix is moving the bundle off a Low path.
        if (!allowLowIntegrity) {
            int rid = IntegrityRid();
            if (rid >= 0 && rid < 8192) {
                fprintf(stderr,
                        "dsh-winsta-run: refusing to run from a low-integrity launcher (Integrity RID=%d); "
                        "move the plugin bundle to a directory without a Low mandatory integrity label, "
                        "or pass --allow-low for a confined sandbox command\n",
                        rid);
                return EXIT_LAUNCHER_FAILURE;
            }
        }
        HDESK desktop = OpenOrCreateDesktop(desktopName);
        if (desktop == NULL) {
            Err(L"CreateDesktopW/OpenDesktopW");
            return EXIT_LAUNCHER_FAILURE;
        }
        int exitCode = LaunchOnDesktop(desktopName, childArgc, childArgv);
        CloseDesktop(desktop);
        return exitCode;
    }

    fputs("dsh-winsta-run: unknown mode\n", stderr);
    return Usage();
}
