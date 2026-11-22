// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_launch_planner.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace fs = std::filesystem;

namespace bbport {

namespace {

constexpr std::uint64_t kEbootBase = 0x400000ULL;
constexpr int kOutputWidth = 1920;
constexpr int kOutputHeight = 1080;
constexpr std::array<double, 5> kPresetScales = {1.0, 1.5, 1.7, 2.0, 3.0};

std::string Trim(std::string_view sv) {
  std::size_t a = 0;
  while (a < sv.size() && std::isspace(static_cast<unsigned char>(sv[a]))) ++a;
  std::size_t b = sv.size();
  while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1]))) --b;
  return std::string(sv.substr(a, b - a));
}

std::string ToLower(std::string_view sv) {
  std::string out(sv);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool ReadFileBytes(const fs::path& p, std::vector<std::uint8_t>& out, std::string* err = nullptr) {
  std::ifstream ifs(p, std::ios::binary | std::ios::ate);
  if (!ifs) {
    if (err) *err = "Cannot open file: " + p.string();
    return false;
  }
  const auto sz = ifs.tellg();
  if (sz < 0) {
    if (err) *err = "Cannot stat file size: " + p.string();
    return false;
  }
  out.resize(static_cast<std::size_t>(sz));
  ifs.seekg(0);
  if (sz > 0 && !ifs.read(reinterpret_cast<char*>(out.data()), sz)) {
    if (err) *err = "Failed reading file: " + p.string();
    return false;
  }
  return true;
}

bool WriteFileBytes(const fs::path& p, const void* data, std::size_t size, std::string* err = nullptr) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
  if (!ofs) {
    if (err) *err = "Cannot open file for writing: " + p.string();
    return false;
  }
  if (size > 0 && !ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size))) {
    if (err) *err = "Failed writing file: " + p.string();
    return false;
  }
  return true;
}

template <typename T>
T ReadLE(const std::uint8_t* ptr) {
  T val{};
  std::memcpy(&val, ptr, sizeof(T));
  return val;
}

template <typename T>
void AppendLE(std::vector<std::uint8_t>& buf, T val) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&val);
  buf.insert(buf.end(), p, p + sizeof(T));
}

void WriteLE64At(std::vector<std::uint8_t>& buf, std::size_t offset, std::uint64_t val) {
  std::memcpy(buf.data() + offset, &val, sizeof(val));
}

// ============================================================================
// 1. PS4 SELF / ELF Parser & BBPROBE5 Linker (prepare.py + link_modules.py)
// ============================================================================

struct ElfPhdr {
  std::uint32_t type = 0;
  std::uint32_t flags = 0;
  std::uint64_t offset = 0;
  std::uint64_t vaddr = 0;
  std::uint64_t paddr = 0;
  std::uint64_t filesz = 0;
  std::uint64_t memsz = 0;
  std::uint64_t align = 0;
};

struct ParsedElf {
  std::vector<std::uint8_t> elf;
  std::uint64_t entry = 0;
  std::uint64_t phoff = 0;
  std::uint16_t phentsize = 0;
  std::uint16_t phnum = 0;
  std::vector<ElfPhdr> ph;
  std::vector<int> missing_headers;
};

bool ParseSelfOrElf(const std::vector<std::uint8_t>& data, ParsedElf& out, std::string* err) {
  if (data.size() < 64) {
    if (err) *err = "File too small to be ELF or SELF";
    return false;
  }

  const bool is_elf = (data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F');
  const bool is_self = (data[0] == 0x4F && data[1] == 0x15 && data[2] == 0x3D && data[3] == 0x1D);
  if (!is_elf && !is_self) {
    if (err) *err = "Expected PS4 SELF or ELF (eboot.bin is neither: encrypted or damaged dump?)";
    return false;
  }

  std::size_t base = 0;
  std::uint16_t self_count = 0;
  if (is_self) {
    self_count = ReadLE<std::uint16_t>(data.data() + 24);
    base = 32 + static_cast<std::size_t>(self_count) * 32;
  }

  if (base + 64 > data.size()) {
    if (err) *err = "SELF header out of bounds";
    return false;
  }

  const std::uint8_t* eh = data.data() + base;
  if (std::memcmp(eh, "\x7F" "ELF\x02\x01\x01", 7) != 0 || ReadLE<std::uint16_t>(eh + 18) != 62) {
    if (err) *err = "Expected little-endian x86-64 ELF";
    return false;
  }

  out.entry = ReadLE<std::uint64_t>(eh + 24);
  out.phoff = ReadLE<std::uint64_t>(eh + 32);
  out.phentsize = ReadLE<std::uint16_t>(eh + 54);
  out.phnum = ReadLE<std::uint16_t>(eh + 56);
  if (out.phentsize != 56 || out.phnum == 0 || out.phnum >= 256) {
    if (err) *err = "Unsupported program headers";
    return false;
  }
  if (base + out.phoff + static_cast<std::size_t>(out.phnum) * 56 > data.size()) {
    if (err) *err = "Program headers out of bounds";
    return false;
  }

  out.ph.resize(out.phnum);
  std::uint64_t max_end = out.phoff + static_cast<std::uint64_t>(out.phnum) * 56;
  for (std::uint16_t i = 0; i < out.phnum; ++i) {
    const std::uint8_t* p = data.data() + base + out.phoff + static_cast<std::size_t>(i) * 56;
    out.ph[i].type = ReadLE<std::uint32_t>(p + 0);
    out.ph[i].flags = ReadLE<std::uint32_t>(p + 4);
    out.ph[i].offset = ReadLE<std::uint64_t>(p + 8);
    out.ph[i].vaddr = ReadLE<std::uint64_t>(p + 16);
    out.ph[i].paddr = ReadLE<std::uint64_t>(p + 24);
    out.ph[i].filesz = ReadLE<std::uint64_t>(p + 32);
    out.ph[i].memsz = ReadLE<std::uint64_t>(p + 40);
    out.ph[i].align = ReadLE<std::uint64_t>(p + 48);
    max_end = std::max(max_end, out.ph[i].offset + out.ph[i].filesz);
  }

  if (max_end > 512ULL * 1024 * 1024) {
    if (err) *err = "Probe image exceeds 512 MiB limit";
    return false;
  }

  if (is_elf) {
    if (max_end > data.size()) {
      if (err) *err = "Plain ELF truncated";
      return false;
    }
    out.elf.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(max_end));
    return true;
  }

  out.elf.assign(static_cast<std::size_t>(max_end), 0);
  const std::uint64_t header_end = out.phoff + static_cast<std::uint64_t>(out.phnum) * 56;
  std::memcpy(out.elf.data(), data.data() + base, static_cast<std::size_t>(header_end));

  std::vector<std::pair<std::uint64_t, std::uint64_t>> covered;
  covered.emplace_back(0, header_end);

  for (std::uint16_t i = 0; i < self_count; ++i) {
    const std::uint8_t* s = data.data() + 32 + static_cast<std::size_t>(i) * 32;
    const std::uint64_t flags = ReadLE<std::uint64_t>(s + 0);
    const std::uint64_t off = ReadLE<std::uint64_t>(s + 8);
    const std::uint64_t size = ReadLE<std::uint64_t>(s + 16);
    const std::uint64_t mem = ReadLE<std::uint64_t>(s + 24);
    if (!(flags & 0x800ULL)) continue;
    if (flags & 10ULL) {
      if (err) *err = "Encrypted/compressed SELF segment is unsupported";
      return false;
    }
    const std::size_t index = static_cast<std::size_t>((flags >> 20) & 4095ULL);
    if (index >= out.ph.size()) {
      if (err) *err = "Invalid SELF segment index";
      return false;
    }
    const auto& ph = out.ph[index];
    if (size != ph.filesz || size != mem || off + size > data.size()) {
      if (err) *err = "Unsupported blocked SELF segment layout";
      return false;
    }
    std::memcpy(out.elf.data() + ph.offset, data.data() + off, static_cast<std::size_t>(size));
    covered.emplace_back(ph.offset, ph.offset + size);
  }

  for (std::size_t i = 0; i < out.ph.size(); ++i) {
    const auto& p = out.ph[i];
    if (p.filesz == 0) continue;
    bool ok = false;
    for (const auto& [a, z] : covered) {
      if (a <= p.offset && p.offset + p.filesz <= z) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      out.missing_headers.push_back(static_cast<int>(i));
      if (p.type == 1 || p.type == 2 || p.type == 0x61000000U || p.type == 0x61000010U) {
        if (err) *err = "Required ELF segment is unavailable in SELF";
        return false;
      }
    }
  }
  return true;
}

std::string EncodeModuleId(std::uint64_t value) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
  std::string result(1, kAlphabet[value & 63]);
  value >>= 6;
  while (value) {
    result.insert(result.begin(), kAlphabet[value & 63]);
    value >>= 6;
  }
  return result;
}

using LibOrModIdentity = std::pair<std::string, std::uint16_t>;
using SymbolIdentity = std::tuple<std::string, LibOrModIdentity, LibOrModIdentity>;

struct ModuleSymbol {
  std::string name;
  std::uint8_t type = 0;
  std::uint8_t binding = 0;
  std::uint16_t section = 0;
  std::uint64_t value = 0;
  std::uint64_t size = 0;
  bool has_identity = false;
  SymbolIdentity identity;
};

struct ModuleReloc {
  std::uint64_t target = 0;
  std::uint32_t kind = 0;
  std::uint32_t symid = 0;
  std::int64_t addend = 0;
};

struct ParsedModule {
  ParsedElf parsed;
  std::map<std::uint64_t, std::uint64_t> tags;
  std::map<std::string, LibOrModIdentity> libraries;
  std::vector<std::pair<std::string, LibOrModIdentity>> libraries_order;
  std::map<std::string, LibOrModIdentity> modules;
  std::vector<std::pair<std::string, LibOrModIdentity>> modules_order;
  std::vector<ModuleSymbol> symbols;
  std::vector<ModuleReloc> relocs;
  std::size_t needed_count = 0;

  bool ParseIdentity(const std::string& sym_name, SymbolIdentity& out_id) const {
    const auto p1 = sym_name.find('#');
    if (p1 == std::string::npos) return false;
    const auto p2 = sym_name.find('#', p1 + 1);
    if (p2 == std::string::npos || sym_name.find('#', p2 + 1) != std::string::npos) return false;
    const std::string n = sym_name.substr(0, p1);
    const std::string lib_key = sym_name.substr(p1 + 1, p2 - p1 - 1);
    const std::string mod_key = sym_name.substr(p2 + 1);
    const auto it_lib = libraries.find(lib_key);
    const auto it_mod = modules.find(mod_key);
    if (it_lib == libraries.end() || it_mod == modules.end()) return false;
    out_id = SymbolIdentity{n, it_lib->second, it_mod->second};
    return true;
  }
};

