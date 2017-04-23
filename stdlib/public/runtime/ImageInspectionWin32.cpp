//===--- ImageInspectionWin32.cpp - Win32 image inspection ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file includes routines that interact with the Win32 API on
// Windows platforms to extract runtime metadata embedded in executables and
// DLLs generated by the Swift compiler.
//
//===----------------------------------------------------------------------===//

#if defined(_WIN32) || defined(__CYGWIN__)

#include "ImageInspection.h"
#include "swift/Runtime/Debug.h"
#include <cstdlib>
#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#if defined(__CYGWIN__)
#include <dlfcn.h>
#endif

using namespace swift;

/// PE section name for the section that contains protocol conformance records.
static const char ProtocolConformancesSection[] = ".sw2prtc";
/// PE section name for the section that contains type metadata records.
static const char TypeMetadataRecordsSection[] = ".sw2tymd";

/// Context information passed down from _swift_dl_iterate_phdr to the
/// callback function.
struct InspectArgs {
  void (*fnAddImageBlock)(const void *, uintptr_t);
  const char *sectionName;
};

struct _swift_dl_phdr_info {
  void *dlpi_addr;
  const char *dlpi_name;
};

static int _swift_dl_iterate_phdr(int (*Callback)(struct _swift_dl_phdr_info *info,
                                                  size_t size, const void *data),
                                  const void *data) {
  DWORD procId = GetCurrentProcessId();
  HANDLE procHandle =
      OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, procId);
  if (!procHandle) {
    swift::fatalError(/* flags = */ 0, "OpenProcess() failed");
    return 0;
  }

  int lastRet = 0;

  std::vector<HMODULE> modules(1024);
  DWORD neededSize;

  BOOL ret = EnumProcessModules(procHandle, modules.data(),
                                modules.size() * sizeof(HMODULE), &neededSize);

  if (!ret) {
    swift::fatalError(/* flags = */ 0, "EnumProcessModules() failed");
    return 0;
  }

  if (modules.size() * sizeof(HMODULE) < neededSize) {
    modules.resize(neededSize / sizeof(HMODULE));
    ret = EnumProcessModules(procHandle, modules.data(),
                             modules.size() * sizeof(HMODULE), &neededSize);
  }

  if (!ret) {
    swift::fatalError(/* flags = */ 0, "EnumProcessModules() failed");
    return 0;
  }

  for (unsigned i = 0; i < neededSize / sizeof(HMODULE); i++) {
    char modName[MAX_PATH];

    if (!GetModuleFileNameExA(procHandle, modules[i], modName,
                              sizeof(modName))) {
      swift::fatalError(/* flags = */ 0, "GetModuleFileNameExA() failed");
    }

    _swift_dl_phdr_info hdr;
    hdr.dlpi_name = modName;
    hdr.dlpi_addr = modules[i];

    lastRet = Callback(&hdr, sizeof(hdr), data);
    if (lastRet != 0)
      break;
  }

  CloseHandle(procHandle);

  return lastRet;
}

