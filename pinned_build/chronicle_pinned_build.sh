#!/usr/bin/env bash

echo "Chronicle Pinned Build"

if [[ "$(uname)" == "Linux" ]]; then
   if [[ -e /etc/os-release ]]; then
      # obtain NAME and other information
      . /etc/os-release
      if [[ ${NAME} != "Ubuntu" ]]; then
         echo "Currently only supporting Ubuntu based builds. Proceed at your own risk."
      fi
   else
       echo "Currently only supporting Ubuntu based builds. /etc/os-release not found. Your Linux distribution is not supported. Proceed at your own risk."
   fi
else
    echo "Currently only supporting Ubuntu based builds. Your architecture is not supported. Proceed at your own risk."
fi

if [ $# -eq 0 ] || [ -z "$1" ]
   then
      echo "Usage:"
      echo "./pinned_build/chronicle_pinned_build.sh DEPS_DIR BUILD_DIR JOBS"
      echo "  DEPS_DIR: directory where to place build dependencies (same deps as for Mandel 3.1)"
      echo "  BUILD_DIR: build directory"
      echo "  JOBS: number of parallel processes"      
      exit -1
fi

DEP_DIR=$1
BUILD_DIR=$2
JOBS=$3
CLANG_VER=11.0.1
BOOST_VER=1.80.0
LLVM_VER=7.1.0
ARCH=`uname -m`
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]:-$0}"; )" &> /dev/null && pwd 2> /dev/null; )";
START_DIR="$(pwd)"


pushdir() {
   DIR=$1
   mkdir -p ${DIR}
   pushd ${DIR} &> /dev/null
}

popdir() {
   EXPECTED=$1
   D=`popd`
   popd &> /dev/null
   echo ${D}
   D=`eval echo $D | head -n1 | cut -d " " -f1`

   if [[ ${D} != ${EXPECTED} ]]; then
     echo "Directory is not where expected EXPECTED=${EXPECTED} at ${D}"
     exit 1 
   fi
}

try(){
   output=$($@)
   res=$?
   if [[ ${res} -ne 0 ]]; then
      exit -1
   fi
}

install_clang() {
   CLANG_DIR=$1
   if [ ! -d "${CLANG_DIR}" ]; then
      echo "Installing Clang ${CLANG_VER} @ ${CLANG_DIR}"
      mkdir -p ${CLANG_DIR}
      if [ ${ARCH} = x86_64 ]; then
         CLANG_FN=clang+llvm-${CLANG_VER}-x86_64-linux-gnu-ubuntu-16.04.tar.xz
      else 
	 if [ ${ARCH} = aarch64 ]; then
            CLANG_FN=clang+llvm-${CLANG_VER}-aarch64-linux-gnu.tar.xz
	 else
            echo "Unknown ARCH: $ARCH"
            exit 1
	 fi
      fi
      try wget https://github.com/llvm/llvm-project/releases/download/llvmorg-${CLANG_VER}/${CLANG_FN}
      try tar -xvf ${CLANG_FN} -C ${CLANG_DIR}
      pushdir ${CLANG_DIR}
      mv clang+*/* .
      popdir ${DEP_DIR}
      rm ${CLANG_FN}
   fi
   export PATH=${CLANG_DIR}/bin:$PATH
   export CLANG_DIR=${CLANG_DIR}
}

install_llvm() {
   LLVM_DIR=$1
   if [ ! -d "${LLVM_DIR}" ]; then
      echo "Installing LLVM ${LLVM_VER} @ ${LLVM_DIR}"
      mkdir -p ${LLVM_DIR}
      try wget https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VER}/llvm-${LLVM_VER}.src.tar.xz
      try tar -xvf llvm-${LLVM_VER}.src.tar.xz
      pushdir "${LLVM_DIR}.src"
      pushdir build
      try cmake -DCMAKE_TOOLCHAIN_FILE=${SCRIPT_DIR}/chronicle_pinned_toolchain.cmake -DCMAKE_INSTALL_PREFIX=${LLVM_DIR} -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=host -DLLVM_BUILD_TOOLS=Off -DLLVM_ENABLE_RTTI=On -DLLVM_ENABLE_TERMINFO=Off -DCMAKE_EXE_LINKER_FLAGS=-pthread -DCMAKE_SHARED_LINKER_FLAGS=-pthread -DLLVM_ENABLE_PIC=NO ..
      try make -j${JOBS} 
      try make -j${JOBS} install
      popdir "${LLVM_DIR}.src"
      popdir ${DEP_DIR}
      rm -rf ${LLVM_DIR}.src 
      rm llvm-${LLVM_VER}.src.tar.xz
   fi
   export LLVM_DIR=${LLVM_DIR}
}

install_boost() {
   BOOST_DIR=$1

   if [ ! -d "${BOOST_DIR}" ]; then
      echo "Installing Boost ${BOOST_VER} @ ${BOOST_DIR}"
      try wget https://boostorg.jfrog.io/artifactory/main/release/${BOOST_VER}/source/boost_${BOOST_VER//\./_}.tar.gz
      try tar -xvzf boost_${BOOST_VER//\./_}.tar.gz -C ${DEP_DIR}
      pushdir ${BOOST_DIR}
      try ./bootstrap.sh -with-toolset=clang --prefix=${BOOST_DIR}/bin
      ./b2 toolset=clang cxxflags='-stdlib=libc++ -D__STRICT_ANSI__ -nostdinc++ -I${CLANG_DIR}/include/c++/v1 -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE' linkflags='-stdlib=libc++ -pie' link=static threading=multi --with-iostreams --with-date_time --with-filesystem --with-system --with-program_options --with-chrono --with-test --with-thread -q -j${JOBS} install
      popdir ${DEP_DIR}
      rm boost_${BOOST_VER//\./_}.tar.gz
   fi
   export BOOST_DIR=${BOOST_DIR}
}

pushdir ${DEP_DIR}

install_clang ${DEP_DIR}/clang-${CLANG_VER}
install_llvm ${DEP_DIR}/llvm-${LLVM_VER}
install_boost ${DEP_DIR}/boost_${BOOST_VER//\./_}

# go back to the directory where the script starts
popdir ${START_DIR}

pushdir ${BUILD_DIR}

# build Chronicle
echo "Building Chronicle"
try cmake -DCMAKE_TOOLCHAIN_FILE=${SCRIPT_DIR}/chronicle_pinned_toolchain.cmake -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=${LLVM_DIR}/lib/cmake -DCMAKE_PREFIX_PATH=${BOOST_DIR}/bin ${MORE_CMAKE_FLAGS} ${SCRIPT_DIR}/..

try make -j${JOBS}
try cpack

echo "Chronicle has successfully built and constructed its packages.  You should be able to find the packages at ${BUILD_DIR}."
