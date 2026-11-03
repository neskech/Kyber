// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Safe reflection helpers: field lookup and text dumps of Frostbite objects through their TypeInfo.

#pragma once

#define _WINSOCKAPI_
#include <Entity/NativeEntityManager.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace Kyber
{
namespace Reflect
{
// Copies size bytes when the range is committed and readable.
bool SafeRead(const void* ptr, void* out, size_t size);

// Printable CString at ptr, or a marker.
std::string ReadCString(const char* ptr);

// True when the TypeInfo and its name can be read (guards garbage object headers).
bool ReadableType(const TypeInfo* type);

// Type name of a TypeInfo, or "?".
const char* TypeName(const TypeInfo* type);

// Finds a field by name on a class (walking superclasses) or value type.
const FieldInfoData* FindField(const TypeInfo* type, const char* name);

// Reads a 32-bit field by name; false when missing or unreadable.
bool ReadU32(const void* object, const TypeInfo* type, const char* name, uint32_t& out);

// Reads a Boolean field by name; false when missing or unreadable.
bool ReadBool(const void* object, const TypeInfo* type, const char* name, bool& out);

// Reads the pointer stored in a class field, or nullptr.
DataContainer* ReadPointerField(const void* object, const TypeInfo* type, const char* name);

// Element count and data pointer of an FBArray stored at slot.
bool ReadArray(const void* slot, const uint8_t*& data, uint32_t& size);

// Dumps every field of a class (with superclasses) or value type at p.
std::string DumpFields(const uint8_t* p, const TypeInfo* type, int depth);

// Reflected value at p as text; nested types are expanded a few levels.
std::string DumpValue(const uint8_t* p, const TypeInfo* type, int depth);

// Raw qwords at p, labelled as TypeInfo, typed object (one more sniff level while depth > 0), or pointer.
std::string Sniff(const void* p, int count, int depth);
} // namespace Reflect
} // namespace Kyber
