# clang-p2996 forces -stdlib=libc++ (its clang++.cfg), while apt's OpenCV is built
# against libstdc++ -- the mangled symbols do not match (NSt7__cxx11 vs NSt3__1),
# so linking fails on undefined symbols. OpenCV has to be compiled once with
# libc++ and shipped in the same image as the toolchain.
#
# The tag is pinned in two places: this file's FROM, and the container image in
# .github/workflows/ci.yml. Change one, change the other.
FROM ghcr.io/shoom1/clang-p2996:p2996-9ffb96e3ce36

# Matching the local Homebrew 4.13.0 keeps local and CI behaviour the same, and
# 4.11+ spells the enum arithmetic as init(+MAT+ACCESS_READ, ...) (unary + widens
# to int), which clang 21 no longer flags as -Wenum-enum-conversion. 4.6 does.
ARG OPENCV_VERSION=4.13.0

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      git ninja-build pkg-config python3-pip \
      libfreetype-dev libharfbuzz-dev \
      libpng-dev libjpeg-dev libtiff-dev libwebp-dev \
      libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
      libgl1-mesa-dev libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev \
 && rm -rf /var/lib/apt/lists/*
# The X11/Wayland/GL headers are for glfw, which the project's CMakeLists fetches
# and configures unconditionally: missing any of them fails glfw's own configure.

# The image's cmake is 3.28 and rejects the cxx_std_26 cppreflect asks for.
RUN python3 -m pip install --break-system-packages "cmake>=4.4,<5"

# Only the four modules the project uses. `freetype` lives in opencv_contrib:
# without the extra modules path the BUILD_LIST whitelist matches nothing and
# OpenCV installs without it *silently* -- the image builds fine and the failure
# only surfaces later, at the consumer's find_package(OpenCV COMPONENTS
# freetype). A full OpenCV is 30-60 minutes; whitelisting keeps this 5-15.
RUN git clone --depth 1 --branch ${OPENCV_VERSION} \
      https://github.com/opencv/opencv.git /src/opencv \
 && git clone --depth 1 --branch ${OPENCV_VERSION} \
      https://github.com/opencv/opencv_contrib.git /src/opencv_contrib \
 && cmake -S /src/opencv -B /src/opencv/build -G Ninja \
      -DCMAKE_INSTALL_PREFIX=/opt/opencv \
      -DCMAKE_C_COMPILER=/opt/clang-p2996/bin/clang \
      -DCMAKE_CXX_COMPILER=/opt/clang-p2996/bin/clang++ \
      -DCMAKE_BUILD_TYPE=Release \
      -DOPENCV_EXTRA_MODULES_PATH=/src/opencv_contrib/modules \
      -DBUILD_LIST=core,imgproc,imgcodecs,freetype \
      -DWITH_FREETYPE=ON -DWITH_GTK=OFF -DWITH_QT=OFF \
      -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF \
      -DBUILD_JAVA=OFF -DBUILD_opencv_python3=OFF -DBUILD_DOCS=OFF \
 && cmake --build /src/opencv/build \
 && cmake --install /src/opencv/build \
 && rm -rf /src/opencv /src/opencv_contrib
# -stdlib=libc++ is deliberately absent: the image's clang++.cfg already forces it.
