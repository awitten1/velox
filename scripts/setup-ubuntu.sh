#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# This script documents setting up a Ubuntu host for Velox
# development.  Running it should make you ready to compile.
#
# Environment variables:
# * INSTALL_PREREQUISITES="N": Skip installation of packages for build.
# * PROMPT_ALWAYS_RESPOND="n": Automatically respond to interactive prompts.
#     Use "n" to never wipe directories.
#
# You can also run individual functions below by specifying them as arguments:
# $ scripts/setup-ubuntu.sh install_googletest install_fmt
#

# Minimal setup for Ubuntu 22.04.
set -eufx -o pipefail
SCRIPTDIR=$(dirname "${BASH_SOURCE[0]}")
source $SCRIPTDIR/setup-helper-functions.sh

# Folly must be built with the same compiler flags so that some low level types
# are the same size.
COMPILER_FLAGS=$(get_cxx_flags)
export COMPILER_FLAGS
NPROC=${BUILD_THREADS:-$(getconf _NPROCESSORS_ONLN)}
BUILD_DUCKDB="${BUILD_DUCKDB:-true}"
BUILD_GEOS="${BUILD_GEOS:-true}"
export CMAKE_BUILD_TYPE=Release
VELOX_BUILD_SHARED=${VELOX_BUILD_SHARED:-"OFF"} #Build folly shared for use in libvelox.so.
SUDO="${SUDO:-"sudo --preserve-env"}"
USE_CLANG="${USE_CLANG:-false}"
export INSTALL_PREFIX=${INSTALL_PREFIX:-"/usr/local"}
DEPENDENCY_DIR=${DEPENDENCY_DIR:-$(pwd)/deps-download}
VERSION=$(cat /etc/os-release | grep VERSION_ID)
PYTHON_VENV=${PYTHON_VENV:-"${SCRIPTDIR}/../.venv"}

# On Ubuntu 20.04 dependencies need to be built using gcc11.
# On Ubuntu 22.04 gcc11 is already the system gcc installed.
if [[ ${VERSION} =~ "20.04" ]]; then
  export CC=/usr/bin/gcc-11
  export CXX=/usr/bin/g++-11
fi

function install_clang15 {
  if [[ ! ${VERSION} =~ "22.04" && ! ${VERSION} =~ "24.04" ]]; then
    echo "Warning: using the Clang configuration is for Ubuntu 22.04 and 24.04. Errors might occur."
  fi
  CLANG_PACKAGE_LIST=clang-15
  if [[ ${VERSION} =~ "22.04" ]]; then
    CLANG_PACKAGE_LIST="${CLANG_PACKAGE_LIST} gcc-12 g++-12 libc++-12-dev"
  fi
  ${SUDO} apt install ${CLANG_PACKAGE_LIST} -y
}

# For Ubuntu 20.04 we need add the toolchain PPA to get access to gcc11.
function install_gcc11_if_needed {
  if [[ ${VERSION} =~ "20.04" ]]; then
    ${SUDO} add-apt-repository ppa:ubuntu-toolchain-r/test -y
    ${SUDO} apt update
    ${SUDO} apt install gcc-11 g++-11 -y
  fi
}

FB_OS_VERSION="v2024.07.01.00"
FMT_VERSION="10.1.1"
BOOST_VERSION="boost-1.84.0"
THRIFT_VERSION="v0.16.0"
# Note: when updating arrow check if thrift needs an update as well.
ARROW_VERSION="15.0.0"
STEMMER_VERSION="2.2.0"
DUCKDB_VERSION="v0.8.1"
GEOS_VERSION="3.12.0"

# Install packages required for build.
function install_build_prerequisites {
  ${SUDO} apt update
  # The is an issue on 22.04 where a version conflict prevents glog install,
  # installing libunwind first fixes this.
  ${SUDO} apt install -y libunwind-dev
  ${SUDO} apt install -y \
    build-essential \
    python3-pip \
    ccache \
    curl \
    ninja-build \
    checkinstall \
    git \
    pkg-config \
    libtool \
    wget

  if [ ! -f ${PYTHON_VENV}/pyvenv.cfg ]; then
    echo "Creating Python Virtual Environment at ${PYTHON_VENV}"
    python3 -m venv ${PYTHON_VENV}
  fi
  source ${PYTHON_VENV}/bin/activate;
  # Install to /usr/local to make it available to all users.
  ${SUDO} pip3 install cmake==3.28.3

  install_gcc11_if_needed

  if [[ ${USE_CLANG} != "false" ]]; then
    install_clang15
  fi

}

# Install packages required to fix format
function install_format_prerequisites {
  pip3 install regex
  ${SUDO} apt install -y \
    clang-format \
    cmake-format
}

# Install packages required for build.
function install_velox_deps_from_apt {
  ${SUDO} apt update
  ${SUDO} apt install -y \
    libc-ares-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    libicu-dev \
    libdouble-conversion-dev \
    libgoogle-glog-dev \
    libbz2-dev \
    libgflags-dev \
    libgmock-dev \
    libevent-dev \
    liblz4-dev \
    libzstd-dev \
    libre2-dev \
    libsnappy-dev \
    libsodium-dev \
    liblzo2-dev \
    libelf-dev \
    libdwarf-dev \
    bison \
    flex \
    libfl-dev \
    tzdata
}

