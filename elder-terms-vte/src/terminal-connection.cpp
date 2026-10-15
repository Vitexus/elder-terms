#include "terminal-connection.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <sys/wait.h>
#include <unistd.h>
// glibc 2.39's public pidfd header does not provide C++ linkage guards.
extern "C" {
#include <sys/pidfd.h>
}

namespace elder_terms {

struct ByteQueue {
  std::vector<unsigned char> storage;
  std::size_t head = 0;
  std::size_t length = 0;
};

static std::span<const unsigned char> front(const ByteQueue &queue) {
  return {queue.storage.data() + queue.head,
      std::min(queue.length, queue.storage.size() - queue.head)};
}

static void append(ByteQueue &queue, std::span<const unsigned char> bytes) {
  const auto tail = (queue.head + queue.length) % queue.storage.size();
  const auto first = std::min(bytes.size(), queue.storage.size() - tail);
  std::memcpy(queue.storage.data() + tail, bytes.data(), first);
  std::memcpy(queue.storage.data(), bytes.data() + first, bytes.size() - first);
  queue.length += bytes.size();
}

static void consume(ByteQueue &queue, std::size_t length) {
  queue.head = (queue.head + length) % queue.storage.size();
  queue.length -= length;
}

struct ConnectionOperation {
  boxrig_posix_pty *pty = nullptr;
  boxrig_session *session = nullptr;
  ByteQueue incoming;
  ByteQueue outgoing;
  std::size_t chunk_size = 0;
  bool eof = false;
  bool write_closed = false;
  bool peer_closed = false;
  bool finishing = false;
  cardio::cancellation_source stop;
  cardio::cancellation_source io_stop;
  cardio::primitives::conditional receive_space;
  cardio::primitives::conditional send_ready;
  cardio::primitives::conditional drained;
};

struct TerminalConnection {
  boxrig_terminal *terminal = nullptr;
  std::function<void()> wake;
  std::function<void(std::span<const unsigned char>, std::span<const unsigned char>)> output;
  TerminalConnectionState state;
  std::unique_ptr<ConnectionOperation> operation;
  std::optional<cardio::promise<void>> completion;
};

static void discard_outgoing(boxrig_terminal *terminal) {
  const uint8_t *bytes = nullptr;
  while (const auto count = boxrig_terminal_output_peek(terminal, &bytes))
    boxrig_terminal_output_consume(terminal, count);
}

static boxrig_transport_status receive(
    void *context, uint8_t *bytes, size_t capacity, size_t *length) {
  auto *operation = static_cast<ConnectionOperation *>(context);
  const auto available = front(operation->incoming);
  *length = std::min(capacity, available.size());
  if (*length == 0) return operation->eof ? BOXRIG_TRANSPORT_CLOSED : BOXRIG_TRANSPORT_WOULD_BLOCK;
  std::memcpy(bytes, available.data(), *length);
  consume(operation->incoming, *length);
  operation->receive_space.trigger();
  return BOXRIG_TRANSPORT_OK;
}

static boxrig_transport_status send(
    void *context, const uint8_t *bytes, size_t length, size_t *written) {
  auto *connection = static_cast<TerminalConnection *>(context);
  auto &operation = *connection->operation;
  *written = 0;
  if (operation.finishing) {
    // The old channel is gone. Drain core replies without forwarding them to
    // any later connection, so retained receive data can still be parsed.
    *written = length;
    return BOXRIG_TRANSPORT_OK;
  }
  // A failed write must not close the session's receive side: a PTY can still
  // contain its final output after the child has gone away.
  if (operation.write_closed) return BOXRIG_TRANSPORT_WOULD_BLOCK;
  *written = std::min(length, operation.outgoing.storage.size() - operation.outgoing.length);
  if (*written == 0) return BOXRIG_TRANSPORT_WOULD_BLOCK;
  append(operation.outgoing, {bytes, *written});
  connection->state.send_peak = std::max(connection->state.send_peak, operation.outgoing.length);
  operation.send_ready.trigger();
  return BOXRIG_TRANSPORT_OK;
}

static boxrig_transport_status receive_connection(
    void *context, uint8_t *bytes, size_t capacity, size_t *length) {
  return receive(static_cast<TerminalConnection *>(context)->operation.get(), bytes, capacity, length);
}

static boxrig_transport_status resize_transport(void *context, size_t rows, size_t columns) {
  auto *connection = static_cast<TerminalConnection *>(context);
  if (boxrig_posix_pty_resize(connection->operation->pty, rows, columns) == 0) return BOXRIG_TRANSPORT_OK;
  connection->state.operation_error = errno;
  return BOXRIG_TRANSPORT_ERROR;
}

static void fail(TerminalConnection *connection, int error) {
  if (connection->state.operation_error == 0) connection->state.operation_error = error;
  connection->state.phase = TerminalConnectionPhase::stopping;
  connection->operation->stop.cancel();
  connection->operation->io_stop.cancel();
  connection->wake();
}

static cardio::promise<void> read_local(TerminalConnection *connection) {
  auto &operation = *connection->operation;
  const auto cancellation = operation.io_stop.get_cancellation();
  std::vector<unsigned char> buffer(operation.chunk_size);
  const auto fd = boxrig_posix_pty_descriptor(operation.pty);
  try {
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      while (operation.incoming.length == operation.incoming.storage.size())
        co_await operation.receive_space.wait(cancellation);
      co_await cardio::from_fd(fd, cardio::fd_event::read, cancellation);
      const auto capacity = std::min(buffer.size(), operation.incoming.storage.size() - operation.incoming.length);
      const auto count = ::read(fd, buffer.data(), capacity);
      if (count > 0) {
        const std::span<const unsigned char> bytes(buffer.data(), static_cast<std::size_t>(count));
        // Local UTF-8 is passed through once. Session parser retries operate on
        // the retained queue, never on this notification or its source bytes.
        if (connection->output) connection->output(bytes, bytes);
        append(operation.incoming, bytes);
        connection->state.receive_peak = std::max(connection->state.receive_peak, operation.incoming.length);
        connection->wake();
      } else if (count == 0 || errno == EIO) {
        operation.eof = true;
        connection->wake();
        break;
      } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        throw std::system_error(errno, std::generic_category(), "read PTY");
      }
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::system_error &error) {
    fail(connection, error.code().value());
  } catch (...) {
    fail(connection, ENOMEM);
  }
}

