#pragma once

// Phase 2 of the ETC2 cache: textures read from the game files (nfsmw_tpk.h) turned into the same cache
// entries the renderer would create while playing, so they can be transcoded before the game starts.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

#include "nfsmw_cache_etc2.h"
#include "nfsmw_tpk.h"

namespace nfsmw::nativo::precarga {

// The fetch constant words the game builds for a pack texture (only the bits the cache key uses; the
// addresses are left at 0). Worked out from the renderer's [diag etc2] lines: see the .cpp.
struct Fetch {
  uint32_t f[6] = {};
};
bool FetchDe(const tpk::Textura& t, Fetch& fetch);

// The cache key and the BC data, untiled and byte swapped, level after level, exactly as the renderer hands
// them to the encoder (SubirTextura). false if it is not a 2D BC texture the cache stores, or its data does
// not cover what the renderer would read.
bool TrabajoDe(const tpk::Textura& t, etc2::Trabajo& trabajo);

// Only the key (and the guest-memory extents it covers), for checking against [diag etc2].
bool ClaveDe(const tpk::Textura& t, uint64_t& clave, uint64_t& huella, uint64_t& extension, uint64_t& extension_mips);

// Progress of Precargar, read by the launcher while it runs (relaxed atomics).
struct Progreso {
  std::atomic<uint64_t> bytes_hechos{0};   // of the game archive walked
  std::atomic<uint64_t> bytes_total{0};
  std::atomic<uint64_t> texturas{0};       // BC 2D textures found (duplicates included)
  std::atomic<uint64_t> nuevas{0};         // transcoded and stored now
  std::atomic<uint64_t> ya_estaban{0};     // already in the cache
  std::atomic<uint64_t> sin_guardar{0};    // BC1 with transparency, or data the renderer would not read
  std::atomic<uint64_t> comprobadas{0};    // already stored, transcoded again and compared byte by byte
  std::atomic<uint64_t> distintas{0};      // ...that did not match (should stay 0)
  std::atomic<uint64_t> del_mundo{0};      // keys marked as world (mundo.bin)
};

// Walks the whole game archive (<carpeta_nfs>/ZDIR.BIN, ZZDATA*.BIN), transcodes every BC texture the cache
// does not have yet with all cores and appends it to <carpeta_cache> (datos.bin, indice.bin, mundo.bin).
// The game must not be running. Returns false if the archive or the cache cannot be opened; `cancelar`
// stops it early (what was stored stays).
bool Precargar(const std::string& carpeta_nfs, const std::filesystem::path& carpeta_cache, Progreso& progreso,
               const std::atomic<bool>& cancelar);

}  // namespace nfsmw::nativo::precarga