static uint8_t *_swift_getSectionDataPE(const void *handle, const char *sectionName,
                                        unsigned long *sectionSize) {
  // In Cygwin, dlopen() returns PE/COFF image pointer.
  // This is relying on undocumented feature of Windows API LoadLibrary().
  unsigned char *peStart = (unsigned char *)handle;
  
  const int kLocationOfNtHeaderOffset = 0x3C;
  int ntHeadersOffset =
  *reinterpret_cast<int32_t *>(peStart + kLocationOfNtHeaderOffset);
  
  bool assert1 =
  peStart[ntHeadersOffset] == 'P' && peStart[ntHeadersOffset + 1] == 'E';
  if (!assert1) {
    swift::fatalError(/* flags = */ 0, "_swift_getSectionDataPE()'s finding PE failed");
  }
  
  unsigned char *coff = peStart + ntHeadersOffset + 4;
  
  int16_t numberOfSections = *(int16_t *)(coff + 2);
  
  // SizeOfOptionalHeader
  int16_t sizeOfOptionalHeader = *(int16_t *)(coff + 16);
  
  const int kCoffFileHeaderSize = 20;
  unsigned char *sectionTableBase =
  coff + kCoffFileHeaderSize + sizeOfOptionalHeader;
  
  // Section Header Record
  const int kSectionRecordSize = 40;
  
  unsigned char *sectionHeader = sectionTableBase;
  for (int i = 0; i < numberOfSections; i++) {
    uint32_t virtualSize = *(uint32_t *)&sectionHeader[8];
    uint32_t virtualAddress = *(uint32_t *)&sectionHeader[12];
    
    char nameOfThisSection[9];
    memcpy(nameOfThisSection, sectionHeader, 8);
    nameOfThisSection[8] = '\0';
    
    if (strcmp(sectionName, nameOfThisSection) == 0) {
      *sectionSize = virtualSize;
      return (uint8_t *)handle + virtualAddress;
    }
    sectionHeader += kSectionRecordSize;
  }
  
  return nullptr;
}

static int _addImageCallback(struct _swift_dl_phdr_info *info,
                             size_t size, const void *data) {
  const InspectArgs *inspectArgs = (InspectArgs *)data;
  // inspectArgs contains addImage*Block function and the section name
#if defined(_WIN32)
  HMODULE handle;

  if (!info->dlpi_name || info->dlpi_name[0] == '\0')
    handle = GetModuleHandle(nullptr);
  else
    handle = GetModuleHandleA(info->dlpi_name);
#else
  void *handle;
  if (!info->dlpi_name || info->dlpi_name[0] == '\0')
    handle = dlopen(nullptr, RTLD_LAZY);
  else
    handle = dlopen(info->dlpi_name, RTLD_LAZY | RTLD_NOLOAD);
#endif

  unsigned long conformancesSize;
  const uint8_t *conformances =
    _swift_getSectionDataPE(handle, inspectArgs->sectionName,
                           &conformancesSize);

  if (conformances)
    inspectArgs->fnAddImageBlock(conformances, conformancesSize);

#if defined(_WIN32)
  //FreeLibrary(handle);
#else
  dlclose(handle);
#endif
  return 0;
}

void swift::initializeProtocolConformanceLookup() {
  // Search the loaded dls. This only searches the already
  // loaded ones.
  // FIXME: Find a way to have this continue to happen for dlopen-ed images.
  // rdar://problem/19045112
  const InspectArgs ProtocolConformancesArgs = {
    addImageProtocolConformanceBlockCallback,
    ProtocolConformancesSection,
  };
  _swift_dl_iterate_phdr(_addImageCallback, &ProtocolConformancesArgs);
}

void swift::initializeTypeMetadataRecordLookup() {
  // Search the loaded dls. This only searches the already
  // loaded ones.
  // FIXME: Find a way to have this continue to happen for dlopen-ed images.
  // rdar://problem/19045112
  const InspectArgs TypeMetadataRecordsArgs = {
    addImageTypeMetadataRecordBlockCallback,
    TypeMetadataRecordsSection,
  };
  _swift_dl_iterate_phdr(_addImageCallback, &TypeMetadataRecordsArgs);
}


int swift::lookupSymbol(const void *address, SymbolInfo *info) {
//#if defined(__CYGWIN__)
#if 0
  Dl_info dlinfo;
  if (dladdr(address, &dlinfo) == 0) {
    return 0;
  }

  info->fileName = dlinfo.dli_fname;
  info->baseAddress = dlinfo.dli_fbase;
  info->symbolName = dli_info.dli_sname;
  info->symbolAddress = dli_saddr;
  return 1;
#else
  return 0;
#endif // __CYGWIN__
}

#endif // defined(_WIN32) || defined(__CYGWIN__)