static cardio::promise<void> write_local(TerminalConnection *connection) {
  auto &operation = *connection->operation;
  const auto cancellation = operation.io_stop.get_cancellation();
  const auto fd = boxrig_posix_pty_descriptor(operation.pty);
  try {
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      while (operation.outgoing.length == 0) co_await operation.send_ready.wait(cancellation);
      co_await cardio::from_fd(fd, cardio::fd_event::write, cancellation);
      const auto bytes = front(operation.outgoing).first(std::min(front(operation.outgoing).size(), operation.chunk_size));
      const auto count = ::write(fd, bytes.data(), bytes.size());
      if (count > 0) {
        consume(operation.outgoing, static_cast<std::size_t>(count));
        connection->wake();
      } else if (count < 0 && (errno == EIO || errno == EPIPE)) {
        operation.write_closed = true;
        connection->wake();
        break;
      } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        throw std::system_error(errno, std::generic_category(), "write PTY");
      }
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::system_error &error) {
    fail(connection, error.code().value());
  } catch (...) {
    fail(connection, ENOMEM);
  }
}

struct SpawnResult {
  boxrig_posix_pty *pty = nullptr;
  boxrig_posix_pty_spawn_result diagnostic{};
};

static void terminate_child(pid_t child) {
  // The sole reaper has not run, so neither the PID nor its group can be
  // recycled. The direct signal also covers cancellation before setsid().
  (void)::kill(-child, SIGKILL);
  (void)::kill(child, SIGKILL);
}

static bool finish_input(TerminalConnection *connection) {
  discard_outgoing(connection->terminal);
  const auto status = boxrig_terminal_finish_input(connection->terminal);
  if (status == BOXRIG_STATUS_WOULD_BLOCK) return false;
  if (status != BOXRIG_STATUS_OK) connection->state.operation_error = EIO;
  connection->operation->finishing = false;
  connection->operation->drained.trigger();
  return true;
}

