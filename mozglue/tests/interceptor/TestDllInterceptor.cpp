/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <shlobj.h>
#include <stdio.h>
#include <commdlg.h>
#define SECURITY_WIN32
#include <security.h>
#include <wininet.h>
#include <schnlsp.h>

#include "mozilla/WindowsVersion.h"
#include "nsWindowsDllInterceptor.h"
#include "nsWindowsHelpers.h"

using namespace mozilla;

struct payload {
  UINT64 a;
  UINT64 b;
  UINT64 c;

  bool operator==(const payload &other) const {
    return (a == other.a &&
            b == other.b &&
            c == other.c);
  }
};

extern "C" __declspec(dllexport) __declspec(noinline) payload rotatePayload(payload p) {
  UINT64 tmp = p.a;
  p.a = p.b;
  p.b = p.c;
  p.c = tmp;
  return p;
}

static bool patched_func_called = false;

static payload (*orig_rotatePayload)(payload);

static payload
patched_rotatePayload(payload p)
{
  patched_func_called = true;
  return orig_rotatePayload(p);
}

typedef bool(*HookTestFunc)(void*);
bool CheckHook(HookTestFunc aHookTestFunc, void* aOrigFunc,
               const char* aDllName, const char* aFuncName)
{
  if (aHookTestFunc(aOrigFunc)) {
    printf("TEST-PASS | WindowsDllInterceptor | "
           "Executed hooked function %s from %s\n", aFuncName, aDllName);
    return true;
  }
  printf("TEST-FAILED | WindowsDllInterceptor | "
         "Failed to execute hooked function %s from %s\n", aFuncName, aDllName);
  return false;
}

template <size_t N>
bool TestHook(HookTestFunc funcTester, const char (&dll)[N], const char *func)
{
  void *orig_func;
  bool successful = false;
  {
    WindowsDllInterceptor TestIntercept;
    TestIntercept.Init(dll);
    successful = TestIntercept.AddHook(func, 0, &orig_func);
  }

  if (successful) {
    printf("TEST-PASS | WindowsDllInterceptor | Could hook %s from %s\n", func, dll);
    return CheckHook(funcTester, orig_func, dll, func);
  } else {
    printf("TEST-UNEXPECTED-FAIL | WindowsDllInterceptor | Failed to hook %s from %s\n", func, dll);
    return false;
  }
}

template <size_t N>
bool TestDetour(const char (&dll)[N], const char *func)
{
  void *orig_func;
  bool successful = false;
  {
    WindowsDllInterceptor TestIntercept;
    TestIntercept.Init(dll);
    successful = TestIntercept.AddDetour(func, 0, &orig_func);
  }

  if (successful) {
    printf("TEST-PASS | WindowsDllInterceptor | Could detour %s from %s\n", func, dll);
    return true;
  } else {
    printf("TEST-UNEXPECTED-FAIL | WindowsDllInterceptor | Failed to detour %s from %s\n", func, dll);
    return false;
  }
}

template <size_t N>
bool MaybeTestHook(const bool cond, HookTestFunc funcTester, const char (&dll)[N], const char* func)
{
  if (!cond) {
    printf("TEST-SKIPPED | WindowsDllInterceptor | Skipped hook test for %s from %s\n", func, dll);
    return true;
  }

  return TestHook(funcTester, dll, func);
}

bool ShouldTestTipTsf()
{
  if (!IsWin8OrLater()) {
    return false;
  }

  nsModuleHandle shell32(LoadLibraryW(L"shell32.dll"));
  if (!shell32) {
    return true;
  }

  auto pSHGetKnownFolderPath = reinterpret_cast<decltype(&SHGetKnownFolderPath)>(GetProcAddress(shell32, "SHGetKnownFolderPath"));
  if (!pSHGetKnownFolderPath) {
    return true;
  }

  PWSTR commonFilesPath = nullptr;
  if (FAILED(pSHGetKnownFolderPath(FOLDERID_ProgramFilesCommon, 0, nullptr,
                                   &commonFilesPath))) {
    return true;
  }

  wchar_t fullPath[MAX_PATH + 1] = {};
  wcscpy(fullPath, commonFilesPath);
  wcscat(fullPath, L"\\Microsoft Shared\\Ink\\tiptsf.dll");
  CoTaskMemFree(commonFilesPath);

  if (!LoadLibraryW(fullPath)) {
    return false;
  }

  // Leak the module so that it's loaded for the interceptor test
  return true;
}

