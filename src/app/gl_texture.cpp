#include "app/gl_texture.h"

#include <opencv2/imgproc.hpp>

namespace campcat::app {

GLuint upload_bgr(const cv::Mat &_bgr, int *_w, int *_h, bool _nearest) {
  if (_bgr.empty() || _bgr.type() != CV_8UC3) {
    return 0;
  }
  cv::Mat rgba;
  cv::cvtColor(_bgr, rgba, cv::COLOR_BGR2RGBA);

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                  _nearest ? GL_NEAREST : GL_LINEAR);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.cols, rgba.rows, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba.ptr());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
  *_w = rgba.cols;
  *_h = rgba.rows;
  return tex;
}

void delete_tex(GLuint *_tex, int *_w, int *_h) {
  if (*_tex != 0) {
    glDeleteTextures(1, _tex);
    *_tex = 0;
  }
  *_w = 0;
  *_h = 0;
}

} // namespace campcat::app
