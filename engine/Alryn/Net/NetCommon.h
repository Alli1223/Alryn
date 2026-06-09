#pragma once

namespace alryn::net {

// Reference-counted global ENet init (enet_initialize/deinitialize).
bool initialize_enet();
void shutdown_enet();

} // namespace alryn::net
