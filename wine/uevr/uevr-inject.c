/*
 * SPDX-License-Identifier: MIT
 * uevr-inject.c - inject UEVR into a running game without its .NET 6 WPF frontend (UEVRInjector.exe), which needs the
 * .NET 6 Desktop Runtime in the Wine prefix and WPF drawing through Wine. It does what that frontend does for an
 * OpenXR injection (read from the frontend's strings and UEVR's VR.cpp, nightly-01143 / 4ee5c6b):
 *   1. writes Frontend_RequestedRuntime=openxr_loader.dll into %APPDATA%\UnrealVRMod\<exe stem>\config.txt
 *   2. LoadLibraryW(<uevr>\openxr_loader.dll) in the game (VR.cpp then takes the "pre-injected" OpenXR path)
 *   3. optionally LoadLibraryW(<uevr>\UEVRPluginNullifier.dll), for games that ship their own VR plugin
 *   4. LoadLibraryW(<uevr>\UEVRBackend.dll)
 * Each load is a CreateRemoteThread at the target's own kernel32!LoadLibraryW, found from the target's module list.
 *
 * usage: uevr-inject.exe <exe name | pid> <uevr dir> [--nullify] [--openvr]
 * build: x86_64-w64-mingw32-gcc -O2 -municode -o uevr-inject.exe uevr-inject.c
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static DWORD find_pid(const wchar_t *name)
{
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD pid = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, name)) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

/* the target's kernel32 base, or NULL; and its exe path for the config dir */
static BYTE *remote_module(DWORD pid, const wchar_t *mod, wchar_t *exe_path, size_t exe_len)
{
    MODULEENTRY32W me = { sizeof(me) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    BYTE *base = NULL;
    BOOL first = TRUE;
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (first && exe_path) { wcsncpy(exe_path, me.szExePath, exe_len - 1); exe_path[exe_len - 1] = 0; }
        first = FALSE;
        if (!_wcsicmp(me.szModule, mod)) base = me.modBaseAddr;
    }
    CloseHandle(snap);
    return base;
}

/* set key=value in UEVR's config.txt (one key=value per line), keeping every other line */
static int set_config(const wchar_t *stem, const char *key, const char *value)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    char line[4096], *out = NULL;
    size_t n = 0, cap = 0, klen = strlen(key);
    int found = 0;
    FILE *f;

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir))) return -1;
    wcsncat(dir, L"\\UnrealVRMod", MAX_PATH - wcslen(dir) - 1);
    CreateDirectoryW(dir, NULL);
    wcsncat(dir, L"\\", MAX_PATH - wcslen(dir) - 1);
    wcsncat(dir, stem, MAX_PATH - wcslen(dir) - 1);
    CreateDirectoryW(dir, NULL);
    _snwprintf(path, MAX_PATH, L"%ls\\config.txt", dir);

    if ((f = _wfopen(path, L"rb"))) {
        while (fgets(line, sizeof(line), f)) {
            const char *keep = line;
            char repl[4096];
            if (!strncmp(line, key, klen) && line[klen] == '=') {
                snprintf(repl, sizeof(repl), "%s=%s\n", key, value);
                keep = repl; found = 1;
            }
            size_t l = strlen(keep);
            if (n + l + 1 > cap) { cap = (n + l + 1) * 2; out = realloc(out, cap); }
            memcpy(out + n, keep, l); n += l;
        }
        fclose(f);
    }
    if (!found) {
        snprintf(line, sizeof(line), "%s=%s\n", key, value);
        size_t l = strlen(line);
        if (n + l + 1 > cap) { cap = n + l + 1; out = realloc(out, cap); }
        memcpy(out + n, line, l); n += l;
    }
    if (!(f = _wfopen(path, L"wb"))) { free(out); return -1; }
    fwrite(out, 1, n, f);
    fclose(f);
    free(out);
    fwprintf(stderr, L"config: %ls: %hs=%hs\n", path, key, value);
    return 0;
}

