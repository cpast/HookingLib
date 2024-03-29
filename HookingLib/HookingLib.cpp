#include "framework.h"
#include "hooking.h"
#include <Windows.h>
#include <Psapi.h>
#define NMD_LDISASM_IMPLEMENTATION
#include "nmd_ldisasm.h"
#include <cstdlib>
#include "hooking_internal.h"

uintptr_t exeStart = NULL;
size_t exeLen = 0;

uintptr_t trampolineRegion = NULL;
ULONG trampolineSize = 0;
int trampolineCount;

HANDLE heap = NULL;

bool EnsureExe()
{
	if (exeStart == NULL) {
		MODULEINFO modInfo = { 0 };
		HANDLE currProc = GetCurrentProcess();
		HMODULE mainExe = GetModuleHandle(NULL);
		if (GetModuleInformation(currProc, mainExe, &modInfo, sizeof(MODULEINFO)) == 0)
			return false;
		exeStart = (uintptr_t)modInfo.lpBaseOfDll;
		exeLen = (size_t)modInfo.SizeOfImage;
	}
	return true;
}

bool EnsureHeap()
{
	if (heap == NULL) {
		heap = HeapCreate(HEAP_CREATE_ENABLE_EXECUTE, 0x1000, 0x5000);
		if (heap == NULL)
			return false;
	}
	return true;
}

bool memptn(const uint8_t* mem, const uint8_t* ptn, const uint8_t* mask, size_t len)
{
	for (int i = 0; i < len; i++) {
		if ((mask[i] & mem[i]) != (mask[i] & ptn[i]))
			return false;
	}
	return true;
}

void CompilePattern(const char* ptnStr, uint8_t** ptnBytes, uint8_t** maskBytes, size_t* ptnLen)
{
	int nibbleCount = 0;
	size_t ptnStrLen = strlen(ptnStr);
	for (size_t i = 0; i < ptnStrLen; i++) {
		if ('0' <= ptnStr[i] && ptnStr[i] <= '9')
			nibbleCount++;
		else if ('a' <= ptnStr[i] && ptnStr[i] <= 'f')
			nibbleCount++;
		else if ('A' <= ptnStr[i] && ptnStr[i] <= 'F')
			nibbleCount++;
		else if (ptnStr[i] == '.' || ptnStr[i] == '*' || ptnStr[i] == '?')
			nibbleCount++;
	}
	int byteCount = (nibbleCount + 1) / 2;
	uint8_t* pattern = (uint8_t*)malloc(byteCount);
	uint8_t* mask = (uint8_t*)malloc(byteCount);
	if (pattern == NULL || mask == NULL)
		return;
	memset(mask, 0, byteCount);
	memset(pattern, 0, byteCount);
	int ptnIdx = 0;
	int offset = 4;
	for (int i = 0; i < ptnStrLen; i++) {
		if ('0' <= ptnStr[i] && ptnStr[i] <= '9') {
			mask[ptnIdx / 2] |= 0xf << offset;
			pattern[ptnIdx / 2] |= (ptnStr[i] - '0') << offset;
			ptnIdx++;
			offset ^= 4;
		}
		else if ('a' <= ptnStr[i] && ptnStr[i] <= 'f') {
			mask[ptnIdx / 2] |= 0xf << offset;
			pattern[ptnIdx / 2] |= (10 + ptnStr[i] - 'a') << offset;
			ptnIdx++;
			offset ^= 4;
		}
		else if ('A' <= ptnStr[i] && ptnStr[i] <= 'F') {
			mask[ptnIdx / 2] |= 0xf << offset;
			pattern[ptnIdx / 2] |= (10 + ptnStr[i] - 'A') << offset;
			ptnIdx++;
			offset ^= 4;
		}
		else if (ptnStr[i] == '.' || ptnStr[i] == '*' || ptnStr[i] == '?') {
			ptnIdx++;
			offset ^= 4;
		}
	}
	*ptnBytes = pattern;
	*maskBytes = mask;
	*ptnLen = byteCount;
}

