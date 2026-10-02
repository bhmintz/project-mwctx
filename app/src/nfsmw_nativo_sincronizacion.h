#pragma once

#include <rex/cvar.h>
#include <vulkan/vulkan.h>

REXCVAR_DECLARE(bool, nfsmw_nativo_sincronizacion_gpu);

namespace nfsmw::nativo {

// GENERAL is a layout, not a memory dependency. The native renderer shares
// images between uploads, resolves, reflections and subsequent render passes.
inline constexpr VkPipelineStageFlags kEtapasImagenes =
    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
inline constexpr VkAccessFlags kAccesosImagenes =
    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
inline constexpr VkPipelineStageFlags kEtapasPase =
    kEtapasImagenes & ~VK_PIPELINE_STAGE_TRANSFER_BIT;
inline constexpr VkAccessFlags kAccesosPase =
    kAccesosImagenes & ~(VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

/*
 * sin_vertices (Mali mode): no dependency ends in the vertex shader stage. On a tiler (Mali) a dependency from
 * one pass's fragment work to the next pass's vertex stage stops the next pass's geometry until the previous
 * pass has finished shading, and with ~20 passes per frame the vertex/tiler and fragment queues never
 * overlap. No vertex shader of the game samples a texture (checked over the 98 translated ones), and vertex
 * data comes from buffers the CPU writes, not from images, so nothing in the vertex stage reads what the
 * passes write.
 */
inline void DependenciasImagenes(VkSubpassDependency (&dependencias)[2], bool sin_vertices = false) {
  const VkPipelineStageFlags destino_entrada =
      sin_vertices ? kEtapasPase & ~VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : kEtapasPase;
  const VkPipelineStageFlags destino_salida =
      sin_vertices ? kEtapasImagenes & ~VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : kEtapasImagenes;
  dependencias[0] = {VK_SUBPASS_EXTERNAL, 0, kEtapasImagenes, destino_entrada,
                     kAccesosImagenes, kAccesosPase, 0};
  dependencias[1] = {0, VK_SUBPASS_EXTERNAL, kEtapasPase, destino_salida,
                     kAccesosPase, kAccesosImagenes, 0};
}

}  // namespace nfsmw::nativo