function install_fmt {
  wget_and_untar https://github.com/fmtlib/fmt/archive/${FMT_VERSION}.tar.gz fmt
  cmake_install_dir fmt -DFMT_TEST=OFF
}

function install_boost {
  wget_and_untar https://github.com/boostorg/boost/releases/download/${BOOST_VERSION}/${BOOST_VERSION}.tar.gz boost
  (
    cd ${DEPENDENCY_DIR}/boost
    if [[ ${USE_CLANG} != "false" ]]; then
      ./bootstrap.sh --prefix=${INSTALL_PREFIX} --with-toolset="clang-15"
      # Switch the compiler from the clang-15 toolset which doesn't exist (clang-15.jam) to
      # clang of version 15 when toolset clang-15 is used.
      # This reconciles the project-config.jam generation with what the b2 build system allows for customization.
      sed -i 's/using clang-15/using clang : 15/g' project-config.jam
      ${SUDO} ./b2 "-j${NPROC}" -d0 install threading=multi toolset=clang-15 --without-python
    else
      ./bootstrap.sh --prefix=${INSTALL_PREFIX}
      ${SUDO} ./b2 "-j${NPROC}" -d0 install threading=multi --without-python
    fi
  )
}

function install_protobuf {
  wget_and_untar https://github.com/protocolbuffers/protobuf/releases/download/v21.8/protobuf-all-21.8.tar.gz protobuf
  cmake_install_dir protobuf -Dprotobuf_BUILD_TESTS=OFF
}

function install_folly {
  wget_and_untar https://github.com/facebook/folly/archive/refs/tags/${FB_OS_VERSION}.tar.gz folly
  cmake_install_dir folly -DBUILD_TESTS=OFF -DBUILD_SHARED_LIBS="$VELOX_BUILD_SHARED" -DFOLLY_HAVE_INT128_T=ON
}

function install_fizz {
  wget_and_untar https://github.com/facebookincubator/fizz/archive/refs/tags/${FB_OS_VERSION}.tar.gz fizz
  cmake_install_dir fizz/fizz -DBUILD_TESTS=OFF
}

function install_wangle {
  wget_and_untar https://github.com/facebook/wangle/archive/refs/tags/${FB_OS_VERSION}.tar.gz wangle
  cmake_install_dir wangle/wangle -DBUILD_TESTS=OFF
}

function install_mvfst {
  wget_and_untar https://github.com/facebook/mvfst/archive/refs/tags/${FB_OS_VERSION}.tar.gz mvfst
  cmake_install_dir mvfst -DBUILD_TESTS=OFF
}

function install_fbthrift {
  wget_and_untar https://github.com/facebook/fbthrift/archive/refs/tags/${FB_OS_VERSION}.tar.gz fbthrift
  cmake_install_dir fbthrift -Denable_tests=OFF -DBUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF
}

function install_conda {
  MINICONDA_PATH="${HOME:-/opt}/miniconda-for-velox"
  if [ -e ${MINICONDA_PATH} ]; then
    echo "File or directory already exists: ${MINICONDA_PATH}"
    return
  fi
  ARCH=$(uname -m)
  if [ "$ARCH" != "x86_64" ] && [ "$ARCH" != "aarch64" ]; then
    echo "Unsupported architecture: $ARCH"
    exit 1
  fi
  (
    mkdir -p conda && cd conda
    wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-$ARCH.sh -O Miniconda3-latest-Linux-$ARCH.sh
    bash Miniconda3-latest-Linux-$ARCH.sh -b -p $MINICONDA_PATH
  )
}

function install_duckdb {
  if [[ "$BUILD_DUCKDB" == "true" ]]; then
    echo 'Building DuckDB'
    wget_and_untar https://github.com/duckdb/duckdb/archive/refs/tags/${DUCKDB_VERSION}.tar.gz duckdb
    cmake_install_dir duckdb -DBUILD_UNITTESTS=OFF -DENABLE_SANITIZER=OFF -DENABLE_UBSAN=OFF -DBUILD_SHELL=OFF -DEXPORT_DLL_SYMBOLS=OFF -DCMAKE_BUILD_TYPE=Release
  fi
}

function install_stemmer {
  wget_and_untar https://snowballstem.org/dist/libstemmer_c-${STEMMER_VERSION}.tar.gz stemmer
  (
    cd ${DEPENDENCY_DIR}/stemmer
    sed -i '/CPPFLAGS=-Iinclude/ s/$/ -fPIC/' Makefile
    make clean && make "-j${NPROC}"
    ${SUDO} cp libstemmer.a ${INSTALL_PREFIX}/lib/
    ${SUDO} cp include/libstemmer.h ${INSTALL_PREFIX}/include/
  )
}

