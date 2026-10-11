#pragma once

#include <opencv2/core.hpp>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

namespace campcat::app {

/**
 * @brief Upload a CV_8UC3 BGR image as an RGBA texture.
 *
 * Returns 0 for anything else and leaves `_w` / `_h` untouched, so a failed
 * decode cannot leave a size pointing at no texture. `_nearest` magnifies with
 * GL_NEAREST, which is what a template preview wants: real pixels, not a blur.
 */
GLuint upload_bgr(const cv::Mat &_bgr, int *_w, int *_h, bool _nearest = false);

/// @brief Delete the texture and zero the size reported with it.
void delete_tex(GLuint *_tex, int *_w, int *_h);

} // namespace campcat::app
