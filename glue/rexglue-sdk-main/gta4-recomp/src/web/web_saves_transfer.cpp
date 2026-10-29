// [web port] The hub's saves in and out of the browser: the saves live in the
// browser's storage for the site (OPFS), so Download Saves zips them into a
// file the browser saves, and Load Saves From a File unzips one the player
// picks back in (the hub backs the current saves up first).
//
// Zips are written stored (no compression: saves are small); reading takes
// stored and deflated entries, so a zip made elsewhere loads too.

#include <emscripten/emscripten.h>
#include <zlib.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace gta4ps5::hub::web_saves {
namespace {

// Where the page leaves a picked file (FS.writeFile), and its flag.
constexpr const char* kUploadPath = "/home/web_user/gta4-saves-upload.zip";
std::atomic<bool> g_uploaded{false};
// ...and the Xbox 360 save packages picked for Import Xbox 360 Saves (in this
// browser's storage, not a folder on the server: each player imports their own)
constexpr const char* kXbox360Folder = "/home/web_user/gta4-360-import";
std::atomic<bool> g_xbox360_picked{false};

void Put16(std::vector<uint8_t>& out, uint32_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); }
void Put32(std::vector<uint8_t>& out, uint32_t v) { Put16(out, v & 0xFFFF); Put16(out, v >> 16); }
uint32_t Get16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t Get32(const uint8_t* p) { return Get16(p) | Get16(p + 2) << 16; }

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace

// Every file under the profile folders, by its path below `root`, as a zip.
bool Zip(const fs::path& root, const std::vector<fs::path>& folders, std::vector<uint8_t>& zip, int* files) {
  struct Entry { std::string name; uint32_t crc, size, offset; };
  std::vector<Entry> entries;
  std::error_code ec;
  for (const fs::path& folder : folders) {
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      std::vector<uint8_t> data;
      if (!ReadFile(it->path(), data)) return false;
      const std::string name = fs::relative(it->path(), root, ec).generic_string();
      if (ec || name.empty() || name.size() > 0xFFFF || data.size() > 0xFFFFFFFFu) return false;
      const uint32_t crc = uint32_t(crc32(0, data.data(), uInt(data.size())));
      Entry e{name, crc, uint32_t(data.size()), uint32_t(zip.size())};
      Put32(zip, 0x04034B50); Put16(zip, 20); Put16(zip, 0); Put16(zip, 0);  // stored
      Put16(zip, 0); Put16(zip, 0x21);                                       // time, date (1980-01-01)
      Put32(zip, crc); Put32(zip, e.size); Put32(zip, e.size);
      Put16(zip, uint32_t(name.size())); Put16(zip, 0);
      zip.insert(zip.end(), name.begin(), name.end());
      zip.insert(zip.end(), data.begin(), data.end());
      entries.push_back(std::move(e));
    }
  }
  const uint32_t directory = uint32_t(zip.size());
  for (const Entry& e : entries) {
    Put32(zip, 0x02014B50); Put16(zip, 20); Put16(zip, 20); Put16(zip, 0); Put16(zip, 0);
    Put16(zip, 0); Put16(zip, 0x21);
    Put32(zip, e.crc); Put32(zip, e.size); Put32(zip, e.size);
    Put16(zip, uint32_t(e.name.size())); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0);
    Put32(zip, 0); Put32(zip, e.offset);
    zip.insert(zip.end(), e.name.begin(), e.name.end());
  }
  Put32(zip, 0x06054B50); Put16(zip, 0); Put16(zip, 0);
  Put16(zip, uint32_t(entries.size())); Put16(zip, uint32_t(entries.size()));
  Put32(zip, uint32_t(zip.size()) - directory); Put32(zip, directory); Put16(zip, 0);
  if (files) *files = int(entries.size());
  return true;
}