bool LoadPs4Module(const fs::path& path, ParsedModule& out, std::string* err) {
  std::vector<std::uint8_t> raw;
  if (!ReadFileBytes(path, raw, err)) return false;
  if (!ParseSelfOrElf(raw, out.parsed, err)) return false;

  const ElfPhdr* dp = nullptr;
  const ElfPhdr* lp = nullptr;
  for (const auto& p : out.parsed.ph) {
    if (p.type == 2 && !dp) dp = &p;
    if (p.type == 0x61000000U && !lp) lp = &p;
  }
  if (!dp || !lp) {
    if (err) *err = "Missing PT_DYNAMIC or PT_SCE_DYNLIBDATA in " + path.string();
    return false;
  }

  std::vector<std::pair<std::uint64_t, std::uint64_t>> dynamic;
  for (std::uint64_t pos = dp->offset; pos + 16 <= dp->offset + dp->filesz; pos += 16) {
    const std::uint64_t tag = ReadLE<std::uint64_t>(out.parsed.elf.data() + pos);
    const std::uint64_t val = ReadLE<std::uint64_t>(out.parsed.elf.data() + pos + 8);
    if (tag == 0) break;
    dynamic.emplace_back(tag, val);
    out.tags[tag] = val;
    if (tag == 1) ++out.needed_count;
  }

  const std::uint8_t* blob = out.parsed.elf.data() + lp->offset;
  const std::size_t blob_size = static_cast<std::size_t>(lp->filesz);

  const std::uint64_t str_off = out.tags[0x61000035ULL];
  const std::uint64_t str_sz = out.tags[0x61000037ULL];
  if (str_off + str_sz > blob_size) {
    if (err) *err = "Dynlib string table out of bounds";
    return false;
  }
  auto read_str = [&](std::uint64_t off) -> std::string {
    if (off >= str_sz) return {};
    const char* s = reinterpret_cast<const char*>(blob + str_off + off);
    const std::size_t max_len = static_cast<std::size_t>(str_sz - off);
    const void* nul = std::memchr(s, 0, max_len);
    return nul ? std::string(s, static_cast<const char*>(nul)) : std::string(s, max_len);
  };

  for (const auto& [tag, value] : dynamic) {
    if (tag == 0x61000013ULL || tag == 0x61000015ULL || tag == 0x6100000dULL || tag == 0x6100000fULL) {
      const bool is_lib = (tag == 0x61000013ULL || tag == 0x61000015ULL);
      const std::string key = EncodeModuleId(value >> 48);
      const LibOrModIdentity id{read_str(value & 0xffffffffULL),
                                static_cast<std::uint16_t>((value >> 32) & 0xffffULL)};
      if (is_lib) {
        out.libraries[key] = id;
        out.libraries_order.emplace_back(key, id);
      } else {
        out.modules[key] = id;
        out.modules_order.emplace_back(key, id);
      }
    }
  }

  const std::uint64_t sym_off = out.tags[0x61000039ULL];
  const std::uint64_t sym_sz = out.tags[0x6100003fULL];
  if (sym_off + sym_sz > blob_size || (sym_sz % 24) != 0) {
    if (err) *err = "Dynlib symbol table out of bounds";
    return false;
  }
  const std::size_t nsyms = static_cast<std::size_t>(sym_sz / 24);
  out.symbols.resize(nsyms);
  for (std::size_t i = 0; i < nsyms; ++i) {
    const std::uint8_t* sp = blob + sym_off + i * 24;
    const std::uint32_t n = ReadLE<std::uint32_t>(sp + 0);
    const std::uint8_t info = sp[4];
    const std::uint16_t section = ReadLE<std::uint16_t>(sp + 6);
    const std::uint64_t val = ReadLE<std::uint64_t>(sp + 8);
    const std::uint64_t sz = ReadLE<std::uint64_t>(sp + 16);
    auto& sym = out.symbols[i];
    sym.name = read_str(n);
    sym.type = info & 15;
    sym.binding = info >> 4;
    sym.section = section;
    sym.value = val;
    sym.size = sz;
    if (!sym.name.empty()) {
      sym.has_identity = out.ParseIdentity(sym.name, sym.identity);
    }
  }

  const std::pair<std::uint64_t, std::uint64_t> reloc_tables[2] = {
      {0x61000029ULL, 0x6100002dULL},
      {0x6100002fULL, 0x61000031ULL},
  };
  for (const auto& [ot, st] : reloc_tables) {
    const std::uint64_t roff = out.tags[ot];
    const std::uint64_t rsz = out.tags[st];
    if (roff + rsz > blob_size || (rsz % 24) != 0) {
      if (err) *err = "Dynlib relocation table out of bounds";
      return false;
    }
    const std::size_t nrel = static_cast<std::size_t>(rsz / 24);
    for (std::size_t i = 0; i < nrel; ++i) {
      const std::uint8_t* rp = blob + roff + i * 24;
      const std::uint64_t target = ReadLE<std::uint64_t>(rp + 0);
      const std::uint64_t info = ReadLE<std::uint64_t>(rp + 8);
      const std::int64_t addend = ReadLE<std::int64_t>(rp + 16);
      out.relocs.push_back(ModuleReloc{
          .target = target,
          .kind = static_cast<std::uint32_t>(info & 0xffffffffULL),
          .symid = static_cast<std::uint32_t>(info >> 32),
          .addend = addend,
      });
    }
  }
  return true;
}

std::size_t PatchFsLoads(std::vector<std::uint8_t>& image,
                         const std::vector<ElfPhdr>& ph,
                         std::uint64_t base) {
  std::size_t patched = 0;
  for (const auto& p : ph) {
    if (p.type != 1 || !(p.flags & 1)) continue;
    const std::size_t start = static_cast<std::size_t>(base + p.vaddr);
    const std::size_t end = static_cast<std::size_t>(base + p.vaddr + p.filesz);
    if (end > image.size() || end < start + 9) continue;
    for (std::size_t pos = start; pos + 9 <= end;) {
      const std::uint8_t b0 = image[pos];
      const std::uint8_t b1 = image[pos + 1];
      const std::uint8_t b2 = image[pos + 2];
      const std::uint8_t b3 = image[pos + 3];
      const std::uint8_t b4 = image[pos + 4];
      if (b0 == 0x64 && (b1 == 0x48 || b1 == 0x4c) && b2 == 0x8b &&
          (b3 == 0x04 || b3 == 0x0c || b3 == 0x14 || b3 == 0x1c ||
           b3 == 0x24 || b3 == 0x2c || b3 == 0x34 || b3 == 0x3c) &&
          b4 == 0x25 && image[pos + 5] == 0 && image[pos + 6] == 0 &&
          image[pos + 7] == 0 && image[pos + 8] == 0) {
        image[pos] = 0x65;
        ++patched;
        pos += 9;
      } else {
        ++pos;
      }
    }
  }
  return patched;
}

// Mirrors scripts/import_scopes.py::canonical
std::string CanonicalImportName(const std::string& name,
                                const std::map<std::string, LibOrModIdentity>& libraries,
                                const std::map<std::string, LibOrModIdentity>& modules) {
  static const std::unordered_map<std::string, const char*> kLibraries = {
      {"libSceAjm", "N"},              {"libSceAppContent", "c"},
      {"libSceAudioIn", "Y"},          {"libSceAudioOut", "G"},
      {"libSceAvPlayer", "m"},         {"libSceCommonDialog", "Z"},
      {"libSceFios2", "F"},            {"libSceGameLiveStreaming", "e"},
      {"libSceGnmDriver", "B"},        {"libSceHttp", "K"},
      {"libSceImeDialog", "b"},        {"libSceMouse", "U"},
      {"libSceMsgDialog", "j"},        {"libSceNet", "E"},
      {"libSceNetCtl", "J"},           {"libSceNpAuth", "X"},
      {"libSceNpCommerce", "k"},       {"libSceNpCommon", "Q"},
      {"libSceNpManager", "R"},        {"libSceNpMatching2", "P"},
      {"libSceNpProfileDialog", "h"},  {"libSceNpScore", "o"},
      {"libSceNpSignaling", "T"},      {"libSceNpTrophy", "l"},
      {"libSceNpUtility", "S"},        {"libSceNpWebApi", "W"},
      {"libScePad", "A"},              {"libScePlayGo", "n"},
      {"libScePosix", "I"},            {"libSceRtc", "H"},
      {"libSceSaveData", "O"},         {"libSceSaveDataDialog", "d"},
      {"libSceScreenShot", "g"},       {"libSceSsl", "L"},
      {"libSceSysmodule", "M"},        {"libSceSystemService", "V"},
      {"libSceUserService", "D"},      {"libSceVideoOut", "C"},
      {"libSceVideoRecording", "f"},   {"libSceVoice", "i"},
      {"libSceWebBrowserDialog", "a"}, {"libc", "q"},
      {"libkernel", "p"},
  };
  static const std::unordered_map<std::string, const char*> kModules = {
      {"libSceAjm", "O"},              {"libSceAppContentUtil", "d"},
      {"libSceAudioIn", "Z"},          {"libSceAudioOut", "H"},
      {"libSceAvPlayer", "n"},         {"libSceCommonDialog", "a"},
      {"libSceFios2", "G"},            {"libSceGameLiveStreaming", "f"},
      {"libSceGnmDriver", "C"},        {"libSceHttp", "L"},
      {"libSceImeDialog", "c"},        {"libSceMouse", "V"},
      {"libSceMsgDialog", "k"},        {"libSceNet", "F"},
      {"libSceNetCtl", "K"},           {"libSceNpAuth", "Y"},
      {"libSceNpCommerce", "l"},       {"libSceNpCommon", "R"},
      {"libSceNpManager", "S"},        {"libSceNpMatching2", "Q"},
      {"libSceNpProfileDialog", "i"},  {"libSceNpScore", "p"},
      {"libSceNpSignaling", "U"},      {"libSceNpTrophy", "m"},
      {"libSceNpUtility", "T"},        {"libSceNpWebApi", "X"},
      {"libScePad", "B"},              {"libScePlayGo", "o"},
      {"libSceRtc", "I"},              {"libSceSaveData", "P"},
      {"libSceSaveDataDialog", "e"},   {"libSceScreenShot", "h"},
      {"libSceSsl", "M"},              {"libSceSysmodule", "N"},
      {"libSceSystemService", "W"},    {"libSceUserService", "E"},
      {"libSceVideoOut", "D"},         {"libSceVideoRecording", "g"},
      {"libSceVoice", "j"},            {"libSceWebBrowserDialog", "b"},
      {"libc", "q"},                   {"libkernel", "J"},
  };

  const auto p1 = name.find('#');
  if (p1 == std::string::npos) return name;
  const auto p2 = name.find('#', p1 + 1);
  if (p2 == std::string::npos || name.find('#', p2 + 1) != std::string::npos) return name;
  const std::string nid = name.substr(0, p1);
  const std::string l_key = name.substr(p1 + 1, p2 - p1 - 1);
  const std::string m_key = name.substr(p2 + 1);
  const auto it_l = libraries.find(l_key);
  const auto it_m = modules.find(m_key);
  if (it_l == libraries.end() || it_m == modules.end()) return name;
  const std::string& lib_name = it_l->second.first;
  const std::string& mod_name = it_m->second.first;
  const auto kl = kLibraries.find(lib_name);
  const auto km = kModules.find(mod_name);
  if (kl != kLibraries.end() && km != kModules.end()) {
    return nid + "#" + kl->second + "#" + km->second;
  }
  return nid + "#" + lib_name;
}

// ============================================================================
// 2. SFO Parser & Content Profile (prepare.py::sfo + content_profile.py)
// ============================================================================

struct SfoData {
  std::map<std::string, std::string> strings;
  std::map<std::string, std::uint32_t> ints;
};

bool ParseSfo(const fs::path& sfo_path, SfoData& out, std::string* err = nullptr) {
  std::vector<std::uint8_t> data;
  if (!ReadFileBytes(sfo_path, data, err)) return false;
  if (data.size() < 20 || std::memcmp(data.data(), "\0PSF", 4) != 0) {
    if (err) *err = "Bad SFO signature in " + sfo_path.string();
    return false;
  }
  const std::uint32_t keys = ReadLE<std::uint32_t>(data.data() + 8);
  const std::uint32_t values = ReadLE<std::uint32_t>(data.data() + 12);
  const std::uint32_t count = ReadLE<std::uint32_t>(data.data() + 16);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t eoff = 20 + static_cast<std::size_t>(i) * 16;
    if (eoff + 16 > data.size()) break;
    const std::uint16_t key_off = ReadLE<std::uint16_t>(data.data() + eoff + 0);
    const std::uint16_t fmt = ReadLE<std::uint16_t>(data.data() + eoff + 2);
    const std::uint32_t size = ReadLE<std::uint32_t>(data.data() + eoff + 4);
    const std::uint32_t val_off = ReadLE<std::uint32_t>(data.data() + eoff + 12);
    if (keys + key_off >= data.size() || values + val_off + size > data.size()) continue;
    const char* kptr = reinterpret_cast<const char*>(data.data() + keys + key_off);
    const void* knul = std::memchr(kptr, 0, data.size() - (keys + key_off));
    if (!knul) continue;
    std::string k(kptr, static_cast<const char*>(knul));
    if (fmt == 0x0404 && size >= 4) {
      out.ints[k] = ReadLE<std::uint32_t>(data.data() + values + val_off);
    } else {
      const char* vptr = reinterpret_cast<const char*>(data.data() + values + val_off);
      std::size_t len = size;
      while (len > 0 && vptr[len - 1] == '\0') --len;
      out.strings[k] = std::string(vptr, len);
    }
  }
  return true;
}