static cardio::promise<void> run_local(TerminalConnection *connection, TerminalLocalCommand command) {
  auto &operation = *connection->operation;
  const auto cancellation = operation.stop.get_cancellation();
  int process = -1;
  std::optional<cardio::promise<void>> reader;
  std::optional<cardio::promise<void>> writer;
  try {
    size_t rows = 0, columns = 0;
    boxrig_terminal_size(connection->terminal, &rows, &columns);
    const auto spawned = co_await cardio::promises::start_new([command = std::move(command), rows, columns]() mutable {
      std::vector<char *> arguments;
      for (auto &argument : command.arguments) arguments.push_back(argument.data());
      arguments.push_back(nullptr);
      SpawnResult result;
      boxrig_posix_pty_spawn_with_diagnostic(arguments.data(), command.terminal_type.c_str(), rows, columns,
          &result.pty, &result.diagnostic);
      return result;
    });
    operation.pty = spawned.pty;
    connection->state.spawn = spawned.diagnostic;
    if (operation.pty) {
      connection->state.child = boxrig_posix_pty_child(operation.pty);
      process = pidfd_open(connection->state.child, 0);
      if (process < 0) throw std::system_error(errno, std::generic_category(), "observe PTY child");
      cancellation.throw_if_cancellation_requested();
      for (;;) {
        if (boxrig_posix_pty_get_spawn_result(operation.pty, &connection->state.spawn) != 0)
          throw std::system_error(errno, std::generic_category(), "PTY spawn diagnostic");
        if (connection->state.spawn.state != BOXRIG_POSIX_PTY_SPAWN_PENDING) break;
        co_await cardio::from_fd(boxrig_posix_pty_spawn_descriptor(operation.pty), cardio::fd_event::read, cancellation);
      }
      // GTK may have completed a layout while synchronous preparation ran.
      // Start this generation at that confirmed grid before exposing input.
      boxrig_terminal_size(connection->terminal, &rows, &columns);
      if (boxrig_posix_pty_resize(operation.pty, rows, columns) != 0)
        throw std::system_error(errno, std::generic_category(), "initial PTY grid");
      const boxrig_session_transport transport{connection, receive_connection, send, resize_transport, nullptr};
      auto config = boxrig_session_config_default();
      config.input_capacity = operation.incoming.storage.size();
      config.maximum_read_bytes_per_dispatch = operation.incoming.storage.size();
      if (boxrig_session_create(connection->terminal, &transport, &config, &operation.session) != BOXRIG_SESSION_OK)
        throw std::bad_alloc();
      reader.emplace(read_local(connection));
      writer.emplace(write_local(connection));
      if (connection->state.spawn.state == BOXRIG_POSIX_PTY_SPAWN_SUCCEEDED) {
        connection->state.phase = TerminalConnectionPhase::connected;
        connection->wake();
      }
      co_await cardio::from_fd(process, cardio::fd_event::read, cancellation);
      // The child has exited. Keep reading until EOF and let the session drain
      // its parser tail before reporting disconnection.
      co_await *reader;
      while (!operation.peer_closed) co_await operation.drained.wait(cancellation);
    }
  } catch (const cardio::canceled_exception &) {
  } catch (const std::system_error &error) {
    connection->state.operation_error = error.code().value();
  } catch (...) {
    connection->state.operation_error = ENOMEM;
  }
  operation.io_stop.cancel();
  if (operation.pty) {
    if (connection->state.cancelled || connection->state.operation_error != 0)
      terminate_child(connection->state.child);
    if (reader) co_await *reader;
    if (writer) co_await *writer;
    // One owner, one wait path, including failure to allocate a pidfd. Even
    // that exceptional blocking wait cannot hold the GTK dispatcher.
    const auto reaped = co_await cardio::promises::start_new([pty = operation.pty] {
      int status = 0;
      const int error = boxrig_posix_pty_wait(pty, &status) == 0 ? 0 : errno;
      return std::pair{status, error};
    });
    connection->state.reaped = reaped.second == 0;
    if (reaped.second != 0) connection->state.operation_error = reaped.second;
    else if (WIFEXITED(reaped.first)) connection->state.exit_code = WEXITSTATUS(reaped.first);
    else if (WIFSIGNALED(reaped.first)) connection->state.signal_number = WTERMSIG(reaped.first);
    boxrig_posix_pty_get_spawn_result(operation.pty, &connection->state.spawn);
  }
  if (process >= 0) ::close(process);
  // Cancellation also ends a stream. Its incomplete UTF-8/control syntax
  // cannot become the prefix of the next connection's input.
  operation.finishing = true;
  operation.eof = true;
  if (operation.session) {
    connection->wake();
    while (!operation.peer_closed) co_await operation.drained.wait();
  }
  while (!finish_input(connection)) {
    connection->wake();
    co_await operation.drained.wait();
  }
  boxrig_session_destroy(operation.session);
  operation.session = nullptr;
  boxrig_posix_pty_destroy(operation.pty);
  operation.pty = nullptr;
  connection->state.phase = TerminalConnectionPhase::closed;
  connection->wake();
}

