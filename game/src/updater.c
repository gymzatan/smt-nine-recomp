#define UNICODE
#define _UNICODE
#include <windows.h>
#include <objbase.h>
#include <stdio.h>

static int resource_file(HINSTANCE instance, int id, const wchar_t *path) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(instance, resource) : NULL;
    DWORD size = resource ? SizeofResource(instance, resource) : 0, written = 0;
    if (!loaded || !size) return 0;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    int ok = WriteFile(file, LockResource(loaded), size, &written, NULL) && written == size;
    CloseHandle(file);
    return ok;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show) {
    wchar_t root[32768], temporary[32768], directory[32768], script[32768] = {0}, installer[32768] = {0};
    wchar_t system[32768], powershell[32768], guid_text[40], command[32768];
    GUID guid;
    (void)previous; (void)args; (void)show;
    DWORD length = GetModuleFileNameW(NULL, root, 32768);
    if (!length || length >= 32768) goto failure;
    wchar_t *slash = wcsrchr(root, L'\\');
    if (!slash) goto failure;
    *slash = 0;
    if (!GetTempPathW(32768, temporary) || FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(&guid, guid_text, 40)) goto failure;
    if (swprintf_s(directory, 32768, L"%lsSMT-NINE-Updater-%ls", temporary, guid_text) < 0 || !CreateDirectoryW(directory, NULL)) goto failure;
    if (swprintf_s(script, 32768, L"%ls\\updater.ps1", directory) < 0 ||
        swprintf_s(installer, 32768, L"%ls\\Apply-Update.ps1", directory) < 0 ||
        !resource_file(instance, 201, script) || !resource_file(instance, 202, installer)) goto cleanup;
    if (!GetSystemDirectoryW(system, 32768) ||
        swprintf_s(powershell, 32768, L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe", system) < 0 ||
        swprintf_s(command, 32768, L"\"%ls\" -NoProfile -STA -ExecutionPolicy Bypass -File \"%ls\" -Target \"%ls\" -Cleanup", powershell, script, root) < 0) goto cleanup;
    STARTUPINFOW startup = {sizeof startup};
    PROCESS_INFORMATION process = {0};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNORMAL;
    if (!CreateProcessW(powershell, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, root, &startup, &process)) goto cleanup;
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The launcher exits so the existing installer can replace Updater.exe too. */
    return 0;
cleanup:
    DeleteFileW(script); DeleteFileW(installer); RemoveDirectoryW(directory);
failure:
    MessageBoxW(NULL, L"Cannot start the updater. Check that the game folder and temporary directory are writable.\n无法启动更新器，请检查游戏目录与临时目录的写入权限。", L"SMT NINE Updater", MB_OK|MB_ICONERROR);
    return 1;
}
