/*
 * Copyright (c) 2012, Psiphon Inc.
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
#include "transport.h"
#include "sshtransport.h"
#include "sessioninfo.h"
#include "psiclient.h"
#include <WinSock2.h>
#include <WinCrypt.h>
#include "utilities.h"
#include "systemproxysettings.h"
#include "config.h"


#define DEFAULT_PLONK_SOCKS_PROXY_PORT  1080
#define SSH_CONNECTION_TIMEOUT_SECONDS  20
#define PLONK_EXE_NAME                  _T("psiphon3-plonk.exe")


static bool SetPlonkSSHHostKey(
        const tstring& sshServerAddress,
        int sshServerPort,
        const tstring& sshServerHostKey);


/******************************************************************************
 SSHTransportBase
******************************************************************************/

SSHTransportBase::SSHTransportBase()
{
    ZeroMemory(&m_plonkProcessInfo, sizeof(m_plonkProcessInfo));
    m_localSocksProxyPort = DEFAULT_PLONK_SOCKS_PROXY_PORT;
}

SSHTransportBase::~SSHTransportBase()
{
    (void)Cleanup();
}

bool SSHTransportBase::IsServerRequestTunnelled() const
{
    return true;
}

tstring SSHTransportBase::GetSessionID(SessionInfo sessionInfo)
{
    return NarrowToTString(sessionInfo.GetSSHSessionID());
}

int SSHTransportBase::GetLocalProxyParentPort() const
{
    return m_localSocksProxyPort;
}

tstring SSHTransportBase::GetLastTransportError() const
{
    return _T("0");
}

bool SSHTransportBase::DoPeriodicCheck()
{
    // Check if we've lost the Plonk process

    if (m_plonkProcessInfo.hProcess != 0)
    {
        // The plonk process handle will be signalled when the process terminates
        DWORD result = WaitForSingleObject(m_plonkProcessInfo.hProcess, 0);

        if (result == WAIT_TIMEOUT)
        {
            // Everything normal
            return true;
        }
        else if (result == WAIT_OBJECT_0)
        {
            // The process has signalled -- which implies that it has died
            return false;
        }
        else
        {
            std::stringstream s;
            s << __FUNCTION__ << ": WaitForSingleObject failed (" << result << ", " << GetLastError() << ")";
            throw Error(s.str().c_str());
        }
    }

    // If we're here, then there's no Plonk process at all.

    return false;
}

bool SSHTransportBase::Cleanup()
{
    // Give the process an opportunity for graceful shutdown, then terminate
    if (m_plonkProcessInfo.hProcess != 0
        && m_plonkProcessInfo.hProcess != INVALID_HANDLE_VALUE)
    {
        StopProcess(m_plonkProcessInfo.dwProcessId, m_plonkProcessInfo.hProcess);
    }

    if (m_plonkProcessInfo.hProcess != 0
        && m_plonkProcessInfo.hProcess != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_plonkProcessInfo.hProcess);
    }
    ZeroMemory(&m_plonkProcessInfo, sizeof(m_plonkProcessInfo));

    return true;
}

void SSHTransportBase::TransportConnect(
                        const SessionInfo& sessionInfo,
                        SystemProxySettings* systemProxySettings)
{
    if (!IsServerSSHCapable(sessionInfo))
    {
        throw TransportFailed();
    }

    try
    {
        TransportConnectHelper(sessionInfo, systemProxySettings);
    }
    catch(...)
    {
        (void)Cleanup();
        throw;
    }
}

