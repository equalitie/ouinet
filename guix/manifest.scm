;; Use these packages from the channels in `channels.scm`
;; to prepare an environment in which to build Ouinet.
(specifications->manifest
 '(
    ;; This list is derived from `Dockerfile`.
    ;"autoconf"  ; gpg-error patching, <https://dev.gnupg.org/T4469>
    ;"automake"  ; gpg-error patching, <https://dev.gnupg.org/T4469>
    "bash"  ; build scripts
    ;"ccache"
    "cmake@3.15.5"  ; 3.15.2 may be sufficient
    "coreutils"  ; build scripts
    "gawk"  ; build scripts
    "gcc-toolchain"  ; cmake
    ;"gettext"  ; gpg-error
    "git"  ; cmake
    "grep"  ; build scripts
    ;"libtool"
    "glibc-utf8-locales"  ; for Go-based submodules, need to set GUIX_LOCPATH=$GUIX_ENVIRONMENT/lib/locale LANG=en_US.UTF-8
    "make"  ; cmake
    ;"ninja"
    "nss-certs"  ; cmake, need to set SSL_CERT_FILE=
    "openssl"
    "patch"  ; cmake
    "patchelf"  ; fix Go binary manually: patchelf --set-interpreter "$(patchelf --print-interpreter /bin/sh)" ouinet-local-build/golang/bin/go
    ;"pkg-config"
    ;"python-twisted"
    ;"rsync"
    "sed"  ; git submodule hooks, <https://debbugs.gnu.org/cgi/bugreport.cgi?bug=37047>
    ;"texinfo"
    ;"unzip"
    ;"wget"
    "zlib"
    ))