bool PrepareLinkAndContent(const fs::path& game_dir,
                           const fs::path& out_dir,
                           const std::string& sku,
                           const NativeLaunchPlanner::LogCallback& log,
                           std::string* err) {
  ParsedModule main_mod;
  if (!LoadPs4Module(game_dir / "eboot.bin", main_mod, err)) return false;

  ParsedModule libc_mod;
  if (!LoadPs4Module(game_dir / "sce_module" / "libc.prx", libc_mod, err)) return false;

  // Check _init_env in libc.prx (NID: bzQExy189ZI)
  bool init_env_is_ret = false;
  for (const auto& s : libc_mod.symbols) {
    const auto hash_pos = s.name.find('#');
    const std::string key = (hash_pos != std::string::npos) ? s.name.substr(0, hash_pos) : s.name;
    if (key == "bzQExy189ZI" && s.section != 0 && s.type == 2) {
      for (const auto& p : libc_mod.parsed.ph) {
        if (p.type == 1 && (p.flags & 1) && p.vaddr <= s.value && s.value < p.vaddr + p.filesz) {
          const std::uint64_t code_off = p.offset + (s.value - p.vaddr);
          if (s.size == 1 && code_off < libc_mod.parsed.elf.size() &&
              libc_mod.parsed.elf[static_cast<std::size_t>(code_off)] == 0xC3) {
            init_env_is_ret = true;
          }
          break;
        }
      }
    }
  }

  std::vector<ElfPhdr> main_loads;
  std::uint64_t main_size = 0;
  for (const auto& p : main_mod.parsed.ph) {
    if (p.type == 1 || p.type == 0x61000010U) {
      main_loads.push_back(p);
      main_size = std::max(main_size, p.vaddr + p.memsz);
    }
  }
  auto is_main_mapped = [&](std::uint64_t addr, std::uint64_t len = 8) {
    for (const auto& p : main_loads) {
      if (p.vaddr <= addr && addr + len <= p.vaddr + p.memsz) return true;
    }
    return false;
  };
  if (main_size == 0 || main_size > 512ULL * 1024 * 1024 || !is_main_mapped(main_mod.parsed.entry, 1)) {
    if (err) *err = "Invalid eboot memory image";
    return false;
  }

  std::vector<std::uint8_t> image(static_cast<std::size_t>(main_size), 0);
  for (const auto& p : main_loads) {
    if (p.filesz > p.memsz) {
      if (err) *err = "Segment file size exceeds memory size";
      return false;
    }
    std::memcpy(image.data() + p.vaddr,
                main_mod.parsed.elf.data() + p.offset,
                static_cast<std::size_t>(p.filesz));
  }

  struct BootSegment {
    std::uint64_t vaddr;
    std::uint64_t memsz;
    std::uint64_t flags;
  };
  struct BootReloc {
    std::uint64_t target;
    std::uint64_t kind;
    std::int64_t value;
    std::int64_t addend;
  };

  std::vector<BootSegment> segments;
  segments.reserve(main_loads.size() + 8);
  for (const auto& p : main_loads) {
    segments.push_back({p.vaddr, p.memsz, p.flags});
  }

  std::vector<BootReloc> relocs;
  relocs.reserve(main_mod.relocs.size() + libc_mod.relocs.size() + 8192);
  std::map<std::uint32_t, std::uint64_t> main_imports;
  std::vector<std::string> names;

  for (const auto& r : main_mod.relocs) {
    if (!is_main_mapped(r.target)) {
      if (err) *err = "Relocation outside mapped segments";
      return false;
    }
    if (r.kind == 8) {
      relocs.push_back({r.target, 0, r.addend, 0});
    } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
      if (r.symid >= main_mod.symbols.size()) {
        if (err) *err = "Relocation symbol index out of range";
        return false;
      }
      const auto& sym = main_mod.symbols[r.symid];
      if (sym.section != 0) {
        if (sym.section == 0xfff1) {
          WriteLE64At(image, static_cast<std::size_t>(r.target),
                      static_cast<std::uint64_t>(static_cast<std::int64_t>(sym.value) + r.addend));
        } else {
          relocs.push_back({r.target, 0, static_cast<std::int64_t>(sym.value) + r.addend, 0});
        }
      } else {
        auto [it, inserted] = main_imports.emplace(r.symid, names.size());
        if (inserted) {
          names.push_back(sym.name);
        }
        const std::uint64_t rk = (sym.type == 1) ? 2 : 1;
        if ((rk == 1 && r.addend != 0) || r.addend < 0 || r.addend >= 4096) {
          if (err) *err = "Unsupported import addend";
          return false;
        }
        relocs.push_back({r.target, rk, static_cast<std::int64_t>(it->second), r.addend});
      }
    } else {
      if (err) *err = "Unsupported relocation kind in eboot.bin";
      return false;
    }
  }

  // Write out/eboot.elf (needed by patches compiler)
  if (!WriteFileBytes(out_dir / "eboot.elf", main_mod.parsed.elf.data(), main_mod.parsed.elf.size(), err)) {
    return false;
  }

  SfoData sfo;
  ParseSfo(game_dir / "sce_sys" / "param.sfo", sfo);
  if (log) {
    const std::string title = sfo.strings.count("TITLE") ? sfo.strings["TITLE"] : "Bloodborne";
    std::ostringstream oss;
    oss << title << " | entry=0x" << std::hex << main_mod.parsed.entry << std::dec
        << " | image=" << main_size << " bytes";
    log(oss.str());
    log(std::to_string(names.size()) + " imported symbols; " +
        std::to_string(main_mod.relocs.size()) + " relocations; " +
        std::to_string(main_mod.needed_count) + " required modules");
    log("Output: " + fs::weakly_canonical(out_dir).string());
    log(std::string("libc _init_env verified RET: ") + (init_env_is_ret ? "True" : "False"));
  }

  // Link system modules (libc.prx, libSceFios2.prx) into BBPROBE5 (out/boot-linked.bin)
  std::map<SymbolIdentity, std::uint64_t> identities;
  std::vector<std::pair<SymbolIdentity, std::uint64_t>> identities_order;
  for (std::size_t i = 0; i < names.size(); ++i) {
    SymbolIdentity id;
    if (main_mod.ParseIdentity(names[i], id)) {
      if (identities.emplace(id, i).second) {
        identities_order.emplace_back(id, i);
      }
    }
  }

  std::map<LibOrModIdentity, std::string> main_libraries;
  for (const auto& [k, id] : main_mod.libraries_order) {
    main_libraries[id] = k;
  }
  std::map<LibOrModIdentity, std::string> main_modules;
  for (const auto& [k, id] : main_mod.modules_order) {
    main_modules[id] = k;
  }

  auto imported = [&](const ModuleSymbol& symbol) -> std::uint64_t {
    const auto& key = symbol.identity;
    auto it = identities.find(key);
    if (it != identities.end()) return it->second;
    const auto& [n, lib, mod] = key;
    std::string name;
    auto it_l = main_libraries.find(lib);
    auto it_m = main_modules.find(mod);
    if (it_l != main_libraries.end() && it_m != main_modules.end()) {
      name = n + "#" + it_l->second + "#" + it_m->second;
    } else {
      name = n + "#" + lib.first;
    }
    const std::uint64_t idx = names.size();
    identities.emplace(key, idx);
    identities_order.emplace_back(key, idx);
    names.push_back(std::move(name));
    return idx;
  };

  struct ExportEntry {
    std::uint64_t address;
    std::uint64_t kind;
    std::string filename;
  };
  std::map<SymbolIdentity, ExportEntry> exports;
  std::map<std::pair<std::string, std::string>, std::vector<ExportEntry>> by_nid;

  struct LinkedModRow {
    std::string file;
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::uint64_t init = 0;
    std::uint64_t tls_address = 0;
    std::uint64_t tls_memsz = 0;
    std::uint64_t tls_filesz = 0;
    std::uint64_t tls_module = 0;
  };
  std::vector<LinkedModRow> table;

  std::uint64_t base = (main_size + 65535ULL) & ~65535ULL;
  std::size_t fs_patched = PatchFsLoads(image, main_mod.parsed.ph, 0);
  std::uint64_t tls_module = 2;

  const std::array<std::string, 2> kModuleNames = {"libc.prx", "libSceFios2.prx"};
  for (const auto& filename : kModuleNames) {
    ParsedModule m;
    if (filename == "libc.prx") {
      m = std::move(libc_mod);
    } else {
      if (!LoadPs4Module(game_dir / "sce_module" / filename, m, err)) return false;
    }

    std::vector<ElfPhdr> loads;
    std::uint64_t modsize = 0;
    const ElfPhdr* tls = nullptr;
    for (const auto& p : m.parsed.ph) {
      if (p.type == 1 || p.type == 0x61000010U) {
        loads.push_back(p);
        modsize = std::max(modsize, p.vaddr + p.memsz);
      }
      if (p.type == 7 && !tls) tls = &p;
    }
    if (base + modsize > 512ULL * 1024 * 1024) {
      if (err) *err = "Linked image exceeds 512 MiB limit";
      return false;
    }
    image.resize(static_cast<std::size_t>(base + modsize), 0);

    auto is_mod_mapped = [&](std::uint64_t addr, std::uint64_t len = 8) {
      for (const auto& p : loads) {
        if (p.vaddr <= addr && addr + len <= p.vaddr + p.memsz) return true;
      }
      return false;
    };

    for (const auto& p : loads) {
      if (p.filesz > p.memsz) {
        if (err) *err = filename + ": invalid segment size";
        return false;
      }
      std::memcpy(image.data() + base + p.vaddr,
                  m.parsed.elf.data() + p.offset,
                  static_cast<std::size_t>(p.filesz));
      segments.push_back({base + p.vaddr, p.memsz, p.flags});
    }

    std::uint64_t module_id = 0;
    if (tls) {
      if (tls->filesz > tls->memsz || !is_mod_mapped(tls->vaddr, tls->memsz)) {
        if (err) *err = filename + ": unsupported TLS layout";
        return false;
      }
      module_id = tls_module++;
    }

    for (const auto& r : m.relocs) {
      if (!is_mod_mapped(r.target)) {
        if (err) *err = filename + ": relocation outside image";
        return false;
      }
      const std::uint64_t target = base + r.target;
      if (r.kind == 8) {
        relocs.push_back({target, 0, static_cast<std::int64_t>(base) + r.addend, 0});
      } else if (r.kind == 16) {
        const auto& sym = m.symbols[r.symid];
        if (!sym.name.empty() || sym.type != 3 || r.addend != 0 || !module_id) {
          if (err) *err = filename + ": unsupported external TLS relocation";
          return false;
        }
        WriteLE64At(image, static_cast<std::size_t>(target), module_id);
      } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
        const auto& sym = m.symbols[r.symid];
        if (sym.section != 0) {
          if (sym.section == 0xfff1) {
            WriteLE64At(image, static_cast<std::size_t>(target),
                        static_cast<std::uint64_t>(static_cast<std::int64_t>(sym.value) + r.addend));
          } else {
            relocs.push_back({target, 0, static_cast<std::int64_t>(base + sym.value) + r.addend, 0});
          }
        } else if (!sym.has_identity && sym.binding == 2) {
          WriteLE64At(image, static_cast<std::size_t>(target), 0);
        } else {
          const std::uint64_t rk = (sym.type == 1) ? 2 : 1;
          if ((rk == 1 && r.addend != 0) || r.addend < 0 || r.addend >= 4096) {
            if (err) *err = filename + ": unsupported import addend";
            return false;
          }
          relocs.push_back({target, rk, static_cast<std::int64_t>(imported(sym)), r.addend});
        }
      } else {
        if (err) *err = filename + ": unsupported relocation kind";
        return false;
      }
    }

    for (const auto& s : m.symbols) {
      if (s.section != 0 && (s.binding == 1 || s.binding == 2) && s.has_identity &&
          (s.type == 1 || s.type == 2)) {
        if (!is_mod_mapped(s.value, std::max<std::uint64_t>(1, s.size))) {
          if (err) *err = filename + ": export outside load segments";
          return false;
        }
        ExportEntry entry_val{base + s.value, (s.type == 1) ? 2ULL : 1ULL, filename};
        exports[s.identity] = entry_val;
        by_nid[{std::get<0>(s.identity), std::get<1>(s.identity).first}].push_back(entry_val);
      }
    }

    fs_patched += PatchFsLoads(image, m.parsed.ph, base);
    const std::uint64_t init_off = m.tags.count(12) ? m.tags[12] : 0;
    table.push_back(LinkedModRow{
        .file = filename,
        .base = base,
        .size = modsize,
        .init = base + init_off,
        .tls_address = tls ? (base + tls->vaddr) : 0,
        .tls_memsz = tls ? tls->memsz : 0,
        .tls_filesz = tls ? tls->filesz : 0,
        .tls_module = module_id,
    });
    base = (base + modsize + 65535ULL) & ~65535ULL;
  }

  struct BindingTuple {
    std::uint64_t index;
    std::uint64_t address;
    std::uint64_t kind;
  };
  std::vector<BindingTuple> bindings;
  std::size_t unresolved_count = 0;
  for (const auto& [id, index] : identities_order) {
    const ExportEntry* found = nullptr;
    auto it = exports.find(id);
    if (it != exports.end()) {
      found = &it->second;
    } else if (std::get<1>(id).first == "libSceLibcInternal") {
      auto it_nid = by_nid.find({std::get<0>(id), "libc"});
      if (it_nid != by_nid.end() && it_nid->second.size() == 1) {
        found = &it_nid->second[0];
      }
    }
    if (found) {
      bindings.push_back({index, found->address, found->kind});
    } else {
      ++unresolved_count;
    }
  }

  const ElfPhdr* main_tls = nullptr;
  const ElfPhdr* procparam = nullptr;
  for (const auto& p : main_mod.parsed.ph) {
    if (p.type == 7 && !main_tls) main_tls = &p;
    if (p.type == 0x61000001U && !procparam) procparam = &p;
  }
  if (!procparam) {
    if (err) *err = "Missing PT_SCE_PROCPARAM in eboot.bin";
    return false;
  }

  const std::uint64_t main_tls_vals[4] = {
      main_tls ? main_tls->vaddr : 0,
      main_tls ? main_tls->filesz : 0,
      main_tls ? main_tls->memsz : 0,
      main_tls ? main_tls->align : 0,
  };

  for (auto& name : names) {
    name = CanonicalImportName(name, main_mod.libraries, main_mod.modules);
  }

  // Serialize BBPROBE5 to out/boot-linked.bin
  std::vector<std::uint8_t> linked_blob;
  linked_blob.reserve(4096 + names.size() * 128 + relocs.size() * 32 + image.size());
  linked_blob.insert(linked_blob.end(), {'B', 'B', 'P', 'R', 'O', 'B', 'E', '5'});
  AppendLE<std::uint64_t>(linked_blob, image.size());
  AppendLE<std::uint64_t>(linked_blob, main_mod.parsed.entry);
  AppendLE<std::uint64_t>(linked_blob, segments.size());
  AppendLE<std::uint64_t>(linked_blob, relocs.size());
  AppendLE<std::uint64_t>(linked_blob, names.size());
  AppendLE<std::uint64_t>(linked_blob, init_env_is_ret ? 1ULL : 0ULL);
  AppendLE<std::uint64_t>(linked_blob, procparam->vaddr);
  for (int i = 0; i < 4; ++i) AppendLE<std::uint64_t>(linked_blob, main_tls_vals[i]);
  AppendLE<std::uint64_t>(linked_blob, table.size());
  for (const auto& t : table) {
    AppendLE<std::uint64_t>(linked_blob, t.base);
    AppendLE<std::uint64_t>(linked_blob, t.size);
    AppendLE<std::uint64_t>(linked_blob, t.init);
    AppendLE<std::uint64_t>(linked_blob, t.tls_address);
    AppendLE<std::uint64_t>(linked_blob, t.tls_memsz);
    AppendLE<std::uint64_t>(linked_blob, t.tls_filesz);
    AppendLE<std::uint64_t>(linked_blob, t.tls_module);
  }
  AppendLE<std::uint64_t>(linked_blob, bindings.size());
  for (const auto& b : bindings) {
    AppendLE<std::uint64_t>(linked_blob, b.index);
    AppendLE<std::uint64_t>(linked_blob, b.address);
    AppendLE<std::uint64_t>(linked_blob, b.kind);
  }
  for (const auto& s : segments) {
    AppendLE<std::uint64_t>(linked_blob, s.vaddr);
    AppendLE<std::uint64_t>(linked_blob, s.memsz);
    AppendLE<std::uint64_t>(linked_blob, s.flags);
  }
  for (const auto& n : names) {
    if (n.size() >= 128) {
      if (err) *err = "Import name too long: " + n;
      return false;
    }
    char slot[128]{};
    std::memcpy(slot, n.data(), n.size());
    linked_blob.insert(linked_blob.end(), slot, slot + 128);
  }
  for (const auto& r : relocs) {
    AppendLE<std::uint64_t>(linked_blob, r.target);
    AppendLE<std::uint64_t>(linked_blob, r.kind);
    AppendLE<std::int64_t>(linked_blob, r.value);
    AppendLE<std::int64_t>(linked_blob, r.addend);
  }
  linked_blob.insert(linked_blob.end(), image.begin(), image.end());

  if (!WriteFileBytes(out_dir / "boot-linked.bin", linked_blob.data(), linked_blob.size(), err)) {
    return false;
  }

  if (log) {
    std::ostringstream oss;
    oss << "Linked modules: ";
    for (std::size_t i = 0; i < table.size(); ++i) {
      if (i > 0) oss << ", ";
      oss << table[i].file << "@0x" << std::hex << table[i].base << std::dec;
    }
    oss << "; " << bindings.size() << " native bindings, " << unresolved_count
        << " imports left to the host runtime, fs->gs patched=" << fs_patched;
    log(oss.str());
  }

  // Write out/content.bin (BBCONT01)
  std::uint8_t content_bin[28]{};
  std::memcpy(content_bin, "BBCONT01", 8);
  const std::uint32_t sku_val = (sku == "trial") ? 1U : 3U;
  std::memcpy(content_bin + 8, &sku_val, 4);
  std::uint32_t params[4]{};
  for (int i = 0; i < 4; ++i) {
    const std::string key = "USER_DEFINED_PARAM_" + std::to_string(i + 1);
    params[i] = sfo.ints.count(key) ? sfo.ints[key] : 0U;
    std::memcpy(content_bin + 12 + i * 4, &params[i], 4);
  }
  if (!WriteFileBytes(out_dir / "content.bin", content_bin, sizeof(content_bin), err)) {
    return false;
  }
  if (log) {
    std::ostringstream oss;
    oss << "AppContent profile: SKU=" << sku << " (probe setting), user params=["
        << params[0] << ", " << params[1] << ", " << params[2] << ", " << params[3]
        << "], mounted add-ons=0";
    log(oss.str());
  }
  return true;
}