function install_thrift {
  wget_and_untar https://github.com/apache/thrift/archive/${THRIFT_VERSION}.tar.gz thrift

  EXTRA_CXXFLAGS="-O3 -fPIC"
  # Clang will generate warnings and they need to be suppressed, otherwise the build will fail.
  if [[ ${USE_CLANG} != "false" ]]; then
    EXTRA_CXXFLAGS="-O3 -fPIC -Wno-inconsistent-missing-override -Wno-unused-but-set-variable"
  fi

  CXX_FLAGS="$EXTRA_CXXFLAGS" cmake_install_dir thrift \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_COMPILER=ON \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TUTORIALS=OFF \
    -DCMAKE_DEBUG_POSTFIX= \
    -DWITH_AS3=OFF \
    -DWITH_CPP=ON \
    -DWITH_C_GLIB=OFF \
    -DWITH_JAVA=OFF \
    -DWITH_JAVASCRIPT=OFF \
    -DWITH_LIBEVENT=OFF \
    -DWITH_NODEJS=OFF \
    -DWITH_PYTHON=OFF \
    -DWITH_QT5=OFF \
    -DWITH_ZLIB=OFF
}

function install_arrow {
  wget_and_untar https://github.com/apache/arrow/archive/apache-arrow-${ARROW_VERSION}.tar.gz arrow
  cmake_install_dir arrow/cpp \
    -DARROW_PARQUET=OFF \
    -DARROW_WITH_THRIFT=ON \
    -DARROW_WITH_LZ4=ON \
    -DARROW_WITH_SNAPPY=ON \
    -DARROW_WITH_ZLIB=ON \
    -DARROW_WITH_ZSTD=ON \
    -DARROW_JEMALLOC=OFF \
    -DARROW_SIMD_LEVEL=NONE \
    -DARROW_RUNTIME_SIMD_LEVEL=NONE \
    -DARROW_WITH_UTF8PROC=OFF \
    -DARROW_TESTING=ON \
    -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} \
    -DCMAKE_BUILD_TYPE=Release \
    -DARROW_BUILD_STATIC=ON \
    -DBOOST_ROOT=${INSTALL_PREFIX}
}

function install_cuda {
  # See https://developer.nvidia.com/cuda-downloads
  if ! dpkg -l cuda-keyring 1>/dev/null; then
    wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
    $SUDO dpkg -i cuda-keyring_1.1-1_all.deb
    rm cuda-keyring_1.1-1_all.deb
    $SUDO apt update
  fi
  local dashed="$(echo $1 | tr '.' '-')"
  $SUDO apt install -y \
    cuda-compat-$dashed \
    cuda-driver-dev-$dashed \
    cuda-minimal-build-$dashed \
    cuda-nvrtc-dev-$dashed
}

function install_geos {
  if [[ "$BUILD_GEOS" == "true" ]]; then
    wget_and_untar https://github.com/libgeos/geos/archive/${GEOS_VERSION}.tar.gz geos
    cmake_install_dir geos -DBUILD_TESTING=OFF
  fi
}

function install_velox_deps {
  run_and_time install_velox_deps_from_apt
  run_and_time install_fmt
  run_and_time install_protobuf
  run_and_time install_boost
  run_and_time install_folly
  run_and_time install_fizz
  run_and_time install_wangle
  run_and_time install_mvfst
  run_and_time install_fbthrift
  run_and_time install_conda
  run_and_time install_duckdb
  run_and_time install_stemmer
  run_and_time install_thrift
  run_and_time install_arrow
  run_and_time install_geos
}

function install_apt_deps {
  install_build_prerequisites
  install_format_prerequisites
  install_velox_deps_from_apt
}

(return 2> /dev/null) && return # If script was sourced, don't run commands.

(
  if [[ ${USE_CLANG} != "false" ]]; then
    export CC=/usr/bin/clang-15
    export CXX=/usr/bin/clang++-15
  fi
  if [[ $# -ne 0 ]]; then
    for cmd in "$@"; do
      run_and_time "${cmd}"
    done
    echo "All specified dependencies installed!"
  else
    if [ "${INSTALL_PREREQUISITES:-Y}" == "Y" ]; then
      echo "Installing build dependencies"
      run_and_time install_build_prerequisites
    else
      echo "Skipping installation of build dependencies since INSTALL_PREREQUISITES is not set"
    fi
    install_velox_deps
    echo "All dependencies for Velox installed!"
    if [[ ${USE_CLANG} != "false" ]]; then
      echo "To use clang for the Velox build set the CC and CXX environment variables in your session."
      echo "  export CC=/usr/bin/clang-15"
      echo "  export CXX=/usr/bin/clang++-15"
    fi
    if [[ ${VERSION} =~ "20.04" && ${USE_CLANG} == "false" ]]; then
      echo "To build Velox gcc-11/g++11 is required. Set the CC and CXX environment variables in your session."
      echo "  export CC=/usr/bin/gcc-11"
      echo "  export CXX=/usr/bin/g++-11"
    fi
  fi
)
