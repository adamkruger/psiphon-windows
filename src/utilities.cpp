/*
* Copyright (c) 2015, Psiphon Inc.
* All rights reserved.
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*/


#include "stdafx.h"
#include "psiclient.h"
#include "logging.h"
#include "config.h"
#include <Shlwapi.h>
#include <ShlObj.h>
#include <WinSock2.h>
#include <TlHelp32.h>
#include <WinCrypt.h>
#include <WinInet.h>
#include "utilities.h"
#include "stopsignal.h"
#include "diagnostic_info.h"
#include "webbrowser.h"
#include <iomanip>

#pragma warning(push, 0)
#pragma warning(disable: 4244)
#include "cryptlib.h"
#include "rsa.h"
#include "base64.h"
#include "osrng.h"
#include "modes.h"
#include "hmac.h"
#pragma warning(pop)


using namespace std::experimental;


extern HINSTANCE g_hInst;

// Adapted from here:
// http://stackoverflow.com/questions/865152/how-can-i-get-a-process-handle-by-its-name-in-c
void TerminateProcessByName(const TCHAR* executableName)
{
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (Process32First(snapshot, &entry))
    {
        do
        {
            if (_tcsicmp(entry.szExeFile, executableName) == 0)
            {
                HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, entry.th32ProcessID);
                if (!TerminateProcess(process, 0) ||
                    WAIT_OBJECT_0 != WaitForSingleObject(process, TERMINATE_PROCESS_WAIT_MS))
                {
                    my_print(NOT_SENSITIVE, false, _T("TerminateProcess failed for process with name %s"), executableName);
                    my_print(NOT_SENSITIVE, false, _T("Please terminate this process manually"));
                }
                CloseHandle(process);
            }
        } while (Process32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
}


bool ExtractExecutable(
    DWORD resourceID,
    const TCHAR* exeFilename,
    tstring& path,
    bool succeedIfExists/*=false*/)
{
    // Extract executable from resources and write to temporary file

    BYTE* data;
    DWORD size;

    if (!GetResourceBytes(resourceID, RT_RCDATA, data, size))
    {
        my_print(NOT_SENSITIVE, false, _T("ExtractExecutable - GetResourceBytes failed (%d)"), GetLastError());
        return false;
    }

    tstring tempPath;
    if (!GetTempPath(tempPath))
    {
        my_print(NOT_SENSITIVE, false, _T("ExtractExecutable - GetTempPath failed (%d)"), GetLastError());
        return false;
    }

    TCHAR filePath[MAX_PATH];
    if (NULL == PathCombine(filePath, tempPath.c_str(), exeFilename))
    {
        my_print(NOT_SENSITIVE, false, _T("ExtractExecutable - PathCombine failed (%d)"), GetLastError());
        return false;
    }

    HANDLE tempFile = INVALID_HANDLE_VALUE;
    bool attemptedTerminate = false;
    while (true)
    {
        tempFile = CreateFile(filePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (tempFile == INVALID_HANDLE_VALUE)
        {
            int lastError = GetLastError();
            if (!attemptedTerminate &&
                ERROR_SHARING_VIOLATION == lastError)
            {
                if (succeedIfExists)
                {
                    // The file must exist, and we can't write to it, most likely because it is
                    // locked by a currently executing process. We can go ahead and consider the
                    // file extracted.
                    // TODO: We should check that the file size and contents are the same. If the file
                    // is different, it would be better to proceed with attempting to extract the
                    // executable and even terminating any locking process -- for example, the locking
                    // process may be a dangling child process left over from before a client upgrade.
                    path = filePath;
                    return true;
                }

                TerminateProcessByName(exeFilename);
                attemptedTerminate = true;
            }
            else
            {
                my_print(NOT_SENSITIVE, false, _T("ExtractExecutable - CreateFile failed (%d)"), lastError);
                return false;
            }
        }
        else
        {
            break;
        }
    }

    DWORD written = 0;
    if (!WriteFile(tempFile, data, size, &written, NULL)
        || written != size
        || !FlushFileBuffers(tempFile))
    {
        auto lastError = GetLastError();
        CloseHandle(tempFile);
        SetLastError(lastError); // restore the previous error code
        my_print(NOT_SENSITIVE, false, _T("ExtractExecutable - WriteFile/FlushFileBuffers failed (%d)"), lastError);
        return false;
    }

    CloseHandle(tempFile);

    path = filePath;

    return true;
}


bool GetDataPath(const vector<tstring>& pathSuffixes, bool ensureExists, tstring& o_path) {
    TCHAR path[MAX_PATH];
    if (!SHGetSpecialFolderPath(NULL, path, CSIDL_APPDATA, FALSE))
    {
        my_print(NOT_SENSITIVE, false, _T("%s - SHGetFolderPath failed (%d)"), __TFUNCTION__, GetLastError());
        return false;
    }

    filesystem::path appDataPath(path);
    auto dataDirectory = filesystem::path(appDataPath);

    for (auto suffix : pathSuffixes) {
        dataDirectory.append(suffix);

        if (ensureExists) {
            if (!CreateDirectory(dataDirectory.c_str(), NULL) && ERROR_ALREADY_EXISTS != GetLastError())
            {
                my_print(NOT_SENSITIVE, false, _T("%s - create directory failed (%d)"), __TFUNCTION__, GetLastError());
                return false;
            }
        }
    }

    o_path = dataDirectory;
    return true;
}


// Caller can check GetLastError() on failure
bool GetTempPath(tstring& o_path)
{
    o_path.clear();

    DWORD ret;
    TCHAR tempPath[MAX_PATH];
    // http://msdn.microsoft.com/en-us/library/aa364991%28v=vs.85%29.aspx notes
    // tempPath can contain no more than MAX_PATH-14 characters
    ret = GetTempPath(MAX_PATH, tempPath);
    if (ret > MAX_PATH - 14 || ret == 0)
    {
        return false;
    }

    o_path = tempPath;
    return true;
}


// Makes an absolute path to a unique temp directory.
// If `create` is true, the directory will also be created.
// Returns true on success, false otherwise. Caller can check GetLastError() on failure.
bool GetUniqueTempDir(tstring& o_path, bool create)
{
    o_path.clear();

    tstring tempPath;
    if (!GetTempPath(tempPath))
    {
        return false;
    }

    tstring guid;
    if (!MakeGUID(guid))
    {
        return false;
    }

    auto tempDir = filesystem::path(tempPath).append(guid);

    if (create && !CreateDirectory(tempDir.tstring().c_str(), NULL)) {
        return false;
    }

    o_path = tempDir.tstring();

    return true;
}

bool GetUniqueTempFilename(const tstring& extension, tstring& o_filepath)
{
    o_filepath.clear();

    tstring tempPath;
    if (!GetTempPath(tempPath))
    {
        return false;
    }

    tstring filename;
    if (!MakeGUID(filename))
    {
        return false;
    }

    if (!extension.empty()) {
        if (extension[0] == _T('.')) {
            filename += extension;
        }
        else {
            filename += _T(".") + extension;
        }
    }

    auto tempFile = filesystem::path(tempPath).append(filename);

    o_filepath = tempFile.tstring();

    return true;
}


// Makes a GUID string. Returns true on success, false otherwise.
bool MakeGUID(tstring& o_guid) {
    o_guid.clear();

    GUID g;
    if (CoCreateGuid(&g) != S_OK)
    {
        return false;
    }

    TCHAR guidString[128];

    if (StringFromGUID2(g, guidString, sizeof(guidString) / sizeof(TCHAR)) <= 0)
    {
        return false;
    }

    o_guid = guidString;

    return true;
}


// Caller can check GetLastError() on failure
bool GetShortPathName(const tstring& path, tstring& o_shortPath)
{
    DWORD ret = GetShortPathName(path.c_str(), NULL, 0);
    if (ret == 0)
    {
        return false;
    }
    TCHAR* buffer = new TCHAR[ret];
    ret = GetShortPathName(path.c_str(), buffer, ret);
    if (ret == 0)
    {
        delete[] buffer;
        return false;
    }
    o_shortPath = buffer;
    delete[] buffer;
    return true;
}


bool WriteFile(const tstring& filename, const string& data)
{
    HANDLE file;
    DWORD bytesWritten;
    if (INVALID_HANDLE_VALUE == (file = CreateFile(
        filename.c_str(), GENERIC_WRITE, 0,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL))
        || !WriteFile(file, data.c_str(), data.length(), &bytesWritten, NULL)
        || bytesWritten != data.length())
    {
        auto lastError = GetLastError();
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
            SetLastError(lastError); // restore the previous error code
        }
        my_print(NOT_SENSITIVE, false, _T("%s - write file failed (%d)"), __TFUNCTION__, lastError);
        return false;
    }
    CloseHandle(file);
    return true;
}


DWORD WaitForConnectability(
    USHORT port,
    DWORD timeout,
    HANDLE process,
    const StopInfo& stopInfo)
{
    // There are a number of options for monitoring the connected status
    // of plonk/polipo. We're going with a quick and dirty solution of
    // (a) monitoring the child processes -- if they exit, there was an error;
    // (b) asynchronously connecting to the plonk SOCKS server, which isn't
    //     started by plonk until its ssh tunnel is established.
    // Note: piping stdout/stderr of the child processes and monitoring
    // messages is problematic because we don't control the C I/O flushing
    // of these processes (http://support.microsoft.com/kb/190351).
    // Additional measures or alternatives include making actual HTTP
    // requests through the entire stack from time to time or switching
    // to integrated ssh/http libraries with APIs.

    if (port <= 0 || port > 0xFFFF)
    {
        return ERROR_UNKNOWN_PORT;
    }

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port = htons(port);

    SOCKET sock = INVALID_SOCKET;
    WSAEVENT connectedEvent = WSACreateEvent();
    WSANETWORKEVENTS networkEvents;

    // Wait up to SSH_CONNECTION_TIMEOUT_SECONDS, checking periodically for user cancel

    DWORD start = GetTickCount();
    DWORD maxWaitMilliseconds = timeout;

    DWORD returnValue = ERROR_SUCCESS;

    while (true)
    {
        DWORD now = GetTickCount();

        if (now < start // Note: GetTickCount wraps after 49 days; small chance of a shorter timeout
            || now >= start + maxWaitMilliseconds)
        {
            returnValue = WAIT_TIMEOUT;
            break;
        }

        // Attempt to connect to SOCKS proxy
        // Just wait 100 ms. and then check for user cancel etc.

        closesocket(sock);
        sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (INVALID_SOCKET != sock
            && 0 == WSAEventSelect(sock, connectedEvent, FD_CONNECT)
            && SOCKET_ERROR == connect(sock, (SOCKADDR*)&serverAddr, sizeof(serverAddr))
            && WSAEWOULDBLOCK == WSAGetLastError()
            && WSA_WAIT_EVENT_0 == WSAWaitForMultipleEvents(1, &connectedEvent, TRUE, 100, FALSE)
            && 0 == WSAEnumNetworkEvents(sock, connectedEvent, &networkEvents)
            && (networkEvents.lNetworkEvents & FD_CONNECT)
            && networkEvents.iErrorCode[FD_CONNECT_BIT] == 0)
        {
            returnValue = ERROR_SUCCESS;
            break;
        }

        // If server aborted, give up

        if (process != NULL
            && WAIT_OBJECT_0 == WaitForSingleObject(process, 0))
        {
            returnValue = ERROR_SYSTEM_PROCESS_TERMINATED;
            break;
        }

        // Check if cancel is signalled

        if (stopInfo.stopSignal != 0 &&
            stopInfo.stopSignal->CheckSignal(stopInfo.stopReasons))
        {
            returnValue = ERROR_OPERATION_ABORTED;
            break;
        }
    }

    closesocket(sock);
    WSACloseEvent(connectedEvent);
    WSACleanup();

    return returnValue;
}


bool TestForOpenPort(int& targetPort, int maxIncrement, const StopInfo& stopInfo)
{
    int maxPort = targetPort + maxIncrement;
    do
    {
        if (targetPort > 0 && targetPort <= 0xFFFF)
        {
            if (ERROR_SUCCESS != WaitForConnectability(
                (USHORT)targetPort,
                100,
                0,
                stopInfo))
            {
                return true;
            }
            my_print(NOT_SENSITIVE, false, _T("Localhost port %d is already in use."), targetPort);
        }
    } while (++targetPort <= maxPort);

    return false;
}


void StopProcess(DWORD processID, HANDLE process)
{
    // TODO: AttachConsole/FreeConsole sequence not threadsafe?
    if (AttachConsole(processID))
    {
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, processID);
        FreeConsole();
        if (WAIT_OBJECT_0 == WaitForSingleObject(process, 100))
        {
            return;
        }
    }
    if (!TerminateProcess(process, 0) ||
        WAIT_OBJECT_0 != WaitForSingleObject(process, TERMINATE_PROCESS_WAIT_MS))
    {
        my_print(NOT_SENSITIVE, false, _T("TerminateProcess failed for process with PID %d"), processID);
    }
}


