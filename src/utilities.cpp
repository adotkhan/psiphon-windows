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
#include "psiclient.h"
#include "config.h"
#include <Shlwapi.h>
#include <WinSock2.h>
#include <TlHelp32.h>
#include "utilities.h"
#include "stopsignal.h"
#include "cryptlib.h"
#include "cryptlib.h"
#include "rsa.h"
#include "base64.h"
#include "osrng.h"
#include "modes.h"
#include "hmac.h"
#include "embeddedvalues.h"
#include "httpsrequest.h"
#include "yaml-cpp/yaml.h"
#include "server_list_reordering.h"
#include "wininet_network_check.h"


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


bool ExtractExecutable(DWORD resourceID, const TCHAR* exeFilename, tstring& path)
{
    // Extract executable from resources and write to temporary file

    HRSRC res;
    HGLOBAL handle = INVALID_HANDLE_VALUE;
    BYTE* data;
    DWORD size;

    res = FindResource(g_hInst, MAKEINTRESOURCE(resourceID), RT_RCDATA);
    if (!res)
    {
        my_print(NOT_SENSITIVE, false, _T("ExtractExecutable - FindResource failed (%d)"), GetLastError());
        return false;
    }

    handle = LoadResource(NULL, res);
    if (!handle)
    {
        my_print(NOT_SENSITIVE, false, _T("ExtractExecutable - LoadResource failed (%d)"), GetLastError());
        return false;
    }

    data = (BYTE*)LockResource(handle);
    size = SizeofResource(NULL, res);

    DWORD ret;
    TCHAR tempPath[MAX_PATH];
    // http://msdn.microsoft.com/en-us/library/aa364991%28v=vs.85%29.aspx notes
    // tempPath can contain no more than MAX_PATH-14 characters
    ret = GetTempPath(MAX_PATH, tempPath);
    if (ret > MAX_PATH-14 || ret == 0)
    {
        my_print(NOT_SENSITIVE, false, _T("ExtractExecutable - GetTempPath failed (%d)"), GetLastError());
        return false;
    }

    TCHAR filePath[MAX_PATH];
    if (NULL == PathCombine(filePath, tempPath, exeFilename))
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
        CloseHandle(tempFile);
        my_print(NOT_SENSITIVE, false, _T("ExtractExecutable - WriteFile/FlushFileBuffers failed (%d)"), GetLastError());
        return false;
    }

    CloseHandle(tempFile);

    path = filePath;

    return true;
}


DWORD WaitForConnectability(
        int port,
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

        if (stopInfo.stopSignal->CheckSignal(stopInfo.stopReasons))
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
        if (ERROR_SUCCESS != WaitForConnectability(targetPort, 100, 0, stopInfo))
        {
            return true;
        }
        my_print(NOT_SENSITIVE, false, _T("Localhost port %d is already in use."), targetPort);
    }
    while (++targetPort <= maxPort);

    return false;
}


void StopProcess(DWORD processID, HANDLE process)
{
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, processID);
    if (WAIT_OBJECT_0 != WaitForSingleObject(process, 100))
    {
        if (!TerminateProcess(process, 0) ||
            WAIT_OBJECT_0 != WaitForSingleObject(process, TERMINATE_PROCESS_WAIT_MS))
        {
            my_print(NOT_SENSITIVE, false, _T("TerminateProcess failed for process with PID %d"), processID);
        }
    }
}


bool WriteRegistryDwordValue(const string& name, DWORD value)
{
    HKEY key = 0;
    DWORD disposition = 0;
    DWORD bufferLength = sizeof(value);

    bool success =
        (ERROR_SUCCESS == RegCreateKeyEx(
                            HKEY_CURRENT_USER,
                            LOCAL_SETTINGS_REGISTRY_KEY,
                            0,
                            0,
                            0,
                            KEY_WRITE,
                            0,
                            &key,
                            &disposition) &&

         ERROR_SUCCESS == RegSetValueExA(
                            key,
                            name.c_str(),
                            0,
                            REG_DWORD,
                            (LPBYTE)&value,
                            bufferLength));
    RegCloseKey(key);

    return success;
}


