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

/**
 * @brief Upload an image that carries its own alpha as an RGBA texture.
 *
 * Takes BGRA (`CV_8UC4`), which is what `cv::imdecode(..., IMREAD_UNCHANGED)`
 * hands back for a PNG with transparency. `upload_bgr` cannot stand in: it
 * converts to RGBA with alpha 255, so a transparent margin would come out as a
 * black box. An icon wants the margin to blend into whatever is behind it --
 * which is a button, and its colour changes with hover.
 */
GLuint upload_bgra(const cv::Mat &_bgra, int *_w, int *_h);

/// @brief Delete the texture and zero the size reported with it.
void delete_tex(GLuint *_tex, int *_w, int *_h);

} // namespace campcat::app