// These test the patched function returned by the DLL
// interceptor.  They check that the patched assembler preamble does
// something sane.  The parameter is a pointer to the patched function.
bool TestGetWindowInfo(void* aFunc)
{
  auto patchedGetWindowInfo =
    reinterpret_cast<decltype(&GetWindowInfo)>(aFunc);
  return patchedGetWindowInfo(0, 0) == FALSE;
}

bool TestSetWindowLongPtr(void* aFunc)
{
  auto patchedSetWindowLongPtr =
    reinterpret_cast<decltype(&SetWindowLongPtr)>(aFunc);
  return patchedSetWindowLongPtr(0, 0, 0) == 0;
}

bool TestSetWindowLong(void* aFunc)
{
  auto patchedSetWindowLong =
    reinterpret_cast<decltype(&SetWindowLong)>(aFunc);
  return patchedSetWindowLong(0, 0, 0) == 0;
}

bool TestTrackPopupMenu(void* aFunc)
{
  auto patchedTrackPopupMenu =
    reinterpret_cast<decltype(&TrackPopupMenu)>(aFunc);
  return patchedTrackPopupMenu(0, 0, 0, 0, 0, 0, 0) == 0;
}

bool TestNtFlushBuffersFile(void* aFunc)
{
  typedef NTSTATUS(WINAPI *NtFlushBuffersFileType)(HANDLE, PIO_STATUS_BLOCK);
  auto patchedNtFlushBuffersFile =
    reinterpret_cast<NtFlushBuffersFileType>(aFunc);
  patchedNtFlushBuffersFile(0, 0);
  return true;
}

bool TestNtCreateFile(void* aFunc)
{
  auto patchedNtCreateFile =
    reinterpret_cast<decltype(&NtCreateFile)>(aFunc);
  return patchedNtCreateFile(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) != 0;
}

bool TestNtReadFile(void* aFunc)
{
  typedef NTSTATUS(WINAPI *NtReadFileType)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
                                           PIO_STATUS_BLOCK, PVOID, ULONG,
                                           PLARGE_INTEGER, PULONG);
  auto patchedNtReadFile =
    reinterpret_cast<NtReadFileType>(aFunc);
  return patchedNtReadFile(0, 0, 0, 0, 0, 0, 0, 0, 0) != 0;
}

bool TestNtReadFileScatter(void* aFunc)
{
  typedef NTSTATUS(WINAPI *NtReadFileScatterType)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
                                                  PIO_STATUS_BLOCK, PFILE_SEGMENT_ELEMENT, ULONG,
                                                  PLARGE_INTEGER, PULONG);
  auto patchedNtReadFileScatter =
    reinterpret_cast<NtReadFileScatterType>(aFunc);
  return patchedNtReadFileScatter(0, 0, 0, 0, 0, 0, 0, 0, 0) != 0;
}

bool TestNtWriteFile(void* aFunc)
{
  typedef NTSTATUS(WINAPI *NtWriteFileType)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
                                            PIO_STATUS_BLOCK, PVOID, ULONG,
                                            PLARGE_INTEGER, PULONG);
  auto patchedNtWriteFile =
    reinterpret_cast<NtWriteFileType>(aFunc);
  return patchedNtWriteFile(0, 0, 0, 0, 0, 0, 0, 0, 0) != 0;
}

bool TestNtWriteFileGather(void* aFunc)
{
  typedef NTSTATUS(WINAPI *NtWriteFileGatherType)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
                                                  PIO_STATUS_BLOCK, PFILE_SEGMENT_ELEMENT, ULONG,
                                                  PLARGE_INTEGER, PULONG);
  auto patchedNtWriteFileGather =
    reinterpret_cast<NtWriteFileGatherType>(aFunc);
  return patchedNtWriteFileGather(0, 0, 0, 0, 0, 0, 0, 0, 0) != 0;
}