bool ReadRegistryDwordValue(const string& name, DWORD& value)
{
    HKEY key = 0;
    DWORD bufferLength = sizeof(value);
    DWORD type;

    bool success =
        (ERROR_SUCCESS == RegOpenKeyEx(
                            HKEY_CURRENT_USER,
                            LOCAL_SETTINGS_REGISTRY_KEY,
                            0,
                            KEY_READ,
                            &key) &&

         ERROR_SUCCESS == RegQueryValueExA(
                            key,
                            name.c_str(),
                            0,
                            &type,
                            (LPBYTE)&value,
                            &bufferLength) &&

        type == REG_DWORD);

    RegCloseKey(key);

    return success;
}


bool WriteRegistryStringValue(const string& name, const string& value)
{
    HKEY key = 0;

    bool success =
        (ERROR_SUCCESS == RegCreateKeyEx(
                            HKEY_CURRENT_USER,
                            LOCAL_SETTINGS_REGISTRY_KEY,
                            0,
                            0,
                            0,
                            KEY_WRITE,
                            0,
                            &key,
                            0) &&
         ERROR_SUCCESS == RegSetValueExA(
                            key,
                            name.c_str(),
                            0,
                            REG_SZ,
                            (LPBYTE)value.c_str(),
                            value.length() + 1)); // Write the null terminator
    RegCloseKey(key);

    return success;
}


bool ReadRegistryStringValue(LPCSTR name, string& value)
{
    bool success = false;
    HKEY key = 0;
    DWORD bufferLength = 0;
    char* buffer = 0;
    DWORD type;

    if (ERROR_SUCCESS == RegOpenKeyEx(
                            HKEY_CURRENT_USER,
                            LOCAL_SETTINGS_REGISTRY_KEY,
                            0,
                            KEY_READ,
                            &key) &&

        ERROR_SUCCESS == RegQueryValueExA(
                            key,
                            name,
                            0,
                            0,
                            NULL,
                            &bufferLength) &&

        (buffer = new char[bufferLength + 1]) &&

        ERROR_SUCCESS == RegQueryValueExA(
                            key,
                            name,
                            0,
                            &type,
                            (LPBYTE)buffer,
                            &bufferLength) &&
        type == REG_SZ)
    {
        buffer[bufferLength] = '\0';
        value = buffer;
        success = true;
    }

    delete[] buffer;
    RegCloseKey(key);

    return success;
}

bool ReadRegistryStringValue(LPCWSTR name, wstring& value)
{
    bool success = false;
    HKEY key = 0;
    DWORD bufferLength = 0;
    wchar_t* buffer = 0;
    DWORD type;

    if (ERROR_SUCCESS == RegOpenKeyEx(
                            HKEY_CURRENT_USER,
                            LOCAL_SETTINGS_REGISTRY_KEY,
                            0,
                            KEY_READ,
                            &key) &&

        ERROR_SUCCESS == RegQueryValueExW(
                            key,
                            name,
                            0,
                            0,
                            NULL,
                            &bufferLength) &&

        (buffer = new wchar_t[bufferLength + 1]) &&

        ERROR_SUCCESS == RegQueryValueExW(
                            key,
                            name,
                            0,
                            &type,
                            (LPBYTE)buffer,
                            &bufferLength) &&
        type == REG_SZ)
    {
        buffer[bufferLength] = '\0';
        value = buffer;
        success = true;
    }

    delete[] buffer;
    RegCloseKey(key);

    return success;
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
        char a = toupper(input[i]);
        const char* p = std::lower_bound(lut, lut + 16, a);
        if (*p != a)
        {
            throw std::invalid_argument("Dehexlify: not a hex digit");
        }

        char b = toupper(input[i + 1]);
        const char* q = std::lower_bound(lut, lut + 16, b);
        if (*q != b)
        {
            throw std::invalid_argument("Dehexlify: not a hex digit");
        }

        output.push_back(((p - lut) << 4) | (q - lut));
    }

    return output;
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
        sizeof(ret)/sizeof(ret[0]),
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

        size_t ivLength = sizeof(iv)*sizeof(iv[0]);
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
        memcpy(ivPlusCiphertext+ivLength, ciphertext.data(), ciphertextLength);

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

        CryptoPP::RSAES_OAEP_SHA_Encryptor rsaEncryptor(
            CryptoPP::StringSource(
                publicKey,
                true,
                new CryptoPP::Base64Decoder()));

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
    catch( const CryptoPP::Exception& e )
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