// The zip's files written below `root`; false (with `error`) when it isn't
// one this reads, or a name would leave `root`.
bool Unzip(const std::vector<uint8_t>& zip, const fs::path& root, int* files, std::string* error) {
  auto fail = [&](const char* why) { if (error) *error = why; return false; };
  if (zip.size() < 22) return fail("That file isn't a zip.");
  size_t end = zip.size() - 22;
  while (end > 0 && Get32(&zip[end]) != 0x06054B50) --end;
  if (Get32(&zip[end]) != 0x06054B50) return fail("That file isn't a zip.");
  const uint32_t count = Get16(&zip[end + 10]), directory = Get32(&zip[end + 16]);
  size_t at = directory;
  int written = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (at + 46 > zip.size() || Get32(&zip[at]) != 0x02014B50) return fail("That zip is damaged.");
    const uint32_t method = Get16(&zip[at + 10]), packed = Get32(&zip[at + 20]), size = Get32(&zip[at + 24]);
    const uint32_t name_len = Get16(&zip[at + 28]), extra = Get16(&zip[at + 30]), comment = Get16(&zip[at + 32]);
    const uint32_t local = Get32(&zip[at + 42]);
    if (at + 46 + name_len > zip.size()) return fail("That zip is damaged.");
    const std::string name(reinterpret_cast<const char*>(&zip[at + 46]), name_len);
    at += 46 + name_len + extra + comment;
    if (name.empty() || name.back() == '/') continue;  // (a folder)
    const fs::path relative = fs::path(name).lexically_normal();
    if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") return fail("That zip has a file outside its folders.");
    if (local + 30 > zip.size() || Get32(&zip[local]) != 0x04034B50) return fail("That zip is damaged.");
    const size_t data = local + 30 + Get16(&zip[local + 26]) + Get16(&zip[local + 28]);
    if (data + packed > zip.size()) return fail("That zip is damaged.");
    std::vector<uint8_t> out(size);
    if (method == 0) {
      if (packed != size) return fail("That zip is damaged.");
      std::memcpy(out.data(), &zip[data], size);
    } else if (method == 8) {
      z_stream z{};
      if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return fail("That zip couldn't be read.");
      z.next_in = const_cast<Bytef*>(&zip[data]);
      z.avail_in = packed;
      z.next_out = out.data();
      z.avail_out = size;
      const int status = inflate(&z, Z_FINISH);
      inflateEnd(&z);
      if (status != Z_STREAM_END || z.total_out != size) return fail("That zip couldn't be read.");
    } else {
      return fail("That zip uses a compression this can't read.");
    }
    const fs::path target = root / relative;
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size())))
      return fail("A save couldn't be written.");
    ++written;
  }
  if (files) *files = written;
  return true;
}

// The zip handed to the browser as a download named `name`.
void Offer(const std::vector<uint8_t>& zip, const std::string& name) {
  MAIN_THREAD_EM_ASM({
    const bytes = HEAPU8.slice(Number($0), Number($0) + Number($1));
    const link = document.createElement('a');
    link.href = URL.createObjectURL(new Blob([bytes], { type: 'application/zip' }));
    link.download = UTF8ToString(Number($2));
    document.body.appendChild(link);
    link.click();
    setTimeout(() => { URL.revokeObjectURL(link.href); link.remove(); }, 10000);
  }, double(reinterpret_cast<uintptr_t>(zip.data())), double(zip.size()),
     double(reinterpret_cast<uintptr_t>(name.c_str())));
}

// The browser's file picker; the file chosen is written for TakeUpload.
void Pick() {
  MAIN_THREAD_ASYNC_EM_ASM({
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.zip,application/zip';
    input.onchange = async () => {
      const file = input.files && input.files[0];
      if (!file) return;
      FS.writeFile(UTF8ToString(Number($0)), new Uint8Array(await file.arrayBuffer()));
      Module._rex_web_saves_uploaded();
    };
    input.click();
  }, double(reinterpret_cast<uintptr_t>(kUploadPath)));
}

// A picked file, once the page has written it.
bool TakeUpload(std::vector<uint8_t>& bytes) {
  if (!g_uploaded.exchange(false)) return false;
  const bool read = ReadFile(kUploadPath, bytes);
  std::error_code ec;
  fs::remove(kUploadPath, ec);
  return read;
}

// The file picker for Xbox 360 save packages (any number; they have no
// extension): each one chosen is written into kXbox360Folder, then
// TakeXbox360Picked says so.
void PickXbox360() {
  std::error_code ec;
  fs::remove_all(kXbox360Folder, ec);
  fs::create_directories(kXbox360Folder, ec);
  MAIN_THREAD_ASYNC_EM_ASM({
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.onchange = async () => {
      const files = input.files ? Array.from(input.files) : [];
      if (!files.length) return;
      const folder = UTF8ToString(Number($0));
      for (const file of files) {
        const name = file.name.replace(/[^A-Za-z0-9._-]/g, '_').replace(/^[.]+/, '_').slice(0, 64);
        FS.writeFile(folder + '/' + name, new Uint8Array(await file.arrayBuffer()));
      }
      Module._rex_web_saves_360_picked();
    };
    input.click();
  }, double(reinterpret_cast<uintptr_t>(kXbox360Folder)));
}

bool TakeXbox360Picked() { return g_xbox360_picked.exchange(false); }

}  // namespace gta4ps5::hub::web_saves

extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_saves_uploaded() {
  gta4ps5::hub::web_saves::g_uploaded = true;
}

extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_saves_360_picked() {
  gta4ps5::hub::web_saves::g_xbox360_picked = true;
}

// (save_import.cpp imports from here in the browser)
extern "C" const char* rex_web_xbox360_import_folder() { return gta4ps5::hub::web_saves::kXbox360Folder; }
