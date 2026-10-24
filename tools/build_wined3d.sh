#!/usr/bin/env bash
set -euo pipefail
project_root=$(cd "$(dirname "$0")/.." && pwd)
build_root="$project_root/build/wined3d"
wine_commit=455e3509b98a6919fd4ad1def4803e08c41c03b2
mkdir -p "$build_root"
cd "$build_root"
git init wine
git -C wine remote add origin https://github.com/wine-mirror/wine.git
git -C wine fetch --depth=1 origin "$wine_commit"
git -C wine checkout --detach FETCH_HEAD
test "$(git -C wine rev-parse HEAD)" = "$wine_commit"
git -C wine apply --check "$project_root/config/wined3d-logic.patch"
git -C wine apply "$project_root/config/wined3d-logic.patch"
mkdir native windows package
cd native
../wine/configure --without-x --enable-win64 --without-freetype
make -j"$(nproc)" __tooldeps__
cd ../windows
../wine/configure --without-x --enable-win64 --disable-kernel32 --disable-tests --without-freetype --host=x86_64-w64-mingw32 --with-wine-tools=../native CFLAGS="-O3 -DWINE_NOWINSOCK -DUSE_WIN32_OPENGL -DUSE_WIN32_VULKAN" LDFLAGS="-static-libgcc"
make -j"$(nproc)" dlls/wined3d/x86_64-windows/wined3d.dll dlls/d3d11/x86_64-windows/d3d11.dll dlls/dxgi/x86_64-windows/dxgi.dll
for dll in wined3d d3d11 dxgi; do
    cp "dlls/$dll/x86_64-windows/$dll.dll" ../package/
done
x86_64-w64-mingw32-strip ../package/*.dll
cd ../package
cp ../wine/COPYING.LIB .
cp "$project_root/config/wined3d-logic.patch" .
printf '%s\n' "$wine_commit" > wine-commit.txt
sha256sum *.dll wined3d-logic.patch > SHA256SUMS
zip -9 ../wined3d.zip *.dll COPYING.LIB wined3d-logic.patch wine-commit.txt SHA256SUMS