static string GetOSVersionString()
{
    string output;

    OSVERSIONINFOEX osvi;
    BOOL bOsVersionInfoEx;
    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    if (!(bOsVersionInfoEx = GetVersionEx ((OSVERSIONINFO *) &osvi)) )
    {
        osvi.dwOSVersionInfoSize = sizeof (OSVERSIONINFO);
        if (!GetVersionEx ( (OSVERSIONINFO *) &osvi) ) 
        {
            return output;
        }
    }
    switch (osvi.dwPlatformId)
    {
    case VER_PLATFORM_WIN32_NT:
        if ( osvi.dwMajorVersion <= 4 ) output += ("Microsoft Windows NT ");
        if ( osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 0 ) output += ("Microsoft Windows 2000 ");
        if ( osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 1 ) output += ("Microsoft Windows XP ");
        if( bOsVersionInfoEx )
        {
            if ( osvi.wProductType == VER_NT_WORKSTATION )
            {
                if( osvi.wSuiteMask & VER_SUITE_PERSONAL ) output += ( "Personal " );
                else output += ( "Professional " );
            }
            else if ( osvi.wProductType == VER_NT_SERVER )
            {
                if( osvi.wSuiteMask & VER_SUITE_DATACENTER ) output += ( "DataCenter Server " );
                else if( osvi.wSuiteMask & VER_SUITE_ENTERPRISE ) output += ( "Advanced Server " );
                else output += ( "Server " );
            }
        }
        else
        {
            HKEY hKey;
            char szProductType[80];
            DWORD dwBufLen;
            RegOpenKeyExA( HKEY_LOCAL_MACHINE,"SYSTEM\\CurrentControlSet\\Control\\ProductOptions", 0, KEY_QUERY_VALUE, &hKey );
            RegQueryValueExA( hKey, "ProductType", NULL, NULL, (LPBYTE) szProductType, &dwBufLen);
            RegCloseKey( hKey );
            if ( lstrcmpiA( "WINNT", szProductType) == 0 ) output += ( "Professional " );
            if ( lstrcmpiA( "LANMANNT", szProductType) == 0 ) output += ( "Server " );
            if ( lstrcmpiA( "SERVERNT", szProductType) == 0 ) output += ( "Advanced Server " );
        }

        if ( osvi.dwMajorVersion <= 4 )
        {
            std::ostringstream ss;
            ss << "version " << osvi.dwMajorVersion << "." << osvi.dwMinorVersion << " " << osvi.szCSDVersion << "(Build " << (osvi.dwBuildNumber & 0xFFFF) << ")";
            output += ss.str();
        }
        else
        { 
            std::ostringstream ss;
            ss << osvi.szCSDVersion << " (Build " << (osvi.dwBuildNumber & 0xFFFF) << ")";
            output += ss.str();
        }
        break;

    case VER_PLATFORM_WIN32_WINDOWS:
        if (osvi.dwMajorVersion == 4 && osvi.dwMinorVersion == 0)
        {
            output += ("Microsoft Windows 95 ");
            if ( osvi.szCSDVersion[1] == 'C' || osvi.szCSDVersion[1] == 'B' ) output += ("OSR2 " );
        } 

        if (osvi.dwMajorVersion == 4 && osvi.dwMinorVersion == 10)
        {
            output += ("Microsoft Windows 98 ");
            if ( osvi.szCSDVersion[1] == 'A' ) output += ("SE " );
        }
        if (osvi.dwMajorVersion == 4 && osvi.dwMinorVersion == 90)
        {
            output += ("Microsoft Windows Me ");
        } 
        break;

    case VER_PLATFORM_WIN32s:
        output += ("Microsoft Win32s ");
        break;
    }

    return output; 
}

