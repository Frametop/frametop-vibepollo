/**
 * @file src/platform/linux/pq_tonemap.h
 * @brief GL pass that tone maps a PQ (HDR10) capture to SDR before encoding.
 */
#pragma once

#include "graphics.h"

#include <memory>
#include <optional>

namespace egl {

  /**
   * Converts a PQ BT.2020 frame into an SDR sRGB BT.709 texture of the same
   * size, so an HDR output can feed the SDR encode path. The math lives in
   * PqToSdr.frag, mirrored by pq_tonemap_math.h for tests.
   */
  class pq_tonemap_t {
  public:
    static std::shared_ptr<pq_tonemap_t> make(int width, int height);

    /**
     * Draw @p source through the tone map. @p lut is a 1D LUT texture applied
     * to the PQ code values first (the CRTC GAMMA_LUT, or identity).
     * Returns the SDR texture, valid until the next call.
     */
    std::optional<GLuint> convert(GLuint source, GLuint lut, float sdr_white_nits);

    [[nodiscard]] bool matches(int width, int height) const {
      return width == this->width && height == this->height;
    }

  private:
    pq_tonemap_t() = default;

    gl::program_t program;
    gl::tex_t output;
    gl::frame_buf_t framebuffer;
    GLint white_location {-1};
    float white_nits {-1.0f};
    int width {};
    int height {};
  };

}  // namespace egl