void SSHTransportBase::TransportConnectHelper(
                        const SessionInfo& sessionInfo,
                        SystemProxySettings* systemProxySettings)
{
    my_print(false, _T("%s connecting..."), GetTransportDisplayName().c_str());

    // Extract executables and put to disk if not already

    if (m_plonkPath.size() == 0)
    {
        if (!ExtractExecutable(IDR_PLONK_EXE, PLONK_EXE_NAME, m_plonkPath))
        {
            throw TransportFailed();
        }
    }

    // Ensure we start from a disconnected/clean state
    if (!Cleanup())
    {
        std::stringstream s;
        s << __FUNCTION__ << ": Cleanup failed (" << GetLastError() << ")";
        throw Error(s.str().c_str());
    }

    // Start plonk using Psiphon server SSH parameters

    tstring serverAddress, serverHostKey, plonkCommandLine;
    int serverPort = 0;

    // Client transmits its session ID prepended to the SSH password; the server
    // uses this to associate the tunnel with web requests -- for GeoIP region stats
    string sshPassword = sessionInfo.GetClientSessionID() + sessionInfo.GetSSHPassword();

    m_localSocksProxyPort = DEFAULT_PLONK_SOCKS_PROXY_PORT;

    // Test if the localSocksProxyPort is already in use.  If it is, try to find
    // one that is available.
    if (!TestForOpenPort(m_localSocksProxyPort, 10, m_stopInfo))
    {
        my_print(false, _T("Local SOCKS proxy could not find an available port."));
        throw TransportFailed();
    }

    if (!GetSSHParams(
        sessionInfo,
        m_localSocksProxyPort,
        sshPassword,
        serverAddress, 
        serverPort, 
        serverHostKey, 
        plonkCommandLine, 
        systemProxySettings))
    {
        throw TransportFailed();
    }

    // Add host to Plonk's known host registry set
    // Note: currently we're not removing this after the session, so we're leaving a trace

    SetPlonkSSHHostKey(serverAddress, serverPort, serverHostKey);

    // Create the Plonk process and connect to server

    if (!LaunchPlonk(plonkCommandLine.c_str()))
    {
        my_print(false, _T("%s - LaunchPlonk failed (%d)"), __TFUNCTION__, GetLastError());
        throw TransportFailed();
    }

    // TODO: wait for parent proxy to be in place? In testing, we found cases 
    // where Polipo stopped responding when the ssh tunnel was torn down.

    DWORD connected = WaitForConnectability(
                        m_localSocksProxyPort,
                        SSH_CONNECTION_TIMEOUT_SECONDS*1000,
                        m_plonkProcessInfo.hProcess,
                        m_stopInfo);

    if (ERROR_OPERATION_ABORTED == connected)
    {
        throw Abort();
    }
    else if (ERROR_SUCCESS != connected)
    {
        my_print(false, _T("Failed to connect (%d, %d)"), connected, GetLastError());
        throw TransportFailed();
    }

    systemProxySettings->SetSocksProxyPort(m_localSocksProxyPort);

    my_print(false, _T("SOCKS proxy is running on localhost port %d."), m_localSocksProxyPort);
}

bool SSHTransportBase::IsServerSSHCapable(const SessionInfo& sessionInfo) const
{
    return sessionInfo.GetSSHHostKey().length() > 0;
}

// Create the Plonk process and connect to server
bool SSHTransportBase::LaunchPlonk(const TCHAR* plonkCommandLine)
{

    STARTUPINFO plonkStartupInfo;
    ZeroMemory(&plonkStartupInfo, sizeof(plonkStartupInfo));
    plonkStartupInfo.cb = sizeof(plonkStartupInfo);

    if (!CreateProcess(
            m_plonkPath.c_str(),
            (TCHAR*)plonkCommandLine,
            NULL,
            NULL,
            FALSE,
#ifdef _DEBUG
            CREATE_NEW_PROCESS_GROUP,
#else
            CREATE_NEW_PROCESS_GROUP|CREATE_NO_WINDOW,
#endif
            NULL,
            NULL,
            &plonkStartupInfo,
            &m_plonkProcessInfo))
    {
        return false;
    }

    // Close the unneccesary handles
    CloseHandle(m_plonkProcessInfo.hThread);
    m_plonkProcessInfo.hThread = NULL;

    WaitForInputIdle(m_plonkProcessInfo.hProcess, 5000);

    return true;
}