struct SystemInfo
{
    string name;
    DWORD platformId;
    DWORD majorVersion;
    DWORD minorVersion;
    DWORD productType;
    WORD servicePackMajor;
    WORD servicePackMinor;
    DWORD edition;
    DWORD suiteMask;
    string csdVersion;
    DWORD buildNumber;
    bool starter;
    bool mideastEnabled;
    bool slowMachine;
    bool wininet_success;
    WininetNetworkInfo wininet_info;
};

// Adapted from http://msdn.microsoft.com/en-us/library/windows/desktop/ms724429%28v=vs.85%29.aspx
bool GetSystemInfo(SystemInfo& sysInfo)
{
    ZeroMemory(&sysInfo, sizeof(sysInfo));

    typedef void (WINAPI *PGNSI)(LPSYSTEM_INFO);
    typedef BOOL (WINAPI *PGPI)(DWORD, DWORD, DWORD, DWORD, PDWORD);

    OSVERSIONINFOEX osvi;
    SYSTEM_INFO si;
    PGNSI pGNSI;
    PGPI pGPI;
    BOOL bOsVersionInfoEx;
    DWORD dwType;

    ostringstream ss;

    ZeroMemory(&si, sizeof(SYSTEM_INFO));
    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));

    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*) &osvi);

    if (bOsVersionInfoEx == 0) 
    {
        return false;
    }

    // Call GetNativeSystemInfo if supported or GetSystemInfo otherwise.

    pGNSI = (PGNSI)GetProcAddress(
                        GetModuleHandle(TEXT("kernel32.dll")), 
                        "GetNativeSystemInfo");
    if(NULL != pGNSI)
    {
        pGNSI(&si);
    }
    else 
    {
        GetSystemInfo(&si);
    }

    if (VER_PLATFORM_WIN32_NT == osvi.dwPlatformId && osvi.dwMajorVersion > 4)
    {
        ss << "Microsoft ";

        // Test for the specific product.

        if (osvi.dwMajorVersion >= 6)
        {
            if (osvi.dwMinorVersion == 0)
            {
                if (osvi.wProductType == VER_NT_WORKSTATION)
                {
                    ss << "Windows Vista ";
                }
                else 
                {
                    ss << "Windows Server 2008 ";
                }
            }
            else if (osvi.dwMinorVersion == 1)
            {
                if (osvi.wProductType == VER_NT_WORKSTATION)
                {
                    ss << "Windows 7 ";
                }
                else 
                {
                    ss << "Windows Server 2008 R2 ";
                }
            }
            else if (osvi.dwMinorVersion == 2)
            {
                if (osvi.wProductType == VER_NT_WORKSTATION)
                {
                    ss << "Windows 8 ";
                }
                else 
                {
                    ss << "Windows Server 2012 ";
                }
            }

            pGPI = (PGPI)GetProcAddress(
                            GetModuleHandle(TEXT("kernel32.dll")), 
                            "GetProductInfo");

            pGPI(osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.wServicePackMajor, osvi.wServicePackMinor, &dwType);

            switch(dwType)
            {
            case PRODUCT_ULTIMATE:
                ss << "Ultimate Edition";
                break;
            case PRODUCT_PROFESSIONAL:
                ss << "Professional";
                break;
            case PRODUCT_HOME_PREMIUM:
                ss << "Home Premium Edition";
                break;
            case PRODUCT_HOME_BASIC:
                ss << "Home Basic Edition";
                break;
            case PRODUCT_ENTERPRISE:
                ss << "Enterprise Edition";
                break;
            case PRODUCT_BUSINESS:
                ss << "Business Edition";
                break;
            case PRODUCT_STARTER:
                ss << "Starter Edition";
                break;
            case PRODUCT_CLUSTER_SERVER:
                ss << "Cluster Server Edition";
                break;
            case PRODUCT_DATACENTER_SERVER:
                ss << "Datacenter Edition";
                break;
            case PRODUCT_DATACENTER_SERVER_CORE:
                ss << "Datacenter Edition (core installation)";
                break;
            case PRODUCT_ENTERPRISE_SERVER:
                ss << "Enterprise Edition";
                break;
            case PRODUCT_ENTERPRISE_SERVER_CORE:
                ss << "Enterprise Edition (core installation)";
                break;
            case PRODUCT_ENTERPRISE_SERVER_IA64:
                ss << "Enterprise Edition for Itanium-based Systems";
                break;
            case PRODUCT_SMALLBUSINESS_SERVER:
                ss << "Small Business Server";
                break;
            case PRODUCT_SMALLBUSINESS_SERVER_PREMIUM:
                ss << "Small Business Server Premium Edition";
                break;
            case PRODUCT_STANDARD_SERVER:
                ss << "Standard Edition";
                break;
            case PRODUCT_STANDARD_SERVER_CORE:
                ss << "Standard Edition (core installation)";
                break;
            case PRODUCT_WEB_SERVER:
                ss << "Web Server Edition";
                break;
            }
        }

        if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 2)
        {
            if (GetSystemMetrics(SM_SERVERR2))
            {
                ss << "Windows Server 2003 R2, ";
            }
            else if (osvi.wSuiteMask & VER_SUITE_STORAGE_SERVER)
            {
                ss << "Windows Storage Server 2003";
            }
            else if (osvi.wSuiteMask & VER_SUITE_WH_SERVER)
            {
                ss << "Windows Home Server";
            }
            else if (osvi.wProductType == VER_NT_WORKSTATION &&
                     si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
            {
                ss << "Windows XP Professional x64 Edition";
            }
            else 
            {
                ss << "Windows Server 2003, ";
            }

            // Test for the server type.
            if (osvi.wProductType != VER_NT_WORKSTATION)
            {
                if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64)
                {
                    if (osvi.wSuiteMask & VER_SUITE_DATACENTER)
                    {
                        ss << "Datacenter Edition for Itanium-based Systems";
                    }
                    else if (osvi.wSuiteMask & VER_SUITE_ENTERPRISE)
                    {
                        ss << "Enterprise Edition for Itanium-based Systems";
                    }
                }

                else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
                {
                    if (osvi.wSuiteMask & VER_SUITE_DATACENTER)
                    {
                        ss << "Datacenter x64 Edition";
                    }
                    else if (osvi.wSuiteMask & VER_SUITE_ENTERPRISE)
                    {
                        ss << "Enterprise x64 Edition";
                    }
                    else
                    {
                        ss << "Standard x64 Edition";
                    }
                }

                else
                {
                    if (osvi.wSuiteMask & VER_SUITE_COMPUTE_SERVER)
                    {
                        ss << "Compute Cluster Edition";
                    }
                    else if (osvi.wSuiteMask & VER_SUITE_DATACENTER)
                    {
                        ss << "Datacenter Edition";
                    }
                    else if (osvi.wSuiteMask & VER_SUITE_ENTERPRISE)
                    {
                        ss << "Enterprise Edition";
                    }
                    else if (osvi.wSuiteMask & VER_SUITE_BLADE)
                    {
                        ss << "Web Edition";
                    }
                    else 
                    {
                        ss << "Standard Edition";
                    }
                }
            }
        }

        if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 1)
        {
            ss << "Windows XP ";
            if (osvi.wSuiteMask & VER_SUITE_PERSONAL)
            {
                ss << "Home Edition";
            }
            else
            {
                ss << "Professional";
            }
        }

        if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 0)
        {
            ss << "Windows 2000 ";

            if (osvi.wProductType == VER_NT_WORKSTATION)
            {
                ss << "Professional" ;
            }
            else 
            {
                if (osvi.wSuiteMask & VER_SUITE_DATACENTER)
                {
                    ss << "Datacenter Server";
                }
                else if (osvi.wSuiteMask & VER_SUITE_ENTERPRISE)
                {
                    ss << "Advanced Server";
                }
                else 
                {
                    ss << "Server";
                }
            }
        }

        // Include service pack (if any) and build number.

        if (_tcslen(osvi.szCSDVersion) > 0)
        {
            ss << " ";
            ss << osvi.szCSDVersion;
        }

        ss << " (build " << osvi.dwBuildNumber << ")";

        if (osvi.dwMajorVersion >= 6)
        {
            if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
            {
                ss << " 64-bit";
            }
            else if (si.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_INTEL)
            {
                ss << " 32-bit";
            }
        }

        sysInfo.name = ss.str(); 
        sysInfo.platformId = osvi.dwPlatformId;
        sysInfo.majorVersion = osvi.dwMajorVersion;
        sysInfo.minorVersion = osvi.dwMinorVersion;
        sysInfo.productType = osvi.wProductType;
        sysInfo.servicePackMajor = osvi.wServicePackMajor;
        sysInfo.servicePackMinor = osvi.wServicePackMinor;
        sysInfo.edition = dwType;
        sysInfo.suiteMask = osvi.wSuiteMask;
        sysInfo.csdVersion = TStringToNarrow(osvi.szCSDVersion);
        sysInfo.buildNumber = osvi.dwBuildNumber;
        sysInfo.starter = (GetSystemMetrics(SM_STARTER) != 0);

        sysInfo.mideastEnabled = (GetSystemMetrics(SM_MIDEASTENABLED) != 0);
        sysInfo.slowMachine = (GetSystemMetrics(SM_SLOWMACHINE) != 0);

        WininetNetworkInfo netInfo;
        if (WininetGetNetworkInfo(netInfo))
        {
            sysInfo.wininet_success = true;
            sysInfo.wininet_info = netInfo;
        }

        return true;
    }

    return false;
}

