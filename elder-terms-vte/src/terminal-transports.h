#pragma once

#include "terminal-transport.h"
#include "terminal-sessions/ssh-session/authenticated-ssh-transport.h"

namespace elder_terms {

/**
 * Creates a TELNET channel factory with protocol negotiation and NAWS.
 * @param settings Destination and reported terminal type.
 * @param encoding Peer character encoding.
 * @param callbacks Connection and SSH proxy authentication callbacks.
 * @param options SSH proxy and known-host overrides.
 * @return Reusable factory; each open owns a new parser and connection route.
 */
TerminalTransportRequest make_telnet_transport_request(TelnetConnectionSettings settings,
    std::string encoding, TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options);

}