// ============================================================================
// 3. Patches Compiler (patches.py -> out/patches.bin in BBPATCH2 format)
// ============================================================================

std::map<std::string, std::string> ReadIniSettings(const fs::path& path) {
  std::map<std::string, std::string> settings;
  std::ifstream ifs(path);
  if (!ifs) return settings;
  std::string line;
  while (std::getline(ifs, line)) {
    const std::string t = Trim(line);
    if (t.empty() || t[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq != std::string::npos) {
      settings[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
  }
  return settings;
}

std::pair<int, int> GetOutputSize(const std::map<std::string, std::string>& settings,
                                  const std::map<std::string, std::string>& env) {
  std::string val = settings.count("output_res") ? ToLower(settings.at("output_res")) : "auto";
  if (val == "auto") {
    val = env.count("BB_MONITOR_SIZE") ? env.at("BB_MONITOR_SIZE") : "";
  }
  const auto x = val.find('x');
  if (x != std::string::npos) {
    try {
      const int w = std::stoi(val.substr(0, x));
      const int h = std::stoi(val.substr(x + 1));
      if (w > 0 && h > 0) {
        return {std::min(w, 7680), std::min(h, 4320)};
      }
    } catch (...) {
    }
  }
  return {kOutputWidth, kOutputHeight};
}

std::optional<std::pair<std::pair<int, int>, std::pair<int, int>>> ScaledSizes(
    const std::map<std::string, std::string>& settings,
    const std::map<std::string, std::string>& env,
    int align = 2) {
  const auto out = GetOutputSize(settings, env);
  const std::string upscaler = settings.count("upscaler") ? settings.at("upscaler") : "fsr3";
  if (upscaler == "taa" || (out.first == kOutputWidth && out.second == kOutputHeight && align != 8)) {
    return std::nullopt;
  }
  double scale = 1.0;
  if (upscaler != "off") {
    int preset = 0;
    if (settings.count("preset") && !settings.at("preset").empty()) {
      try {
        preset = std::stoi(settings.at("preset"));
      } catch (...) {
        preset = 0;
      }
    }
    preset = std::clamp(preset, 0, static_cast<int>(kPresetScales.size()) - 1);
    scale = kPresetScales[static_cast<std::size_t>(preset)];
  }
  auto round_align = [align, scale](int v) {
    return std::max(align, static_cast<int>(std::llround(double(v) / scale / double(align))) * align);
  };
  std::pair<int, int> render{round_align(out.first), round_align(out.second)};
  if (render.first == kOutputWidth && render.second == kOutputHeight) {
    const std::string fg = settings.count("frame_generation") ? settings.at("frame_generation") : "off";
    const bool has_fg = (fg != "off" && fg != "0" && !fg.empty());
    if (out.first == kOutputWidth && out.second == kOutputHeight && !has_fg) {
      return std::nullopt;
    }
    render = (align == 8) ? std::pair<int, int>{1912, 1072} : std::pair<int, int>{1916, 1078};
  }
  return std::make_pair(render, out);
}

std::optional<std::pair<int, int>> RenderSize(const std::map<std::string, std::string>& settings,
                                              const std::string& override_res) {
  if (!override_res.empty()) {
    const std::string low = ToLower(override_res);
    const auto x = low.find('x');
    if (x != std::string::npos) {
      try {
        return std::make_pair(std::stoi(low.substr(0, x)), std::stoi(low.substr(x + 1)));
      } catch (...) {
      }
    }
  }
  const std::string upscaler = settings.count("upscaler") ? settings.at("upscaler") : "fsr3";
  if (upscaler == "off") return std::nullopt;
  int preset = 0;
  if (settings.count("preset") && !settings.at("preset").empty()) {
    try {
      preset = std::stoi(settings.at("preset"));
    } catch (...) {
    }
  }
  preset = std::clamp(preset, 0, static_cast<int>(kPresetScales.size()) - 1);
  const double scale = kPresetScales[static_cast<std::size_t>(preset)];
  if (scale == 1.0) return std::nullopt;
  return std::make_pair(
      std::max(2, static_cast<int>(std::llround(double(kOutputWidth) / scale / 2.0)) * 2),
      std::max(2, static_cast<int>(std::llround(double(kOutputHeight) / scale / 2.0)) * 2));
}

bool IsIntelCpu(const std::map<std::string, std::string>& env) {
  std::ifstream cpuinfo("/proc/cpuinfo");
  if (cpuinfo) {
    std::string line;
    while (std::getline(cpuinfo, line)) {
      if (line.rfind("vendor_id", 0) == 0 && line.find("GenuineIntel") != std::string::npos) {
        return true;
      }
    }
    return false;
  }
  auto it = env.find("PROCESSOR_IDENTIFIER");
  return it != env.end() && it->second.find("GenuineIntel") != std::string::npos;
}

struct XmlPatchLine {
  std::string type;
  std::string address;
  std::string value;
};

struct XmlMetadataBlock {
  std::string name;
  std::string app_ver;
  std::string app_elf = "eboot.bin";
  std::string is_enabled = "false";
  std::vector<XmlPatchLine> lines;
};

std::string ExtractXmlAttr(std::string_view tag, std::string_view attr_name) {
  std::string needle = std::string(attr_name) + "=\"";
  auto pos = tag.find(needle);
  if (pos == std::string_view::npos) return {};
  pos += needle.size();
  auto end = tag.find('"', pos);
  if (end == std::string_view::npos) return {};
  std::string raw(tag.substr(pos, end - pos));
  // Unescape basic XML entities
  auto replace_all = [&](const char* from, const char* to) {
    std::size_t p = 0;
    const std::size_t flen = std::strlen(from);
    const std::size_t tlen = std::strlen(to);
    while ((p = raw.find(from, p)) != std::string::npos) {
      raw.replace(p, flen, to);
      p += tlen;
    }
  };
  replace_all("&quot;", "\"");
  replace_all("&apos;", "'");
  replace_all("&lt;", "<");
  replace_all("&gt;", ">");
  replace_all("&amp;", "&");
  return raw;
}

std::vector<XmlMetadataBlock> ParsePatchXmlFile(const fs::path& xml_path,
                                                std::set<std::string>* out_title_ids = nullptr) {
  std::vector<XmlMetadataBlock> blocks;
  std::ifstream ifs(xml_path);
  if (!ifs) return blocks;
  std::ostringstream ss;
  ss << ifs.rdbuf();
  const std::string content = ss.str();

  if (out_title_ids) {
    std::size_t id_pos = 0;
    while ((id_pos = content.find("<ID>", id_pos)) != std::string::npos) {
      id_pos += 4;
      auto end_id = content.find("</ID>", id_pos);
      if (end_id == std::string::npos) break;
      out_title_ids->insert(Trim(std::string_view(content).substr(id_pos, end_id - id_pos)));
      id_pos = end_id + 5;
    }
  }

  std::size_t pos = 0;
  while ((pos = content.find("<Metadata", pos)) != std::string::npos) {
    const auto tag_end = content.find('>', pos);
    if (tag_end == std::string::npos) break;
    const std::string_view meta_tag(content.data() + pos, tag_end - pos + 1);
    XmlMetadataBlock blk;
    blk.name = ExtractXmlAttr(meta_tag, "Name");
    blk.app_ver = ExtractXmlAttr(meta_tag, "AppVer");
    const std::string elf = ExtractXmlAttr(meta_tag, "AppElf");
    blk.app_elf = elf.empty() ? "eboot.bin" : elf;
    const std::string en = ExtractXmlAttr(meta_tag, "isEnabled");
    blk.is_enabled = en.empty() ? "false" : en;

    if (meta_tag.size() >= 2 && meta_tag[meta_tag.size() - 2] == '/') {
      blocks.push_back(std::move(blk));
      pos = tag_end + 1;
      continue;
    }

    const auto close_meta = content.find("</Metadata>", tag_end);
    if (close_meta == std::string::npos) break;

    std::size_t lpos = tag_end + 1;
    while ((lpos = content.find("<Line", lpos)) != std::string::npos && lpos < close_meta) {
      const auto l_end = content.find('>', lpos);
      if (l_end == std::string::npos || l_end > close_meta) break;
      const std::string_view line_tag(content.data() + lpos, l_end - lpos + 1);
      XmlPatchLine line;
      line.type = ExtractXmlAttr(line_tag, "Type");
      line.address = ExtractXmlAttr(line_tag, "Address");
      line.value = ExtractXmlAttr(line_tag, "Value");
      if (!line.type.empty() && !line.address.empty()) {
        blk.lines.push_back(std::move(line));
      }
      lpos = l_end + 1;
    }
    blocks.push_back(std::move(blk));
    pos = close_meta + 11;
  }
  return blocks;
}

bool EncodePatchLine(const XmlPatchLine& line, std::vector<std::uint8_t>& out, std::string* err) {
  out.clear();
  if (line.type == "bytes") {
    std::string hex;
    for (char c : line.value) {
      if (!std::isspace(static_cast<unsigned char>(c))) hex.push_back(c);
    }
    if (hex.size() % 2 != 0) {
      if (err) *err = "Invalid hex byte string";
      return false;
    }
    out.resize(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
      out[i] = static_cast<std::uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    return true;
  }
  if (line.type == "bytes16" || line.type == "bytes32" || line.type == "bytes64") {
    const int nbytes = std::stoi(line.type.substr(5)) / 8;
    std::uint64_t val = 0;
    const std::string v = Trim(line.value);
    if (!v.empty() && v[0] == '-') {
      val = static_cast<std::uint64_t>(std::stoll(v, nullptr, 0));
    } else {
      val = std::stoull(v, nullptr, 0);
    }
    out.resize(static_cast<std::size_t>(nbytes));
    for (int i = 0; i < nbytes; ++i) {
      out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((val >> (i * 8)) & 0xFFU);
    }
    return true;
  }
  if (line.type == "float32") {
    const float f = std::stof(line.value);
    out.resize(4);
    std::memcpy(out.data(), &f, 4);
    return true;
  }
  if (line.type == "float64") {
    const double d = std::stod(line.value);
    out.resize(8);
    std::memcpy(out.data(), &d, 8);
    return true;
  }
  if (line.type == "utf8") {
    out.assign(line.value.begin(), line.value.end());
    out.push_back(0);
    return true;
  }
  if (line.type == "utf16") {
    for (unsigned char c : line.value) {
      out.push_back(c);
      out.push_back(0);
    }
    out.push_back(0);
    out.push_back(0);
    return true;
  }
  if (err) *err = "Unsupported patch type: " + line.type;
  return false;
}

using PatchWrite = std::pair<std::uint64_t, std::vector<std::uint8_t>>;

bool CompileNamedPatches(const std::vector<XmlMetadataBlock>& blocks,
                         const std::vector<std::string>& names,
                         const std::string& app_version,
                         const std::vector<std::pair<std::uint64_t, std::uint64_t>>& segments,
                         std::vector<PatchWrite>& out_writes,
                         std::string* err) {
  std::map<std::string, const XmlMetadataBlock*> found;
  for (const auto& b : blocks) {
    if (b.app_ver == app_version && b.app_elf == "eboot.bin") {
      if (std::find(names.begin(), names.end(), b.name) != names.end()) {
        found[b.name] = &b;
      }
    }
  }
  for (const auto& n : names) {
    if (!found.count(n)) {
      if (err) *err = "Patch not found for app version " + app_version + ": " + n;
      return false;
    }
  }
  for (const auto& n : names) {
    const auto* blk = found[n];
    for (const auto& line : blk->lines) {
      const std::uint64_t addr = std::stoull(Trim(line.address), nullptr, 0);
      if (addr < kEbootBase) {
        if (err) *err = n + ": address below eboot base";
        return false;
      }
      const std::uint64_t offset = addr - kEbootBase;
      std::vector<std::uint8_t> data;
      if (!EncodePatchLine(line, data, err)) return false;
      bool in_seg = false;
      for (const auto& [start, end] : segments) {
        if (start <= offset && offset + data.size() <= end) {
          in_seg = true;
          break;
        }
      }
      if (!in_seg) {
        if (err) *err = n + ": address " + line.address + " is outside the eboot";
        return false;
      }
      out_writes.emplace_back(offset, std::move(data));
    }
  }
  return true;
}

bool CompileAllPatches(const fs::path& out_dir,
                       const std::string& fps,
                       const std::string& extra,
                       const fs::path& settings_path,
                       const fs::path& game_dir,
                       const std::string& render_res,
                       const fs::path& patches_dir,
                       const fs::path& patches_config,
                       bool large_heaps,
                       const fs::path& xml_path,
                       const std::map<std::string, std::string>& env,
                       const NativeLaunchPlanner::LogCallback& log,
                       std::string* err) {
  auto settings = ReadIniSettings(settings_path);
  std::vector<std::string> names;

  const std::string frame_hooks = env.count("BB_FRAME_HOOKS") ? env.at("BB_FRAME_HOOKS") : "1";
  if (fps == "30" || frame_hooks == "0") {
    if (fps == "60") {
      names.push_back("60 FPS++");
    } else if (fps == "90") {
      names.push_back("90 FPS++");
      names.push_back("Sprint Fix (High FPS)");
    } else if (fps == "uncap") {
      names.push_back("Uncap FPS++");
      names.push_back("Sprint Fix (High FPS)");
    }
  }

  std::stringstream ss(extra);
  std::string item;
  while (std::getline(ss, item, ';')) {
    const std::string t = Trim(item);
    if (!t.empty()) names.push_back(t);
  }

  // Effect patches (mirrors patches.py::effect_patches)
  const bool hot = (!env.count("BB_HOT_EFFECTS") || env.at("BB_HOT_EFFECTS") != "0");
  bool game_aa = true;
  bool fxaa = false;
  if (settings.count("anti_aliasing")) {
    const std::string mode = settings["anti_aliasing"];
    const bool upscaling = (!settings.count("upscaler") || settings["upscaler"] != "off");
    game_aa = (mode == "game" || mode == "fxaa" || (mode == "auto" && !upscaling));
    fxaa = (mode == "fxaa");
  } else {
    game_aa = (!settings.count("effect_game_aa") || settings["effect_game_aa"] == "1");
  }
  settings["effect_game_aa"] = game_aa ? "1" : "0";
  if (fxaa && !hot && std::find(names.begin(), names.end(), "FXAA2_GS") == names.end()) {
    names.push_back("FXAA2_GS");
  }

  struct EffectRule {
    const char* key;
    const char* off_patch;
    const char* on_patch;
    bool is_hot;
  };
  static constexpr std::array<EffectRule, 11> kEffectRules = {{
      {"effect_chromatic_aberration", "Disable Chromatic Aberration", nullptr, true},
      {"effect_dof", "Disable DoF", nullptr, true},
      {"effect_motion_blur", "Disable Motion Blur (perf increase)", nullptr, true},
      {"effect_ssao", "Disable SSAO", nullptr, true},
      {"effect_game_aa", "Disable AA", nullptr, true},
      {"effect_dynamic_shadows", "Disable Dynamic Light Shadows (perf increase)", nullptr, true},
      {"effect_ssr", nullptr, "Enable Screen Space Reflections (READ NOTE)", false},
      {"effect_bloom", "Disable Bloom", nullptr, true},
      {"skip_intro", nullptr, "Skip Intro", false},
      {"debug_camera", nullptr, "Restore Debug Camera", false},
      {"debug_menu", nullptr, "Restore Debug Menu (READ NOTES)", false},
  }};
  for (const auto& rule : kEffectRules) {
    if (!settings.count(rule.key)) continue;
    if (hot && rule.is_hot) continue;
    const char* chosen = (settings[rule.key] == "1") ? rule.on_patch : rule.off_patch;
    if (chosen && std::find(names.begin(), names.end(), chosen) == names.end()) {
      names.push_back(chosen);
    }
  }

  const std::string lod = settings.count("model_lod") ? settings["model_lod"] : "0";
  const char* lod_patch = (lod == "-2")  ? "Model LOD -2 (Highest)"
                        : (lod == "1")   ? "Model LOD 1 (Lower)"
                        : (lod == "2")   ? "Model LOD 2 (Lowest)"
                                         : nullptr;
  if (lod_patch && std::find(names.begin(), names.end(), lod_patch) == names.end()) {
    names.push_back(lod_patch);
  }

  // Intel tonemap fix
  bool intel_fix = false;
  if (env.count("BB_INTEL_TONEMAP_FIX") &&
      (env.at("BB_INTEL_TONEMAP_FIX") == "0" || env.at("BB_INTEL_TONEMAP_FIX") == "1")) {
    intel_fix = (env.at("BB_INTEL_TONEMAP_FIX") == "1");
  } else {
    intel_fix = IsIntelCpu(env);
  }
  if (intel_fix && std::find(names.begin(), names.end(), "Intel Black Tonemap Fix") == names.end()) {
    names.push_back("Intel Black Tonemap Fix");
  }

  if ((!env.count("BB_SKIP_NETWORK_CHOICE") || env.at("BB_SKIP_NETWORK_CHOICE") != "0") &&
      std::find(names.begin(), names.end(), "Skip Online/Offline Choice") == names.end()) {
    names.push_back("Skip Online/Offline Choice");
  }
  if ((!env.count("BB_GX_COMMAND_HEAP") || env.at("BB_GX_COMMAND_HEAP") != "0") &&
      std::find(names.begin(), names.end(), "Larger GX Per-Frame Command Heap") == names.end()) {
    names.push_back("Larger GX Per-Frame Command Heap");
  }

  // Check debug menu font files if enabled
  auto debug_it = std::find(names.begin(), names.end(), "Restore Debug Menu (READ NOTES)");
  if (debug_it != names.end()) {
    std::error_code ec;
    if (!fs::is_regular_file(game_dir / "dvdroot_ps4" / "adhoc" / "font" / "DbgFont14h.ccm", ec)) {
      names.erase(debug_it);
    }
  }

  std::vector<std::uint8_t> elf;
  if (!ReadFileBytes(out_dir / "eboot.elf", elf, err)) return false;
  if (elf.size() < 64) {
    if (err) *err = "eboot.elf too small";
    return false;
  }

  const std::uint64_t phoff = ReadLE<std::uint64_t>(elf.data() + 0x20);
  const std::uint16_t phentsize = ReadLE<std::uint16_t>(elf.data() + 0x36);
  const std::uint16_t phnum = ReadLE<std::uint16_t>(elf.data() + 0x38);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> segments;
  for (std::uint16_t i = 0; i < phnum; ++i) {
    const std::uint8_t* p = elf.data() + phoff + static_cast<std::size_t>(i) * phentsize;
    const std::uint32_t kind = ReadLE<std::uint32_t>(p + 0);
    const std::uint64_t vaddr = ReadLE<std::uint64_t>(p + 16);
    const std::uint64_t memsz = ReadLE<std::uint64_t>(p + 40);
    if (kind == 1) segments.emplace_back(vaddr, vaddr + memsz);
  }

  const auto xml_blocks = ParsePatchXmlFile(xml_path);
  std::vector<PatchWrite> writes;
  if (!CompileNamedPatches(xml_blocks, names, "01.09", segments, writes, err)) return false;

  // Language table writes (mirrors patches.py::language_writes)
  static constexpr std::array<const char*, 21> kGameLangs = {
      "jajp", "engb", "frfr", "itit", "dede", "eses", "esar", "ptbr", "enus", "nlnl", "plpl",
      "ruru", "dadk", "nono", "fifi", "svse", "trtr", "ptpt", "zhtw", "kokr", "zhcn",
  };
  static constexpr std::array<std::int32_t, 21> kEuExpected = {
      1, 1, 2, 3, 4, 5, 5, 17, 1, 9, 10, 11, 12, 13, 14, 15, 16, 17, 1, 1, 1,
  };
  static constexpr std::array<std::uint64_t, 6> kTextTables = {
      0x47366b0, 0x4736720, 0x47367c0, 0x4736830, 0x4736890, 0x4736950,
  };
  static constexpr std::array<std::uint64_t, 6> kVoiceTables = {
      0x47369b0, 0x4736a20, 0x4736a80, 0x4736ae0, 0x4736b40, 0x4736ba0,
  };

  if (elf.size() >= 0x4000 + 0x47367c0 + 84 &&
      std::memcmp(elf.data() + 0x4000 + 0x47367c0, kEuExpected.data(), 84) == 0) {
    std::string text = settings.count("game_text_language") ? settings["game_text_language"] : "auto";
    std::string voice = settings.count("game_voice_language") ? settings["game_voice_language"] : "auto";
    int text_idx = -1;
    int voice_idx = -1;
    for (int i = 0; i < 21; ++i) {
      if (text == kGameLangs[static_cast<std::size_t>(i)]) text_idx = i;
      if (i < 8 && voice == kGameLangs[static_cast<std::size_t>(i)]) voice_idx = i;
    }
    if (text != "auto" && text_idx < 0) text = "auto";
    if (voice != "auto" && voice_idx < 0) voice = "auto";

    std::vector<std::uint8_t> text_blob(84);
    std::vector<std::uint8_t> voice_blob(84);
    for (int i = 0; i < 21; ++i) {
      const std::int32_t tv = (text_idx >= 0) ? text_idx : i;
      const std::int32_t vv = (voice_idx >= 0) ? voice_idx : ((i < 8) ? i : 1);
      std::memcpy(text_blob.data() + i * 4, &tv, 4);
      std::memcpy(voice_blob.data() + i * 4, &vv, 4);
    }
    for (std::uint64_t site : kTextTables) writes.emplace_back(site, text_blob);
    for (std::uint64_t site : kVoiceTables) writes.emplace_back(site, voice_blob);
    if (log) {
      log("Patches: game languages: text " + text + ", voice " + voice + " (any sell region)");
    }
  }

  // Resolution writes
  const auto size_opt = !render_res.empty() ? RenderSize(settings, render_res) : std::nullopt;
  if (size_opt) {
    std::vector<PatchWrite> res_writes;
    if (!CompileNamedPatches(xml_blocks, {"Resolution Patch 1280x720 (16:9)"}, "01.09",
                             segments, res_writes, err)) {
      return false;
    }
    constexpr std::uint64_t kSceneW = 0x02196A6B - kEbootBase;
    constexpr std::uint64_t kSceneH = 0x02196A7A - kEbootBase;
    constexpr std::uint64_t kUiW = 0x02358554 - kEbootBase;
    constexpr std::uint64_t kUiH = 0x0235855D - kEbootBase;
    for (auto& [off, data] : res_writes) {
      auto patch_imm = [&](std::uint8_t op, int val) {
        data = {op,
                static_cast<std::uint8_t>(val & 0xFF),
                static_cast<std::uint8_t>((val >> 8) & 0xFF),
                static_cast<std::uint8_t>((val >> 16) & 0xFF)};
      };
      if (off == kSceneW) patch_imm(0xB8, size_opt->first);
      else if (off == kSceneH) patch_imm(0xB8, size_opt->second);
      else if (off == kUiW) patch_imm(0xB8, kOutputWidth);
      else if (off == kUiH) patch_imm(0xB9, kOutputHeight);
      writes.emplace_back(off, std::move(data));
    }
  }

  // Graphics heaps writes
  const bool need_heaps = large_heaps ||
      (size_opt && (size_opt->first * size_opt->second > kOutputWidth * kOutputHeight));
  if (need_heaps &&
      std::find(names.begin(), names.end(), "Increased Graphics Heap Sizes") == names.end()) {
    auto elf_read_u64 = [&](std::uint64_t vaddr, std::uint64_t& out_val) -> bool {
      for (std::uint16_t i = 0; i < phnum; ++i) {
        const std::uint8_t* p = elf.data() + phoff + static_cast<std::size_t>(i) * phentsize;
        const std::uint32_t kind = ReadLE<std::uint32_t>(p + 0);
        const std::uint64_t offset = ReadLE<std::uint64_t>(p + 8);
        const std::uint64_t start = ReadLE<std::uint64_t>(p + 16);
        const std::uint64_t filesz = ReadLE<std::uint64_t>(p + 32);
        if (kind == 1 && start <= vaddr && vaddr + 8 <= start + filesz) {
          out_val = ReadLE<std::uint64_t>(elf.data() + offset + (vaddr - start));
          return true;
        }
      }
      return false;
    };

    const double scale = large_heaps ? 1.0
        : std::min(1.0, std::max(0.0, (double(size_opt->first) * double(size_opt->second) /
                                           double(kOutputWidth * kOutputHeight) - 1.0) / 3.0));
    struct HeapSpec {
      std::uint64_t addr;
      std::uint64_t orig;
      std::uint64_t full;
    };
    static constexpr std::array<HeapSpec, 2> kHeaps = {{
        {0x04b36dd8ULL, 0x8ef00000ULL, 0xf7000000ULL},
        {0x04b36de0ULL, 0x37600000ULL, 0xa0000000ULL},
    }};
    std::uint64_t cur0 = 0, cur1 = 0;
    if (elf_read_u64(kHeaps[0].addr - kEbootBase, cur0) && cur0 == kHeaps[0].orig &&
        elf_read_u64(kHeaps[1].addr - kEbootBase, cur1) && cur1 == kHeaps[1].orig) {
      for (const auto& h : kHeaps) {
        const std::uint64_t granule = 16ULL << 20;
        const std::uint64_t delta = static_cast<std::uint64_t>(double(h.full - h.orig) * scale);
        const std::uint64_t grown =
            std::min(h.full, h.orig + ((delta + granule - 1) / granule) * granule);
        std::vector<std::uint8_t> d(8);
        std::memcpy(d.data(), &grown, 8);
        writes.emplace_back(h.addr - kEbootBase, std::move(d));
      }
    } else {
      CompileNamedPatches(xml_blocks, {"Increased Graphics Heap Sizes"}, "01.09", segments, writes, err);
      names.push_back("Increased Graphics Heap Sizes");
    }
  }

  if (size_opt && log) {
    log("Patches: scene " + std::to_string(size_opt->first) + "x" +
        std::to_string(size_opt->second) + "; UI " + std::to_string(kOutputWidth) + "x" +
        std::to_string(kOutputHeight));
  }

  // External XML patches in patches_dir
  std::error_code ec;
  if (!patches_dir.empty() && fs::is_directory(patches_dir, ec)) {
    std::set<std::string> enabled_ext;
    std::set<std::string> disabled_ext;
    if (fs::is_regular_file(patches_config, ec)) {
      try {
        std::ifstream cfg_ifs(patches_config);
        nlohmann::json j = nlohmann::json::parse(cfg_ifs);
        if (j.contains("enabled") && j["enabled"].is_array()) {
          for (const auto& e : j["enabled"]) {
            if (e.is_string()) enabled_ext.insert(e.get<std::string>());
          }
        }
        if (j.contains("disabled") && j["disabled"].is_array()) {
          for (const auto& d : j["disabled"]) {
            if (d.is_string()) disabled_ext.insert(d.get<std::string>());
          }
        }
      } catch (...) {
      }
    }

    const fs::path canonical_builtin = fs::weakly_canonical(xml_path, ec);
    std::vector<fs::path> xml_files;
    for (const auto& entry : fs::directory_iterator(patches_dir, ec)) {
      if (entry.is_regular_file(ec) && entry.path().extension() == ".xml") {
        if (fs::weakly_canonical(entry.path(), ec) != canonical_builtin) {
          xml_files.push_back(entry.path());
        }
      }
    }
    std::sort(xml_files.begin(), xml_files.end());

    for (const auto& ext_xml : xml_files) {
      std::set<std::string> ids;
      const auto ext_blocks = ParsePatchXmlFile(ext_xml, &ids);
      for (const auto& blk : ext_blocks) {
        if (blk.app_ver != "01.09" || blk.app_elf != "eboot.bin") continue;
        const std::string key = ext_xml.filename().string() + "/" + blk.name;
        const bool is_sel = enabled_ext.count(key) ||
                            (!disabled_ext.count(key) && ToLower(blk.is_enabled) == "true");
        if (!is_sel) continue;

        std::vector<PatchWrite> ours;
        bool ok = true;
        for (const auto& line : blk.lines) {
          const std::uint64_t addr = std::stoull(Trim(line.address), nullptr, 0);
          if (addr < kEbootBase) { ok = false; break; }
          const std::uint64_t offset = addr - kEbootBase;
          std::vector<std::uint8_t> data;
          if (!EncodePatchLine(line, data, nullptr)) { ok = false; break; }
          bool in_seg = false;
          for (const auto& [start, end] : segments) {
            if (start <= offset && offset + data.size() <= end) { in_seg = true; break; }
          }
          if (!in_seg) { ok = false; break; }
          ours.emplace_back(offset, std::move(data));
        }
        if (ok) {
          writes.insert(writes.end(), ours.begin(), ours.end());
          if (log) log("Patches: external " + key + " (" + std::to_string(ours.size()) + " writes)");
        }
      }
    }
  }

  // Serialize BBPATCH2
  std::vector<std::uint8_t> blob;
  blob.reserve(24 + writes.size() * 24);
  blob.insert(blob.end(), {'B', 'B', 'P', 'A', 'T', 'C', 'H', '2'});
  AppendLE<std::uint64_t>(blob, kEbootBase);
  AppendLE<std::uint64_t>(blob, writes.size());
  for (const auto& [off, data] : writes) {
    AppendLE<std::uint64_t>(blob, off);
    AppendLE<std::uint64_t>(blob, data.size());
    blob.insert(blob.end(), data.begin(), data.end());
  }
  if (!WriteFileBytes(out_dir / "patches.bin", blob.data(), blob.size(), err)) return false;

  if (log) {
    std::ostringstream oss;
    oss << "Patches: FPS preset " << fps << "; " << writes.size() << " writes from [";
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (i > 0) oss << ", ";
      oss << "'" << names[i] << "'";
    }
    oss << "]";
    log(oss.str());
  }
  return true;
}

// ============================================================================
// 4. Monitor Resolution Query via bb-gpu-capabilities --displays
// ============================================================================

std::string QueryMonitorResolution(const fs::path& caps_path, const std::string& wanted_display) {
  std::error_code ec;
  if (caps_path.empty() || !fs::is_regular_file(caps_path, ec)) return {};
  const std::string cmd = "\"" + caps_path.string() + "\" --displays";
#ifdef _WIN32
  FILE* pipe = ::_popen(cmd.c_str(), "r");
#else
  FILE* pipe = ::popen(cmd.c_str(), "r");
#endif
  if (!pipe) return {};
  std::string out;
  char buf[512];
  while (std::size_t n = std::fread(buf, 1, sizeof(buf), pipe)) {
    out.append(buf, n);
  }
#ifdef _WIN32
  ::_pclose(pipe);
#else
  ::pclose(pipe);
#endif

  std::istringstream iss(out);
  std::string row;
  std::string chosen;
  int number = 0;
  while (std::getline(iss, row)) {
    ++number;
    const auto t1 = row.find('\t');
    if (t1 == std::string::npos) continue;
    const auto t2 = row.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    const std::string name = row.substr(0, t1);
    const std::string size = row.substr(t1 + 1, t2 - t1 - 1);
    const std::string primary = Trim(row.substr(t2 + 1));
    if (!wanted_display.empty()) {
      if (wanted_display == name || wanted_display == std::to_string(number)) {
        chosen = size;
      }
    } else if (primary == "1") {
      chosen = size;
    }
  }
  const auto x = chosen.find('x');
  if (x == std::string::npos || x == 0 || x + 1 >= chosen.size()) return {};
  return chosen;
}

// ============================================================================
// 5. Mods Overlay Builder (mods.py)
// ============================================================================

const std::set<std::string>& GameDvdrootFolders() {
  static const std::set<std::string> kFolders = {
      "action", "adhoc",    "chr",    "event", "facegen", "font",   "map",
      "menu",   "movie",    "msg",    "mtd",   "obj",     "other",  "param",
      "paramdef", "parts",  "remo",   "script", "sfx",    "shader", "sound",
  };
  return kFolders;
}

fs::path ChildIgnoreCase(const fs::path& folder, std::string_view name) {
  std::error_code ec;
  const std::string target = ToLower(name);
  if (fs::is_directory(folder, ec)) {
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
      if (ToLower(entry.path().filename().string()) == target) {
        return entry.path();
      }
    }
  }
  return folder / std::string(name);
}

std::optional<std::pair<fs::path, std::string>> ModContentRoot(const fs::path& folder, int depth = 0) {
  if (depth > 2) return std::nullopt;
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) return std::nullopt;

  static constexpr std::array<const char*, 11> kWrappers = {
      "", "app0", "CUSA03173", "CUSA00900", "CUSA00207", "CUSA00208",
      "CUSA00299", "CUSA01363", "CUSA03179", "CUSA03014", "CUSA03023",
  };
  for (const char* w : kWrappers) {
    const fs::path base = (w[0] != '\0') ? ChildIgnoreCase(folder, w) : folder;
    const fs::path dvdroot = ChildIgnoreCase(base, "dvdroot_ps4");
    if (fs::is_directory(dvdroot, ec)) {
      return std::make_pair(base, std::string{});
    }
  }

  std::vector<fs::path> subdirs;
  bool all_game_folders = true;
  bool has_non_doc_files = false;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    const std::string fname = entry.path().filename().string();
    if (fname.empty() || fname[0] == '.') continue;
    if (entry.is_directory(ec)) {
      subdirs.push_back(entry.path());
      if (!GameDvdrootFolders().count(ToLower(fname))) {
        all_game_folders = false;
      }
    } else if (entry.is_regular_file(ec)) {
      const std::string ext = ToLower(entry.path().extension().string());
      if (ext != ".txt" && ext != ".md" && ext != ".jpg" && ext != ".png" && ext != ".ini") {
        has_non_doc_files = true;
      }
    }
  }
  if (!subdirs.empty() && all_game_folders) {
    return std::make_pair(folder, std::string("dvdroot_ps4"));
  }
  if (subdirs.size() == 1 && !has_non_doc_files) {
    return ModContentRoot(subdirs[0], depth + 1);
  }
  return std::nullopt;
}