bool TestNtQueryFullAttributesFile(void* aFunc)
{
  typedef NTSTATUS(WINAPI *NtQueryFullAttributesFileType)(POBJECT_ATTRIBUTES,
                                                          PVOID);
  auto patchedNtQueryFullAttributesFile =
    reinterpret_cast<NtQueryFullAttributesFileType>(aFunc);
  return patchedNtQueryFullAttributesFile(0, 0) != 0;
}

bool TestLdrUnloadDll(void* aFunc)
{
  typedef NTSTATUS (NTAPI *LdrUnloadDllType)(HMODULE);
  auto patchedLdrUnloadDll = reinterpret_cast<LdrUnloadDllType>(aFunc);
  return patchedLdrUnloadDll(0) != 0;
}

bool TestLdrResolveDelayLoadedAPI(void* aFunc)
{
  // These pointers are disguised as PVOID to avoid pulling in obscure headers
  typedef PVOID (WINAPI *LdrResolveDelayLoadedAPIType)(PVOID, PVOID, PVOID,
                                                       PVOID, PVOID, ULONG);
  auto patchedLdrResolveDelayLoadedAPI =
    reinterpret_cast<LdrResolveDelayLoadedAPIType>(aFunc);
  // No idea how to call this API. Flags==99 is just an arbitrary number that
  // doesn't crash when the other params are null.
  return patchedLdrResolveDelayLoadedAPI(0, 0, 0, 0, 0, 99) == 0;
}

#ifdef _M_AMD64
bool TestRtlInstallFunctionTableCallback(void* aFunc)
{
  auto patchedRtlInstallFunctionTableCallback =
    reinterpret_cast<decltype(RtlInstallFunctionTableCallback)*>(aFunc);

  return patchedRtlInstallFunctionTableCallback(0, 0, 0, 0, 0, 0) == FALSE;
}
#endif

bool TestSetUnhandledExceptionFilter(void* aFunc)
{
  auto patchedSetUnhandledExceptionFilter =
    reinterpret_cast<decltype(&SetUnhandledExceptionFilter)>(aFunc);
  // Retrieve the current filter as we set the new filter to null, then restore the current filter.
  LPTOP_LEVEL_EXCEPTION_FILTER current = patchedSetUnhandledExceptionFilter(0);
  patchedSetUnhandledExceptionFilter(current);
  return true;
}

bool TestVirtualAlloc(void* aFunc)
{
  auto patchedVirtualAlloc =
    reinterpret_cast<decltype(&VirtualAlloc)>(aFunc);
  return patchedVirtualAlloc(0, 0, 0, 0) == 0;
}

bool TestMapViewOfFile(void* aFunc)
{
  auto patchedMapViewOfFile =
    reinterpret_cast<decltype(&MapViewOfFile)>(aFunc);
  return patchedMapViewOfFile(0, 0, 0, 0, 0) == 0;
}

bool TestCreateDIBSection(void* aFunc)
{
  auto patchedCreateDIBSection =
    reinterpret_cast<decltype(&CreateDIBSection)>(aFunc);
  // MSDN is wrong here.  This does not return ERROR_INVALID_PARAMETER.  It
  // sets the value of GetLastError to ERROR_INVALID_PARAMETER.
  // CreateDIBSection returns 0 on error.
  return patchedCreateDIBSection(0, 0, 0, 0, 0, 0) == 0;
}

bool TestCreateFileW(void* aFunc)
{
  auto patchedCreateFileW =
    reinterpret_cast<decltype(&CreateFileW)>(aFunc);
  return patchedCreateFileW(0, 0, 0, 0, 0, 0, 0) == INVALID_HANDLE_VALUE;
}

