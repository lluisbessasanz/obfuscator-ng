#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

uint32_t crc32(const void *data, size_t length) {
    static uint32_t table[256];
    static int initialized = 0;
    
    if (!initialized) {
        const uint32_t polynomial = 0xEDB88320u;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                if (crc & 1)
                    crc = (crc >> 1) ^ polynomial;
                else
                    crc >>= 1;
            }
            table[i] = crc;
        }
        initialized = 1;
    }
    
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        uint8_t index = (uint8_t)((crc ^ bytes[i]) & 0xFF);
        crc = (crc >> 8) ^ table[index];
    }
    return crc ^ 0xFFFFFFFFu;
}

static void *getFunctionAddressByName(const char *library, const char *targetName);

static void *resolveForwarder(const char *forwarder)
{
    char dllName[MAX_PATH];
    char funcName[256];

    const char *dot = strchr(forwarder, '.');
    if (!dot)
        return NULL;

    size_t dllLen = (size_t)(dot - forwarder);
    if (dllLen == 0 || dllLen >= sizeof(dllName))
        return NULL;

    memcpy(dllName, forwarder, dllLen);
    dllName[dllLen] = '\0';

    if (strstr(dllName, ".dll") == NULL && strstr(dllName, ".DLL") == NULL) {
        if (dllLen + 4 >= sizeof(dllName))
            return NULL;
        strcat(dllName, ".dll");
    }

    const char *namePart = dot + 1;
    if (!*namePart)
        return NULL;

    if (namePart[0] == '#') {
        /*
           Ordinal forwarder, for example:
           API-MS-WIN-CORE...#123
           Not handled in this simple resolver.
        */
        return NULL;
    }

    if (strlen(namePart) >= sizeof(funcName))
        return NULL;

    strcpy(funcName, namePart);

    return getFunctionAddressByName(dllName, funcName);
}

static void *getFunctionAddressByName(const char *library, const char *targetName)
{
    HMODULE libraryBase = LoadLibraryA(library);
    if (!libraryBase)
        return NULL;

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)libraryBase;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;

    PIMAGE_NT_HEADERS ntHeaders =
        (PIMAGE_NT_HEADERS)((BYTE *)libraryBase + dosHeader->e_lfanew);

    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return NULL;

    IMAGE_DATA_DIRECTORY exportData =
        ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

    DWORD exportRVA = exportData.VirtualAddress;
    DWORD exportSize = exportData.Size;

    if (!exportRVA || !exportSize)
        return NULL;

    PIMAGE_EXPORT_DIRECTORY exportDir =
        (PIMAGE_EXPORT_DIRECTORY)((BYTE *)libraryBase + exportRVA);

    DWORD *functions =
        (DWORD *)((BYTE *)libraryBase + exportDir->AddressOfFunctions);

    DWORD *names =
        (DWORD *)((BYTE *)libraryBase + exportDir->AddressOfNames);

    WORD *ordinals =
        (WORD *)((BYTE *)libraryBase + exportDir->AddressOfNameOrdinals);

    for (DWORD i = 0; i < exportDir->NumberOfNames; i++) {
        const char *functionName = (const char *)((BYTE *)libraryBase + names[i]);

        if (strcmp(functionName, targetName) == 0) {
            WORD ordinal = ordinals[i];
            DWORD functionRVA = functions[ordinal];

            if (functionRVA >= exportRVA && functionRVA < exportRVA + exportSize) {
                const char *forwarder =
                    (const char *)((BYTE *)libraryBase + functionRVA);

                return resolveForwarder(forwarder);
            }

            return (void *)((BYTE *)libraryBase + functionRVA);
        }
    }

    return NULL;
}

__declspec(noinline)
void *getFunctionAddressByHash(const char *library, uint32_t hash)
{
    HMODULE libraryBase = LoadLibraryA(library);
    if (!libraryBase)
        return NULL;

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)libraryBase;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;

    PIMAGE_NT_HEADERS ntHeaders =
        (PIMAGE_NT_HEADERS)((BYTE *)libraryBase + dosHeader->e_lfanew);

    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return NULL;

    IMAGE_DATA_DIRECTORY exportData =
        ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

    DWORD exportRVA = exportData.VirtualAddress;
    DWORD exportSize = exportData.Size;

    if (!exportRVA || !exportSize)
        return NULL;

    PIMAGE_EXPORT_DIRECTORY exportDir =
        (PIMAGE_EXPORT_DIRECTORY)((BYTE *)libraryBase + exportRVA);

    DWORD *functions =
        (DWORD *)((BYTE *)libraryBase + exportDir->AddressOfFunctions);

    DWORD *names =
        (DWORD *)((BYTE *)libraryBase + exportDir->AddressOfNames);

    WORD *ordinals =
        (WORD *)((BYTE *)libraryBase + exportDir->AddressOfNameOrdinals);

    for (DWORD i = 0; i < exportDir->NumberOfNames; i++) {
        const char *functionName = (const char *)((BYTE *)libraryBase + names[i]);
        uint32_t functionNameHash = crc32(functionName, strlen(functionName));
        if (functionNameHash == hash) {
            WORD ordinal = ordinals[i];
            DWORD functionRVA = functions[ordinal];

            if (functionRVA >= exportRVA && functionRVA < exportRVA + exportSize) {
                const char *forwarder =
                    (const char *)((BYTE *)libraryBase + functionRVA);

                return resolveForwarder(forwarder);
            }

            return (void *)((BYTE *)libraryBase + functionRVA);
        }
    }

    return NULL;
}