std::vector<std::string> SelectedMods(const fs::path& mods_dir, const fs::path& config_path) {
  std::error_code ec;
  std::vector<std::string> available;
  if (fs::is_directory(mods_dir, ec)) {
    for (const auto& entry : fs::directory_iterator(mods_dir, ec)) {
      if (entry.is_directory(ec) && ModContentRoot(entry.path()).has_value()) {
        available.push_back(entry.path().filename().string());
      }
    }
  }
  std::sort(available.begin(), available.end(), [](const std::string& a, const std::string& b) {
    const std::string la = ToLower(a);
    const std::string lb = ToLower(b);
    return (la != lb) ? (la < lb) : (a < b);
  });

  if (config_path.empty() || !fs::is_regular_file(config_path, ec)) {
    return available;
  }

  std::set<std::string> disabled;
  std::vector<std::string> order;
  try {
    std::ifstream ifs(config_path);
    nlohmann::json j = nlohmann::json::parse(ifs);
    if (j.contains("disabled") && j["disabled"].is_array()) {
      for (const auto& d : j["disabled"]) {
        if (d.is_string()) disabled.insert(d.get<std::string>());
      }
    }
    if (j.contains("order") && j["order"].is_array()) {
      for (const auto& o : j["order"]) {
        if (o.is_string()) order.push_back(o.get<std::string>());
      }
    }
  } catch (...) {
    return available;
  }

  std::vector<std::string> result;
  std::set<std::string> seen;
  const std::set<std::string> avail_set(available.begin(), available.end());
  for (const auto& list : {order, available}) {
    for (const auto& n : list) {
      if (avail_set.count(n) && !disabled.count(n) && seen.insert(n).second) {
        result.push_back(n);
      }
    }
  }
  return result;
}

