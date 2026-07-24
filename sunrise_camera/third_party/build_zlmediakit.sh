#!/bin/sh
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

echo "build script path: ${SCRIPT_DIR}"

#获取代码
git clone https://gitee.com/xia-chu/ZLMediaKit
cd ${SCRIPT_DIR}/ZLMediaKit
git submodule update --init
git checkout 479a3fb9bbc4e18d935032769e042c17300bf1bf

#给ZLMediaKit 打patch
git am ${SCRIPT_DIR}/0001-Compatible-with-X5.patch
git am ${SCRIPT_DIR}/0002-fix-build-for-cross-compile.patch
git am ${SCRIPT_DIR}/0003-fix-mjpeg-dqt-fmp4-timestamp-rtcp-sr.patch

#给ZLToolKit子仓库打Patch(修复交叉编译wepoll.c报windows.h缺失，子模块自身的NOT WIN32排除逻辑同样被误判)
cd ${SCRIPT_DIR}/ZLMediaKit/3rdpart/ZLToolKit
git am ${SCRIPT_DIR}/0001-fix-build-for-ZLToolKit.patch

#给media-server子仓库打Patch
cd ${SCRIPT_DIR}/ZLMediaKit/3rdpart/media-server
git am ${SCRIPT_DIR}/0001-Compatible-with-X5-for-media-server.patch

#编译并安装
cd ${SCRIPT_DIR}/ZLMediaKit
./build_x5.sh