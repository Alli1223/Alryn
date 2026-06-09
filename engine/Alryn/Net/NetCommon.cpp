#include <Alryn/Net/NetCommon.h>

#include <enet/enet.h>

namespace alryn::net {

namespace {
int g_refcount = 0;
}

bool initialize_enet() {
    if (g_refcount > 0) {
        ++g_refcount;
        return true;
    }
    if (enet_initialize() != 0) {
        return false;
    }
    ++g_refcount;
    return true;
}

void shutdown_enet() {
    if (g_refcount > 0 && --g_refcount == 0) {
        enet_deinitialize();
    }
}

} // namespace alryn::net