bool TestCreateFileA(void* aFunc)
{
  auto patchedCreateFileA =
    reinterpret_cast<decltype(&CreateFileA)>(aFunc);
//  return patchedCreateFileA(0, 0, 0, 0, 0, 0, 0) == INVALID_HANDLE_VALUE;
  printf("TEST-SKIPPED | WindowsDllInterceptor | "
          "Will not attempt to execute patched CreateFileA -- patched method is known to fail.\n");
  return true;
}

bool TestQueryDosDeviceW(void* aFunc)
{
  auto patchedQueryDosDeviceW =
    reinterpret_cast<decltype(&QueryDosDeviceW)>(aFunc);
  return patchedQueryDosDeviceW(nullptr, nullptr, 0) == 0;
}

bool TestInSendMessageEx(void* aFunc)
{
  auto patchedInSendMessageEx =
    reinterpret_cast<decltype(&InSendMessageEx)>(aFunc);
  patchedInSendMessageEx(0);
  return true;
}

bool TestImmGetContext(void* aFunc)
{
  auto patchedImmGetContext =
    reinterpret_cast<decltype(&ImmGetContext)>(aFunc);
  patchedImmGetContext(0);
  return true;
}

bool TestImmGetCompositionStringW(void* aFunc)
{
  auto patchedImmGetCompositionStringW =
    reinterpret_cast<decltype(&ImmGetCompositionStringW)>(aFunc);
  patchedImmGetCompositionStringW(0, 0, 0, 0);
  return true;
}

bool TestImmSetCandidateWindow(void* aFunc)
{
  auto patchedImmSetCandidateWindow =
    reinterpret_cast<decltype(&ImmSetCandidateWindow)>(aFunc);
//  return patchedImmSetCandidateWindow(0, 0) == 0;
  // ImmSetCandidateWindow crashes if given bad parameters.
  printf("TEST-SKIPPED | WindowsDllInterceptor | "
          "Will not attempt to execute patched ImmSetCandidateWindow.\n");
  return true;
}

bool TestImmNotifyIME(void* aFunc)
{
  auto patchedImmNotifyIME =
    reinterpret_cast<decltype(&ImmNotifyIME)>(aFunc);
  return patchedImmNotifyIME(0, 0, 0, 0) == 0;
}

bool TestGetSaveFileNameW(void* aFunc)
{
  auto patchedGetSaveFileNameWType =
    reinterpret_cast<decltype(&GetSaveFileNameW)>(aFunc);
  patchedGetSaveFileNameWType(0);
  return true;
}

bool TestGetOpenFileNameW(void* aFunc)
{
  auto patchedGetOpenFileNameWType =
    reinterpret_cast<decltype(&GetOpenFileNameW)>(aFunc);
  patchedGetOpenFileNameWType(0);
  return true;
}

bool TestGetKeyState(void* aFunc)
{
  auto patchedGetKeyState =
    reinterpret_cast<decltype(&GetKeyState)>(aFunc);
  patchedGetKeyState(0);
  return true;
}

bool TestSendMessageTimeoutW(void* aFunc)
{
  auto patchedSendMessageTimeoutW =
    reinterpret_cast<decltype(&SendMessageTimeoutW)>(aFunc);
  return patchedSendMessageTimeoutW(0, 0, 0, 0, 0, 0, 0) == 0;
}

bool TestProcessCaretEvents(void* aFunc)
{
  auto patchedProcessCaretEvents =
    reinterpret_cast<WINEVENTPROC>(aFunc);
  patchedProcessCaretEvents(0, 0, 0, 0, 0, 0, 0);
  return true;
}

bool TestSetCursorPos(void* aFunc)
{
  // SetCursorPos has some issues in automation -- see bug 1368033.
  // For that reason, we don't check the return value -- we only
  // check that the method runs without producing an exception.
  auto patchedSetCursorPos =
    reinterpret_cast<decltype(&SetCursorPos)>(aFunc);
  patchedSetCursorPos(512, 512);
  return true;
}

static DWORD sTlsIndex = 0;

bool TestTlsAlloc(void* aFunc)
{
  auto patchedTlsAlloc =
    reinterpret_cast<decltype(&TlsAlloc)>(aFunc);
  sTlsIndex = patchedTlsAlloc();
  return sTlsIndex != TLS_OUT_OF_INDEXES;
}