uintptr_t FindPattern(const char* pattern, const int offset)
{
	if (!EnsureExe())
		return NULL;
	return FindPatternEx(exeStart, exeLen, pattern, offset);
}

uintptr_t GetExeBase(void)
{
	if (!EnsureExe())
		return NULL;
	return exeStart;
}

uintptr_t FindPattern(const pattern& pattern)
{
	return FindPattern(pattern.pattern, pattern.offset);
}

uintptr_t FindCompiledPattern(const uint8_t* pattern, const uint8_t* mask, size_t patternLength, const uint8_t* buffer, size_t bufferLength)
{
	size_t skip[256];
	for (int i = 0; i < 256; i++) {
		skip[i] = patternLength;
	}
	for (int i = 0; i < patternLength - 1; i++) {
		size_t skipVal = patternLength - 1 - i;
		if (mask[i] == 0xff)
			skip[pattern[i]] = skipVal;
		else
			for (int j = 0; j < 256; j++)
				if ((mask[i] & j) == (mask[i] & pattern[i]))
					skip[j] = skipVal;
	}
	for (size_t idx = 0; idx + patternLength <= bufferLength; idx += skip[buffer[idx + patternLength - 1]]) {
		if (memptn(buffer + idx, pattern, mask, patternLength)) {
			return idx;
		}
	}
	return -1;
}

uintptr_t FindPatternEx(uintptr_t start, size_t len, const char* ptnStr, const int offset)
{
	
	uint8_t* pattern;
	uint8_t* mask;
	uint8_t* buffer = (uint8_t*)start;
	size_t patternLength;
	CompilePattern(ptnStr, &pattern, &mask, &patternLength);
	uintptr_t relativeOffset = FindCompiledPattern(pattern, mask, patternLength, buffer, len);
	free(pattern);
	free(mask);
	if (relativeOffset == -1)
		return NULL;
	return relativeOffset + start - offset;
}

uintptr_t FindPatternEx(uintptr_t start, size_t len, const pattern& pattern)
{
	return FindPatternEx(start, len, pattern.pattern, pattern.offset);
}

uintptr_t NopInstruction(uintptr_t address)
{
	size_t length = nmd_x86_ldisasm((void*)address, NMD_LDISASM_X86_MODE_64);
	void* nops = malloc(length);
	if (nops == NULL) return NULL;
	memset(nops, 0x90, length);
	bool success = WriteForeignMemory(address, nops, length);
	free(nops);
	if (!success)
		return NULL;
	return address + length;
}

uintptr_t InsertHook(uintptr_t address, uintptr_t hook)
{
	return InsertHookWithSkip(address, address, hook);
}

bool WriteLongJump(uintptr_t from, uintptr_t target) {
	uint8_t jmp[6 + sizeof(uint64_t)] = { 0 };
	jmp[0] = 0xff;
	jmp[1] = 0x25;
	uint64_t targetAddr = (uint64_t)target;
	memcpy(jmp + 6, &targetAddr, sizeof(uint64_t));
	return WriteForeignMemory(from, jmp, 6 + sizeof(uint64_t));
}

uintptr_t InsertHookWithSkip(uintptr_t branchAddress, uintptr_t returnAddress, uintptr_t hook)
{
	uintptr_t actualRetAddr = returnAddress;
	size_t skipLength = returnAddress - branchAddress;
	size_t minHookLength = 0xe;
	size_t clobberLength = 0;
	size_t ptrLength = 0;
	while (clobberLength < minHookLength) {
		size_t nextInstrLength = nmd_x86_ldisasm((void*)(branchAddress + clobberLength), NMD_LDISASM_X86_MODE_64);
		if (nextInstrLength == 0)
			return NULL;
		clobberLength += nextInstrLength;
	}
	size_t copyLength = 0;
	if (clobberLength > skipLength) {
		copyLength = clobberLength - skipLength;
	}

	if (copyLength > 0) {
		if (!EnsureHeap())
			return NULL;
		actualRetAddr = (uintptr_t)HeapAlloc(heap, 0, copyLength + minHookLength);
		if (actualRetAddr == NULL)
			return NULL;
		memcpy((void*)actualRetAddr, (void*)returnAddress, copyLength);
		if (!WriteLongJump(actualRetAddr + copyLength, returnAddress + copyLength)) {
			HeapFree(heap, 0, (void*)actualRetAddr);
			return NULL;
		}
	}

	if (!WriteLongJump(branchAddress, hook))
		return NULL;
	return actualRetAddr;
}