string GetDiagnosticInfo(const string& diagnosticInfoID)
{
    YAML::Emitter out;
        
    out << YAML::BeginMap;

    /*
        * Metadata
        */

    out << YAML::Key << "Metadata";
    out << YAML::Value;
    out << YAML::BeginMap;
    out << YAML::Key << "platform" << YAML::Value << "windows";
    out << YAML::Key << "version" << YAML::Value << 1;
    out << YAML::Key << "id" << YAML::Value << diagnosticInfoID;
    out << YAML::EndMap;

    /*
     * System Information
     */

    out << YAML::Key << "SystemInformation";
    out << YAML::Value;
    out << YAML::BeginMap;

    out << YAML::Key << "psiphonEmbeddedValues";
    out << YAML::Value;
    out << YAML::BeginMap;
    out << YAML::Key << "PROPAGATION_CHANNEL_ID";
    out << YAML::Value << PROPAGATION_CHANNEL_ID;
    out << YAML::Key << "SPONSOR_ID";
    out << YAML::Value << SPONSOR_ID;
    out << YAML::Key << "CLIENT_VERSION";
    out << YAML::Value << CLIENT_VERSION;
    out << YAML::EndMap;

    SystemInfo sysInfo;
    ZeroMemory(&sysInfo, sizeof(sysInfo));
    // We'll fill in the values even if this call fails.
    (void)GetSystemInfo(sysInfo);
    out << YAML::Key << "OSInfo";
    out << YAML::Value;
    out << YAML::BeginMap;
    out << YAML::Key << "name" << YAML::Value << sysInfo.name.c_str();
    out << YAML::Key << "platformId" << YAML::Value << sysInfo.platformId;
    out << YAML::Key << "majorVersion" << YAML::Value << sysInfo.majorVersion;
    out << YAML::Key << "minorVersion" << YAML::Value << sysInfo.minorVersion;
    out << YAML::Key << "productType" << YAML::Value << sysInfo.productType;
    out << YAML::Key << "servicePackMajor" << YAML::Value << sysInfo.servicePackMajor;
    out << YAML::Key << "servicePackMinor" << YAML::Value << sysInfo.servicePackMinor;
    out << YAML::Key << "ediition" << YAML::Value << sysInfo.edition;
    out << YAML::Key << "suiteMask" << YAML::Value << sysInfo.suiteMask;
    out << YAML::Key << "csdVersion" << YAML::Value << sysInfo.csdVersion.c_str();
    out << YAML::Key << "buildNumber" << YAML::Value << sysInfo.buildNumber;
    out << YAML::Key << "starter" << YAML::Value << sysInfo.starter;
    out << YAML::EndMap;

    out << YAML::Key << "NetworkInfo";
    out << YAML::Value;
    out << YAML::BeginMap;
    if (sysInfo.wininet_success)
    {
        out << YAML::Key << "internetConnected" << YAML::Value << true;
        out << YAML::Key << "internetConnectionConfigured" << YAML::Value << sysInfo.wininet_info.internetConnectionConfigured;
        out << YAML::Key << "internetConnectionLAN" << YAML::Value << sysInfo.wininet_info.internetConnectionLAN;
        out << YAML::Key << "internetConnectionModem" << YAML::Value << sysInfo.wininet_info.internetConnectionModem;
        out << YAML::Key << "internetConnectionOffline" << YAML::Value << sysInfo.wininet_info.internetConnectionOffline;
        out << YAML::Key << "internetConnectionProxy" << YAML::Value << sysInfo.wininet_info.internetConnectionProxy;
        out << YAML::Key << "internetRASInstalled" << YAML::Value << sysInfo.wininet_info.internetRASInstalled;
    }
    else 
    {
        out << YAML::Key << "internetConnected" << YAML::Value << false;
        out << YAML::Key << "internetConnectionConfigured" << YAML::Value << YAML::Null;
        out << YAML::Key << "internetConnectionLAN" << YAML::Value << YAML::Null;
        out << YAML::Key << "internetConnectionModem" << YAML::Value << YAML::Null;
        out << YAML::Key << "internetConnectionOffline" << YAML::Value << YAML::Null;
        out << YAML::Key << "internetConnectionProxy" << YAML::Value << YAML::Null;
        out << YAML::Key << "internetRASInstalled" << YAML::Value << YAML::Null;
    }

    out << YAML::Key << "Misc";
    out << YAML::Value;
    out << YAML::BeginMap;
    out << YAML::Key << "mideastEnabled" << YAML::Value << sysInfo.mideastEnabled;
    out << YAML::Key << "slowMachine" << YAML::Value << sysInfo.slowMachine;
    out << YAML::EndMap;


    out << YAML::EndMap;

    /*
     * Server Response Check
     */

    out << YAML::Key << "ServerResponseCheck";
    out << YAML::Value;
    out << YAML::BeginSeq;

    vector<ServerReponseCheck> serverReponseCheckHistory;
    GetServerResponseCheckHistory(serverReponseCheckHistory);
    for (vector<ServerReponseCheck>::const_iterator entry = serverReponseCheckHistory.begin();
         entry != serverReponseCheckHistory.end();
         entry++)
    {
        out << YAML::BeginMap;
        out << YAML::Key << "ipAddress" << YAML::Value << entry->serverAddress.c_str();
        out << YAML::Key << "responded" << YAML::Value << entry->responded;
        out << YAML::Key << "responseTime" << YAML::Value << entry->responseTime;
        out << YAML::Key << "timestamp" << YAML::Value << entry->timestamp.c_str();
        out << YAML::EndMap;
    }

    out << YAML::EndSeq;

    /*
     * Server Response Check
     */

    out << YAML::Key << "StatusHistory";
    out << YAML::Value;
    out << YAML::BeginSeq;

    vector<MessageHistoryEntry> messageHistory;
    GetMessageHistory(messageHistory);
    for (vector<MessageHistoryEntry>::const_iterator entry = messageHistory.begin();
         entry != messageHistory.end();
         entry++)
    {
        out << YAML::BeginMap;
        out << YAML::Key << "message" << YAML::Value << TStringToNarrow(entry->message).c_str();
        out << YAML::Key << "debug" << YAML::Value << entry->debug;
        out << YAML::Key << "timestamp" << YAML::Value << TStringToNarrow(entry->timestamp).c_str();
        out << YAML::EndMap;
    }

    out << YAML::EndSeq;


    out << YAML::EndMap;

    return out.c_str();
}