// Create the pipe that will be used to communicate between the child process
// process and this process.
// Note that this function effectively causes the subprocess's stdout and stderr
// to come to the same pipe.
// Returns true on success.
bool CreateSubprocessPipes(
    HANDLE& o_parentOutputPipe, // Parent reads the child's stdout/stdin from this
    HANDLE& o_parentInputPipe,  // Parent writes to the child's stdin with this
    HANDLE& o_childStdinPipe,   // Child's stdin pipe
    HANDLE& o_childStdoutPipe,  // Child's stdout pipe
    HANDLE& o_childStderrPipe)  // Child's stderr pipe (dup of stdout)
{
    o_parentOutputPipe = INVALID_HANDLE_VALUE;
    o_parentInputPipe = INVALID_HANDLE_VALUE;
    o_childStdinPipe = INVALID_HANDLE_VALUE;
    o_childStdoutPipe = INVALID_HANDLE_VALUE;
    o_childStderrPipe = INVALID_HANDLE_VALUE;

    // Most of this code is adapted from:
    // http://support.microsoft.com/kb/190351

    // Set up the security attributes struct.
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE
        hParentOutputReadTmp = INVALID_HANDLE_VALUE,
        hParentOutputRead = INVALID_HANDLE_VALUE,
        hChildStdoutWrite = INVALID_HANDLE_VALUE,
        hChildStderrWrite = INVALID_HANDLE_VALUE,
        hChildStdinRead = INVALID_HANDLE_VALUE,
        hParentInputWriteTmp = INVALID_HANDLE_VALUE,
        hParentInputWrite = INVALID_HANDLE_VALUE;

    // Create the child output pipe.
    if (!CreatePipe(&hParentOutputReadTmp, &hChildStdoutWrite, &sa, 0))
    {
        if (hParentOutputReadTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputReadTmp);
        if (hParentOutputRead != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputRead);
        if (hChildStdoutWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStdoutWrite);
        if (hChildStderrWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStderrWrite);
        if (hChildStdinRead != INVALID_HANDLE_VALUE) CloseHandle(hChildStdinRead);
        if (hParentInputWriteTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWriteTmp);
        if (hParentInputWrite != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWrite);
        my_print(NOT_SENSITIVE, false, _T("%s:%d - CreatePipe failed (%d)"), __TFUNCTION__, __LINE__, GetLastError());
        return false;
    }

    // Create a duplicate of the output write handle for the std error
    // write handle. This is necessary in case the child application
    // closes one of its std output handles.
    if (!DuplicateHandle(
        GetCurrentProcess(), hChildStdoutWrite,
        GetCurrentProcess(), &hChildStderrWrite,
        0, TRUE, DUPLICATE_SAME_ACCESS))
    {
        if (hParentOutputReadTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputReadTmp);
        if (hParentOutputRead != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputRead);
        if (hChildStdoutWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStdoutWrite);
        if (hChildStderrWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStderrWrite);
        if (hChildStdinRead != INVALID_HANDLE_VALUE) CloseHandle(hChildStdinRead);
        if (hParentInputWriteTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWriteTmp);
        if (hParentInputWrite != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWrite);
        my_print(NOT_SENSITIVE, false, _T("%s:%d - DuplicateHandle failed (%d)"), __TFUNCTION__, __LINE__, GetLastError());
        return false;
    }

    // Create new output read handle and the input write handles. Set
    // the Properties to FALSE. Otherwise, the child inherits the
    // properties and, as a result, non-closeable handles to the pipes
    // are created.
    if (!DuplicateHandle(GetCurrentProcess(), hParentOutputReadTmp,
        GetCurrentProcess(),
        &hParentOutputRead, // Address of new handle.
        0,
        FALSE, // Make it uninheritable.
        DUPLICATE_SAME_ACCESS))
    {
        if (hParentOutputReadTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputReadTmp);
        if (hParentOutputRead != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputRead);
        if (hChildStdoutWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStdoutWrite);
        if (hChildStderrWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStderrWrite);
        if (hChildStdinRead != INVALID_HANDLE_VALUE) CloseHandle(hChildStdinRead);
        if (hParentInputWriteTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWriteTmp);
        if (hParentInputWrite != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWrite);
        my_print(NOT_SENSITIVE, false, _T("%s:%d - DuplicateHandle failed (%d)"), __TFUNCTION__, __LINE__, GetLastError());
        return false;
    }

    // Close inheritable copies of the handles you do not want to be
    // inherited.
    if (!CloseHandle(hParentOutputReadTmp))
    {
        if (hParentOutputReadTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputReadTmp);
        if (hParentOutputRead != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputRead);
        if (hChildStdoutWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStdoutWrite);
        if (hChildStderrWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStderrWrite);
        if (hChildStdinRead != INVALID_HANDLE_VALUE) CloseHandle(hChildStdinRead);
        if (hParentInputWriteTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWriteTmp);
        if (hParentInputWrite != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWrite);
        my_print(NOT_SENSITIVE, false, _T("%s:%d - CloseHandle failed (%d)"), __TFUNCTION__, __LINE__, GetLastError());
        return false;
    }
    hParentOutputReadTmp = INVALID_HANDLE_VALUE;

    // Create the pipe the parent can use to write to the child's stdin
    if (!CreatePipe(&hChildStdinRead, &hParentInputWriteTmp, &sa, 0))
    {
        if (hParentOutputReadTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputReadTmp);
        if (hParentOutputRead != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputRead);
        if (hChildStdoutWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStdoutWrite);
        if (hChildStderrWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStderrWrite);
        if (hChildStdinRead != INVALID_HANDLE_VALUE) CloseHandle(hChildStdinRead);
        if (hParentInputWriteTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWriteTmp);
        if (hParentInputWrite != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWrite);
        my_print(NOT_SENSITIVE, false, _T("%s:%d - CreatePipe failed (%d)"), __TFUNCTION__, __LINE__, GetLastError());
        return false;
    }

    // Duplicate the parent's end of the pipe, so the child can't inherit it.
    if (!DuplicateHandle(GetCurrentProcess(), hParentInputWriteTmp,
        GetCurrentProcess(),
        &hParentInputWrite, // Address of new handle.
        0,
        FALSE, // Make it uninheritable.
        DUPLICATE_SAME_ACCESS))
    {
        if (hParentOutputReadTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputReadTmp);
        if (hParentOutputRead != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputRead);
        if (hChildStdoutWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStdoutWrite);
        if (hChildStderrWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStderrWrite);
        if (hChildStdinRead != INVALID_HANDLE_VALUE) CloseHandle(hChildStdinRead);
        if (hParentInputWriteTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWriteTmp);
        if (hParentInputWrite != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWrite);
        my_print(NOT_SENSITIVE, false, _T("%s:%d - DuplicateHandle failed (%d)"), __TFUNCTION__, __LINE__, GetLastError());
        return false;
    }

    // Close inheritable copies of the handles you do not want to be
    // inherited.
    if (!CloseHandle(hParentInputWriteTmp))
    {
        if (hParentOutputReadTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputReadTmp);
        if (hParentOutputRead != INVALID_HANDLE_VALUE) CloseHandle(hParentOutputRead);
        if (hChildStdoutWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStdoutWrite);
        if (hChildStderrWrite != INVALID_HANDLE_VALUE) CloseHandle(hChildStderrWrite);
        if (hChildStdinRead != INVALID_HANDLE_VALUE) CloseHandle(hChildStdinRead);
        if (hParentInputWriteTmp != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWriteTmp);
        if (hParentInputWrite != INVALID_HANDLE_VALUE) CloseHandle(hParentInputWrite);
        my_print(NOT_SENSITIVE, false, _T("%s:%d - CloseHandle failed (%d)"), __TFUNCTION__, __LINE__, GetLastError());
        return false;
    }
    hParentInputWriteTmp = INVALID_HANDLE_VALUE;

    o_parentOutputPipe = hParentOutputRead;
    o_parentInputPipe = hParentInputWrite;
    o_childStdoutPipe = hChildStdoutWrite;
    o_childStderrPipe = hChildStderrWrite;
    o_childStdinPipe = hChildStdinRead;

    return true;
}