bool SSHTransportBase::GetUserParentProxySettings(
    SystemProxySettings* systemProxySettings,
    tstring& o_UserSSHParentProxyType,
    tstring& o_UserSSHParentProxyHostname,
    int& o_UserSSHParentProxyPort,
    tstring& o_UserSSHParentProxyUsername,
    tstring& o_UserSSHParentProxyPassword)
{
    o_UserSSHParentProxyType.clear();
    o_UserSSHParentProxyHostname.clear();
    o_UserSSHParentProxyUsername.clear();
    o_UserSSHParentProxyPassword.clear();
    o_UserSSHParentProxyPort = 0;

    
    //Check if user wants to use parent proxy
    if(UserSkipSSHParentProxySettings())
    {
        return false;
    }
    //Registry values take precedence over system settings
    //Username and password for 'Basic' HTTP or SOCKS authentication
    //must be stored in registry

    o_UserSSHParentProxyType = NarrowToTString(UserSSHParentProxyType());
    o_UserSSHParentProxyHostname = NarrowToTString(UserSSHParentProxyHostname());
    o_UserSSHParentProxyPort =  UserSSHParentProxyPort();
    o_UserSSHParentProxyUsername = NarrowToTString(UserSSHParentProxyUsername());
    o_UserSSHParentProxyPassword =  NarrowToTString(UserSSHParentProxyPassword());

    if(!o_UserSSHParentProxyType.empty() 
        && !o_UserSSHParentProxyHostname.empty()
        && 0 != o_UserSSHParentProxyPort)
    {
        return true;
    }

    //if no registry values try system settings
    return(systemProxySettings->GetUserLanProxy(
        o_UserSSHParentProxyType, 
        o_UserSSHParentProxyHostname, 
        o_UserSSHParentProxyPort));
}

bool SSHTransportBase::GetSSHParams(
    const SessionInfo& sessionInfo,
    const int localSocksProxyPort,
    const string& sshPassword,
    tstring& o_serverAddress, 
    int& o_serverPort, 
    tstring& o_serverHostKey, 
    tstring& o_plonkCommandLine,
    SystemProxySettings* systemProxySettings)
{
    o_serverAddress.clear();
    o_serverPort = 0;
    o_serverHostKey.clear();
    o_plonkCommandLine.clear();

    o_serverAddress = NarrowToTString(sessionInfo.GetServerAddress());
    o_serverPort = sessionInfo.GetSSHObfuscatedPort();
    o_serverHostKey = NarrowToTString(sessionInfo.GetSSHHostKey());

    // Note: -batch ensures plonk doesn't hang on a prompt when the server's host key isn't
    // the expected value we just set in the registry

    tstringstream args;
    args << _T(" -ssh -C -N -batch")
         << _T(" -P ") << o_serverPort
         << _T(" -l ") << NarrowToTString(sessionInfo.GetSSHUsername()).c_str()
         << _T(" -pw ") << NarrowToTString(sshPassword).c_str()
         << _T(" -D ") << localSocksProxyPort;
#ifdef _DEBUG
    args << _T(" -v");
#endif

    tstring proxy_type, proxy_host, proxy_username, proxy_password;
    int proxy_port;

    if(GetUserParentProxySettings(
        systemProxySettings, 
        proxy_type, 
        proxy_host, 
        proxy_port, 
        proxy_username, 
        proxy_password))
    {
        args << _T(" -proxy_type ") << proxy_type.c_str();
        args << _T(" -proxy_host ") << proxy_host.c_str();
        args << _T(" -proxy_port ") << proxy_port;
        if(!proxy_username.empty())
        {
            args << _T(" -proxy_username ") << proxy_username.c_str();
        }
        if(!proxy_password.empty())
        {
            args << _T(" -proxy_password ") << proxy_password.c_str();
        }

    }
    
    o_plonkCommandLine = m_plonkPath + args.str();
    return true;
}

/******************************************************************************
 SSHTransport
******************************************************************************/

static const TCHAR* SSH_TRANSPORT_PROTOCOL_NAME = _T("SSH");
static const TCHAR* SSH_TRANSPORT_DISPLAY_NAME = _T("SSH");

// Support the registration of this transport type
static ITransport* NewSSH()
{
    return new SSHTransport();
}

// static
void SSHTransport::GetFactory(
                    tstring& o_transportName,
                    TransportFactory& o_transportFactory)
{
    o_transportFactory = NewSSH;
    o_transportName = SSH_TRANSPORT_DISPLAY_NAME;
}


SSHTransport::SSHTransport()
{
}

