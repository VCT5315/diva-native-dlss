#pragma once
#include "common.hpp"
#include <cstring>

// Verified against the user's MM+ executable. Validate code and its own event
// name before detouring; never interpret a layout or alpha blend as UI.
inline void* FindForegroundPass(HMODULE image){
    auto base=reinterpret_cast<const unsigned char*>(image);
    auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(!base||dos->e_magic!=IMAGE_DOS_SIGNATURE)return nullptr;
    auto nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return nullptr;
    const size_t size=nt->OptionalHeader.SizeOfImage,rva=0x4DAA00;
    if(size<rva+0x60)return nullptr;
    const auto p=base+rva;
    const unsigned char prefix[]={0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x30,0x48,0x8B,0xDA,0x48,0x8B,0xF9};
    if(memcmp(p,prefix,sizeof(prefix))||p[0x10]!=0x48||p[0x11]!=0x8D||p[0x12]!=0x15||p[0x17]!=0xE8||p[0x47]!=0xE8||p[0x59]!=0xE9)return nullptr;
    int32_t displacement=0;memcpy(&displacement,p+0x13,4);
    const intptr_t nameRva=intptr_t(rva+0x17)+displacement;
    if(nameRva<0||size_t(nameRva)+12>size||memcmp(base+nameRva,"pass_sprite\0",12))return nullptr;
    memcpy(&displacement,p+0x48,4);
    const intptr_t drawRva=intptr_t(rva+0x4C)+displacement;
    if(drawRva<0||size_t(drawRva)>=size)return nullptr;
    MEMORY_BASIC_INFORMATION memory{};
    if(!VirtualQuery(p,&memory,sizeof(memory))||memory.State!=MEM_COMMIT)return nullptr;
    const DWORD executable=PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY;
    if(!(memory.Protect&executable))return nullptr;
    return const_cast<unsigned char*>(p);
}