void MakeLinkOrCopy(const fs::path& link_path, const fs::path& target) {
  std::error_code ec;
  if (fs::is_directory(target, ec)) {
    fs::create_directory_symlink(target, link_path, ec);
  } else {
    fs::create_symlink(target, link_path, ec);
    if (ec) {
      ec.clear();
      fs::create_hard_link(target, link_path, ec);
    }
    if (ec) {
      ec.clear();
      fs::copy_file(target, link_path, fs::copy_options::overwrite_existing, ec);
    }
  }
}

void PrepareModsAndBackupSaves(const fs::path& original_game,
                               const fs::path& out_dir,
                               const fs::path& data_dir,
                               const fs::path& user_dir,
                               const std::map<std::string, std::string>& env,
                               fs::path& out_effective_game,
                               std::optional<fs::path>& out_mod_overlay,
                               const NativeLaunchPlanner::LogCallback& log) {
  out_effective_game = original_game;
  out_mod_overlay = std::nullopt;

  const fs::path mods_dir = env.count("BB_MODS_DIR") && !env.at("BB_MODS_DIR").empty()
                                ? fs::path(env.at("BB_MODS_DIR"))
                                : (data_dir / "mods");
  const fs::path mods_cfg = env.count("BB_MODS_CONFIG") && !env.at("BB_MODS_CONFIG").empty()
                                ? fs::path(env.at("BB_MODS_CONFIG"))
                                : (data_dir / "mods.json");
  const bool enabled = (!env.count("BB_MODS_ENABLED") || env.at("BB_MODS_ENABLED") != "0");
  const std::vector<std::string> active = enabled ? SelectedMods(mods_dir, mods_cfg) : std::vector<std::string>{};

  // Backup saves when active mod set changes (issue #212)
  const fs::path state_path = data_dir / "mods.last";
  const fs::path saves_dir = user_dir / "savedata";
  std::string now_state;
  for (std::size_t i = 0; i < active.size(); ++i) {
    if (i > 0) now_state.push_back('\n');
    now_state += active[i];
  }
  std::error_code ec;
  bool has_before = fs::is_regular_file(state_path, ec);
  std::string before_state;
  if (has_before) {
    std::vector<std::uint8_t> raw;
    if (ReadFileBytes(state_path, raw)) {
      before_state.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    }
  }
  if (!has_before || before_state != now_state) {
    if ((has_before || !active.empty()) && fs::is_directory(saves_dir, ec) && !fs::is_empty(saves_dir, ec)) {
      std::time_t t = std::time(nullptr);
      char stamp[32]{};
      std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
      const fs::path backup_target = saves_dir.parent_path() / "savedata-backups" / stamp;
      fs::create_directories(backup_target.parent_path(), ec);
      fs::copy(saves_dir, backup_target,
               fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
      if (log) log("Mods: the set of mods changed; saves copied to " + backup_target.string());
    }
    WriteFileBytes(state_path, now_state.data(), now_state.size());
  }

  if (active.empty()) return;

  // Collect mod replacement files
  std::vector<std::pair<fs::path, fs::path>> replacements;
  for (const auto& mod_name : active) {
    const fs::path mod_root_dir = mods_dir / mod_name;
    const auto layout = ModContentRoot(mod_root_dir);
    if (!layout) continue;
    const fs::path root = fs::weakly_canonical(layout->first, ec);
    const std::string prefix = layout->second;
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
      if (!entry.is_regular_file(ec)) continue;
      const fs::path rel_to_root = fs::relative(entry.path(), root, ec);
      const fs::path rel = prefix.empty() ? rel_to_root : (fs::path(prefix) / rel_to_root);
      auto it = rel.begin();
      if (it == rel.end() || ToLower(it->string()) != "dvdroot_ps4") continue;
      replacements.emplace_back(rel, entry.path());
      ++count;
    }
    if (log) log("Mods: " + mod_name + ": " + std::to_string(count) + " files");
  }

  if (replacements.empty()) return;

  const fs::path overlay = out_dir / ("mod-game-" + std::to_string(std::time(nullptr)));
  fs::remove_all(overlay, ec);
  fs::create_directories(overlay, ec);
  for (const auto& entry : fs::directory_iterator(original_game, ec)) {
    MakeLinkOrCopy(overlay / entry.path().filename(), entry.path());
  }

  for (const auto& [rel, src] : replacements) {
    fs::path cur = overlay;
    std::vector<std::string> parts;
    for (const auto& p : rel) parts.push_back(p.string());
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
      cur = ChildIgnoreCase(cur, parts[i]);
      if (fs::is_symlink(cur, ec)) {
        const fs::path target = fs::read_symlink(cur, ec);
        fs::remove(cur, ec);
        fs::create_directories(cur, ec);
        for (const auto& sub : fs::directory_iterator(target, ec)) {
          MakeLinkOrCopy(cur / sub.path().filename(), sub.path());
        }
      } else {
        fs::create_directories(cur, ec);
      }
    }
    const fs::path dest = ChildIgnoreCase(cur, parts.back());
    fs::remove(dest, ec);
    MakeLinkOrCopy(dest, src);
  }

  out_effective_game = overlay;
  out_mod_overlay = overlay;
}

