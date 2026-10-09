/* glib-networking's OpenSSL backend, linked into the program: GIO would load it as a module
 * (libgioopenssl.so) from its module directory, which a static program cannot. A type module whose
 * "load" registers the backend's types plays that module. */
#include <gio/gio.h>

void g_io_openssl_load(GIOModule *module);

typedef GTypeModule WpeStaticModule;
typedef GTypeModuleClass WpeStaticModuleClass;
G_DEFINE_TYPE(WpeStaticModule, wpe_static_module, G_TYPE_TYPE_MODULE)

static gboolean wpe_static_module_load(GTypeModule *module)
{
    /* GTypeModule is all the backend's registration uses of a GIOModule */
    g_io_openssl_load((GIOModule *)module);
    return TRUE;
}

static void wpe_static_module_unload(GTypeModule *module)
{
    (void)module;
}

static void wpe_static_module_class_init(WpeStaticModuleClass *klass)
{
    klass->load = wpe_static_module_load;
    klass->unload = wpe_static_module_unload;
}

static void wpe_static_module_init(WpeStaticModule *module)
{
    (void)module;
}

void wpe_register_tls_backend(void)
{
    static gsize registered;
    if (g_once_init_enter(&registered)) {
        /* GIO registers its extension points when it first looks for a backend: the TLS one
         * must be there before the backend implements it (registering again is harmless) */
        GIOExtensionPoint *point = g_io_extension_point_register(G_TLS_BACKEND_EXTENSION_POINT_NAME);
        g_io_extension_point_set_required_type(point, G_TYPE_TLS_BACKEND);
        GTypeModule *module = g_object_new(wpe_static_module_get_type(), NULL);
        g_type_module_set_name(module, "gioopenssl (linked in)");
        g_type_module_use(module); /* never unused: the types stay */
        g_once_init_leave(&registered, 1);
    }
}