bool DoesRegistryValueExist(const string& name)
{
    HKEY key = 0;

    LONG returnCode = RegOpenKeyEx(
        HKEY_CURRENT_USER,
        LOCAL_SETTINGS_REGISTRY_KEY,
        0,
        KEY_READ,
        &key);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegOpenKeyEx failed for '%hs' with code %ld"), __TFUNCTION__, name.c_str(), returnCode);
        return false;
    }

    DWORD type;
    returnCode = RegQueryValueExA(
        key,
        name.c_str(),
        NULL,
        &type,
        NULL,
        NULL);

    auto lastError = GetLastError();
    RegCloseKey(key);
    SetLastError(lastError); // restore the previous error code

    return returnCode == ERROR_SUCCESS;
}


bool WriteRegistryDwordValue(const string& name, DWORD value)
{
    HKEY key = 0;
    DWORD disposition = 0;
    DWORD bufferLength = sizeof(value);

    LONG returnCode = RegCreateKeyEx(
        HKEY_CURRENT_USER,
        LOCAL_SETTINGS_REGISTRY_KEY,
        0,
        0,
        0,
        KEY_WRITE,
        0,
        &key,
        &disposition);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegCreateKeyEx failed for '%hs' with code %ld"), __TFUNCTION__, name.c_str(), returnCode);
        return false;
    }

    auto closeKey = finally([=]() {
        auto lastError = GetLastError();
        RegCloseKey(key);
        SetLastError(lastError); // restore the previous error code
    });

    returnCode = RegSetValueExA(
                key,
                name.c_str(),
                0,
                REG_DWORD,
                (LPBYTE)&value,
                bufferLength);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegSetValueExA failed for '%hs' with code %ld"), __TFUNCTION__, name.c_str(), returnCode);
        return false;
    }

    return true;
}