bool TestTlsFree(void* aFunc)
{
  auto patchedTlsFree =
    reinterpret_cast<decltype(&TlsFree)>(aFunc);
  return sTlsIndex != 0 && patchedTlsFree(sTlsIndex);
}

bool TestCloseHandle(void* aFunc)
{
  auto patchedCloseHandle =
    reinterpret_cast<decltype(&CloseHandle)>(aFunc);
  return patchedCloseHandle(0) == FALSE;
}

bool TestDuplicateHandle(void* aFunc)
{
  auto patchedDuplicateHandle =
    reinterpret_cast<decltype(&DuplicateHandle)>(aFunc);
  return patchedDuplicateHandle(0, 0, 0, 0, 0, 0, 0) == FALSE;
}

bool TestPrintDlgW(void* aFunc)
{
  auto patchedPrintDlgW =
    reinterpret_cast<decltype(&PrintDlgW)>(aFunc);
  patchedPrintDlgW(0);
  return true;
}

bool TestInternetConnectA(void* aFunc)
{
  auto patchedInternetConnectA =
    reinterpret_cast<decltype(&InternetConnectA)>(aFunc);
  return patchedInternetConnectA(0, 0, 0, 0, 0, 0, 0, 0) == 0;
}

HINTERNET sInternet = 0;

bool TestInternetOpenA(void* aFunc)
{
  auto patchedInternetOpenA =
    reinterpret_cast<decltype(&InternetOpenA)>(aFunc);
  sInternet = patchedInternetOpenA(0, 0, 0, 0, 0);
  return sInternet != 0;
}

bool TestInternetCloseHandle(void* aFunc)
{
  auto patchedInternetCloseHandle =
    reinterpret_cast<decltype(&InternetCloseHandle)>(aFunc);
  return patchedInternetCloseHandle(sInternet);
}

bool TestInternetQueryDataAvailable(void* aFunc)
{
  auto patchedInternetQueryDataAvailable =
    reinterpret_cast<decltype(&InternetQueryDataAvailable)>(aFunc);
  return patchedInternetQueryDataAvailable(0, 0, 0, 0) == FALSE;
}

bool TestInternetReadFile(void* aFunc)
{
  auto patchedInternetReadFile =
    reinterpret_cast<decltype(&InternetReadFile)>(aFunc);
  return patchedInternetReadFile(0, 0, 0, 0) == FALSE;
}

bool TestInternetWriteFile(void* aFunc)
{
  auto patchedInternetWriteFile =
    reinterpret_cast<decltype(&InternetWriteFile)>(aFunc);
  return patchedInternetWriteFile(0, 0, 0, 0) == FALSE;
}

bool TestInternetSetOptionA(void* aFunc)
{
  auto patchedInternetSetOptionA =
    reinterpret_cast<decltype(&InternetSetOptionA)>(aFunc);
  return patchedInternetSetOptionA(0, 0, 0, 0) == FALSE;
}

bool TestHttpAddRequestHeadersA(void* aFunc)
{
  auto patchedHttpAddRequestHeadersA =
    reinterpret_cast<decltype(&HttpAddRequestHeadersA)>(aFunc);
  return patchedHttpAddRequestHeadersA(0, 0, 0, 0) == FALSE;
}

bool TestHttpOpenRequestA(void* aFunc)
{
  auto patchedHttpOpenRequestA =
    reinterpret_cast<decltype(&HttpOpenRequestA)>(aFunc);
  return patchedHttpOpenRequestA(0, 0, 0, 0, 0, 0, 0, 0) == 0;
}

bool TestHttpQueryInfoA(void* aFunc)
{
  auto patchedHttpQueryInfoA =
    reinterpret_cast<decltype(&HttpQueryInfoA)>(aFunc);
  return patchedHttpQueryInfoA(0, 0, 0, 0, 0) == FALSE;
}

