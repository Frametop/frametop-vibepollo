/*
 * FrametopHostSetup.exe: Frametop's Windows host setup as a program to double-click
 * (https://github.com/Frametop/frametop, host/windows/README.md).
 *
 * It holds no versions or hashes. It runs the same line the README gives for PowerShell:
 *
 *   irm https://raw.githubusercontent.com/Frametop/frametop/main/host/windows/get.ps1 | iex
 *
 * get.ps1 downloads the host kit from Frametop's main branch, and the kit pins Frametop's build
 * of Vibepollo (a release of this fork) and checks every file's SHA-256 before it installs it.
 * So one copy of this exe keeps installing whatever build main pins, and a new build needs no
 * new exe. The kit asks Windows for admin itself (this exe runs asInvoker, see the manifest).
 *
 * Options, passed on to the kit: -Check, -Undo, -SkipLogin. FRAMETOP_HOST_BASE (get.ps1) still
 * points it at another branch's kit.
 *
 * Built by .github/workflows/frametop-build.yml with MSYS2's gcc and windres.
 */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define GET_PS1 L"https://raw.githubusercontent.com/Frametop/frametop/main/host/windows/get.ps1"

static const wchar_t *const options[] = {L"-Check", L"-Undo", L"-SkipLogin"};

static void wait_for_enter(void) {
  printf("\nPress Enter to close.");
  fflush(stdout);
  (void) getchar();
}

int main(void) {
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    fprintf(stderr, "FrametopHostSetup: can't read the command line (error %lu)\n", GetLastError());
    return 1;
  }
  wchar_t pass[64] = L"";
  for (int i = 1; i < argc; i++) {
    const wchar_t *known = NULL;
    for (size_t k = 0; k < sizeof options / sizeof options[0]; k++) {
      if (_wcsicmp(argv[i], options[k]) == 0) {
        known = options[k];
      }
    }
    if (!known) {
      fwprintf(stderr, L"FrametopHostSetup: unknown option %ls (it takes -Check, -Undo and -SkipLogin)\n", argv[i]);
      return 2;
    }
    if (!wcsstr(pass, known)) {  // each at most once, so pass always fits
      wcscat(pass, L" ");
      wcscat(pass, known);
    }
  }
  LocalFree(argv);

  // PowerShell by its full path: a bare name would be looked up in this exe's folder first
  // (often Downloads), where any powershell.exe could sit.
  wchar_t system[MAX_PATH];
  UINT n = GetSystemDirectoryW(system, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    fprintf(stderr, "FrametopHostSetup: can't find the Windows folder (error %lu)\n", GetLastError());
    wait_for_enter();
    return 1;
  }
  wchar_t powershell[MAX_PATH + 64];
  swprintf(powershell, sizeof powershell / sizeof powershell[0], L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe", system);

  // Stop on any error, so a failed download exits 1 (by default PowerShell would go on to the
  // exit with $LASTEXITCODE unset, which is 0). TLS 1.2 before the download: Windows
  // PowerShell 5.1 on an older .NET may not offer it, and GitHub needs it. Then get.ps1, as the
  // README's "pass options" line runs it.
  wchar_t command[2048];
  int len = swprintf(command, sizeof command / sizeof command[0],
    L"\"%ls\" -NoProfile -ExecutionPolicy Bypass -Command \""
    L"$ErrorActionPreference = 'Stop'; "
    L"[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072; "
    L"& ([scriptblock]::Create((irm '%ls')))%ls; exit $LASTEXITCODE\"",
    powershell, GET_PS1, pass);
  if (len < 0) {
    fprintf(stderr, "FrametopHostSetup: command line too long\n");
    return 1;
  }

  printf("Frametop host setup: downloading the host kit from github.com/Frametop/frametop...\n");
  fflush(stdout);
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof si);
  si.cb = sizeof si;
  ZeroMemory(&pi, sizeof pi);
  if (!CreateProcessW(powershell, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    fprintf(stderr, "FrametopHostSetup: can't start PowerShell (error %lu)\n", GetLastError());
    wait_for_enter();
    return 1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  // Without admin, the kit opens an admin window and ends here with 0: the setup goes on
  // there. An error (no network, the admin prompt declined) stays on screen until read.
  if (code != 0) {
    fprintf(stderr, "\nFrametop host setup didn't finish (exit code %lu). The lines above say why;\n"
                    "%%ProgramData%%\\Frametop\\host-setup.log has the setup's log.\n", code);
    wait_for_enter();
  } else if (argc == 1) {
    // Already admin, the setup ran here and waited for Enter itself.
    printf("If an admin window opened, the setup goes on there.\n");
    Sleep(3000);
  }
  return (int) code;
}