bool OpenEmailAndSendDiagnosticInfo(
        const string& emailAddress, 
        const string& emailAddressEncoded, 
        const string& diagnosticInfoID, 
        const StopInfo& stopInfo)
{
    if (emailAddress.length() > 0)
    {
        assert(emailAddressEncoded.length() > 0);
        //
        // First put the address into the clipboard
        //

        if (!OpenClipboard(NULL))
        {
            return false;
        }

        // Remove the current Clipboard contents 
        if( !EmptyClipboard() )
        {
            return false;
        }
   
        // Get the currently selected data
        HGLOBAL hGlob = GlobalAlloc(GMEM_FIXED, emailAddress.length()+1);
        strcpy_s((char*)hGlob, emailAddress.length()+1, emailAddress.c_str());
    
        // Note that the system takes ownership of hGlob
        if (::SetClipboardData( CF_TEXT, hGlob ) == NULL)
        {
            CloseClipboard();
            GlobalFree(hGlob);
            return false;
        }

        CloseClipboard();

        //
        // Launch the email handler
        //

        string command = "mailto:" + emailAddress;

        (void)::ShellExecuteA( 
                    NULL, 
                    "open", 
                    command.c_str(), 
                    NULL, 
                    NULL, 
                    SW_SHOWNORMAL); 

        // TODO: What does ShellExecute return if there's no registered mailto handler?
        // For now: Don't bother checking the return value at all. We've copied the
        // address to the clipboard and that will have to be good enough.
    }

    //
    // Upload the diagnostic info
    //

    if (diagnosticInfoID.length() > 0)
    {
        string diagnosticInfo = GetDiagnosticInfo(diagnosticInfoID);

        string encryptedPayload;
        if (!PublicKeyEncryptData(
                FEEDBACK_ENCRYPTION_PUBLIC_KEY, 
                diagnosticInfo.c_str(), 
                encryptedPayload))
        {
            return false;
        }

        tstring uploadLocation = NarrowToTString(FEEDBACK_DIAGNOSTIC_INFO_UPLOAD_PATH)
                                    + NarrowToTString(diagnosticInfoID);
        
        string response;
        HTTPSRequest httpsRequest;
        if (!httpsRequest.MakeRequest(
                NarrowToTString(FEEDBACK_DIAGNOSTIC_INFO_UPLOAD_SERVER).c_str(),
                443,
                string(), // Do standard cert validation
                uploadLocation.c_str(),
                response,
                stopInfo,
                false, // don't use local proxy
                NarrowToTString(FEEDBACK_DIAGNOSTIC_INFO_UPLOAD_SERVER_HEADERS).c_str(),
                (LPVOID)encryptedPayload.c_str(),
                encryptedPayload.length(),
                L"PUT"))
        {
            return false;
        }
    }

    return true;
}