bool ReadRegistryDwordValue(const string& name, DWORD& value)
{
    HKEY key = 0;
    DWORD bufferLength = sizeof(value);
    DWORD type;

    LONG returnCode = RegOpenKeyEx(
                        HKEY_CURRENT_USER,
                        LOCAL_SETTINGS_REGISTRY_KEY,
                        0,
                        KEY_READ,
                        &key);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegOpenKeyEx failed for '%hs' with code %ld"), __TFUNCTION__, name.c_str(), returnCode);
        return false;
    }

    auto closeKey = finally([=]() {
        auto lastError = GetLastError();
        RegCloseKey(key);
        SetLastError(lastError); // restore the previous error code
    });

    returnCode = RegQueryValueExA(
                    key,
                    name.c_str(),
                    0,
                    &type,
                    (LPBYTE)&value,
                    &bufferLength);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegQueryValueExA failed for '%hs' with code %ld"), __TFUNCTION__, name.c_str(), returnCode);
        return false;
    }

    if (type != REG_DWORD)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegQueryValueExA says type of '%hs' is %ld, not REG_DWORD"), __TFUNCTION__, name.c_str(), type);
        return false;
    }

    return true;
}


bool WriteRegistryStringValue(const string& name, const string& value, RegistryFailureReason& reason)
{
    HKEY key = 0;
    reason = REGISTRY_FAILURE_NO_REASON;

    LONG returnCode = RegCreateKeyEx(
                        HKEY_CURRENT_USER,
                        LOCAL_SETTINGS_REGISTRY_KEY,
                        0,
                        0,
                        0,
                        KEY_WRITE,
                        0,
                        &key,
                        0);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegCreateKeyEx failed for '%hs' with code %ld"), __TFUNCTION__, name.c_str(), returnCode);
        return false;
    }

    auto closeKey = finally([=]() {
        auto lastError = GetLastError();
        RegCloseKey(key);
        SetLastError(lastError); // restore the previous error code
    });

    returnCode = RegSetValueExA(
        key,
        name.c_str(),
        0,
        REG_SZ,
        (LPBYTE)value.c_str(),
        value.length() + 1); // Write the null terminator
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegSetValueExA failed for '%hs' with code %ld"), __TFUNCTION__, name.c_str(), returnCode);

        if (ERROR_NO_SYSTEM_RESOURCES == returnCode)
        {
            reason = REGISTRY_FAILURE_WRITE_TOO_LONG;
        }

        return false;
    }

    return true;
}


bool WriteRegistryStringValue(const string& name, const wstring& value, RegistryFailureReason& reason)
{
    HKEY key = 0;
    reason = REGISTRY_FAILURE_NO_REASON;
    wstring wName = UTF8ToWString(name);

    LONG returnCode = RegCreateKeyEx(
                        HKEY_CURRENT_USER,
                        LOCAL_SETTINGS_REGISTRY_KEY,
                        0,
                        0,
                        0,
                        KEY_WRITE,
                        0,
                        &key,
                        0);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegCreateKeyEx failed for '%hs' with code %ld"), __TFUNCTION__, name.c_str(), returnCode);
        return false;
    }

    auto closeKey = finally([=]() {
        auto lastError = GetLastError();
        RegCloseKey(key);
        SetLastError(lastError); // restore the previous error code
    });

    returnCode = RegSetValueExW(
        key,
        wName.c_str(),
        0,
        REG_SZ,
        (LPBYTE)value.c_str(),
        (value.length() + 1) * sizeof(wchar_t)); // Write the null terminator
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegSetValueExW failed for '%hs' with code %ld"), __TFUNCTION__, name.c_str(), returnCode);

        if (ERROR_NO_SYSTEM_RESOURCES == returnCode)
        {
            reason = REGISTRY_FAILURE_WRITE_TOO_LONG;
        }

        return false;
    }

    return true;
}


bool ReadRegistryStringValue(LPCSTR name, string& value)
{
    value.clear();

    HKEY key = 0;
    LONG returnCode = RegOpenKeyEx(
                        HKEY_CURRENT_USER,
                        LOCAL_SETTINGS_REGISTRY_KEY,
                        0,
                        KEY_READ,
                        &key);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegOpenKeyEx failed for '%hs' with code %ld"), __TFUNCTION__, name, returnCode);
        return false;
    }

    auto closeKey = finally([=]() {
        auto lastError = GetLastError();
        RegCloseKey(key);
        SetLastError(lastError); // restore the previous error code
    });

    DWORD bufferLength = 0;
    returnCode = RegQueryValueExA(
                    key,
                    name,
                    0,
                    0,
                    NULL,
                    &bufferLength);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegQueryValueExA(1) failed for '%hs' with code %ld"), __TFUNCTION__, name, returnCode);
        return false;
    }

    // resize to string length excluding the terminating null character
    if (bufferLength > 0)
    {
        value.resize(bufferLength - 1);
    }

    DWORD type;
    returnCode = RegQueryValueExA(
                    key,
                    name,
                    0,
                    &type,
                    (LPBYTE)value.c_str(),
                    &bufferLength);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegQueryValueExA(2) failed for '%hs' with code %ld"), __TFUNCTION__, name, returnCode);
        return false;
    }

    if (type != REG_SZ)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegQueryValueExA says type of '%hs' is %ld, not REG_SZ"), __TFUNCTION__, name, type);
        return false;
    }

    return true;
}