uintptr_t DecodeRM(uintptr_t rmbyte) {
	uint8_t rm = *(uint8_t*)rmbyte;
	if (rm >> 6 == 0 && (rm & 0x7) == 5) {
		return rmbyte + 5 + *(int32_t*)(rmbyte + 1);
	}
	return NULL;
}

uintptr_t GetReferencedAddress(uintptr_t instruction)
{
	uint8_t opcode = *(uint8_t*)instruction;
	int64_t offset = 0;
	if ((opcode & 0xf0) == 0x40) {
		instruction++;
		opcode = *(uint8_t*)instruction;
	}
	switch (opcode) {
	case 0x00:
	case 0x01:
	case 0x02:
	case 0x03:
		return DecodeRM(instruction + 1);
	case 0xe9:
	case 0xe8:
		offset = *(int32_t*)(instruction + 1);
		offset += 5;
		break;
	case 0x70:
	case 0x71:
	case 0x72:
	case 0x73:
	case 0x74:
	case 0x75:
	case 0x76:
	case 0x77:
	case 0x78:
	case 0x79:
	case 0x7a:
	case 0x7b:
	case 0x7c:
	case 0x7d:
	case 0x7e:
	case 0x7f:
	case 0xeb:
		offset = *(int8_t*)(instruction + 1);
		offset += 2;
		break;
	case 0x0f:
		instruction++;
		opcode = *(uint8_t*)instruction;
		switch (opcode) {
			case 0x80:
			case 0x81:
			case 0x82:
			case 0x83:
			case 0x84:
			case 0x85:
			case 0x86:
			case 0x87:
			case 0x88:
			case 0x89:
			case 0x8a:
			case 0x8b:
			case 0x8c:
			case 0x8d:
			case 0x8e:
			case 0x8f:
				offset = *(int32_t*)(instruction + 1);
				offset += 5;
				break;
			default:
				return NULL;
		}
		break;
	case 0x88:
	case 0x89:
	case 0x8a:
	case 0x8b:
	case 0x8d:
		return DecodeRM(instruction + 1);
	default:
		return NULL;
	}
	return instruction + offset;
}

bool WriteForeignMemory(uintptr_t target, void* source, size_t length)
{
	DWORD oldProtect = 0;
	DWORD newProtect = 0;
	if (!VirtualProtect((void*)target, length, PAGE_EXECUTE_READWRITE, &oldProtect))
		return false;
	memcpy((void*)target, source, length);
	if (!VirtualProtect((void*)target, length, oldProtect, &newProtect))
		return false;
	return true;
}

bool isRttiLocator(uint32_t typeDescriptorFieldOffset) {
	if (typeDescriptorFieldOffset > exeLen)
		return false;
	uint32_t locatorOffset = typeDescriptorFieldOffset - 0xc;
	return (*(uint32_t*)(exeStart + typeDescriptorFieldOffset + 0x8) == locatorOffset);
}



