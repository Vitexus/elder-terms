// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/connection-identity.h>
#include <elder-terms/settings.h>
#include <elder-terms/settings/general-settings.h>

#include <cardio.h>
#include <glib.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace elder_terms_connection_identity_test {

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void write_text(const std::filesystem::path &path,
                       const std::string &contents) {
  std::ofstream output(path);
  output << contents;
  expect(output.good(), "test profile should be written");
}

static std::string read_text(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

static void test_global_exclusion(const std::filesystem::path &directory) {
  auto store = elder_terms::create_default_settings(
      elder_terms::default_terminal_display_settings(12), "Connection");
  const auto key = elder_terms::general_connection_id_setting_key();
  const auto id = elder_terms::generate_connection_id();
  expect(g_uuid_string_is_valid(id.c_str()), "generated ID should be a UUID");
  expect(elder_terms::set_explicit_setting_value(&store, key, id),
         "connection ID should be accepted");
  expect(elder_terms::general_connection_id(store) == id,
         "connection ID getter should expose the saved ID");
  const auto global = directory / "global.ini";
  expect(elder_terms::save_global_settings(store, global).saved,
         "global defaults should save");
  expect(read_text(global).find("connection_id=") == std::string::npos,
         "global defaults should exclude connection IDs");
  write_text(global, "[general]\nconnection_id=" + id + "\n");
  const auto loaded = elder_terms::load_settings(
      {.config_path = directory / "absent.ini",
       .startup_config_path = std::nullopt, .global_config_path = global}, 12);
  expect(elder_terms::general_connection_id(loaded.store).empty(),
         "global ID should not be inherited by a connection");
}

static cardio::promise<void> test_ensure_async(
    const std::filesystem::path &directory,
    cardio::dispatcher_group_glib *group, std::exception_ptr *failure) {
  try {
    const auto legacy = directory / "legacy.ini";
    write_text(legacy, "# Keep this comment\n[general]\nname=Legacy\n"
                       "[terminal]\nwidth=91\n");
    auto first = elder_terms::ensure_connection_id_async(legacy, {});
    auto second = elder_terms::ensure_connection_id_async(legacy, {});
    const auto first_id = co_await first;
    const auto second_id = co_await second;
    expect(g_uuid_string_is_valid(first_id.c_str()) && first_id == second_id,
           "concurrent ensure calls should return one persisted ID");
    const auto text = read_text(legacy);
    expect(text.find("# Keep this comment") != std::string::npos &&
               text.find("width=91") != std::string::npos,
           "adding an ID should retain existing profile content");
    const auto loaded = elder_terms::load_settings(
        {.config_path = legacy, .startup_config_path = std::nullopt,
         .global_config_path = directory / "unused-global.ini"}, 12);
    expect(elder_terms::general_connection_id(loaded.store) == first_id,
           "ID should survive a fresh settings load");
    expect(co_await elder_terms::ensure_connection_id_async(legacy, {}) ==
               first_id,
           "ensuring again should keep the persisted ID");

    const auto recreated = directory / "recreated.ini";
    write_text(recreated, "[general]\nname=Recreated\n");
    const auto old_id = co_await elder_terms::ensure_connection_id_async(
        recreated, {});
    const auto replacement = directory / "recreated-replacement.ini";
    write_text(replacement, "[general]\nname=Recreated\n");
    std::filesystem::rename(replacement, recreated);
    const auto new_id = co_await elder_terms::ensure_connection_id_async(
        recreated, {});
    expect(old_id != new_id,
           "a new profile at the same path should receive a different ID");

    const auto invalid = directory / "invalid.ini";
    write_text(invalid, "[general]\nconnection_id=invalid\nname=Legacy\n");
    const auto repaired = co_await elder_terms::ensure_connection_id_async(
        invalid, {});
    expect(g_uuid_string_is_valid(repaired.c_str()) &&
               read_text(invalid).find("connection_id=" + repaired) !=
                   std::string::npos,
           "invalid IDs should be replaced with a persisted UUID");

    cardio::cancellation_source canceled;
    canceled.cancel();
    bool canceled_result = false;
    try {
      (void)co_await elder_terms::ensure_connection_id_async(
          legacy, canceled.get_cancellation());
    } catch (const cardio::canceled_exception &) {
      canceled_result = true;
    }
    expect(canceled_result, "canceled ensure should propagate cancellation");
  } catch (...) {
    *failure = std::current_exception();
  }
  group->shutdown();
}

static cardio::promise<void> ensure_for_subprocess(
    const std::filesystem::path &path, cardio::dispatcher_group_glib *group,
    std::string *id, std::exception_ptr *failure) {
  try {
    *id = co_await elder_terms::ensure_connection_id_async(path, {});
  } catch (...) {
    *failure = std::current_exception();
  }
  group->shutdown();
}

static std::string read_pipe(int descriptor) {
  std::string output;
  char buffer[128];
  for (;;) {
    const auto count = read(descriptor, buffer, sizeof(buffer));
    if (count == 0) return output;
    expect(count > 0, "child output should be readable");
    output.append(buffer, static_cast<std::size_t>(count));
  }
}

static void test_two_processes(const std::filesystem::path &directory,
                               const std::filesystem::path &executable) {
  const auto legacy = directory / "two-processes.ini";
  write_text(legacy, "[general]\nname=Two processes\n");
  struct Child {
    pid_t pid;
    int output;
    int start;
  };
  Child children[2]{};
  for (auto &child : children) {
    int output_pipe[2];
    int start_pipe[2];
    expect(pipe(output_pipe) == 0 && pipe(start_pipe) == 0,
           "child pipes should open");
    const auto pid = fork();
    expect(pid >= 0, "child process should start");
    if (pid == 0) {
      (void)dup2(output_pipe[1], STDOUT_FILENO);
      close(output_pipe[0]);
      if (output_pipe[1] != 3) close(output_pipe[1]);
      (void)dup2(start_pipe[0], 3);
      if (start_pipe[0] != 3) close(start_pipe[0]);
      close(start_pipe[1]);
      const auto binary = executable.string();
      const auto file = legacy.string();
      char *const arguments[] = {const_cast<char *>(binary.c_str()),
                                 const_cast<char *>("--ensure"),
                                 const_cast<char *>(file.c_str()), nullptr};
      execv(binary.c_str(), arguments);
      _exit(127);
    }
    close(output_pipe[1]);
    close(start_pipe[0]);
    child = {.pid = pid, .output = output_pipe[0], .start = start_pipe[1]};
  }
  for (auto &child : children) {
    expect(write(child.start, "x", 1) == 1, "child should be released");
    close(child.start);
  }
  const auto first = read_pipe(children[0].output);
  const auto second = read_pipe(children[1].output);
  for (auto &child : children) {
    close(child.output);
    int status = 0;
    expect(waitpid(child.pid, &status, 0) == child.pid &&
               WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "child ensure should finish successfully");
  }
  expect(g_uuid_string_is_valid(first.c_str()) && first == second,
         "separate processes should derive the same legacy ID");
  const auto persisted = read_text(legacy);
  expect(persisted.find("connection_id=" + first) != std::string::npos,
         "persisted ID should match both processes");
}

} // namespace elder_terms_connection_identity_test

