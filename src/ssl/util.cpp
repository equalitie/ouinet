#include "util.h"

namespace ouinet::ssl::util {

#if defined(_WIN32)
static std::optional<bool> g_is_running_on_wine;

static bool is_running_on_wine() {
    if (g_is_running_on_wine) {
        return *g_is_running_on_wine;
    }

    using F = const char* (CDECL *)(void);

    HMODULE hntdll = GetModuleHandle("ntdll.dll");

    if(!hntdll) {
        g_is_running_on_wine = false;
    } else {
        F pwine_get_version = (F) GetProcAddress(hntdll, "wine_get_version");
        if(pwine_get_version) {
            g_is_running_on_wine = true;
        } else {
            g_is_running_on_wine = false;
        }
    }

    return *g_is_running_on_wine;
}
#endif

void set_default_verify_paths(asio::ssl::context& ctx) {
    ctx.set_default_verify_paths();

#ifdef _WIN32
    // The above does not load the certificates when running on Wine.
    if (is_running_on_wine()) {
        ctx.add_verify_path("/etc/ssl/certs");
    }
#endif
}

} // namespace