TerminalConnection *create_terminal_connection(
    boxrig_terminal *terminal, std::function<void()> wake,
    std::function<void(std::span<const unsigned char>, std::span<const unsigned char>)> output) {
  auto connection = std::make_unique<TerminalConnection>();
  connection->terminal = terminal;
  connection->wake = std::move(wake);
  connection->output = std::move(output);
  return connection.release();
}

void destroy_terminal_connection(TerminalConnection *connection) {
  delete connection;
}

bool terminal_connection_start_local(TerminalConnection *connection, TerminalLocalCommand command) {
  if ((connection->state.phase != TerminalConnectionPhase::idle &&
       connection->state.phase != TerminalConnectionPhase::closed) ||
      connection->state.generation == std::numeric_limits<std::uint64_t>::max() ||
      command.queue_capacity == 0 || command.io_chunk_size == 0 || command.queue_capacity > 1024 * 1024) return false;
  const auto generation = connection->state.generation + 1;
  connection->completion.reset();
  connection->operation = std::make_unique<ConnectionOperation>();
  connection->operation->incoming.storage.resize(command.queue_capacity);
  connection->operation->outgoing.storage.resize(command.queue_capacity);
  connection->operation->chunk_size = std::min(command.io_chunk_size, command.queue_capacity);
  connection->state = {};
  connection->state.generation = generation;
  connection->state.phase = TerminalConnectionPhase::starting;
  discard_outgoing(connection->terminal);
  connection->completion.emplace(run_local(connection, std::move(command)));
  connection->wake();
  return true;
}

void terminal_connection_stop(TerminalConnection *connection) {
  if (connection->state.phase == TerminalConnectionPhase::idle ||
      connection->state.phase == TerminalConnectionPhase::closed) return;
  connection->state.cancelled = true;
  connection->state.phase = TerminalConnectionPhase::stopping;
  connection->operation->stop.cancel();
  connection->operation->io_stop.cancel();
  connection->wake();
}

cardio::promise<void> terminal_connection_wait_stopped(TerminalConnection *connection) {
  if (connection->completion) co_await *connection->completion;
}

bool terminal_connection_dispatch(TerminalConnection *connection) {
  if (connection->operation && connection->operation->finishing &&
      (!connection->operation->session || connection->operation->peer_closed)) {
    finish_input(connection);
    return false;
  }
  if (!connection->operation || !connection->operation->session ||
      (connection->state.phase == TerminalConnectionPhase::stopping &&
       !connection->operation->finishing)) return false;
  auto &operation = *connection->operation;
  uint32_t ready = BOXRIG_SESSION_READY_NONE;
  if (operation.incoming.length || operation.eof) ready |= BOXRIG_SESSION_READY_READ;
  if (operation.finishing || operation.outgoing.length < operation.outgoing.storage.size() || operation.write_closed)
    ready |= BOXRIG_SESSION_READY_WRITE;
  boxrig_session_dispatch_result result{};
  if (boxrig_session_dispatch(operation.session, ready, &result) != BOXRIG_SESSION_OK) {
    fail(connection, EIO);
    if (operation.finishing) {
      operation.peer_closed = true;
      operation.drained.trigger();
    }
    return false;
  }
  if (result.peer_closed) {
    operation.peer_closed = true;
    operation.drained.trigger();
  }
  const auto interests = boxrig_session_interests(operation.session);
  return result.terminal_bytes_fed != 0 || result.transport_bytes_written != 0 ||
      ((interests & BOXRIG_SESSION_INTEREST_READ) && operation.incoming.length != 0);
}

bool terminal_connection_resize(TerminalConnection *connection, std::size_t rows, std::size_t columns) {
  if (!connection->operation || !connection->operation->session || connection->operation->finishing)
    return boxrig_terminal_resize(connection->terminal, rows, columns) == BOXRIG_STATUS_OK;
  const bool resized = boxrig_session_resize(connection->operation->session, rows, columns) == BOXRIG_SESSION_OK;
  if (!resized) fail(connection, EIO);
  return resized;
}

TerminalConnectionState terminal_connection_state(const TerminalConnection *connection) { return connection->state; }

}
