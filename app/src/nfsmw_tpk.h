#pragma once

// Reading NFS Most Wanted's texture packs (TPK) straight from the game files, without running the game:
// the ZDIR.BIN index, the ZZDATA*.BIN archives, the bChunk tree and the per-texture JDLZ / HUFF compression.
// Used to fill the ETC2 cache before playing (nfsmw_precarga_etc2.cpp). No dependency on the game or the
// renderer, so it also builds in host tools.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace nfsmw::tpk {

// One file of the game's archive (a ZDIR.BIN record, 24 bytes, little endian).
struct Entrada {
  uint32_t hash = 0;
  uint32_t archivo = 0;  // N of ZZDATAN.BIN
  uint32_t sector = 0;   // 2048-byte sector inside that archive
  uint32_t sector_global = 0;
  uint32_t bytes = 0;
  uint32_t suma = 0;
};
bool LeerIndice(const std::string& ruta_zdir, std::vector<Entrada>& entradas);

// One texture as the game keeps it in memory: its data (base level, then the mip region, Xbox 360 layout)
// and what the pack says about it.
struct Textura {
  std::string paquete;   // the pack's name (33310001)
  std::string nombre;
  uint32_t hash = 0;
  uint32_t ancho = 0, alto = 0;
  uint32_t niveles = 1;
  uint32_t formato_d3d = 0;   // D3DFORMAT (e.g. 0x1A200154 = DXT5, tiled)
  uint32_t bytes_base = 0;    // size of the base level region
  uint32_t bytes_imagen = 0;  // base + mips
  uint32_t colocacion = 0;    // where its data starts in the pack's data
  std::vector<uint32_t> cola;  // the raw info block (diagnostic)
  std::vector<uint8_t> datos;
  bool del_mundo = false;  // from the streamed world sections (not cars, vinyls, menus or the HUD)
};

// Decompressors. false if the data is not what they expect or does not decompress to its declared size.
bool DescomprimirJdlz(const uint8_t* origen, size_t bytes, std::vector<uint8_t>& salida);
bool DescomprimirHuff(const uint8_t* origen, size_t bytes, std::vector<uint8_t>& salida);

// Counters of one walk, for the log.
struct Estadisticas {
  uint64_t archivos = 0;
  uint64_t paquetes = 0;
  uint64_t texturas = 0;
  uint64_t fallos = 0;               // textures that did not decompress or did not make sense
  uint64_t paquetes_sin_comprimir = 0;  // packs in the uncompressed layout
  uint64_t archivos_comprimidos = 0;    // whole files compressed with JDLZ
  uint64_t archivos_por_bloques = 0;    // streamed files of 0x55441122 blocks (the world)
  uint64_t secciones = 0;               // sections of those files
  uint64_t saltados = 0;                // audio entries skipped without reading
};

// Walks every texture of every pack in one file of the archive (already read into memory). The callback
// gets each texture; returning false stops the walk.
bool RecorrerArchivo(const std::vector<uint8_t>& archivo, const std::function<bool(Textura&)>& alTextura,
                     Estadisticas& estadisticas);

// The same for one entry read straight from its ZZDATA file, in pieces (the world file is 481 MB). Audio
// entries are skipped. `avance` gets the bytes of the entry done as it goes (for a progress bar).
bool RecorrerEntrada(FILE* zzdata, const Entrada& entrada, const std::function<bool(Textura&)>& alTextura,
                     Estadisticas& estadisticas, const std::function<void(uint64_t)>& avance);

}  // namespace nfsmw::tpk
