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

/**
 * Creates an SSH shell factory using the shared authentication implementation.
 * @param settings Endpoint, login and requested terminal type.
 * @param encoding Peer character encoding.
 * @param callbacks Authentication prompts and connection notifications.
 * @param options SSH proxy, client configuration and known-host overrides.
 * @return Reusable factory with asynchronous shell I/O and resize completion.
 */
TerminalTransportRequest make_ssh_transport_request(SshConnectionSettings settings,
    std::string encoding, TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options);

/**
 * Creates a serial byte-stream factory using the saved device identity.
 * @param settings Device identity, character framing, baud rate and flow control.
 * @param encoding Peer character encoding.
 * @param callbacks Receive and send activity notifications.
 * @return Reusable factory; each open resolves and configures a fresh descriptor.
 * @remarks This factory opens one port. Device and carrier recovery policy is
 * separate from its bounded stream operations.
 */
TerminalTransportRequest make_serial_transport_request(SerialConnectionSettings settings,
    std::string encoding, TerminalSessionCallbacks callbacks);

}