SSHTransport::~SSHTransport()
{
    IWorkerThread::Stop();
}

// static
tstring SSHTransport::GetTransportProtocolName() const 
{
    return SSH_TRANSPORT_PROTOCOL_NAME;
}

tstring SSHTransport::GetTransportDisplayName() const 
{ 
    return SSH_TRANSPORT_DISPLAY_NAME; 
}

bool SSHTransport::IsHandshakeRequired(SessionInfo sessionInfo) const
{
    bool sufficientInfo = 
        sessionInfo.GetServerAddress().length() > 0
        && sessionInfo.GetSSHPort() > 0
        && sessionInfo.GetSSHHostKey().length() > 0
        && sessionInfo.GetSSHUsername().length() > 0
        && sessionInfo.GetSSHPassword().length() > 0;
    return !sufficientInfo;
}

bool SSHTransport::GetSSHParams(
    const SessionInfo& sessionInfo,
    const int localSocksProxyPort,
    const string& sshPassword,
    tstring& o_serverAddress, 
    int& o_serverPort, 
    tstring& o_serverHostKey, 
    tstring& o_plonkCommandLine,
    SystemProxySettings* systemProxySettings)
{
    tstring o_plonk_options;

    if(!SSHTransportBase::GetSSHParams(
        sessionInfo,
        localSocksProxyPort,
        sshPassword,
        o_serverAddress, 
        o_serverPort, 
        o_serverHostKey, 
        o_plonk_options,
        systemProxySettings))
    {
        return false;
    }
    tstringstream args;
    args << o_plonk_options << _T(" ") << o_serverAddress;
    o_plonkCommandLine = args.str();

    return true;
}


/******************************************************************************
 OSSHTransport
******************************************************************************/

static const TCHAR* OSSH_TRANSPORT_PROTOCOL_NAME = _T("OSSH");
static const TCHAR* OSSH_TRANSPORT_DISPLAY_NAME = _T("SSH+");

// Support the registration of this transport type
static ITransport* NewOSSH()
{
    return new OSSHTransport();
}

// static
void OSSHTransport::GetFactory(
                    tstring& o_transportName,
                    TransportFactory& o_transportFactory)
{
    o_transportFactory = NewOSSH;
    o_transportName = OSSH_TRANSPORT_DISPLAY_NAME;
}


OSSHTransport::OSSHTransport()
{
}

OSSHTransport::~OSSHTransport()
{
    IWorkerThread::Stop();
}

tstring OSSHTransport::GetTransportProtocolName() const 
{
    return OSSH_TRANSPORT_PROTOCOL_NAME;
}

tstring OSSHTransport::GetTransportDisplayName() const 
{
    return OSSH_TRANSPORT_DISPLAY_NAME;
}

bool OSSHTransport::IsHandshakeRequired(SessionInfo sessionInfo) const
{
    bool sufficientInfo = 
        sessionInfo.GetServerAddress().length() > 0
        && sessionInfo.GetSSHObfuscatedPort() > 0
        && sessionInfo.GetSSHHostKey().length() > 0
        && sessionInfo.GetSSHUsername().length() > 0
        && sessionInfo.GetSSHPassword().length() > 0
        && sessionInfo.GetSSHObfuscatedKey().length() > 0;
    return !sufficientInfo;
}

bool OSSHTransport::GetSSHParams(
    const SessionInfo& sessionInfo,
    const int localSocksProxyPort,
    const string& sshPassword,
    tstring& o_serverAddress, 
    int& o_serverPort, 
    tstring& o_serverHostKey, 
    tstring& o_plonkCommandLine,
    SystemProxySettings* systemProxySettings)
{

    if (sessionInfo.GetSSHObfuscatedPort() <= 0 
        || sessionInfo.GetSSHObfuscatedKey().size() <= 0)
    {
        my_print(false, _T("%s - missing parameters"), __TFUNCTION__);
        return false;
    }

    tstring o_plonk_options;

    if(!SSHTransportBase::GetSSHParams(
        sessionInfo,
        localSocksProxyPort,
        sshPassword,
        o_serverAddress, 
        o_serverPort, 
        o_serverHostKey, 
        o_plonk_options,
        systemProxySettings))
    {
        return false;
    }

    tstringstream args;

    args << o_plonk_options; 
    args << _T(" -z -Z ") << NarrowToTString(sessionInfo.GetSSHObfuscatedKey()).c_str();
    args << _T(" ") << o_serverAddress.c_str();
    o_plonkCommandLine = args.str();

    return true;
}


