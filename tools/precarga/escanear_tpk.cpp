// Development tool for phase 2 of the ETC2 cache (filling it from the game files). Walks every texture pack
// of the game's archive with app/src/nfsmw_tpk.cpp and writes one line per texture: pack, name, size,
// levels, D3DFORMAT, base and image sizes, the XXH3 of its first 4 KB (the "pref" of the renderer's
// [diag etc2] lines, to find the same texture there) and the raw info block.
//
// Build (NDK, run on the phone over adb):
//   aarch64-linux-android29-clang++ -O2 -std=c++20 -I app/src -I sdk/thirdparty/eac_huff -I sdk/thirdparty/xxHash
//       tools/precarga/escanear_tpk.cpp app/src/nfsmw_tpk.cpp -o escanear_tpk
// Usage: escanear_tpk <NFS folder> > texturas.txt

#include <cstdio>
#include <string>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "nfsmw_precarga_etc2.h"
#include "nfsmw_tpk.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "uso: escanear_tpk <carpeta NFS>\n");
    return 1;
  }
  const std::string carpeta = argv[1];
  std::vector<nfsmw::tpk::Entrada> entradas;
  if (!nfsmw::tpk::LeerIndice(carpeta + "/ZDIR.BIN", entradas)) {
    std::fprintf(stderr, "no se pudo leer ZDIR.BIN\n");
    return 1;
  }
  nfsmw::tpk::Estadisticas e;
  FILE* abiertos[16] = {};
  for (const auto& en : entradas) {
    if (en.archivo >= 16) {
      continue;
    }
    FILE*& f = abiertos[en.archivo];
    if (!f) {
      f = std::fopen((carpeta + "/ZZDATA" + std::to_string(en.archivo) + ".BIN").c_str(), "rb");
      if (!f) {
        continue;
      }
    }
    const uint64_t fallos_antes = e.fallos;
    nfsmw::tpk::RecorrerEntrada(
        f, en,
        [&](nfsmw::tpk::Textura& t) {
          const size_t n = t.datos.size() < 4096 ? t.datos.size() : 4096;
          std::printf("%08X\t%s\t%s\t%08X\t%ux%u\t%u\t%08X\t%X\t%X\t%X\t%016llX\t", en.hash, t.paquete.c_str(),
                      t.nombre.c_str(), t.hash, t.ancho, t.alto, t.niveles, t.formato_d3d, t.bytes_base,
                      t.bytes_imagen, unsigned(t.datos.size()),
                      (unsigned long long)XXH3_64bits(t.datos.data(), n));
          uint64_t clave = 0, huella = 0, ext = 0, extm = 0;
          if (nfsmw::nativo::precarga::ClaveDe(t, clave, huella, ext, extm)) {
            std::printf("%016llX\t%016llX\t%llX\t%llX\t", (unsigned long long)clave, (unsigned long long)huella,
                        (unsigned long long)ext, (unsigned long long)extm);
          } else {
            std::printf("-\t-\t-\t-\t");
          }
          nfsmw::nativo::etc2::Trabajo trabajo;
          const bool hecho = nfsmw::nativo::precarga::TrabajoDe(t, trabajo);
          // escanear_tpk <NFS> <carpeta> <texto>: the level 0 of every texture whose name contains <texto>,
          // untiled BC, to <carpeta>/<paquete>_<nombre>_<formato>_<ancho>x<alto>.bc
          if (hecho && argc > 3 && t.nombre.find(argv[3]) != std::string::npos) {
            const std::string ruta = std::string(argv[2]) + "/" + t.paquete + "_" + t.nombre + "_" +
                                     std::to_string(int(trabajo.bc)) + "_" + std::to_string(trabajo.ancho) + "x" +
                                     std::to_string(trabajo.alto) + ".bc";
            if (FILE* v = std::fopen(ruta.c_str(), "wb")) {
              std::fwrite(trabajo.datos.data(), 1, trabajo.niveles > 1 ? trabajo.desplazamientos[1] : trabajo.datos.size(), v);
              std::fclose(v);
            }
          }
          std::printf("%s\t%s\t", hecho ? "trabajo" : "sin_trabajo",
                      t.del_mundo ? "mundo" : "-");
          for (uint32_t w : t.cola) {
            std::printf(" %08X", w);
          }
          std::printf("\n");
          return true;
        },
        e, nullptr);
    if (e.fallos != fallos_antes) {
      std::fprintf(stderr, "fallos en la entrada %08X (ZZDATA%u, %u bytes): %llu\n", en.hash, en.archivo, en.bytes,
                   (unsigned long long)(e.fallos - fallos_antes));
    }
  }
  std::fprintf(stderr, "por bloques %llu (%llu secciones)\n", (unsigned long long)e.archivos_por_bloques,
               (unsigned long long)e.secciones);
  std::fprintf(stderr, "archivos %llu (%llu JDLZ enteros), paquetes %llu (%llu sin comprimir), texturas %llu, fallos %llu\n",
               (unsigned long long)e.archivos, (unsigned long long)e.archivos_comprimidos,
               (unsigned long long)e.paquetes, (unsigned long long)e.paquetes_sin_comprimir,
               (unsigned long long)e.texturas, (unsigned long long)e.fallos);
  return 0;
}