bool TestHttpSendRequestA(void* aFunc)
{
  auto patchedHttpSendRequestA =
    reinterpret_cast<decltype(&HttpSendRequestA)>(aFunc);
  return patchedHttpSendRequestA(0, 0, 0, 0, 0) == FALSE;
}

bool TestHttpSendRequestExA(void* aFunc)
{
  auto patchedHttpSendRequestExA =
    reinterpret_cast<decltype(&HttpSendRequestExA)>(aFunc);
  return patchedHttpSendRequestExA(0, 0, 0, 0, 0) == FALSE;
}

bool TestHttpEndRequestA(void* aFunc)
{
  auto patchedHttpEndRequestA =
    reinterpret_cast<decltype(&HttpEndRequestA)>(aFunc);
  return patchedHttpEndRequestA(0, 0, 0, 0) == FALSE;
}

bool TestInternetQueryOptionA(void* aFunc)
{
  auto patchedInternetQueryOptionA =
    reinterpret_cast<decltype(&InternetQueryOptionA)>(aFunc);
  return patchedInternetQueryOptionA(0, 0, 0, 0) == FALSE;
}

bool TestInternetErrorDlg(void* aFunc)
{
  auto patchedInternetErrorDlg =
    reinterpret_cast<decltype(&InternetErrorDlg)>(aFunc);
  return patchedInternetErrorDlg(0, 0, 0, 0, 0) == ERROR_INVALID_HANDLE;
}

CredHandle sCredHandle;

bool TestAcquireCredentialsHandleA(void* aFunc)
{
  auto patchedAcquireCredentialsHandleA =
    reinterpret_cast<decltype(&AcquireCredentialsHandleA)>(aFunc);
  SCHANNEL_CRED cred;
  memset(&cred, 0, sizeof(cred));
  cred.dwVersion = SCHANNEL_CRED_VERSION;
  return patchedAcquireCredentialsHandleA(0, UNISP_NAME, SECPKG_CRED_OUTBOUND,
                                          0, &cred, 0, 0, &sCredHandle, 0) == S_OK;
}

bool TestQueryCredentialsAttributesA(void* aFunc)
{
  auto patchedQueryCredentialsAttributesA =
    reinterpret_cast<decltype(&QueryCredentialsAttributesA)>(aFunc);
  return patchedQueryCredentialsAttributesA(&sCredHandle, 0, 0) == SEC_E_UNSUPPORTED_FUNCTION;
}

bool TestFreeCredentialsHandle(void* aFunc)
{
  auto patchedFreeCredentialsHandle =
    reinterpret_cast<decltype(&FreeCredentialsHandle)>(aFunc);
  return patchedFreeCredentialsHandle(&sCredHandle) == S_OK;
}

