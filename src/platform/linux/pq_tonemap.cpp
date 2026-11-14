/**
 * @file src/platform/linux/pq_tonemap.cpp
 * @brief GL pass that tone maps a PQ (HDR10) capture to SDR before encoding.
 */
#include "pq_tonemap.h"

#include "src/file_handler.h"
#include "src/logging.h"

#if !defined(SUNSHINE_SHADERS_DIR)  // Same default as graphics.cpp; tests define it in cmake.
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/opengl"
#endif

using namespace std::literals;

namespace egl {

  namespace {
    std::optional<gl::shader_t> compile(const char *path, GLenum type) {
      const auto source = file_handler::read_file(path);
      if (source.empty()) {
        BOOST_LOG(error) << "OpenGL shader source is missing, unreadable, or empty: ["sv << path << ']';
        return std::nullopt;
      }
      auto shader = gl::shader_t::compile(source, type);
      if (shader.has_right()) {
        BOOST_LOG(error) << path << ": "sv << shader.right();
        return std::nullopt;
      }
      return std::move(shader.left());
    }
  }  // namespace

  std::shared_ptr<pq_tonemap_t> pq_tonemap_t::make(int width, int height) {
    auto vert = compile(SUNSHINE_SHADERS_DIR "/Scene.vert", GL_VERTEX_SHADER);
    auto frag = compile(SUNSHINE_SHADERS_DIR "/PqToSdr.frag", GL_FRAGMENT_SHADER);
    if (!vert || !frag) {
      return nullptr;
    }
    auto program = gl::program_t::link(*vert, *frag);
    if (program.has_right()) {
      BOOST_LOG(error) << "GL linker (PqToSdr): "sv << program.right();
      return nullptr;
    }

    std::shared_ptr<pq_tonemap_t> pass {new pq_tonemap_t};
    pass->program = std::move(program.left());
    pass->width = width;
    pass->height = height;

    const auto handle = pass->program.handle();
    const auto image_location = gl::ctx.GetUniformLocation(handle, "image");
    const auto lut_location = gl::ctx.GetUniformLocation(handle, "output_lut");
    pass->white_location = gl::ctx.GetUniformLocation(handle, "sdr_white_nits");
    if (image_location < 0 || lut_location < 0 || pass->white_location < 0) {
      BOOST_LOG(error) << "Couldn't find the PqToSdr shader uniforms"sv;
      return nullptr;
    }
    gl::ctx.UseProgram(handle);
    gl::ctx.Uniform1i(image_location, 0);
    gl::ctx.Uniform1i(lut_location, 1);

    pass->output = gl::tex_t::make(1);
    gl::ctx.BindTexture(GL_TEXTURE_2D, pass->output[0]);
    gl::ctx.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    gl::ctx.BindTexture(GL_TEXTURE_2D, 0);

    pass->framebuffer = gl::frame_buf_t::make(1);
    pass->framebuffer.bind(&pass->output[0], &pass->output[0] + 1);
    const auto status = gl::ctx.CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      BOOST_LOG(error) << "PqToSdr framebuffer incomplete: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    BOOST_LOG(info) << "Tone mapping PQ capture to SDR at "sv << width << 'x' << height;
    return pass;
  }

  std::optional<GLuint> pq_tonemap_t::convert(GLuint source, GLuint lut, float sdr_white_nits) {
    // Log and clear earlier errors so the check below is this pass's own.
    gl_drain_errors;
    gl::ctx.UseProgram(program.handle());
    if (sdr_white_nits != white_nits) {
      gl::ctx.Uniform1f(white_location, sdr_white_nits);
      white_nits = sdr_white_nits;
    }

    gl::ctx.ActiveTexture(GL_TEXTURE1);
    gl::ctx.BindTexture(GL_TEXTURE_2D, lut);
    gl::ctx.ActiveTexture(GL_TEXTURE0);
    gl::ctx.BindTexture(GL_TEXTURE_2D, source);

    GLenum attachment = GL_COLOR_ATTACHMENT0;
    gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, framebuffer[0]);
    gl::ctx.DrawBuffers(1, &attachment);
    gl::ctx.Viewport(0, 0, width, height);
    gl::ctx.DrawArrays(GL_TRIANGLES, 0, 3);

    gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
    gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, 0);

    if (const auto gl_error = gl::ctx.GetError(); gl_error != GL_NO_ERROR) {
      BOOST_LOG(error) << "PqToSdr draw failed: GL error [0x"sv << util::hex(gl_error).to_string_view() << ']';
      return std::nullopt;
    }
    return output[0];
  }

}  // namespace egl
