#pragma once
/*
 * Mali mode on Vulkan 1.3 (Mesa's PanVK on the Mali-G52): render passes without VkRenderPass or VkFramebuffer.
 *
 * The Mali mode was written for the vendor driver, Vulkan 1.1: every pass of the ring needs a VkRenderPass
 * (cached by formats, load mode and the clears folded into the pass) and a VkFramebuffer (cached by views and
 * size), and every pipeline is created against a render pass. With dynamic rendering (core in 1.3) a pass is
 * opened with vkCmdBeginRendering straight from the views, and a pipeline only needs the formats. Nothing else
 * changes: the same loadOp/storeOp (clears still open with the pass, which is free on a tiler), the GENERAL
 * layout everywhere, and the same external dependencies of DependenciasImagenes, now as synchronization2
 * barriers right before vkCmdBeginRendering and right after vkCmdEndRendering.
 *
 * Used only when the Mali mode is on and the device has dynamicRendering and synchronization2
 * (nfsmw_nativo_mali_vk13); with the vendor's 1.1 driver everything stays as it was.
 */

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

#include "../nfsmw_nativo_sincronizacion.h"

namespace nfsmw::vk13 {

// The Vulkan 1.3 core functions this module records. The SDK's table does not have them: they are requested
// from the driver.
struct Funciones {
  PFN_vkCmdBeginRendering begin_rendering = nullptr;
  PFN_vkCmdEndRendering end_rendering = nullptr;
  PFN_vkCmdPipelineBarrier2 barrera2 = nullptr;

  bool Cargar(PFN_vkGetDeviceProcAddr pedir, VkDevice device) {
    begin_rendering = reinterpret_cast<PFN_vkCmdBeginRendering>(pedir(device, "vkCmdBeginRendering"));
    end_rendering = reinterpret_cast<PFN_vkCmdEndRendering>(pedir(device, "vkCmdEndRendering"));
    barrera2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(pedir(device, "vkCmdPipelineBarrier2"));
    return begin_rendering && end_rendering && barrera2;
  }
};

inline bool TieneEstencil(VkFormat formato) {
  return formato == VK_FORMAT_D24_UNORM_S8_UINT || formato == VK_FORMAT_D32_SFLOAT_S8_UINT ||
         formato == VK_FORMAT_D16_UNORM_S8_UINT || formato == VK_FORMAT_S8_UINT;
}

// What a pipeline needs instead of a render pass: the formats of the pass, with the color slots packed in the
// same order CrearPase gives its attachments (an empty slot 0-3 is skipped, so location 0 is the first one
// there is).
struct FormatosPipeline {
  std::array<VkFormat, 4> colores{};
  VkPipelineRenderingCreateInfo info{};

  // `formatos`: 0-3 color, 4 depth/stencil, 0 = no attachment. The returned info points into this object.
  const VkPipelineRenderingCreateInfo* De(const uint32_t formatos[5]) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (formatos[i]) {
        colores[n++] = VkFormat(formatos[i]);
      }
    }
    info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.colorAttachmentCount = n;
    info.pColorAttachmentFormats = colores.data();
    const VkFormat profundidad = VkFormat(formatos[4]);
    info.depthAttachmentFormat = profundidad;
    info.stencilAttachmentFormat = TieneEstencil(profundidad) ? profundidad : VK_FORMAT_UNDEFINED;
    return &info;
  }
};

// The external dependencies of DependenciasImagenes as a global synchronization2 barrier. The access and
// stage bits used there have the same values in the 2 flags.
inline void Barrera(const Funciones& f, VkCommandBuffer cmd, VkPipelineStageFlags2 desde_etapas,
                    VkAccessFlags2 desde_accesos, VkPipelineStageFlags2 hacia_etapas, VkAccessFlags2 hacia_accesos) {
  VkMemoryBarrier2 memoria{};
  memoria.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  memoria.srcStageMask = desde_etapas;
  memoria.srcAccessMask = desde_accesos;
  memoria.dstStageMask = hacia_etapas;
  memoria.dstAccessMask = hacia_accesos;
  VkDependencyInfo dependencia{};
  dependencia.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependencia.memoryBarrierCount = 1;
  dependencia.pMemoryBarriers = &memoria;
  f.barrera2(cmd, &dependencia);
}

// sin_vertices: as in DependenciasImagenes (the Mali mode's nfsmw_nativo_mali_sin_burbuja).
inline void BarreraEntrada(const Funciones& f, VkCommandBuffer cmd, bool sin_vertices) {
  using namespace nfsmw::nativo;
  const VkPipelineStageFlags destino =
      sin_vertices ? kEtapasPase & ~VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : kEtapasPase;
  Barrera(f, cmd, kEtapasImagenes, kAccesosImagenes, destino, kAccesosPase);
}

inline void BarreraSalida(const Funciones& f, VkCommandBuffer cmd, bool sin_vertices) {
  using namespace nfsmw::nativo;
  const VkPipelineStageFlags destino =
      sin_vertices ? kEtapasImagenes & ~VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : kEtapasImagenes;
  Barrera(f, cmd, kEtapasPase, kAccesosPase, destino, kAccesosImagenes);
}

// Opening a pass. `formatos` and `vistas` by slot (0-3 color, 4 depth/stencil); `borrar`: slots that open with
// loadOp = CLEAR and their `valores`; otherwise `ignorar` (DONT_CARE) or LOAD. Everything is stored and stays
// in GENERAL, as in CrearPase.
inline void AbrirPase(const Funciones& f, VkCommandBuffer cmd, const uint32_t formatos[5],
                      const std::array<VkImageView, 5>& vistas, const VkRect2D& area, uint32_t borrar, bool ignorar,
                      const VkClearValue* valores) {
  std::array<VkRenderingAttachmentInfo, 4> colores{};
  VkRenderingAttachmentInfo profundidad{};
  uint32_t n = 0;
  const auto rellenar = [&](VkRenderingAttachmentInfo& a, uint32_t i) {
    a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageView = vistas[i];
    a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    const bool borra = (borrar >> i) & 0x1;
    a.loadOp = borra ? VK_ATTACHMENT_LOAD_OP_CLEAR : ignorar ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (borra && valores) {
      a.clearValue = valores[i];
    }
  };
  for (uint32_t i = 0; i < 4; ++i) {
    if (formatos[i]) {
      rellenar(colores[n++], i);
    }
  }
  VkRenderingInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  info.renderArea = area;
  info.layerCount = 1;
  info.colorAttachmentCount = n;
  info.pColorAttachments = colores.data();
  VkRenderingAttachmentInfo estencil{};
  if (formatos[4]) {
    rellenar(profundidad, 4);
    info.pDepthAttachment = &profundidad;
    if (TieneEstencil(VkFormat(formatos[4]))) {
      // As CrearPase: the stencil is cleared with the depth, otherwise loaded (even when the depth is not),
      // and always stored.
      estencil = profundidad;
      if (estencil.loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE) {
        estencil.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      }
      info.pStencilAttachment = &estencil;
    }
  }
  f.begin_rendering(cmd, &info);
}

}  // namespace nfsmw::vk13
