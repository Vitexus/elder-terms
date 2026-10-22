// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-transfer.h"

namespace elder_terms {

const char *terminal_transfer_protocol_token(
    TerminalTransferProtocol protocol) {
  if (protocol == TerminalTransferProtocol::xmodem) {
    return "xmodem";
  }
  if (protocol == TerminalTransferProtocol::ymodem) {
    return "ymodem";
  }
  return "zmodem";
}

const char *terminal_transfer_protocol_label(
    TerminalTransferProtocol protocol) {
  if (protocol == TerminalTransferProtocol::xmodem) {
    return "XMODEM";
  }
  if (protocol == TerminalTransferProtocol::ymodem) {
    return "YMODEM";
  }
  return "ZMODEM";
}

} // namespace elder_terms
