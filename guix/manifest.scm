;; Use these packages from the channels in `channels.scm`
;; to prepare an environment in which to build Ouinet.
(specifications->manifest
 '(
    "bash"  ; build scripts
    "cmake@3.15.5"
    "coreutils"  ; build scripts
    "gawk"  ; build scripts
    "gcc-toolchain"  ; cmake
    "git"  ; cmake
    "grep"  ; build scripts
    "glibc-utf8-locales"  ; for Go-based submodules, need to set GUIX_LOCPATH=$GUIX_ENVIRONMENT/lib/locale LANG=en_US.UTF-8
    "make"  ; cmake
    "nss-certs"  ; cmake, need to set SSL_CERT_FILE=
    "openssl"
    "patch"  ; cmake
    "patchelf"  ; fix Go binary manually
    "sed"  ; git submodule hooks, <https://debbugs.gnu.org/cgi/bugreport.cgi?bug=37047>
    "zlib"
    ))