int main()
{
  LARGE_INTEGER start;
  QueryPerformanceCounter(&start);

  // We disable this part of the test because the code coverage instrumentation
  // injects code in rotatePayload in a way that WindowsDllInterceptor doesn't
  // understand.
#ifndef MOZ_CODE_COVERAGE
  payload initial = { 0x12345678, 0xfc4e9d31, 0x87654321 };
  payload p0, p1;
  ZeroMemory(&p0, sizeof(p0));
  ZeroMemory(&p1, sizeof(p1));

  p0 = rotatePayload(initial);

  {
    WindowsDllInterceptor ExeIntercept;
    ExeIntercept.Init("TestDllInterceptor.exe");
    if (ExeIntercept.AddHook("rotatePayload", reinterpret_cast<intptr_t>(patched_rotatePayload), (void**) &orig_rotatePayload)) {
      printf("TEST-PASS | WindowsDllInterceptor | Hook added\n");
    } else {
      printf("TEST-UNEXPECTED-FAIL | WindowsDllInterceptor | Failed to add hook\n");
      return 1;
    }

    p1 = rotatePayload(initial);

    if (patched_func_called) {
      printf("TEST-PASS | WindowsDllInterceptor | Hook called\n");
    } else {
      printf("TEST-UNEXPECTED-FAIL | WindowsDllInterceptor | Hook was not called\n");
      return 1;
    }

    if (p0 == p1) {
      printf("TEST-PASS | WindowsDllInterceptor | Hook works properly\n");
    } else {
      printf("TEST-UNEXPECTED-FAIL | WindowsDllInterceptor | Hook didn't return the right information\n");
      return 1;
    }
  }

  patched_func_called = false;
  ZeroMemory(&p1, sizeof(p1));

  p1 = rotatePayload(initial);

  if (!patched_func_called) {
    printf("TEST-PASS | WindowsDllInterceptor | Hook was not called after unregistration\n");
  } else {
    printf("TEST-UNEXPECTED-FAIL | WindowsDllInterceptor | Hook was still called after unregistration\n");
    return 1;
  }

  if (p0 == p1) {
    printf("TEST-PASS | WindowsDllInterceptor | Original function worked properly\n");
  } else {
    printf("TEST-UNEXPECTED-FAIL | WindowsDllInterceptor | Original function didn't return the right information\n");
    return 1;
  }
#endif

  if (TestHook(TestGetWindowInfo, "user32.dll", "GetWindowInfo") &&
#ifdef _WIN64
      TestHook(TestSetWindowLongPtr, "user32.dll", "SetWindowLongPtrA") &&
      TestHook(TestSetWindowLongPtr, "user32.dll", "SetWindowLongPtrW") &&
#else
      TestHook(TestSetWindowLong, "user32.dll", "SetWindowLongA") &&
      TestHook(TestSetWindowLong, "user32.dll", "SetWindowLongW") &&
#endif
      TestHook(TestTrackPopupMenu, "user32.dll", "TrackPopupMenu") &&
#ifdef _M_IX86
      // We keep this test to hook complex code on x86. (Bug 850957)
      TestHook(TestNtFlushBuffersFile, "ntdll.dll", "NtFlushBuffersFile") &&
#endif
      TestHook(TestNtCreateFile, "ntdll.dll", "NtCreateFile") &&
      TestHook(TestNtReadFile, "ntdll.dll", "NtReadFile") &&
      TestHook(TestNtReadFileScatter, "ntdll.dll", "NtReadFileScatter") &&
      TestHook(TestNtWriteFile, "ntdll.dll", "NtWriteFile") &&
      TestHook(TestNtWriteFileGather, "ntdll.dll", "NtWriteFileGather") &&
      TestHook(TestNtQueryFullAttributesFile, "ntdll.dll", "NtQueryFullAttributesFile") &&
#ifndef MOZ_ASAN
      // Bug 733892: toolkit/crashreporter/nsExceptionHandler.cpp
      // This fails on ASan because the ASan runtime already hooked this function
      TestHook(TestSetUnhandledExceptionFilter, "kernel32.dll", "SetUnhandledExceptionFilter") &&
#endif
#ifdef _M_IX86
      // Bug 670967: xpcom/base/AvailableMemoryTracker.cpp
      TestHook(TestVirtualAlloc, "kernel32.dll", "VirtualAlloc") &&
      TestHook(TestMapViewOfFile, "kernel32.dll", "MapViewOfFile") &&
      TestHook(TestCreateDIBSection, "gdi32.dll", "CreateDIBSection") &&
      TestHook(TestCreateFileW, "kernel32.dll", "CreateFileW") &&    // see Bug 1316415
#endif
      TestHook(TestCreateFileA, "kernel32.dll", "CreateFileA") &&
      TestHook(TestQueryDosDeviceW, "kernelbase.dll", "QueryDosDeviceW") &&
      TestDetour("user32.dll", "CreateWindowExW") &&
      TestHook(TestInSendMessageEx, "user32.dll", "InSendMessageEx") &&
      TestHook(TestImmGetContext, "imm32.dll", "ImmGetContext") &&
      TestHook(TestImmGetCompositionStringW, "imm32.dll", "ImmGetCompositionStringW") &&
      TestHook(TestImmSetCandidateWindow, "imm32.dll", "ImmSetCandidateWindow") &&
      TestHook(TestImmNotifyIME, "imm32.dll", "ImmNotifyIME") &&
      TestHook(TestGetSaveFileNameW, "comdlg32.dll", "GetSaveFileNameW") &&
      TestHook(TestGetOpenFileNameW, "comdlg32.dll", "GetOpenFileNameW") &&
#ifdef _M_X64
      TestHook(TestGetKeyState, "user32.dll", "GetKeyState") &&    // see Bug 1316415
      TestHook(TestLdrUnloadDll, "ntdll.dll", "LdrUnloadDll") &&
      MaybeTestHook(IsWin8OrLater(), TestLdrResolveDelayLoadedAPI, "ntdll.dll", "LdrResolveDelayLoadedAPI") &&
      MaybeTestHook(!IsWin8OrLater(), TestRtlInstallFunctionTableCallback, "kernel32.dll", "RtlInstallFunctionTableCallback") &&
      TestHook(TestPrintDlgW, "comdlg32.dll", "PrintDlgW") &&
#endif
      MaybeTestHook(ShouldTestTipTsf(), TestProcessCaretEvents, "tiptsf.dll", "ProcessCaretEvents") &&
#ifdef _M_IX86
      TestHook(TestSendMessageTimeoutW, "user32.dll", "SendMessageTimeoutW") &&
#endif
      TestHook(TestSetCursorPos, "user32.dll", "SetCursorPos") &&
      TestHook(TestTlsAlloc, "kernel32.dll", "TlsAlloc") &&
      TestHook(TestTlsFree, "kernel32.dll", "TlsFree") &&
      TestHook(TestCloseHandle, "kernel32.dll", "CloseHandle") &&
      TestHook(TestDuplicateHandle, "kernel32.dll", "DuplicateHandle") &&

      TestHook(TestInternetOpenA, "wininet.dll", "InternetOpenA") &&
      TestHook(TestInternetCloseHandle, "wininet.dll", "InternetCloseHandle") &&
      TestHook(TestInternetConnectA, "wininet.dll", "InternetConnectA") &&
      TestHook(TestInternetQueryDataAvailable, "wininet.dll", "InternetQueryDataAvailable") &&
      TestHook(TestInternetReadFile, "wininet.dll", "InternetReadFile") &&
      TestHook(TestInternetWriteFile, "wininet.dll", "InternetWriteFile") &&
      TestHook(TestInternetSetOptionA, "wininet.dll", "InternetSetOptionA") &&
      TestHook(TestHttpAddRequestHeadersA, "wininet.dll", "HttpAddRequestHeadersA") &&
      TestHook(TestHttpOpenRequestA, "wininet.dll", "HttpOpenRequestA") &&
      TestHook(TestHttpQueryInfoA, "wininet.dll", "HttpQueryInfoA") &&
      TestHook(TestHttpSendRequestA, "wininet.dll", "HttpSendRequestA") &&
      TestHook(TestHttpSendRequestExA, "wininet.dll", "HttpSendRequestExA") &&
      TestHook(TestHttpEndRequestA, "wininet.dll", "HttpEndRequestA") &&
      TestHook(TestInternetQueryOptionA, "wininet.dll", "InternetQueryOptionA") &&

      TestHook(TestAcquireCredentialsHandleA, "sspicli.dll", "AcquireCredentialsHandleA") &&
      TestHook(TestQueryCredentialsAttributesA, "sspicli.dll", "QueryCredentialsAttributesA") &&
      TestHook(TestFreeCredentialsHandle, "sspicli.dll", "FreeCredentialsHandle") &&

      TestDetour("kernel32.dll", "BaseThreadInitThunk") &&
      TestDetour("ntdll.dll", "LdrLoadDll")) {
    printf("TEST-PASS | WindowsDllInterceptor | all checks passed\n");

    LARGE_INTEGER end, freq;
    QueryPerformanceCounter(&end);

    QueryPerformanceFrequency(&freq);

    LARGE_INTEGER result;
    result.QuadPart = end.QuadPart - start.QuadPart;
    result.QuadPart *= 1000000;
    result.QuadPart /= freq.QuadPart;

    printf("Elapsed time: %lld microseconds\n", result.QuadPart);

    return 0;
  }

  return 1;
}