bool ReadRegistryStringValue(LPCSTR name, wstring& value)
{
    value.clear();

    wstring wName = UTF8ToWString(name);

    HKEY key = 0;
    LONG returnCode = RegOpenKeyEx(
                        HKEY_CURRENT_USER,
                        LOCAL_SETTINGS_REGISTRY_KEY,
                        0,
                        KEY_READ,
                        &key);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegOpenKeyEx failed for '%hs' with code %ld"), __TFUNCTION__, name, returnCode);
        return false;
    }

    auto closeKey = finally([=]() {
        auto lastError = GetLastError();
        RegCloseKey(key);
        SetLastError(lastError); // restore the previous error code
    });

    DWORD bufferLength = 0;
    returnCode = RegQueryValueExW(
                    key,
                    wName.c_str(),
                    0,
                    0,
                    NULL,
                    &bufferLength);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegQueryValueExW(1) failed for '%hs' with code %ld"), __TFUNCTION__, name, returnCode);
        return false;
    }

    // bufferLength is the size of the data in bytes.
    if (bufferLength % sizeof(wchar_t) != 0) {
        my_print(NOT_SENSITIVE, true, _T("%s: RegQueryValueExW(1) for %hs says bufferLength is not a multiple of sizeof(wchar_t): %ld"), __TFUNCTION__, name, bufferLength);
        return false;
    }

    // resize to string length excluding the terminating null character
    if (bufferLength > 0)
    {
        value.resize((bufferLength / sizeof(wchar_t)) - 1);
    }

    DWORD type;
    returnCode = RegQueryValueExW(
        key,
        wName.c_str(),
        0,
        &type,
        (LPBYTE)value.c_str(),
        &bufferLength);
    if (returnCode != ERROR_SUCCESS)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegQueryValueExW(2) failed for '%hs' with code %ld"), __TFUNCTION__, name, returnCode);
        return false;
    }

    if (type != REG_SZ)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: RegQueryValueExW says type of '%hs' is %ld, not REG_SZ"), __TFUNCTION__, name, type);
        return false;
    }

    return true;
}


int TextHeight(void)
{
    HWND hWnd = CreateWindow(L"Static", 0, 0, 0, 0, 0, 0, 0, 0, g_hInst, 0);
    HGDIOBJ font = GetStockObject(DEFAULT_GUI_FONT);
    SendMessage(hWnd, WM_SETFONT, (WPARAM)font, NULL);
    TEXTMETRIC textMetric;
    BOOL success = GetTextMetrics(GetDC(hWnd), &textMetric);
    DestroyWindow(hWnd);
    return success ? textMetric.tmHeight : 0;
}


int TextWidth(const TCHAR* text)
{
    HWND hWnd = CreateWindow(L"Static", 0, 0, 0, 0, 0, 0, 0, 0, g_hInst, 0);
    HGDIOBJ font = GetStockObject(DEFAULT_GUI_FONT);
    SendMessage(hWnd, WM_SETFONT, (WPARAM)font, NULL);
    SIZE size;
    BOOL success = GetTextExtentPoint32(GetDC(hWnd), text, _tcslen(text), &size);
    DestroyWindow(hWnd);
    return success ? size.cx : 0;
}


int LongestTextWidth(const TCHAR* texts[], int count)
{
    int longestWidth = 0;
    for (int i = 0; i < count; i++)
    {
        int width = TextWidth(texts[i]);
        if (width > longestWidth)
        {
            longestWidth = width;
        }
    }
    return longestWidth;
}


bool TestBoolArray(const vector<const bool*>& boolArray)
{
    for (size_t i = 0; i < boolArray.size(); i++)
    {
        if (*(boolArray[i]))
        {
            return true;
        }
    }

    return false;
}


// Note that this does not work for hex encoding. Its output is stupid.
string CryptBinaryToStringWrapper(const unsigned char* input, size_t length, DWORD flags)
{
    DWORD outsize = 0;

    // Get the required size
    if (!CryptBinaryToStringA(
        input,
        length,
        flags | CRYPT_STRING_NOCR,
        NULL,
        &outsize))
    {
        return "";
    }

    string output;
    output.resize(outsize + 1);

    if (!CryptBinaryToStringA(
        input,
        length,
        flags | CRYPT_STRING_NOCR,
        (LPSTR)output.c_str(),
        &outsize))
    {
        return "";
    }

    ((LPSTR)output.c_str())[outsize] = '\0';

    return output;
}

string CryptStringToBinaryWrapper(const string& input, DWORD flags)
{
    DWORD outsize = 0;

    // Get the required size
    if (!CryptStringToBinaryA(
        input.c_str(),
        input.length(),
        flags,
        NULL,
        &outsize,
        NULL,
        NULL))
    {
        return "";
    }

    string output;
    output.resize(outsize);

    if (!CryptStringToBinaryA(
        input.c_str(),
        input.length(),
        flags,
        (BYTE*)output.c_str(),
        &outsize,
        NULL,
        NULL))
    {
        return "";
    }

    return output;
}


string Base64Encode(const unsigned char* input, size_t length)
{
    return CryptBinaryToStringWrapper(input, length, CRYPT_STRING_BASE64);
}

string Base64Decode(const string& input)
{
    return CryptStringToBinaryWrapper(input, CRYPT_STRING_BASE64);
}


// Adapted from here:
// http://stackoverflow.com/questions/3381614/c-convert-string-to-hexadecimal-and-vice-versa
string Hexlify(const unsigned char* input, size_t length)
{
    static const char* const lut = "0123456789ABCDEF";

    string output;
    output.reserve(2 * length);
    for (size_t i = 0; i < length; ++i)
    {
        const unsigned char c = input[i];
        output.push_back(lut[c >> 4]);
        output.push_back(lut[c & 15]);
    }
    return output;
}

string Dehexlify(const string& input)
{
    static const char* const lut = "0123456789ABCDEF";
    size_t len = input.length();
    if (len & 1)
    {
        throw std::invalid_argument("Dehexlify: odd length");
    }

    string output;
    output.reserve(len / 2);
    for (size_t i = 0; i < len; i += 2)
    {
        char a = (char)toupper(input[i]);
        const char* p = std::lower_bound(lut, lut + 16, a);
        if (*p != a)
        {
            throw std::invalid_argument("Dehexlify: not a hex digit");
        }

        char b = (char)toupper(input[i + 1]);
        const char* q = std::lower_bound(lut, lut + 16, b);
        if (*q != b)
        {
            throw std::invalid_argument("Dehexlify: not a hex digit");
        }

        output.push_back((char)(((p - lut) << 4) | (q - lut)));
    }

    return output;
}


// Adapted from:
// http://stackoverflow.com/questions/154536/encode-decode-urls-in-c
// NOTE that the encode version does not seem to work
tstring UrlCodec(const tstring& input, bool encode)
{
    assert(!encode);

    DWORD flags = encode ? 0 : ICU_DECODE;
    tstring encodedURL = _T("");
    DWORD outputBufferSize = input.size() * 2;
    LPTSTR outputBuffer = new TCHAR[outputBufferSize];
    BOOL result = ::InternetCanonicalizeUrl(input.c_str(), outputBuffer, &outputBufferSize, flags);
    DWORD error = ::GetLastError();
    if (!result && error == ERROR_INSUFFICIENT_BUFFER)
    {
        delete[] outputBuffer;
        outputBuffer = new TCHAR[outputBufferSize];
        result = ::InternetCanonicalizeUrl(input.c_str(), outputBuffer, &outputBufferSize, flags);
    }

    if (result)
    {
        encodedURL = outputBuffer;
    }
    else
    {
        my_print(NOT_SENSITIVE, true, _T("%s: InternetCanonicalizeUrl failed for %s with code %ld"), __TFUNCTION__, input.c_str(), GetLastError());
    }

    if (outputBuffer != 0)
    {
        delete[] outputBuffer;
        outputBuffer = 0;
    }

    return encodedURL;
}

