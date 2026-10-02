// See nfsmw_tpk.h.
//
// What the files look like (worked out on the Spanish Xbox 360 disc, 2026-10-02):
// - NFS/ZDIR.BIN: 1,390 records of 24 bytes {hash, archive N, sector in ZZDATAN.BIN, global sector, bytes,
//   checksum}, little endian. Sectors are 2048 bytes.
// - Each file is a bChunk tree (id, size, little endian; ids with the top bit set are containers). 247 files
//   start with a texture pack (0xB3300000); others carry packs inside; 11 are whole-file JDLZ.
// - The packs seen so far are the compressed kind: 0xB3310000 holds 33310001 (version, name, file name),
//   33310002 (texture hashes) and 33310003 (one 24-byte record per texture: hash, position of its block,
//   compressed size, decompressed size, flags, 0). Each block is JDLZ or HUFF (the EA Huffman codec, 0x30FB)
//   and decompresses to the texture's data followed by a 0x9C-byte info block: name at +0x0C, hash at
//   +0x24, base level size at +0x40, width and height at +0x44, mip levels at +0x4E and the D3DFORMAT at
//   +0x90. The data is the base level region (bytes_base) then the mip region, in the Xbox 360 layout
//   (tiled, 4 KB aligned), the same bytes the renderer reads from guest memory.

#include "nfsmw_tpk.h"

