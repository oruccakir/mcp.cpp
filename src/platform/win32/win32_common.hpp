#pragma once

// Shared bits for the win32 PAL backend. Only backend sources include this.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include "../pal.hpp"

namespace mcp::pal {

/// Idempotent WSAStartup (2.2); process-lifetime cleanup.
void ensure_wsa();

/// True when the descriptor is a SOCKET (vs a CRT fd for pipes/stdio).
bool is_socket(fd_t fd);

}  // namespace mcp::pal