static int inject(HANDLE proc, void *load_library, const wchar_t *dll)
{
    size_t bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    DWORD code = 0;
    void *mem;
    HANDLE th;

    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { fwprintf(stderr, L"missing: %ls\n", dll); return -1; }
    if (!(mem = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) {
        fwprintf(stderr, L"VirtualAllocEx failed: %lu\n", GetLastError()); return -1;
    }
    if (!WriteProcessMemory(proc, mem, dll, bytes, NULL)) {
        fwprintf(stderr, L"WriteProcessMemory failed: %lu\n", GetLastError()); return -1;
    }
    if (!(th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)load_library, mem, 0, NULL))) {
        fwprintf(stderr, L"CreateRemoteThread failed: %lu\n", GetLastError()); return -1;
    }
    if (WaitForSingleObject(th, 60000) != WAIT_OBJECT_0) { fwprintf(stderr, L"LoadLibraryW timed out: %ls\n", dll); return -1; }
    GetExitCodeThread(th, &code);   /* the low 32 bits of the HMODULE: 0 means LoadLibraryW failed */
    CloseHandle(th);
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    fwprintf(stderr, L"%ls: %ls\n", code ? L"loaded" : L"FAILED", dll);
    return code ? 0 : -1;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t exe_path[MAX_PATH] = L"", stem[MAX_PATH], path[MAX_PATH];
    const wchar_t *dir;
    int nullify = 0, openvr = 0;
    DWORD pid;
    HANDLE proc;
    BYTE *local_k32, *remote_k32;
    void *load_library;

    if (argc < 3) { fwprintf(stderr, L"usage: uevr-inject.exe <exe name | pid> <uevr dir> [--nullify] [--openvr]\n"); return 2; }
    for (int i = 3; i < argc; i++) {
        if (!wcscmp(argv[i], L"--nullify")) nullify = 1;
        else if (!wcscmp(argv[i], L"--openvr")) openvr = 1;
        else { fwprintf(stderr, L"unknown option: %ls\n", argv[i]); return 2; }
    }
    dir = argv[2];
    pid = iswdigit(argv[1][0]) ? (DWORD)wcstoul(argv[1], NULL, 10) : find_pid(argv[1]);
    if (!pid) { fwprintf(stderr, L"no process: %ls\n", argv[1]); return 1; }

    if (!(proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid))) { fwprintf(stderr, L"OpenProcess(%lu) failed: %lu\n", pid, GetLastError()); return 1; }

    /* LoadLibraryW at the target's kernel32 base plus its offset in ours: the same address unless the bases differ */
    local_k32 = (BYTE *)GetModuleHandleW(L"kernel32.dll");
    remote_k32 = remote_module(pid, L"kernel32.dll", exe_path, MAX_PATH);
    load_library = (void *)GetProcAddress((HMODULE)local_k32, "LoadLibraryW");
    if (remote_k32) load_library = remote_k32 + ((BYTE *)load_library - local_k32);
    else fwprintf(stderr, L"warning: target's module list unreadable, assuming its kernel32 is at ours (%p)\n", local_k32);
    fwprintf(stderr, L"pid %lu, exe %ls, kernel32 %p (ours %p), LoadLibraryW %p\n", pid, exe_path, remote_k32, local_k32, load_library);

    if (exe_path[0]) {
        const wchar_t *base = wcsrchr(exe_path, L'\\') ? wcsrchr(exe_path, L'\\') + 1 : exe_path;
        wcsncpy(stem, base, MAX_PATH - 1); stem[MAX_PATH - 1] = 0;
        if (wcsrchr(stem, L'.')) *wcsrchr(stem, L'.') = 0;
        if (set_config(stem, "Frontend_RequestedRuntime", openvr ? "openvr_api.dll" : "openxr_loader.dll"))
            fwprintf(stderr, L"warning: could not write config.txt\n");
    } else fwprintf(stderr, L"warning: no exe path, config.txt not written\n");

    _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, openvr ? L"openvr_api.dll" : L"openxr_loader.dll");
    if (inject(proc, load_library, path)) return 1;
    if (nullify) {
        _snwprintf(path, MAX_PATH, L"%ls\\UEVRPluginNullifier.dll", dir);
        if (inject(proc, load_library, path)) return 1;
    }
    _snwprintf(path, MAX_PATH, L"%ls\\UEVRBackend.dll", dir);
    if (inject(proc, load_library, path)) return 1;
    CloseHandle(proc);
    return 0;
}