// Adapted from:
// http://stackoverflow.com/questions/154536/encode-decode-urls-in-c
tstring UrlEncode(const tstring& input)
{
    tstringstream escapedURL;
    escapedURL.fill('0');
    escapedURL << hex;

    for (tstring::const_iterator i = input.begin(), n = input.end(); i != n; ++i) {
        tstring::value_type c = (*i);

        // Keep alphanumeric and other accepted characters intact
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escapedURL << c;
            continue;
        }

        // Any other characters are percent-encoded
        escapedURL << uppercase;
        escapedURL << '%' << setw(2) << int((unsigned char)c);
        escapedURL << nouppercase;
    }

    return escapedURL.str();
}

tstring UrlDecode(const tstring& input)
{
    return UrlCodec(input, false);
}


tstring GetLocaleName()
{
    int size = GetLocaleInfo(
        LOCALE_USER_DEFAULT,
        LOCALE_SISO639LANGNAME,
        NULL,
        0);

    if (size <= 0)
    {
        return _T("");
    }

    LPTSTR buf = new TCHAR[size];

    size = GetLocaleInfo(
        LOCALE_USER_DEFAULT,
        LOCALE_SISO639LANGNAME,
        buf,
        size);

    if (size <= 0)
    {
        return _T("");
    }

    tstring ret = buf;

    delete[] buf;

    return ret;
}


tstring GetISO8601DatetimeString()
{
    SYSTEMTIME systime;
    GetSystemTime(&systime);

    TCHAR ret[64];
    _sntprintf_s(
        ret,
        sizeof(ret) / sizeof(ret[0]),
        _T("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ"),
        systime.wYear,
        systime.wMonth,
        systime.wDay,
        systime.wHour,
        systime.wMinute,
        systime.wSecond,
        systime.wMilliseconds);

    return ret;
}


static wstring g_uiLocale;
void SetUiLocale(const wstring& uiLocale)
{
    g_uiLocale = uiLocale;
}

wstring GetDeviceRegion()
{
    // There are a few different indicators of the device region, none of which
    // are perfect. So we'll look at what indicators we have and take a best guess.

    //
    // Read the system dialing code and convert to a country code.
    // Based on comparing user feedback language to dialing code, we have found
    // that dialing code is correct about 65% of the time.
    //

    // Multiple countries can have the same dialing code (such as
    // the US, Canada, and Puerto Rico all using '1'), so we'll need
    // a vector of possibilities.
    vector<wstring> dialingCodeCountries;

    wstring countryDialingCode;
    if (GetCountryDialingCode(countryDialingCode)
        && countryDialingCode.length() > 0)
    {
        BYTE* countryDialingCodesBytes = 0;
        DWORD countryDialingCodesLen = 0;
        if (GetResourceBytes(
            _T("COUNTRY_DIALING_CODES.JSON"), RT_RCDATA,
            countryDialingCodesBytes, countryDialingCodesLen))
        {
            Json::Value json;
            Json::Reader reader;

            string utf8JSON((char*)countryDialingCodesBytes, countryDialingCodesLen);
            bool parsingSuccessful = reader.parse(utf8JSON, json);
            if (parsingSuccessful)
            {
                // Sometimes (for some reason) the country dialing code given by the system
                // has an additional trailing digit. So we'll also match against a truncated
                // version of that value. If we don't get a match on the full value, we'll
                // use the matches on the truncated value.
                wstring countryDialingCodeTruncated;
                vector<wstring> dialingCodeCountriesTruncatedMatch;
                if (countryDialingCode.length() > 1)
                {
                    countryDialingCodeTruncated = countryDialingCode.substr(0, countryDialingCode.length() - 1);
                }

                for (const Json::Value& entry : json)
                {
                    wstring entryDialingCode = UTF8ToWString(entry.get("dialing_code", "").asString());
                    if (!entryDialingCode.empty()
                        && entryDialingCode == countryDialingCode)
                    {
                        wstring entryCountryCode = UTF8ToWString(entry.get("country_code", "").asString());
                        if (!entryCountryCode.empty())
                        {
                            std::transform(entryCountryCode.begin(), entryCountryCode.end(), entryCountryCode.begin(), ::toupper);
                            dialingCodeCountries.push_back(entryCountryCode);
                        }
                    }

                    if (!entryDialingCode.empty()
                        && entryDialingCode == countryDialingCodeTruncated)
                    {
                        wstring entryCountryCode = UTF8ToWString(entry.get("country_code", "").asString());
                        if (!entryCountryCode.empty())
                        {
                            std::transform(entryCountryCode.begin(), entryCountryCode.end(), entryCountryCode.begin(), ::toupper);
                            dialingCodeCountriesTruncatedMatch.push_back(entryCountryCode);
                        }
                    }
                }

                if (dialingCodeCountries.empty() && !dialingCodeCountriesTruncatedMatch.empty())
                {
                    // We failed to match on the full country dialing code, but
                    // we did get matches on the truncated form.
                    dialingCodeCountries = dialingCodeCountriesTruncatedMatch;
                }
            }
            else
            {
                my_print(NOT_SENSITIVE, true, _T("%s:%d: Failed to parse country dialing codes JSON"), __TFUNCTION__, __LINE__);
            }
        }
        else
        {
            my_print(NOT_SENSITIVE, true, _T("%s:%d: Failed to load country dialing codes JSON resource"), __TFUNCTION__, __LINE__);
        }
    }
    else
    {
        my_print(NOT_SENSITIVE, true, _T("%s:%d: GetCountryDialingCode failed"), __TFUNCTION__, __LINE__);
    }

    // At this point, dialingCodeCountries either has a value or is unusable.

    //
    // Derive region from UI locale.
    //

    // Country information defaults to "US", so that tells us very little.
    const wstring GENERIC_COUNTRY = L"US";

    wstring uiLocaleUpper = g_uiLocale;
    std::transform(uiLocaleUpper.begin(), uiLocaleUpper.end(), uiLocaleUpper.begin(), ::toupper);

    // This is hand-wavy, imperfect, and will need to be expanded in the future.
    std::map<wstring, wstring> localeToCountryMap = {
        { L"AR", L"SA" },
        { L"EN", L"US" },
        { L"FA", L"IR" },
        { L"RU", L"RU" },
        { L"TK", L"TM" },
        { L"TR", L"TR" },
        { L"VI", L"VN" },
        { L"ZH", L"CN" },
    };

    wstring uiLocaleCountry = localeToCountryMap[uiLocaleUpper];

    //
    // Combine values to make best guess.
    //

    // If we have a non-generic dialing code country, use that.
    // We'll prefer using this over the locale, because many of our two-letter
    // locale/language codes (e.g., "FA") might be used by multiple countries
    // (e.g., Iran, Afghanistan, Tajikistan, Uzbekistan, etc.).

    auto genericCountryPos = std::find(
        dialingCodeCountries.begin(), dialingCodeCountries.end(),
        GENERIC_COUNTRY);
    bool genericCountryInDialingCodeCountries = (genericCountryPos != dialingCodeCountries.end());
    if (!dialingCodeCountries.empty() && !genericCountryInDialingCodeCountries)
    {
        // We'll use the locale to help us pick which among the dialing code countries
        // we should use.

        auto localePos = std::find(
            dialingCodeCountries.begin(), dialingCodeCountries.end(),
            uiLocaleCountry);

        if (!uiLocaleCountry.empty() && localePos != dialingCodeCountries.end())
        {
            my_print(NOT_SENSITIVE, true, _T("%s:%d: uiLocaleCountry found in dialingCodeCountries: %s"), __TFUNCTION__, __LINE__, (*localePos).c_str());
            return *localePos;
        }

        // The locale didn't help, and there's no other way of distinguishing
        // between the matched countries, so just use the first one.
        my_print(NOT_SENSITIVE, true, _T("%s:%d: using first dialingCodeCountries: %s"), __TFUNCTION__, __LINE__, (*dialingCodeCountries.begin()).c_str());
        return *dialingCodeCountries.begin();
    }

    // If we have a UI locale value, use it, even if it's generic.
    if (!uiLocaleCountry.empty())
    {
        my_print(NOT_SENSITIVE, true, _T("%s:%d: using uiLocaleCountry: %s"), __TFUNCTION__, __LINE__, uiLocaleCountry.c_str());
        return uiLocaleCountry;
    }

    // We have no info to work with.
    my_print(NOT_SENSITIVE, true, _T("%s:%d: uiLocaleCountry and dialingCodeCountries are empty"), __TFUNCTION__, __LINE__);
    return L"";
}

