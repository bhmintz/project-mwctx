#pragma once

// Persistent cache of BC textures transcoded to ETC2/EAC, for the native renderer's Mali mode (phones whose
// GPU cannot sample BC). See nfsmw_cache_etc2.cpp.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace nfsmw::nativo::etc2 {

// Opens (or creates) the cache in `carpeta` and starts the encoder thread. False if it is off
// (nfsmw_nativo_mali_cache_etc2) or the folder cannot be used; then everything else is a no-op.
bool Iniciar(const std::filesystem::path& carpeta);
void Terminar();
bool Activo();

// The ETC2/EAC format a BC format is stored as (same bytes per 4x4 block), or VK_FORMAT_UNDEFINED.
VkFormat FormatoEtc2De(VkFormat bc);

// Is `clave` stored? Fills its format and size.
bool Buscar(uint64_t clave, VkFormat& formato, uint32_t& bytes);
// Reads the stored data (all levels, same layout as the BC data it came from). False if it cannot be read
// or does not pass its checksum (the entry is then forgotten).
bool Leer(uint64_t clave, std::vector<uint8_t>& destino);

// A BC texture to transcode in the background: its data exactly as the ring prepared it for DecodificarBc
// (level after level, layer after layer, tight blocks), with the offset of each level.
struct Trabajo {
  uint64_t clave = 0;
  VkFormat bc = VK_FORMAT_UNDEFINED;
  uint32_t ancho = 0, alto = 0, rebanadas = 1, niveles = 1;
  std::array<uint32_t, 16> desplazamientos{};
  std::vector<uint8_t> datos;
};
// Queues it (moves the data out). False if the queue is full, it is already queued or stored, or the
// cache is off.
bool Encolar(Trabajo&& trabajo);

// Whether the key is a world texture (mundo.bin, written by the launcher's prefill): the only ones the
// texture quality option lowers. False for everything else, and for all of them before the prefill ran.
bool EsDelMundo(uint64_t clave);

// Whether the key is a map texture (sin_etc2.bin, from the prefill): never created as ETC2 nor encoded.
bool SinEtc2(uint64_t clave);

// One line every 10 s with hits, misses and the encoder's progress (from the ring thread). `aciertos` and
// `fallos` are the running totals: textures created from the cache, and BC textures it did not have.
void Informe(uint64_t aciertos, uint64_t fallos);

}  // namespace nfsmw::nativo::etc2