uintptr_t GetClassVftable(const char* className)
{
	if (!EnsureExe())
		return NULL;
	size_t len = strlen(className) + 1;
	uint8_t* mask = (uint8_t*) malloc(len);
	if (mask == NULL)
		return NULL;
	memset(mask, 0xff, len);
	uintptr_t stringOffset = FindCompiledPattern((const uint8_t*)className, mask, len, (uint8_t*)exeStart, exeLen);
	free(mask);
	if (stringOffset == -1)
		return NULL;
	uint32_t typeInfoOffset = (uint32_t)(stringOffset - 0x10);
	uint32_t offsetMask = 0xffffffff;
	uint32_t currentOffset = 0;
	uintptr_t objLocatorLoc = FindCompiledPattern((uint8_t*)&typeInfoOffset, (uint8_t*)&offsetMask, 4, (uint8_t*)exeStart + currentOffset, exeLen - currentOffset);
	while (objLocatorLoc != -1 && !isRttiLocator((uint32_t)objLocatorLoc + currentOffset)) {
		currentOffset += (uint32_t)objLocatorLoc;
		objLocatorLoc = FindCompiledPattern((uint8_t*)&typeInfoOffset, (uint8_t*)&offsetMask, 4, (uint8_t*)exeStart + currentOffset, exeLen - currentOffset);
	}
	if (objLocatorLoc == -1)
		return NULL;
	objLocatorLoc += currentOffset;
	if (!isRttiLocator(objLocatorLoc))
		return NULL;
	objLocatorLoc -= 0xc;
	objLocatorLoc += exeStart;
	uint64_t objLocatorAddress = objLocatorLoc;
	uint64_t addressMask = -1;
	uintptr_t result = FindCompiledPattern((uint8_t*)&objLocatorAddress, (uint8_t*)&addressMask, 8, (uint8_t*)exeStart, exeLen);
	if (result == -1)
		return NULL;
	return result + 8;
}

bool InsertTrampoline(uintptr_t branchAddress, uintptr_t targetAddress)
{
	if (trampolineRegion == NULL)
		return false;
	uintptr_t trampStart = trampolineRegion + trampolineCount * 0xe;
	if (trampStart - trampolineRegion > trampolineSize)
		return false;
	if (!WriteLongJump(trampStart, targetAddress))
		return false;
	uint32_t offset = (uint32_t)(trampStart - branchAddress);
	offset -= 5;
	uint8_t jump[5] = { 0 };
	jump[0] = 0xe9;
	memcpy(jump + 1, &offset, 4);
	return WriteForeignMemory(branchAddress, jump, 5);
}

uintptr_t InsertNearHook(uintptr_t address, uintptr_t hook)
{
	return InsertNearHookWithSkip(address, address, hook);
}

uintptr_t InsertNearHookWithSkip(uintptr_t branchAddress, uintptr_t returnAddress, uintptr_t hook)
{
	uintptr_t actualRetAddr = returnAddress;
	size_t skipLength = returnAddress - branchAddress;
	size_t minHookLength = 0x5;
	size_t clobberLength = 0;
	size_t ptrLength = 0;
	while (clobberLength < minHookLength) {
		size_t nextInstrLength = nmd_x86_ldisasm((void*)(branchAddress + clobberLength), NMD_LDISASM_X86_MODE_64);
		if (nextInstrLength == 0)
			return NULL;
		clobberLength += nextInstrLength;
	}
	size_t copyLength = 0;
	if (clobberLength > skipLength) {
		copyLength = clobberLength - skipLength;
	}

	if (copyLength > 0) {
		if (!EnsureHeap())
			return NULL;
		actualRetAddr = (uintptr_t)HeapAlloc(heap, 0, copyLength + minHookLength);
		if (actualRetAddr == NULL)
			return NULL;
		memcpy((void*)actualRetAddr, (void*)returnAddress, copyLength);
		if (!WriteLongJump(actualRetAddr + copyLength, returnAddress + copyLength)) {
			HeapFree(heap, 0, (void*)actualRetAddr);
			return NULL;
		}
	}

	if (!InsertTrampoline(branchAddress, hook))
		return NULL;
	return actualRetAddr;
}

bool InitializeNearHooks()
{
	EnsureExe();
	trampolineRegion = (uintptr_t)LhAllocateMemoryEx((void*)exeStart, &trampolineSize);
	if (trampolineRegion == NULL)
		return false;
	DWORD oldProtect = 0;
	if (!VirtualProtect((void*)trampolineRegion, trampolineSize, PAGE_EXECUTE_READ, &oldProtect))
		return false;
	return true;
}