int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--ensure") {
      char signal = 0;
      if (read(3, &signal, 1) != 1) return 1;
      std::exception_ptr failure;
      std::string id;
      {
        cardio::dispatcher_group_glib group;
        cardio::dispatcher_host_glib dispatcher(group);
        auto task = elder_terms_connection_identity_test::ensure_for_subprocess(
            argv[2], &group, &id, &failure);
        dispatcher.park();
      }
      if (failure != nullptr) std::rethrow_exception(failure);
      std::cout << id;
      return 0;
    }
    const auto directory = std::filesystem::temp_directory_path() /
        ("elder-terms-connection-identity-" +
         std::to_string(g_get_monotonic_time()));
    std::filesystem::create_directories(directory);
    elder_terms_connection_identity_test::test_global_exclusion(directory);
    std::exception_ptr failure;
    {
      cardio::dispatcher_group_glib group;
      cardio::dispatcher_host_glib dispatcher(group);
      auto task = elder_terms_connection_identity_test::test_ensure_async(
          directory, &group, &failure);
      dispatcher.park();
    }
    if (failure != nullptr) std::rethrow_exception(failure);
    elder_terms_connection_identity_test::test_two_processes(
        directory, std::filesystem::absolute(argv[0]));
    std::filesystem::remove_all(directory);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