bool IsOSSupported()
{
    static int cachedResult = 0;
    if (cachedResult != 0) {
        return cachedResult > 0;
    }

    OSVERSIONINFO osver = { sizeof(osver) };

    if (!GetVersionEx(&osver))
    {
        // Default to true. This is effectively "default to allowing the app to try to work",
        // since this function is used to determine if we're on an unsupported platform.
        return true;
    }

    // Windows 7 is major:6 minor:1. https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-osversioninfoa#remarks
    bool supported = (osver.dwMajorVersion > 6) || (osver.dwMajorVersion == 6 && osver.dwMinorVersion > 0);
    cachedResult = supported ? 1 : -1;

    return supported;
}

void EnforceOSSupport(HWND parentWnd, const wstring& message)
{
    if (IsOSSupported())
    {
        return;
    }

    const wstring url = L"https://psiphon3.com/faq.html#windows-xp-eol";
    const wstring messageURL = message + L"\n" + url;

    ::MessageBoxW(parentWnd, messageURL.c_str(), L"Psiphon", MB_OK | MB_ICONSTOP);
    OpenBrowser(url);
    ExitProcess(1);
}


/*
Resource Utilities
*/

bool GetResourceBytes(DWORD name, DWORD type, BYTE*& o_pBytes, DWORD& o_size)
{
    return GetResourceBytes(MAKEINTRESOURCE(name), MAKEINTRESOURCE(type), o_pBytes, o_size);
}

bool GetResourceBytes(DWORD name, LPCTSTR type, BYTE*& o_pBytes, DWORD& o_size)
{
    return GetResourceBytes(MAKEINTRESOURCE(name), type, o_pBytes, o_size);
}

bool GetResourceBytes(LPCTSTR name, DWORD type, BYTE*& o_pBytes, DWORD& o_size)
{
    return GetResourceBytes(name, MAKEINTRESOURCE(type), o_pBytes, o_size);
}

bool GetResourceBytes(LPCTSTR name, LPCTSTR type, BYTE*& o_pBytes, DWORD& o_size)
{
    o_pBytes = NULL;
    o_size = 0;

    HRSRC res;
    HGLOBAL handle = INVALID_HANDLE_VALUE;

    res = FindResource(g_hInst, name, type);
    if (!res)
    {
        my_print(NOT_SENSITIVE, false, _T("GetResourceBytes - FindResource failed (%d)"), GetLastError());
        return false;
    }

    handle = LoadResource(NULL, res);
    if (!handle)
    {
        my_print(NOT_SENSITIVE, false, _T("GetResourceBytes - LoadResource failed (%d)"), GetLastError());
        return false;
    }

    o_pBytes = (BYTE*)LockResource(handle);
    o_size = SizeofResource(NULL, res);

    return true;
}


/*
* Feedback Encryption
*/

bool PublicKeyEncryptData(const char* publicKey, const char* plaintext, string& o_encrypted)
{
    o_encrypted.clear();

    CryptoPP::AutoSeededRandomPool rng;

    string b64Ciphertext, b64Mac, b64WrappedEncryptionKey, b64WrappedMacKey, b64IV;

    try
    {
        string ciphertext, mac, wrappedEncryptionKey, wrappedMacKey;

        // NOTE: We are doing encrypt-then-MAC.

        // CryptoPP::AES::MIN_KEYLENGTH is 128 bits.
        int KEY_LENGTH = CryptoPP::AES::MIN_KEYLENGTH;

        //
        // Encrypt
        //

        CryptoPP::SecByteBlock encryptionKey(KEY_LENGTH);
        rng.GenerateBlock(encryptionKey, encryptionKey.size());

        byte iv[CryptoPP::AES::BLOCKSIZE];
        rng.GenerateBlock(iv, CryptoPP::AES::BLOCKSIZE);

        CryptoPP::CBC_Mode<CryptoPP::AES>::Encryption encryptor;
        encryptor.SetKeyWithIV(encryptionKey, encryptionKey.size(), iv);

        CryptoPP::StringSource(
            plaintext,
            true,
            new CryptoPP::StreamTransformationFilter(
                encryptor,
                new CryptoPP::StringSink(ciphertext),
                CryptoPP::StreamTransformationFilter::PKCS_PADDING));

        CryptoPP::StringSource(
            ciphertext,
            true,
            new CryptoPP::Base64Encoder(
                new CryptoPP::StringSink(b64Ciphertext),
                false));

        size_t ivLength = sizeof(iv) * sizeof(iv[0]);
        CryptoPP::StringSource(
            iv,
            ivLength,
            true,
            new CryptoPP::Base64Encoder(
                new CryptoPP::StringSink(b64IV),
                false));

        //
        // HMAC
        //

        // Include the IV in the MAC'd data, as per http://tools.ietf.org/html/draft-mcgrew-aead-aes-cbc-hmac-sha2-01
        size_t ciphertextLength = ciphertext.length() * sizeof(ciphertext[0]);
        byte* ivPlusCiphertext = new byte[ivLength + ciphertextLength];
        if (!ivPlusCiphertext)
        {
            return false;
        }
        memcpy(ivPlusCiphertext, iv, ivLength);
        memcpy(ivPlusCiphertext + ivLength, ciphertext.data(), ciphertextLength);

        CryptoPP::SecByteBlock macKey(KEY_LENGTH);
        rng.GenerateBlock(macKey, macKey.size());

        CryptoPP::HMAC<CryptoPP::SHA256> hmac(macKey, macKey.size());

        CryptoPP::StringSource(
            ivPlusCiphertext,
            ivLength + ciphertextLength,
            true,
            new CryptoPP::HashFilter(
                hmac,
                new CryptoPP::StringSink(mac)));

        delete[] ivPlusCiphertext;

        CryptoPP::StringSource(
            mac,
            true,
            new CryptoPP::Base64Encoder(
                new CryptoPP::StringSink(b64Mac),
                false));

        //
        // Wrap the keys
        //

#pragma warning(push, 0)
#pragma warning(disable: 4239)
        CryptoPP::RSAES_OAEP_SHA_Encryptor rsaEncryptor(
            CryptoPP::StringSource(
                publicKey,
                true,
                new CryptoPP::Base64Decoder()));
#pragma warning(pop)

        CryptoPP::StringSource(
            encryptionKey.data(),
            encryptionKey.size(),
            true,
            new CryptoPP::PK_EncryptorFilter(
                rng,
                rsaEncryptor,
                new CryptoPP::StringSink(wrappedEncryptionKey)));

        CryptoPP::StringSource(
            macKey.data(),
            macKey.size(),
            true,
            new CryptoPP::PK_EncryptorFilter(
                rng,
                rsaEncryptor,
                new CryptoPP::StringSink(wrappedMacKey)));

        CryptoPP::StringSource(
            wrappedEncryptionKey,
            true,
            new CryptoPP::Base64Encoder(
                new CryptoPP::StringSink(b64WrappedEncryptionKey),
                false));

        CryptoPP::StringSource(
            wrappedMacKey,
            true,
            new CryptoPP::Base64Encoder(
                new CryptoPP::StringSink(b64WrappedMacKey),
                false));
    }
    catch (const CryptoPP::Exception& e)
    {
        my_print(NOT_SENSITIVE, false, _T("%s - Encryption failed (%d): %S"), __TFUNCTION__, GetLastError(), e.what());
        return false;
    }

    stringstream ss;
    ss << "{  \n";
    ss << "  \"contentCiphertext\": \"" << b64Ciphertext << "\",\n";
    ss << "  \"iv\": \"" << b64IV << "\",\n";
    ss << "  \"wrappedEncryptionKey\": \"" << b64WrappedEncryptionKey << "\",\n";
    ss << "  \"contentMac\": \"" << b64Mac << "\",\n";
    ss << "  \"wrappedMacKey\": \"" << b64WrappedMacKey << "\"\n";
    ss << "}";

    o_encrypted = ss.str();

    return true;
}


