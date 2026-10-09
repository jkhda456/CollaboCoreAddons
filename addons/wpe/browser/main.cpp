/*
 * The wpe add-on's program: every process of the browser in one static WebAssembly program (the
 * guest cannot load libraries), picked by the name it is run as:
 *
 *   WPEWebProcess, WPENetworkProcess   WebKit's own processes (links in /usr/libexec/wpe-webkit-2.0,
 *                                      where WebKit starts them from)
 *   anything else                      the browser, the UI process (browser.c)
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace WebKit {
int WebProcessMain(int argc, char** argv);
int NetworkProcessMain(int argc, char** argv);
}

extern "C" int wpe_browser_main(int argc, char** argv);
extern "C" void wpe_register_tls_backend(void);

// abort() (libc++ with exceptions off, failed assertions of the libraries) would end the process
// with no word of where; a trap there instead has the engine report the program's frames
// ("a guest program trapped": the abort's caller among them).
static void trapOnAbort(int)
{
    __builtin_trap();
}

int main(int argc, char** argv)
{
    std::signal(SIGABRT, trapOnAbort);
    const char* name = std::strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    // WebKit's processes end when the browser lets them go (their run loop stops): at once, without
    // the static destructors and atexit handlers of everything linked in, some of which wait for
    // threads that never come back here
    if (!std::strcmp(name, "WPEWebProcess")) {
        int status = WebKit::WebProcessMain(argc, argv);
        if (getenv("WPE_BROWSER_DEBUG"))
            fprintf(stderr, "WPEWebProcess %d: done (%d)\n", getpid(), status);
        _exit(status);
    }
    // TLS for GIO is a module glib-networking would load; here it is linked in
    wpe_register_tls_backend();
    if (!std::strcmp(name, "WPENetworkProcess"))
        _exit(WebKit::NetworkProcessMain(argc, argv));
    return wpe_browser_main(argc, argv);
}
