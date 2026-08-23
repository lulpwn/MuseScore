//=============================================================================
//  MuseScore
//  Windows local crash capture
//=============================================================================

#include "crashhandler.h"

#include <windows.h>
#include <dbghelp.h>

namespace Ms {

namespace {

constexpr DWORD PATH_CAPACITY = 32768;
wchar_t crashDirectory[PATH_CAPACITY] = {};
volatile LONG handlingCrash = 0;

void appendPath(wchar_t* destination, const wchar_t* suffix)
      {
      while (*destination)
            ++destination;
      while (*suffix)
            *destination++ = *suffix++;
      *destination = L'\0';
      }

bool ensureWritableDirectory(wchar_t* directory)
      {
      if (!CreateDirectoryW(directory, nullptr)
          && GetLastError() != ERROR_ALREADY_EXISTS)
            return false;

      wchar_t probe[PATH_CAPACITY] = {};
      lstrcpynW(probe, directory, PATH_CAPACITY);
      appendPath(probe, L"\\.musescore-crash-write-test.tmp");
      HANDLE file = CreateFileW(probe, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY, nullptr);
      if (file == INVALID_HANDLE_VALUE)
            return false;
      CloseHandle(file);
      DeleteFileW(probe);
      return true;
      }

void createCrashDirectory()
      {
      // Keep reports with a portable/developer build whenever its directory is
      // writable, so the crash data travels with that MuseScore installation.
      DWORD length = GetModuleFileNameW(nullptr, crashDirectory, PATH_CAPACITY);
      if (length && length < PATH_CAPACITY) {
            wchar_t* end = crashDirectory + length;
            while (end != crashDirectory && end[-1] != L'\\')
                  --end;
            *end = L'\0';
            appendPath(crashDirectory, L"CrashReports");
            if (ensureWritableDirectory(crashDirectory))
                  return;
            }

      crashDirectory[0] = L'\0';
      length = GetEnvironmentVariableW(L"LOCALAPPDATA", crashDirectory, PATH_CAPACITY);
      if (!length || length >= PATH_CAPACITY - 40) {
            const DWORD fallbackLength = GetTempPathW(PATH_CAPACITY, crashDirectory);
            if (!fallbackLength || fallbackLength >= PATH_CAPACITY - 40)
                  crashDirectory[0] = L'\0';
            }
      if (!crashDirectory[0])
            return;

      wchar_t* end = crashDirectory;
      while (*end)
            ++end;
      if (end != crashDirectory && end[-1] != L'\\')
            *end++ = L'\\';
      const wchar_t applicationFolder[] = L"MuseScore3Evo";
      for (const wchar_t* source = applicationFolder; *source; ++source)
            *end++ = *source;
      *end = L'\0';
      CreateDirectoryW(crashDirectory, nullptr);

      const wchar_t reportFolder[] = L"\\CrashReports";
      for (const wchar_t* source = reportFolder; *source; ++source)
            *end++ = *source;
      *end = L'\0';
      if (!ensureWritableDirectory(crashDirectory))
            crashDirectory[0] = L'\0';
      }

void writeTextRecord(const wchar_t* path, const wchar_t* dumpPath,
                     EXCEPTION_POINTERS* exception, BOOL dumpWritten,
                     DWORD dumpError)
      {
      HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE)
            return;

      wchar_t executable[PATH_CAPACITY] = {};
      GetModuleFileNameW(nullptr, executable, PATH_CAPACITY);
      const DWORD code = exception && exception->ExceptionRecord
                       ? exception->ExceptionRecord->ExceptionCode : 0;
      const void* address = exception && exception->ExceptionRecord
                          ? exception->ExceptionRecord->ExceptionAddress : nullptr;
      wchar_t text[4096] = {};
      const int characters = wsprintfW(text,
            L"MuseScore3Evo native crash report\r\n"
            L"Exception code: 0x%08lX\r\n"
            L"Exception address: 0x%p\r\n"
            L"Process ID: %lu\r\n"
            L"Thread ID: %lu\r\n"
            L"Executable: %s\r\n"
            L"Minidump: %s\r\n"
            L"Minidump written: %s\r\n"
            L"MiniDumpWriteDump error: %lu\r\n",
            code, address, GetCurrentProcessId(), GetCurrentThreadId(), executable,
            dumpPath, dumpWritten ? L"yes" : L"no", dumpError);
      DWORD bytesWritten = 0;
      const wchar_t byteOrderMark = 0xfeff;
      WriteFile(file, &byteOrderMark, sizeof(byteOrderMark), &bytesWritten, nullptr);
      if (characters > 0)
            WriteFile(file, text, DWORD(characters * sizeof(wchar_t)), &bytesWritten, nullptr);
      FlushFileBuffers(file);
      CloseHandle(file);
      }

LONG WINAPI crashFilter(EXCEPTION_POINTERS* exception)
      {
      if (InterlockedCompareExchange(&handlingCrash, 1, 0) != 0)
            return EXCEPTION_EXECUTE_HANDLER;
      if (!crashDirectory[0])
            return EXCEPTION_EXECUTE_HANDLER;

      SYSTEMTIME time = {};
      GetLocalTime(&time);
      wchar_t stem[96] = {};
      wsprintfW(stem, L"\\MuseScore3Evo-%04u%02u%02u-%02u%02u%02u-%lu",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                time.wSecond, GetCurrentProcessId());

      wchar_t dumpPath[PATH_CAPACITY] = {};
      lstrcpynW(dumpPath, crashDirectory, PATH_CAPACITY);
      appendPath(dumpPath, stem);
      appendPath(dumpPath, L".dmp");

      BOOL dumpWritten = FALSE;
      DWORD dumpError = ERROR_SUCCESS;
      HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (dump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION information = {};
            information.ThreadId = GetCurrentThreadId();
            information.ExceptionPointers = exception;
            information.ClientPointers = FALSE;
            const MINIDUMP_TYPE type = MINIDUMP_TYPE(
                  MiniDumpNormal
                  | MiniDumpWithDataSegs
                  | MiniDumpWithHandleData
                  | MiniDumpWithIndirectlyReferencedMemory
                  | MiniDumpWithThreadInfo
                  | MiniDumpWithUnloadedModules);
            dumpWritten = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                            dump, type, &information, nullptr, nullptr);
            if (!dumpWritten)
                  dumpError = GetLastError();
            FlushFileBuffers(dump);
            CloseHandle(dump);
            }
      else
            dumpError = GetLastError();

      wchar_t textPath[PATH_CAPACITY] = {};
      lstrcpynW(textPath, crashDirectory, PATH_CAPACITY);
      appendPath(textPath, stem);
      appendPath(textPath, L".txt");
      writeTextRecord(textPath, dumpPath, exception, dumpWritten, dumpError);
      return EXCEPTION_EXECUTE_HANDLER;
      }

}

void installLocalCrashHandler()
      {
      createCrashDirectory();
      SetUnhandledExceptionFilter(crashFilter);
      }

}