// ============================================================================
// 6. Online Host Overrides (online.py + launch_plan.py::_plan_online)
// ============================================================================

std::string CommunityAddress(std::string_view value) {
  std::string v = Trim(value);
  if (v.empty()) v = "http://play.seamlesshunt.com";
  std::string scheme;
  std::string rest = v;
  if (auto pos = v.rfind("://"); pos != std::string::npos) {
    scheme = v.substr(0, pos);
    rest = v.substr(pos + 3);
  }
  if (auto slash = rest.find('/'); slash != std::string::npos) rest = rest.substr(0, slash);
  if (auto colon = rest.find(':'); colon != std::string::npos) rest = rest.substr(0, colon);
  if (scheme.empty()) {
    scheme = (rest == "play.seamlesshunt.com") ? "http" : "https";
  }
  return scheme + "://" + rest;
}

std::string ThdBase(std::string_view value) {
  std::string v = Trim(value);
  while (!v.empty() && v.back() == '/') v.pop_back();
  if (v.empty()) v = "https://thehuntersdream.com";
  if (v.find("://") == std::string::npos) v = "https://" + v;
  const auto sep = v.find("://");
  const std::string scheme = v.substr(0, sep);
  std::string rest = v.substr(sep + 3);
  if (auto slash = rest.find('/'); slash != std::string::npos) rest = rest.substr(0, slash);
  return scheme + "://" + rest;
}

void PlanOnline(std::map<std::string, std::string>& env,
                std::vector<std::string>& unset,
                const fs::path& port_dir,
                const fs::path& user_dir,
                const NativeLaunchPlanner::LogCallback& log) {
  if (!env.count("BB_ONLINE") || env["BB_ONLINE"] != "1") return;

  std::error_code ec;
#ifdef _WIN32
  fs::path mod_path = env.count("BB_NET_MODULE") && !env["BB_NET_MODULE"].empty()
                          ? fs::path(env["BB_NET_MODULE"])
                          : (port_dir / "online" / "bbnet20.dll");
  if (!fs::is_regular_file(mod_path, ec)) {
    if (log) log("Online: no " + mod_path.string() + ": offline play");
    env.erase("BB_ONLINE");
    unset.push_back("BB_ONLINE");
    return;
  }
  env["BB_NET_MODULE"] = mod_path.string();
#else
  if (!env.count("BB_NET_MODULE") || env["BB_NET_MODULE"].empty()) {
    const bool prebuilt = env.count("BB_PREBUILT") && env["BB_PREBUILT"] == "1";
    const fs::path candidate = port_dir / (prebuilt ? "bin" : "out") / "gpu" / "libbbnet20.so";
    env["BB_NET_MODULE"] = fs::weakly_canonical(candidate, ec).string();
  }
  if (!env.count("SSL_CERT_FILE") || !fs::is_regular_file(env["SSL_CERT_FILE"], ec)) {
    const std::array<std::string, 5> bundles = {
        env.count("NIX_SSL_CERT_FILE") ? env["NIX_SSL_CERT_FILE"] : "",
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem",
        "/etc/ssl/ca-bundle.pem",
    };
    for (const auto& b : bundles) {
      if (!b.empty() && fs::is_regular_file(b, ec)) {
        env["SSL_CERT_FILE"] = b;
        break;
      }
    }
  }
#endif

  fs::create_directories(user_dir, ec);
  const fs::path custom_overrides = user_dir / "host_overrides.custom.json";
  const fs::path overrides_path = user_dir / "host_overrides.json";

  if (env.count("BB_NET_BACKEND") && env["BB_NET_BACKEND"] == "thd") {
    if (env.count("BB_BBSC")) {
      env.erase("BB_BBSC");
      unset.push_back("BB_BBSC");
    }
    const std::string base = ThdBase(env.count("BB_COMMUNITY_SERVER") ? env["BB_COMMUNITY_SERVER"] : "");
    if (fs::is_regular_file(custom_overrides, ec)) {
      env["SHADPS4_HTTP_HOST_OVERRIDES_JSON"] = custom_overrides.string();
    } else {
      nlohmann::json j;
      j["*.scej-network.jp"] = base;
      j["bb-playlog-prod.s3.amazonaws.com"] = base + ":18671";
      j["bb-playlog-test.s3.amazonaws.com"] = base + ":18671";
      const std::string text = j.dump(2) + "\n";
      WriteFileBytes(overrides_path, text.data(), text.size());
      env["SHADPS4_HTTP_HOST_OVERRIDES_JSON"] = overrides_path.string();
    }
    if (!env.count("BB_THD_SERVER") || env["BB_THD_SERVER"].empty()) env["BB_THD_SERVER"] = base + ":18671";
    if (!env.count("BB_THD_AUTH") || env["BB_THD_AUTH"].empty()) env["BB_THD_AUTH"] = base;
    for (const char* k : {"BB_SHADNET_SERVER", "BB_SHADNET_WEBAPI", "BB_SHADNET_NPID",
                          "BB_SHADNET_PASSWORD", "BB_SHADNET_TOKEN"}) {
      env.erase(k);
      unset.push_back(k);
    }
    if (log) log("Co-op: The Hunter's Dream " + base);
    return;
  }

  if (!env.count("BB_SHADNET_SERVER") || env["BB_SHADNET_SERVER"].empty()) {
    env["BB_SHADNET_SERVER"] = "play.seamlesshunt.com:31313";
  }
  if (!env.count("BB_SHADNET_WEBAPI") || env["BB_SHADNET_WEBAPI"].empty()) {
    const std::string srv = env["BB_SHADNET_SERVER"];
    const auto colon = srv.rfind(':');
    const std::string host = (colon != std::string::npos) ? srv.substr(0, colon) : srv;
    env["BB_SHADNET_WEBAPI"] = "http://" + host + ":31315";
  }

  const std::string comm = CommunityAddress(env.count("BB_COMMUNITY_SERVER") ? env["BB_COMMUNITY_SERVER"] : "");
  if (fs::is_regular_file(custom_overrides, ec)) {
    env["SHADPS4_HTTP_HOST_OVERRIDES_JSON"] = custom_overrides.string();
  } else {
    nlohmann::json j;
    j["https://ss4.scej-network.jp:20443"] = comm;
    const std::string text = j.dump(2) + "\n";
    WriteFileBytes(overrides_path, text.data(), text.size());
    env["SHADPS4_HTTP_HOST_OVERRIDES_JSON"] = overrides_path.string();
  }
  if (log) log("Co-op: game server " + comm);
}

} // namespace