/******************************************************************************
 Helpers
******************************************************************************/

bool SetPlonkSSHHostKey(
        const tstring& sshServerAddress,
        int sshServerPort,
        const tstring& sshServerHostKey)
{
    // Add Plonk registry entry for host for non-interactive host key validation

    // Host key is base64 encoded set of fields

    BYTE* decodedFields = NULL;
    DWORD size = 0;

    if (!CryptStringToBinary(sshServerHostKey.c_str(), sshServerHostKey.length(), CRYPT_STRING_BASE64, NULL, &size, NULL, NULL)
        || !(decodedFields = new (std::nothrow) BYTE[size])
        || !CryptStringToBinary(sshServerHostKey.c_str(), sshServerHostKey.length(), CRYPT_STRING_BASE64, decodedFields, &size, NULL, NULL))
    {
        my_print(false, _T("SetPlonkSSHHostKey: CryptStringToBinary failed (%d)"), GetLastError());
        return false;
    }

    // field format: {<4 byte len (big endian), len bytes field>}+
    // first field is key type, expecting "ssh-rsa";
    // remaining fields are opaque number value -- simply emit in new format which is comma delimited hex strings

    const char* expectedKeyTypeValue = "ssh-rsa";
    unsigned long expectedKeyTypeLen = htonl(strlen(expectedKeyTypeValue));

    if (memcmp(decodedFields + 0, &expectedKeyTypeLen, sizeof(unsigned long))
        || memcmp(decodedFields + sizeof(unsigned long), expectedKeyTypeValue, strlen(expectedKeyTypeValue)))
    {
        delete [] decodedFields;

        my_print(false, _T("SetPlonkSSHHostKey: unexpected key type"));
        return false;
    }

    string data;

    unsigned long offset = sizeof(unsigned long) + strlen(expectedKeyTypeValue);

    while (offset < size - sizeof(unsigned long))
    {
        unsigned long nextLen = ntohl(*((long*)(decodedFields + offset)));
        offset += sizeof(unsigned long);

        if (nextLen > 0 && offset + nextLen <= size)        
        {
            string field = "";
            const char* hexDigits = "0123456789abcdef";
            for (unsigned long i = 0; i < nextLen; i++)
            {
                char digit = hexDigits[decodedFields[offset + i] >> 4];
                // Don't add leading zeroes
                if (digit != '0' || field.length() > 0) field += digit;
                digit = hexDigits[decodedFields[offset + i] & 0x0F];
                // Always include last nibble (e.g. 0x0)
                if (i == nextLen-1 || (digit != '0' || field.length() > 0)) field += digit;
            }
            field = "0x" + field;
            if (data.length() > 0) data += ",";
            data += field;
            offset += nextLen;
        }
    }

    delete [] decodedFields;

    stringstream value;
    value << "rsa2@" << sshServerPort << ":" << TStringToNarrow(sshServerAddress);

    const TCHAR* plonkRegistryKey = _T("Software\\SimonTatham\\PuTTY\\SshHostKeys");

    HKEY key = 0;
    LONG returnCode = RegCreateKeyEx(HKEY_CURRENT_USER, plonkRegistryKey, 0, 0, 0, KEY_WRITE, 0, &key, NULL);
    if (ERROR_SUCCESS != returnCode)
    {
        my_print(false, _T("SetPlonkSSHHostKey: Create Registry Key failed (%d)"), returnCode);
        return false;
    }

    returnCode = RegSetValueExA(key, value.str().c_str(), 0, REG_SZ, (PBYTE)data.c_str(), data.length()+1);
    if (ERROR_SUCCESS != returnCode)
    {
        RegCloseKey(key);

        my_print(false, _T("SetPlonkSSHHostKey: Set Registry Value failed (%d)"), returnCode);
        return false;
    }

    RegCloseKey(key);

    return true;
}

