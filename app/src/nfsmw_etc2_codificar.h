#pragma once

// The engine-free half of the ETC2 cache (nfsmw_cache_etc2.cpp): the BC -> ETC2/EAC transcoder and the
// on-disk format of indice.bin / datos.bin. Shared by the game's cache and by the prefill library the
// launcher runs before playing (nfsmw_precarga_etc2.cpp), so both write exactly the same files.

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nfsmw_cache_etc2.h"

namespace nfsmw::nativo::etc2 {

// indice.bin: a 16-byte header ("NFSETC2\0", version, reserved) followed by these records.
struct Registro {
  uint64_t clave;
  uint64_t offset;  // in datos.bin
  uint32_t bytes;
  uint32_t formato;  // VkFormat
  uint64_t suma;     // XXH3 of the data
};
static_assert(sizeof(Registro) == 32);

// Reads indice.bin, dropping records that point past the end of datos.bin (a crash mid-write). If the index
// is missing or from another version, both files are created empty. tam_datos = size of datos.bin.
bool AbrirIndice(const std::filesystem::path& carpeta, std::unordered_map<uint64_t, Registro>& registros,
                 uint64_t& tam_datos, uint64_t& descartados);

// BC data (Trabajo's layout) to ETC2/EAC with the same layout. false for BC1 with transparent texels (no
// punch-through in etcpak) or inconsistent data.
bool Codificar(const Trabajo& t, VkFormat& formato, std::vector<uint8_t>& salida);

// mundo.bin: the keys of the textures that come from the world's streamed sections (written by the prefill).
// The texture quality option lowers only these: cars, vinyls, logos, menus and the HUD keep full detail.
bool LeerMundo(const std::filesystem::path& carpeta, std::unordered_set<uint64_t>& claves);
bool EscribirMundo(const std::filesystem::path& carpeta, const std::unordered_set<uint64_t>& claves);

// sin_etc2.bin (same format): the keys that must never become ETC2, the map (TRACKMAPS, the minimap and the
// world map). The game rewrites the minimap's tiles in place with other tiles, and their thin roads suffer
// with ETC2; they keep the decode path. Written by the prefill.
bool LeerClaves(const std::filesystem::path& carpeta, const char* archivo, std::unordered_set<uint64_t>& claves);
bool EscribirClaves(const std::filesystem::path& carpeta, const char* archivo,
                    const std::unordered_set<uint64_t>& claves);

// XXH3 of the data, as stored in Registro::suma.
uint64_t Suma(const std::vector<uint8_t>& datos);

}  // namespace nfsmw::nativo::etc2