bool NativeLaunchPlanner::PlanAndPrepare(
    const std::map<std::string, std::string>& base_env,
    const fs::path& port_dir_in,
    const fs::path& data_dir_in,
    const fs::path& caps_path,
    bool skip_prepare,
    bool skip_online,
    NativeExecutionPlan& out_plan,
    std::string* out_error,
    LogCallback log) {
  std::error_code ec;
  const fs::path port_dir = fs::weakly_canonical(port_dir_in, ec);
  const fs::path data_dir = fs::weakly_canonical(data_dir_in, ec);
  const fs::path out_dir = data_dir / "out";
  fs::create_directories(out_dir, ec);

  std::map<std::string, std::string> env = base_env;
  std::vector<std::string> unset;

  auto set_default = [&](const std::string& k, const std::string& v) {
    if (!env.count(k) || env[k].empty()) env[k] = v;
  };

  set_default("BB_CONFIG", (data_dir / "bbport.ini").string());
  const fs::path user_dir = (env.count("BB_USER_DIR") && !env["BB_USER_DIR"].empty())
                                ? fs::path(env["BB_USER_DIR"])
                                : (data_dir / "user");
  set_default("BB_GPU_USER_DIR", user_dir.string());
  fs::create_directories(user_dir, ec);

  const fs::path game_default = port_dir.parent_path() / "CUSA03173";
  fs::path game = (env.count("BB_GAME_DIR") && !env["BB_GAME_DIR"].empty())
                      ? fs::path(env["BB_GAME_DIR"])
                      : game_default;
  if (!fs::is_regular_file(game / "eboot.bin", ec)) {
    if (out_error) *out_error = "No eboot.bin in " + game.string() + " (set BB_GAME_DIR).";
    return false;
  }

  const fs::path original_game = fs::weakly_canonical(game, ec);
  fs::path effective_game = original_game;
  std::optional<fs::path> mod_game;
  PrepareModsAndBackupSaves(original_game, out_dir, data_dir, user_dir, env, effective_game, mod_game, log);
  game = effective_game;

  if (!skip_prepare) {
    const std::string sku = (env.count("BB_CONTENT_SKU") && !env["BB_CONTENT_SKU"].empty())
                                ? env["BB_CONTENT_SKU"]
                                : "full";
    if (!PrepareLinkAndContent(game, out_dir, sku, log, out_error)) {
      return false;
    }
  }

  if (env.count("BB_AUTO_RENDER_RES") && env["BB_AUTO_RENDER_RES"] == "1") {
    for (const char* k : {"BB_RENDER_RES", "BB_OUTPUT_RES", "BB_AUTO_RENDER_RES"}) {
      env.erase(k);
      unset.push_back(k);
    }
  }

  const std::string fps = (env.count("BB_FPS") && !env["BB_FPS"].empty()) ? env["BB_FPS"] : "uncap";
  const fs::path config_path(env["BB_CONFIG"]);
  const auto ini_settings = ReadIniSettings(config_path);

  const std::string out_res_setting =
      ini_settings.count("output_res") ? ToLower(ini_settings.at("output_res")) : "auto";
  if (out_res_setting == "auto" && (!env.count("BB_MONITOR_SIZE") || env["BB_MONITOR_SIZE"].empty())) {
    const std::string wanted_disp = env.count("BB_DISPLAY") ? env["BB_DISPLAY"] : "";
    const std::string mon_size = QueryMonitorResolution(caps_path, wanted_disp);
    if (!mon_size.empty()) {
      env["BB_MONITOR_SIZE"] = mon_size;
      if (log) log("Output: the monitor's resolution, " + mon_size + " (output_res=auto)");
    }
  }

  // Resolution & hybrid preset negotiation
  std::string scaled_render;
  std::string scaled_output;
  if (!env.count("BB_RENDER_RES") || env["BB_RENDER_RES"].empty()) {
    if (auto sizes = ScaledSizes(ini_settings, env, 2)) {
      scaled_render = std::to_string(sizes->first.first) + "x" + std::to_string(sizes->first.second);
      scaled_output = std::to_string(sizes->second.first) + "x" + std::to_string(sizes->second.second);
    }
  }

  bool live = false;
  const bool live_hybrid_enabled = (!env.count("BB_LIVE_HYBRID") || env["BB_LIVE_HYBRID"] == "1");
  if (scaled_output.empty() && (!env.count("BB_RENDER_RES") || env["BB_RENDER_RES"].empty()) &&
      live_hybrid_enabled) {
    if (auto sizes = ScaledSizes(ini_settings, env, 8)) {
      scaled_render = std::to_string(sizes->first.first) + "x" + std::to_string(sizes->first.second);
      scaled_output = std::to_string(sizes->second.first) + "x" + std::to_string(sizes->second.second);
      live = true;
    }
  }

  if (!scaled_output.empty() && !live) {
    live = (!env.count("BB_LIVE_RES") || env["BB_LIVE_RES"] == "1");
  }

  auto check_large_scene_mem = [&](const std::string& scene) {
    const auto x = scene.find('x');
    if (x != std::string::npos) {
      try {
        const int w = std::stoi(scene.substr(0, x));
        const int h = std::stoi(scene.substr(x + 1));
        if (w * h > kOutputWidth * kOutputHeight) {
          set_default("BB_DMEM_MB", "9152");
        }
      } catch (...) {
      }
    }
  };

  if (live && live_hybrid_enabled) {
    if (auto sizes = ScaledSizes(ini_settings, env, 8)) {
      scaled_render = std::to_string(sizes->first.first) + "x" + std::to_string(sizes->first.second);
      scaled_output = std::to_string(sizes->second.first) + "x" + std::to_string(sizes->second.second);
    }
    env["BB_RENDER_RES"] = scaled_render;
    env["BB_OUTPUT_RES"] = scaled_output;
    env["BB_AUTO_RENDER_RES"] = "1";
    env["BB_LIVE_HYBRID"] = "1";
    check_large_scene_mem(scaled_render);
    if (log) {
      log("Output " + scaled_output + ": scene " + scaled_render +
          ", live preset changes over it (BB_LIVE_HYBRID=0: proxies only)");
    }
  } else if (live) {
    if (log) log("Output " + scaled_output + ": live resolution changes (BB_LIVE_HYBRID=0: proxies only)");
  } else if (!scaled_output.empty()) {
    env["BB_RENDER_RES"] = scaled_render;
    env["BB_OUTPUT_RES"] = scaled_output;
    env["BB_AUTO_RENDER_RES"] = "1";
    check_large_scene_mem(scaled_render);
    if (log) {
      const std::string dmem = env.count("BB_DMEM_MB") ? env["BB_DMEM_MB"] : "5056";
      log("Output " + scaled_output + ": scene " + scaled_render + ", direct memory " + dmem +
          " MiB (BB_LIVE_RES=0: no live changes)");
    }
  }

  const bool large_heaps = (env.count("BB_LARGE_HEAPS") && env["BB_LARGE_HEAPS"] == "1");
  if (large_heaps) {
    set_default("BB_DMEM_MB", "9152");
  }

  if (!skip_prepare || fs::is_regular_file(out_dir / "eboot.elf", ec)) {
    const fs::path patches_dir = (env.count("BB_PATCHES_DIR") && !env["BB_PATCHES_DIR"].empty())
                                     ? fs::path(env["BB_PATCHES_DIR"])
                                     : (data_dir / "patches");
    const fs::path patches_cfg = (env.count("BB_PATCHES_CONFIG") && !env["BB_PATCHES_CONFIG"].empty())
                                     ? fs::path(env["BB_PATCHES_CONFIG"])
                                     : (data_dir / "patches.json");
    if (!CompileAllPatches(out_dir,
                           fps,
                           env.count("BB_PATCHES") ? env["BB_PATCHES"] : "",
                           config_path,
                           game,
                           env.count("BB_RENDER_RES") ? env["BB_RENDER_RES"] : "",
                           patches_dir,
                           patches_cfg,
                           large_heaps,
                           port_dir / "patches" / "Bloodborne.xml",
                           env,
                           log,
                           out_error)) {
      return false;
    }
  }

  set_default("BB_COPY_GPU_BUFFERS", "1");
  set_default("BB_GPU_WRITE_TWINS", "1");
  set_default("BB_GPU_WRITE_TWINS_MAX", "65536");
  set_default("BB_VBLANK_HZ", (fps == "uncap") ? "1000" : ((fps == "90") ? "90" : "60"));
  set_default("BB_FPS", fps);

  if (!env.count("BB_SHADER_RECIPES") && fs::is_regular_file(port_dir / "shader_recipes.bin", ec)) {
    env["BB_SHADER_RECIPES"] = (port_dir / "shader_recipes.bin").string();
  }
  if (!env.count("BB_IMAGE_SHAPES") && fs::is_regular_file(port_dir / "image_shapes.txt", ec)) {
    env["BB_IMAGE_SHAPES"] = (port_dir / "image_shapes.txt").string();
  }
  set_default("BB_SHADER_GAME_DIR", original_game.string());

  if (!skip_online) {
    PlanOnline(env, unset, port_dir, user_dir, log);
  }

  out_plan.game_dir = game;
  out_plan.original_game = original_game;
  out_plan.mod_game = mod_game;
  out_plan.out_dir = out_dir;
  out_plan.user_dir = user_dir;

#ifdef _WIN32
  const char* probe_name = "bb-probe.exe";
#else
  const char* probe_name = "bb-probe";
#endif
  if (env.count("BB_PROBE") && !env["BB_PROBE"].empty()) {
    out_plan.probe_executable = fs::path(env["BB_PROBE"]);
  } else if (fs::is_regular_file(port_dir / "bin" / probe_name, ec)) {
    out_plan.probe_executable = port_dir / "bin" / probe_name;
  } else {
    out_plan.probe_executable = port_dir / "out" / probe_name;
  }

  out_plan.probe_args = {
      (out_dir / "boot-linked.bin").string(),
      "--content-profile",
      (out_dir / "content.bin").string(),
      "--patches",
      (out_dir / "patches.bin").string(),
      "--app0",
      game.string(),
      "--user",
      user_dir.string(),
      "--timeout",
      (env.count("BB_TIMEOUT") && !env["BB_TIMEOUT"].empty()) ? env["BB_TIMEOUT"] : "0",
  };

  out_plan.env_set.clear();
  for (const auto& [k, v] : env) {
    auto it = base_env.find(k);
    if (it == base_env.end() || it->second != v) {
      out_plan.env_set[k] = v;
    }
  }
  out_plan.env_unset.clear();
  std::set<std::string> seen_unsets;
  for (const auto& k : unset) {
    if (!env.count(k) && seen_unsets.insert(k).second) {
      out_plan.env_unset.push_back(k);
    }
  }
  out_plan.full_env = std::move(env);
  return true;
}

bool NativeLaunchPlanner::WriteNulPlanFile(const NativeExecutionPlan& plan,
                                           const fs::path& dest,
                                           std::string* out_error) {
  std::vector<std::uint8_t> buf;
  auto add = [&](std::string_view kind, std::string_view key, std::string_view val) {
    buf.insert(buf.end(), kind.begin(), kind.end());
    buf.push_back(0);
    buf.insert(buf.end(), key.begin(), key.end());
    buf.push_back(0);
    buf.insert(buf.end(), val.begin(), val.end());
    buf.push_back(0);
  };
  for (const auto& k : plan.env_unset) {
    add("UNSET", k, "");
  }
  for (const auto& [k, v] : plan.env_set) {
    add("SET", k, v);
  }
  add("META", "game", plan.game_dir.string());
  add("META", "original_game", plan.original_game.string());
  add("META", "mod_game", plan.mod_game ? plan.mod_game->string() : "");
  add("META", "bbsc_shadnet", "");
  for (const auto& arg : plan.probe_args) {
    add("ARG", "", arg);
  }
  return WriteFileBytes(dest, buf.data(), buf.size(), out_error);
}

} // namespace bbport