#include <cstdio>
#include <algorithm>
#include <cstring>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
// EA's own decoder for the HUFF blocks (C&C Generals Zero Hour source release, GPL-3; sdk/thirdparty/eac_huff).
#include "huffdecode.cpp"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace nfsmw::tpk {
namespace {

constexpr uint32_t kPaquete = 0xB3300000;
constexpr uint32_t kCabecera = 0xB3310000;
constexpr uint32_t kInfoPaquete = 0x33310001;
constexpr uint32_t kBloquesComprimidos = 0x33310003;
constexpr uint32_t kInfoTexturas = 0x33310004;
constexpr uint32_t kFormatosTexturas = 0x33310005;
constexpr uint32_t kDatosPaquete = 0xB3320000;
constexpr uint32_t kDatos = 0x33320003;
constexpr uint32_t kTamInfo = 0x7C;     // one TextureInfo (33310004, and the start of each compressed tail)
constexpr uint32_t kTamFormato = 0x20;  // one 33310005 record; the D3DFORMAT is at +0x14
constexpr uint32_t kTamCola = 0x9C;
constexpr uint32_t kBloque = 0x55441122;
constexpr uint32_t kAudio = 0x6C484353;          // "SCHl", EA audio stream
constexpr uint32_t kAudioEnBloques = 0x8CA5CEFA;  // the audio bank that starts with this chunk and SCHl inside

uint32_t L32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint16_t L16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

struct Trozo {
  uint32_t id;
  size_t datos;  // where its contents start
  size_t bytes;
};

// The direct children of [inicio, fin). Stops at the first chunk that does not fit.
void Hijos(const std::vector<uint8_t>& a, size_t inicio, size_t fin, std::vector<Trozo>& hijos) {
  hijos.clear();
  size_t p = inicio;
  while (p + 8 <= fin) {
    const uint32_t id = L32(&a[p]);
    const uint32_t bytes = L32(&a[p + 4]);
    if (p + 8 + uint64_t(bytes) > fin) {
      break;
    }
    hijos.push_back({id, p + 8, bytes});
    p += 8 + size_t(bytes);
  }
}

std::string Cadena(const uint8_t* p, size_t max) {
  size_t n = 0;
  while (n < max && p[n]) {
    ++n;
  }
  return std::string(reinterpret_cast<const char*>(p), n);
}

bool Descomprimir(const uint8_t* p, size_t bytes, std::vector<uint8_t>& salida) {
  if (bytes >= 4 && !std::memcmp(p, "JDLZ", 4)) {
    return DescomprimirJdlz(p, bytes, salida);
  }
  if (bytes >= 4 && !std::memcmp(p, "HUFF", 4)) {
    return DescomprimirHuff(p, bytes, salida);
  }
  return false;
}

// What a TextureInfo says (the 0x7C bytes of 33310004, or the start of a compressed block's tail).
void DeLaInfo(const uint8_t* info, uint32_t formato_d3d, Textura& t) {
  t.nombre = Cadena(info + 0x0C, 24);
  t.hash = L32(info + 0x24);
  t.colocacion = L32(info + 0x30);
  t.bytes_imagen = L32(info + 0x38);
  t.bytes_base = L32(info + 0x40);
  t.ancho = L16(info + 0x44);
  t.alto = L16(info + 0x46);
  t.niveles = info[0x4E] ? info[0x4E] : 1;
  t.formato_d3d = formato_d3d;
  t.cola.resize(kTamInfo / 4);
  for (uint32_t k = 0; k < kTamInfo / 4; ++k) {
    t.cola[k] = L32(info + 4 * k);
  }
}

// An uncompressed pack: the infos in 33310004, the formats in 33310005 and all the data together at the end
// of 33320003 (after 0x11 padding), each texture at its placement. The data is taken as ending where the
// chunk ends, which is how the samples are laid out (the padding aligns the start, not the end).
bool LeerPaqueteSinComprimir(const std::vector<uint8_t>& a, const Trozo& infos, const Trozo& formatos,
                             const Trozo& datos, const std::string& nombre_paquete,
                             const std::function<bool(Textura&)>& alTextura, Estadisticas& e) {
  const size_t n = infos.bytes / kTamInfo;
  if (!n || formatos.bytes < n * kTamFormato) {
    e.fallos += n;
    return true;
  }
  uint64_t total = 0;
  for (size_t k = 0; k < n; ++k) {
    const uint8_t* info = &a[infos.datos + k * kTamInfo];
    total = std::max<uint64_t>(total, uint64_t(L32(info + 0x30)) + L32(info + 0x38));
  }
  if (total > datos.bytes) {
    e.fallos += n;
    return true;
  }
  const size_t inicio = datos.datos + datos.bytes - size_t(total);
  for (size_t k = 0; k < n; ++k) {
    Textura t;
    t.paquete = nombre_paquete;
    DeLaInfo(&a[infos.datos + k * kTamInfo], L32(&a[formatos.datos + k * kTamFormato + 0x14]), t);
    if (!t.ancho || !t.alto || !t.bytes_imagen || t.bytes_base > t.bytes_imagen) {
      ++e.fallos;
      continue;
    }
    t.datos.assign(a.begin() + (inicio + t.colocacion), a.begin() + (inicio + t.colocacion + t.bytes_imagen));
    ++e.texturas;
    if (!alTextura(t)) {
      return false;
    }
  }
  return true;
}

// A compressed pack: one block per 33310003 record. Positions are from the start of the pack chunk's header
// (seen at offset 0 of the file, where both readings agree); the start of the file is tried as well.
bool LeerPaqueteComprimido(const std::vector<uint8_t>& a, size_t inicio_paquete, const Trozo& registros,
                           const std::string& nombre_paquete, const std::function<bool(Textura&)>& alTextura,
                           Estadisticas& e) {
  std::vector<uint8_t> bloque;
  for (size_t r = 0; r + 24 <= registros.bytes; r += 24) {
    const uint8_t* reg = &a[registros.datos + r];
    const uint32_t hash = L32(reg);
    const uint32_t pos = L32(reg + 4);
    const uint32_t comprimidos = L32(reg + 8);
    const uint32_t descomprimidos = L32(reg + 12);
    size_t origen = SIZE_MAX;
    for (const size_t base : {inicio_paquete, size_t(0)}) {
      const uint64_t o = uint64_t(base) + pos;
      if (o + comprimidos <= a.size() && comprimidos >= 16 &&
          (!std::memcmp(&a[o], "JDLZ", 4) || !std::memcmp(&a[o], "HUFF", 4))) {
        origen = size_t(o);
        break;
      }
    }
    if (origen == SIZE_MAX || !Descomprimir(&a[origen], comprimidos, bloque) || bloque.size() != descomprimidos ||
        bloque.size() < kTamCola) {
      ++e.fallos;
      continue;
    }
    const uint8_t* cola = bloque.data() + bloque.size() - kTamCola;
    Textura t;
    t.paquete = nombre_paquete;
    DeLaInfo(cola, L32(cola + kTamInfo + 0x14), t);
    const size_t bytes_datos = bloque.size() - kTamCola;
    if (t.hash != hash || !t.ancho || !t.alto || t.bytes_imagen > bytes_datos || t.bytes_base > t.bytes_imagen) {
      ++e.fallos;
      continue;
    }
    bloque.resize(bytes_datos);
    t.datos.swap(bloque);
    ++e.texturas;
    if (!alTextura(t)) {
      return false;
    }
  }
  return true;
}

bool Recorrer(const std::vector<uint8_t>& a, size_t inicio, size_t fin, int profundidad,
              const std::function<bool(Textura&)>& alTextura, Estadisticas& e) {
  std::vector<Trozo> hijos;
  Hijos(a, inicio, fin, hijos);
  for (const Trozo& t : hijos) {
    if (t.id == kPaquete) {
      ++e.paquetes;
      std::vector<Trozo> partes;
      Hijos(a, t.datos, t.datos + t.bytes, partes);
      std::string nombre;
      Trozo registros{};
      Trozo infos{};
      Trozo formatos{};
      Trozo datos{};
      for (const Trozo& parte : partes) {
        if (parte.id != kCabecera && parte.id != kDatosPaquete) {
          continue;
        }
        std::vector<Trozo> cab;
        Hijos(a, parte.datos, parte.datos + parte.bytes, cab);
        for (const Trozo& c : cab) {
          if (c.id == kInfoPaquete && c.bytes >= 0x24) {
            nombre = Cadena(&a[c.datos + 4], 0x1C);
          } else if (c.id == kBloquesComprimidos) {
            registros = c;
          } else if (c.id == kInfoTexturas) {
            infos = c;
          } else if (c.id == kFormatosTexturas) {
            formatos = c;
          } else if (c.id == kDatos) {
            datos = c;
          }
        }
      }
      if (registros.bytes) {
        if (!LeerPaqueteComprimido(a, t.datos - 8, registros, nombre, alTextura, e)) {
          return false;
        }
      } else if (infos.bytes) {
        ++e.paquetes_sin_comprimir;
        if (!LeerPaqueteSinComprimir(a, infos, formatos, datos, nombre, alTextura, e)) {
          return false;
        }
      }
    } else if ((t.id & 0x80000000u) && profundidad < 6) {
      if (!Recorrer(a, t.datos, t.datos + t.bytes, profundidad + 1, alTextura, e)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool LeerIndice(const std::string& ruta_zdir, std::vector<Entrada>& entradas) {
  entradas.clear();
  FILE* f = std::fopen(ruta_zdir.c_str(), "rb");
  if (!f) {
    return false;
  }
  uint8_t r[24];
  while (std::fread(r, 1, sizeof(r), f) == sizeof(r)) {
    entradas.push_back({L32(r), L32(r + 4), L32(r + 8), L32(r + 12), L32(r + 16), L32(r + 20)});
  }
  std::fclose(f);
  return !entradas.empty();
}

bool DescomprimirJdlz(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
  if (n < 16 || std::memcmp(src, "JDLZ", 4)) {
    return false;
  }
  const uint32_t usize = L32(src + 8);
  out.assign(usize, 0);
  size_t i = 16;
  size_t o = 0;
  uint32_t f1 = 1;
  uint32_t f2 = 1;
  while (i < n && o < usize) {
    if (f1 == 1) {
      f1 = src[i++] | 0x100u;
    }
    if (f2 == 1) {
      if (i >= n) {
        return false;
      }
      f2 = src[i++] | 0x100u;
    }
    if (f1 & 1) {
      if (i + 1 >= n) {
        return false;
      }
      uint32_t largo;
      uint32_t atras;
      if (f2 & 1) {
        largo = (src[i + 1] | ((src[i] & 0xF0u) << 4)) + 3;
        atras = (src[i] & 0x0Fu) + 1;
      } else {
        atras = (src[i + 1] | ((src[i] & 0xE0u) << 3)) + 17;
        largo = (src[i] & 0x1Fu) + 3;
      }
      i += 2;
      if (atras > o) {
        return false;
      }
      for (uint32_t k = 0; k < largo && o < usize; ++k, ++o) {
        out[o] = out[o - atras];
      }
      f2 >>= 1;
    } else {
      if (i >= n) {
        return false;
      }
      out[o++] = src[i++];
    }
    f1 >>= 1;
  }
  return o == usize;
}

bool DescomprimirHuff(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
  if (n < 18 || std::memcmp(src, "HUFF", 4)) {
    return false;
  }
  const uint32_t usize = L32(src + 8);
  // EA's decoder reads a little past the stream and writes past the end on a bad stream: both buffers get
  // slack, and the result counts only if it is exactly the declared size.
  static thread_local std::vector<uint8_t> entrada;
  entrada.assign(src + 16, src + n);
  entrada.resize(entrada.size() + 64, 0);
  out.assign(size_t(usize) + 65536, 0);
  const int r = HUFF_decompress(entrada.data(), out.data());
  out.resize(usize);
  return r >= 0 && uint32_t(r) == usize;
}

namespace {

// The bytes of one archive entry, read through a window so the 481 MB world file never sits in memory whole.
class Ventana {
 public:
  Ventana(FILE* f, uint64_t inicio, uint64_t bytes) : f_(f), inicio_(inicio), bytes_(bytes) {}
  Ventana(const std::vector<uint8_t>& memoria) : memoria_(&memoria), bytes_(memoria.size()) {}
  uint64_t Bytes() const { return bytes_; }
  // Pointer to [pos, pos + n) of the entry, valid until the next call; nullptr past the end or on a read error.
  const uint8_t* Ver(uint64_t pos, size_t n) {
    if (pos + n > bytes_) {
      return nullptr;
    }
    if (memoria_) {
      return memoria_->data() + pos;
    }
    if (pos < desde_ || pos + n > desde_ + buffer_.size()) {
      constexpr size_t kVentana = 4u << 20;
      const size_t a_leer = size_t(std::min<uint64_t>(std::max(kVentana, n), bytes_ - pos));
      buffer_.resize(a_leer);
      if (fseeko(f_, off_t(inicio_ + pos), SEEK_SET) != 0 || std::fread(buffer_.data(), 1, a_leer, f_) != a_leer) {
        buffer_.clear();
        return nullptr;
      }
      desde_ = pos;
    }
    return buffer_.data() + (pos - desde_);
  }

 private:
  FILE* f_ = nullptr;
  const std::vector<uint8_t>* memoria_ = nullptr;
  uint64_t inicio_ = 0;
  uint64_t bytes_ = 0;
  uint64_t desde_ = 0;
  std::vector<uint8_t> buffer_;
};

// The streamed world file: 0x55441122 blocks of 24-byte header {magic, decompressed bytes, block bytes
// (with the header), offset of the block in its section, offset in the file, 0} and a JDLZ or HUFF body.
// A block at offset 0 starts a new section; each section is a chunk tree of its own.
bool RecorrerBloques(Ventana& v, const std::function<bool(Textura&)>& alTextura, Estadisticas& e,
                     const std::function<void(uint64_t)>& avance) {
  // Everything in these sections is the world (roads, buildings, scenery): marked so the texture quality option
  // can lower it and leave cars, vinyls and logos alone.
  const std::function<bool(Textura&)> del_mundo = [&](Textura& t) {
    t.del_mundo = true;
    return alTextura(t);
  };
  std::vector<uint8_t> seccion;
  std::vector<uint8_t> bloque;
  uint64_t p = 0;
  uint64_t avisado = 0;
  while (const uint8_t* cabecera = v.Ver(p, 24)) {
    if (L32(cabecera) != kBloque) {
      p += 4;  // padding between blocks
      continue;
    }
    const uint32_t descomprimidos = L32(cabecera + 4);
    const uint32_t bytes = L32(cabecera + 8);
    const uint32_t destino = L32(cabecera + 12);
    if (bytes < 24 || descomprimidos > (64u << 20)) {
      ++e.fallos;
      break;
    }
    if (destino == 0 && !seccion.empty()) {
      ++e.secciones;
      if (!Recorrer(seccion, 0, seccion.size(), 0, del_mundo, e)) {
        return false;
      }
      seccion.clear();
    }
    const uint8_t* cuerpo = v.Ver(p + 24, bytes - 24);
    if (!cuerpo) {
      ++e.fallos;
      break;
    }
    const size_t bytes_cuerpo = bytes - 24;
    if (!Descomprimir(cuerpo, bytes_cuerpo, bloque)) {
      bloque.assign(cuerpo, cuerpo + std::min<size_t>(bytes_cuerpo, descomprimidos));  // stored as is
    }
    if (seccion.size() < size_t(destino) + bloque.size()) {
      seccion.resize(size_t(destino) + bloque.size());
    }
    std::memcpy(seccion.data() + destino, bloque.data(), bloque.size());
    p += bytes;
    if (avance && p - avisado >= (8u << 20)) {
      avance(p - avisado);
      avisado = p;
    }
  }
  if (avance) {
    avance(v.Bytes() - std::min(avisado, v.Bytes()));
  }
  if (!seccion.empty()) {
    ++e.secciones;
    return Recorrer(seccion, 0, seccion.size(), 0, del_mundo, e);
  }
  return true;
}

}  // namespace

bool RecorrerArchivo(const std::vector<uint8_t>& archivo, const std::function<bool(Textura&)>& alTextura,
                     Estadisticas& e) {
  ++e.archivos;
  if (archivo.size() >= 24 && L32(archivo.data()) == kBloque) {
    ++e.archivos_por_bloques;
    Ventana v(archivo);
    return RecorrerBloques(v, alTextura, e, nullptr);
  }
  if (archivo.size() >= 16 && !std::memcmp(archivo.data(), "JDLZ", 4)) {
    std::vector<uint8_t> descomprimido;
    if (!DescomprimirJdlz(archivo.data(), archivo.size(), descomprimido)) {
      ++e.fallos;
      return true;
    }
    ++e.archivos_comprimidos;
    return Recorrer(descomprimido, 0, descomprimido.size(), 0, alTextura, e);
  }
  return Recorrer(archivo, 0, archivo.size(), 0, alTextura, e);
}

bool RecorrerEntrada(FILE* zzdata, const Entrada& entrada, const std::function<bool(Textura&)>& alTextura,
                     Estadisticas& e, const std::function<void(uint64_t)>& avance) {
  if (entrada.bytes < 8) {
    return true;  // the index has empty entries (92 on the Spanish disc): nothing to read
  }
  Ventana v(zzdata, uint64_t(entrada.sector) * 2048, entrada.bytes);
  const uint8_t* cabecera = v.Ver(0, 4);
  if (!cabecera) {
    ++e.fallos;
    if (avance) {
      avance(entrada.bytes);
    }
    return true;
  }
  const uint32_t magia = L32(cabecera);
  if (magia == kAudio || magia == kAudioEnBloques) {
    ++e.archivos;
    ++e.saltados;
    if (avance) {
      avance(entrada.bytes);
    }
    return true;
  }
  if (magia == kBloque) {
    ++e.archivos;
    ++e.archivos_por_bloques;
    return RecorrerBloques(v, alTextura, e, avance);
  }
  std::vector<uint8_t> archivo(entrada.bytes);
  const uint8_t* todo = v.Ver(0, entrada.bytes);
  if (!todo) {
    ++e.fallos;
    if (avance) {
      avance(entrada.bytes);
    }
    return true;
  }
  std::memcpy(archivo.data(), todo, entrada.bytes);
  const bool seguir = RecorrerArchivo(archivo, alTextura, e);
  if (avance) {
    avance(entrada.bytes);
  }
  return seguir;
}

}  // namespace nfsmw::tpk
