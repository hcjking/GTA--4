// [web port] The hub's install folder on the computer the page is served from
// (web_host_files.cpp). (Xbox 360 saves to import are picked by each player
// instead: web_saves_transfer.cpp.)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace gta4web {

// The folder ("install") as the computer shows it, "~/.../install/";
// empty when the server doesn't have one.
std::string HostFolderShown(const std::string& folder);

// The folder's files mounted at `at` (read-only, each read when it's read);
// files gone since the last look taken away. False when the server didn't answer.
bool SyncHostFolder(const std::string& folder, const std::filesystem::path& at);

// A request to the server the page came from (synchronous: on a worker only);
// its answer in `body` when given.
bool Request(const char* method, const std::string& url, std::string* body);

}  // namespace gta4web

// (for the hub's code, which doesn't include the web port's headers)
extern "C" const char* rex_web_host_folder_shown(const char* folder);
extern "C" bool rex_web_sync_host_folder(const char* folder, const char* at);
extern "C" uint64_t rex_web_host_free_space();
