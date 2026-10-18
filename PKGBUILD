# Maintainer: RazorCoding <https://github.com/RazorCoding>
pkgname=lsl
pkgver=r18.79a70bb

pkgrel=1
pkgdesc="Lossless Scaling-style Linux Vulkan compute upscaler + GTK4 GUI (per-game profiles, implicit layer)"
arch=('x86_64')
url="https://github.com/RazorCoding/lsl"
license=('MIT')
depends=('vulkan-icd-loader' 'gtkmm-4.0' 'python')
makedepends=('cmake' 'ninja' 'vulkan-headers' 'pkgconf')
optdepends=('vulkan-tools: vkcube test driver'
            'steam: game launching via steam -applaunch')
source=("git+https://github.com/RazorCoding/lsl.git")
sha256sums=('SKIP')

build() {
    cd "$srcdir/lsl"
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
}

check() {
    cd "$srcdir/lsl"
    ctest --test-dir build --output-on-failure || true
}

package() {
    cd "$srcdir/lsl"
    DESTDIR="$pkgdir" cmake --install build

    # Implicit manifest: copy the (already-correct, runtime-pathed) manifest
    # the cmake install wrote into the loader's implicit directory.  Do this by
    # hand rather than via install-layer.sh, so DESTDIR staging keeps the
    # runtime /usr library_path (install-layer.sh would rewrite it to $pkgdir).
    install -d "$pkgdir/usr/share/vulkan/implicit_layer.d"
    install -m644 "$pkgdir/usr/lib/lsl/layer/VkLayer_lsl_frame.json" \
        "$pkgdir/usr/share/vulkan/implicit_layer.d/VkLayer_lsl_frame.json"

    install -Dm755 build/lsl-gui     "$pkgdir/usr/bin/lsl-gui"
    install -Dm755 build/lsl-upscale  "$pkgdir/usr/bin/lsl-upscale"

    install -d "$pkgdir/usr/share/applications"
    printf '%s\n' \
        '[Desktop Entry]' \
        'Name=lsl' \
        'Comment=Lossless Scaling-style Vulkan upscaler' \
        'Exec=lsl-gui' \
        'Type=Application' \
        'Categories=Game;Graphics;' \
        'Terminal=false' \
        > "$pkgdir/usr/share/applications/lsl.desktop"
    chmod 644 "$pkgdir/usr/share/applications/lsl.desktop"

    if [[ -f LICENSE ]]; then
        install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    fi
}