DWORD GetTickCountDiff(DWORD start, DWORD end)
{
    if (start == 0)
    {
        return 0;
    }

    // Has tick count wrapped around?
    if (end < start)
    {
        return (MAXDWORD - start) + end;
    }

    return end - start;
}

/*
AutoHANDLE and AutoMUTEX
*/

AutoMUTEX::AutoMUTEX(HANDLE mutex, TCHAR* logInfo/*=0*/)
    : m_mutex(mutex)
{
    if (logInfo) m_logInfo = logInfo;
    if (m_logInfo.length()>0) my_print(NOT_SENSITIVE, true, _T("%s: obtaining 0x%x: %s"), __TFUNCTION__, (int)m_mutex, m_logInfo.c_str());
    WaitForSingleObject(m_mutex, INFINITE);
    if (m_logInfo.length()>0) my_print(NOT_SENSITIVE, true, _T("%s: obtained 0x%x: %s"), __TFUNCTION__, (int)m_mutex, m_logInfo.c_str());
}

AutoMUTEX::~AutoMUTEX()
{
    if (m_logInfo.length()>0) my_print(NOT_SENSITIVE, true, _T("%s: releasing 0x%x: %s"), __TFUNCTION__, (int)m_mutex, m_logInfo.c_str());
    ReleaseMutex(m_mutex);
}

/*
DPI Awareness Utilities
*/

HRESULT SetProcessDpiAwareness(PROCESS_DPI_AWARENESS value)
{
    // In the no-op/unsupported case we're going to return success.
    HRESULT res = S_OK;

    HINSTANCE hinstSHCORE = NULL;

    static const int maxPathBufferSize = MAX_PATH + 1;
    TCHAR SystemDirectoryPathBuffer[maxPathBufferSize];
    if (GetSystemDirectory(SystemDirectoryPathBuffer, maxPathBufferSize))
    {
        tstring libraryPath = tstring(SystemDirectoryPathBuffer) + TEXT("\\SHCORE.DLL");
        hinstSHCORE = LoadLibrary(libraryPath.c_str());
    }

    if (hinstSHCORE)
    {
        typedef HRESULT STDAPICALLTYPE SETPROCESSDPIAWARENESSFN(PROCESS_DPI_AWARENESS value);
        SETPROCESSDPIAWARENESSFN *pfnSetProcessDpiAwareness;
        pfnSetProcessDpiAwareness = (SETPROCESSDPIAWARENESSFN*)GetProcAddress(hinstSHCORE, "SetProcessDpiAwareness");

        if (pfnSetProcessDpiAwareness)
        {
            res = pfnSetProcessDpiAwareness(value);
        }

        FreeLibrary(hinstSHCORE);
    }

    return res;
}

HRESULT GetDpiForMonitor(HMONITOR hmonitor, MONITOR_DPI_TYPE dpiType, UINT *dpiX, UINT *dpiY)
{
    HRESULT res = ERROR_NOT_SUPPORTED;

    HINSTANCE hinstSHCORE = NULL;

    static const int maxPathBufferSize = MAX_PATH + 1;
    TCHAR SystemDirectoryPathBuffer[maxPathBufferSize];
    if (GetSystemDirectory(SystemDirectoryPathBuffer, maxPathBufferSize))
    {
        tstring libraryPath = tstring(SystemDirectoryPathBuffer) + TEXT("\\SHCORE.DLL");
        hinstSHCORE = LoadLibrary(TEXT("SHCORE.DLL"));
    }

    if (hinstSHCORE)
    {
        typedef HRESULT STDAPICALLTYPE GETDPIFORMONITORFN(HMONITOR hmonitor, MONITOR_DPI_TYPE dpiType, UINT *dpiX, UINT *dpiY);
        GETDPIFORMONITORFN *pfnGetDpiForMonitor;
        pfnGetDpiForMonitor = (GETDPIFORMONITORFN*)GetProcAddress(hinstSHCORE, "GetDpiForMonitor");

        if (pfnGetDpiForMonitor)
        {
            res = pfnGetDpiForMonitor(hmonitor, dpiType, dpiX, dpiY);
        }

        FreeLibrary(hinstSHCORE);
    }

    return res;
}

HRESULT GetDpiForCurrentMonitor(HWND hWnd, UINT& o_dpi)
{
    o_dpi = 0;

    HMONITOR const monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);

    UINT x = 0, y = 0;

    HRESULT res = GetDpiForMonitor(
        monitor,
        MDT_EFFECTIVE_DPI,
        &x,
        &y);

    if (res != S_OK)
    {
        return res;
    }

    o_dpi = y;

    return S_OK;
}

HRESULT GetDpiScalingForCurrentMonitor(HWND hWnd, float& o_scale)
{
    o_scale = 1.0;

    UINT dpi = 0;

    HRESULT res = GetDpiForCurrentMonitor(hWnd, dpi);

    if (res != S_OK)
    {
        return res;
    }

    o_scale = ConvertDpiToScaling(dpi);

    return S_OK;
}

HRESULT GetDpiScalingForMonitorFromPoint(POINT pt, float& o_scale)
{
    o_scale = 1.0;

    HMONITOR const monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);

    UINT x = 0, y = 0;

    HRESULT res = GetDpiForMonitor(
        monitor,
        MDT_EFFECTIVE_DPI,
        &x,
        &y);

    if (res != S_OK)
    {
        return res;
    }

    o_scale = ConvertDpiToScaling(y);

    return S_OK;
}

float ConvertDpiToScaling(UINT dpi)
{
    const UINT defaultDPI = 96;
    return dpi / (float)defaultDPI;
}
