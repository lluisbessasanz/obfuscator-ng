
#include <iostream>
#include <windows.h>
#include <cstdint>
#include <cstddef>

extern "C" uint32_t crc32(const void *data, size_t length) {
  static uint32_t table[256];
  static bool initialized = false;

  if (!initialized) {
    constexpr uint32_t polynomial = 0xEDB88320u;

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

    initialized = true;
  }

  const uint8_t *bytes = static_cast<const uint8_t *>(data);

  uint32_t crc = 0xFFFFFFFFu;

  for (size_t i = 0; i < length; ++i) {
    uint8_t index = static_cast<uint8_t>((crc ^ bytes[i]) & 0xFF);
    crc = (crc >> 8) ^ table[index];
  }

  return crc ^ 0xFFFFFFFFu;
}

extern "C" PDWORD getFunctionAddressByHash(char *library, DWORD hash)
{
	PDWORD functionAddress = (PDWORD)0;

	// Get base address of the module in which our exported function of interest resides (kernel32 in the case of CreateThread)
	HMODULE libraryBase = LoadLibraryA(library);

	PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)libraryBase;
	PIMAGE_NT_HEADERS imageNTHeaders = (PIMAGE_NT_HEADERS)((DWORD_PTR)libraryBase + dosHeader->e_lfanew);
	
	DWORD_PTR exportDirectoryRVA = imageNTHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
	
	PIMAGE_EXPORT_DIRECTORY imageExportDirectory = (PIMAGE_EXPORT_DIRECTORY)((DWORD_PTR)libraryBase + exportDirectoryRVA);
	
	// Get RVAs to exported function related information
	PDWORD addresOfFunctionsRVA = (PDWORD)((DWORD_PTR)libraryBase + imageExportDirectory->AddressOfFunctions);
	PDWORD addressOfNamesRVA = (PDWORD)((DWORD_PTR)libraryBase + imageExportDirectory->AddressOfNames);
	PWORD addressOfNameOrdinalsRVA = (PWORD)((DWORD_PTR)libraryBase + imageExportDirectory->AddressOfNameOrdinals);

	// Iterate through exported functions, calculate their hashes and check if any of them match our hash of 0x00544e304 (CreateThread)
	// If yes, get its virtual memory address (this is where CreateThread function resides in memory of our process)
	for (DWORD i = 0; i < imageExportDirectory->NumberOfFunctions; i++)
	{
		DWORD functionNameRVA = addressOfNamesRVA[i];
		DWORD_PTR functionNameVA = (DWORD_PTR)libraryBase + functionNameRVA;
		char* functionName = (char*)functionNameVA;
		DWORD_PTR functionAddressRVA = 0;

		// Calculate hash for this exported function
		DWORD functionNameHash = crc32(functionName, strlen(functionName));
		
		// If hash for CreateThread is found, resolve the function address
		if (functionNameHash == hash)
		{
			functionAddressRVA = addresOfFunctionsRVA[addressOfNameOrdinalsRVA[i]];
			functionAddress = (PDWORD)((DWORD_PTR)libraryBase + functionAddressRVA);
			printf("%s : 0x%x : %p\n", functionName, functionNameHash, functionAddress);
			return functionAddress;
		}
	}
	printf("Api not found\n");
	return nullptr;
}
