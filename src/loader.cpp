#include "loader.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace bb::loader {

// PS4-specific definitions
static constexpr uint32_t SELF_MAGIC = 0x1d3d154f; // 'O\x15=\x1d'
static constexpr uint32_t PT_SCE_PROCPARAM = 0x61000001;
static constexpr uint32_t PT_SCE_MODULE_PARAM = 0x61000010;
static constexpr uint32_t PT_SCE_DYNLIBDATA = 0x61000000;

static constexpr uint64_t DT_SCE_ORIGINAL_FILENAME = 0x6100000d;
static constexpr uint64_t DT_SCE_NEEDED_MODULE = 0x6100000f;
static constexpr uint64_t DT_SCE_EXPORT_LIB = 0x61000013;
static constexpr uint64_t DT_SCE_IMPORT_LIB = 0x61000015;
static constexpr uint64_t DT_SCE_RELA = 0x61000029;
static constexpr uint64_t DT_SCE_RELASZ = 0x6100002d;
static constexpr uint64_t DT_SCE_JMPREL = 0x6100002f;
static constexpr uint64_t DT_SCE_PLTRELSZ = 0x61000031;
static constexpr uint64_t DT_SCE_STRTAB = 0x61000035;
static constexpr uint64_t DT_SCE_STRSZ = 0x61000037;
static constexpr uint64_t DT_SCE_SYMTAB = 0x61000039;
static constexpr uint64_t DT_SCE_SYMTABSZ = 0x6100003f;

/* --------------------------------------------------------------------------
 * SHA-1 Implementation (RFC 3174)
 * -------------------------------------------------------------------------- */

namespace {

class Sha1 {
public:
    Sha1() { Reset(); }

    void Reset() {
        state_[0] = 0x67452301;
        state_[1] = 0xefcdab89;
        state_[2] = 0x98badcfe;
        state_[3] = 0x10325476;
        state_[4] = 0xc3d2e1f0;
        count_ = 0;
    }

    void Update(const uint8_t* data, size_t len) {
        size_t i = 0;
        size_t index = (count_ >> 3) & 63;
        count_ += static_cast<uint64_t>(len) << 3;
        size_t part_len = 64 - index;

        if (len >= part_len) {
            std::memcpy(&buffer_[index], data, part_len);
            Transform(buffer_);
            for (i = part_len; i + 63 < len; i += 64) {
                Transform(&data[i]);
            }
            index = 0;
        }
        std::memcpy(&buffer_[index], &data[i], len - i);
    }

    void Final(uint8_t digest[20]) {
        uint8_t final_count[8];
        for (int i = 0; i < 8; ++i) {
            final_count[i] = static_cast<uint8_t>((count_ >> ((7 - i) * 8)) & 0xff);
        }
        uint8_t c = 0x80;
        Update(&c, 1);
        while ((count_ & 504) != 448) {
            c = 0x00;
            Update(&c, 1);
        }
        Update(final_count, 8);
        for (int i = 0; i < 5; ++i) {
            digest[i * 4] = static_cast<uint8_t>((state_[i] >> 24) & 0xff);
            digest[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xff);
            digest[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xff);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xff);
        }
    }

private:
    uint32_t state_[5];
    uint64_t count_;
    uint8_t buffer_[64];

    static uint32_t Rol(uint32_t val, int bits) {
        return (val << bits) | (val >> (32 - bits));
    }

    void Transform(const uint8_t block[64]) {
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            uint32_t temp = Rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rol(b, 30);
            b = a;
            a = temp;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }
};

static std::string Base64Encode(const uint8_t* data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (static_cast<uint32_t>(data[i]) << 16);
        if (i + 1 < len) b |= (static_cast<uint32_t>(data[i + 1]) << 8);
        if (i + 2 < len) b |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(table[(b >> 18) & 0x3f]);
        out.push_back(table[(b >> 12) & 0x3f]);
        if (i + 1 < len) out.push_back(table[(b >> 6) & 0x3f]);
        else out.push_back('=');
        if (i + 2 < len) out.push_back(table[b & 0x3f]);
        else out.push_back('=');
    }
    return out;
}

} // namespace

std::string CalculateSonyNid(std::string_view name) {
    static const uint8_t salt[16] = {
        0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1,
        0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30
    };
    Sha1 sha;
    sha.Update(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    sha.Update(salt, sizeof(salt));
    uint8_t digest[20];
    sha.Final(digest);

    uint8_t reversed[8];
    for (int i = 0; i < 8; ++i) {
        reversed[i] = digest[7 - i];
    }

    std::string b64 = Base64Encode(reversed, 8);
    while (!b64.empty() && b64.back() == '=') {
        b64.pop_back();
    }
    for (char& c : b64) {
        if (c == '/') c = '-';
    }
    return b64;
}

std::string EncodeId(uint64_t value) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result;
    result += alphabet[value & 63];
    value >>= 6;
    while (value) {
        result.insert(result.begin(), alphabet[value & 63]);
        value >>= 6;
    }
    return result;
}

/* --------------------------------------------------------------------------
 * Canonical Import Scopes
 * -------------------------------------------------------------------------- */

namespace {

static const std::unordered_map<std::string, char> kCanonicalLibraries = {
    {"libSceAjm", 'N'}, {"libSceAppContent", 'c'}, {"libSceAudioIn", 'Y'}, {"libSceAudioOut", 'G'},
    {"libSceAvPlayer", 'm'}, {"libSceCommonDialog", 'Z'}, {"libSceFios2", 'F'},
    {"libSceGameLiveStreaming", 'e'}, {"libSceGnmDriver", 'B'}, {"libSceHttp", 'K'},
    {"libSceImeDialog", 'b'}, {"libSceMouse", 'U'}, {"libSceMsgDialog", 'j'}, {"libSceNet", 'E'},
    {"libSceNetCtl", 'J'}, {"libSceNpAuth", 'X'}, {"libSceNpCommerce", 'k'},
    {"libSceNpCommon", 'Q'}, {"libSceNpManager", 'R'}, {"libSceNpMatching2", 'P'},
    {"libSceNpProfileDialog", 'h'}, {"libSceNpScore", 'o'}, {"libSceNpSignaling", 'T'},
    {"libSceNpTrophy", 'l'}, {"libSceNpUtility", 'S'}, {"libSceNpWebApi", 'W'}, {"libScePad", 'A'},
    {"libScePlayGo", 'n'}, {"libScePosix", 'I'}, {"libSceRtc", 'H'}, {"libSceSaveData", 'O'},
    {"libSceSaveDataDialog", 'd'}, {"libSceScreenShot", 'g'}, {"libSceSsl", 'L'},
    {"libSceSysmodule", 'M'}, {"libSceSystemService", 'V'}, {"libSceUserService", 'D'},
    {"libSceVideoOut", 'C'}, {"libSceVideoRecording", 'f'}, {"libSceVoice", 'i'},
    {"libSceWebBrowserDialog", 'a'}, {"libc", 'q'}, {"libkernel", 'p'}
};

static const std::unordered_map<std::string, char> kCanonicalModules = {
    {"libSceAjm", 'O'}, {"libSceAppContentUtil", 'd'}, {"libSceAudioIn", 'Z'}, {"libSceAudioOut", 'H'},
    {"libSceAvPlayer", 'n'}, {"libSceCommonDialog", 'a'}, {"libSceFios2", 'G'},
    {"libSceGameLiveStreaming", 'f'}, {"libSceGnmDriver", 'C'}, {"libSceHttp", 'L'},
    {"libSceImeDialog", 'c'}, {"libSceMouse", 'V'}, {"libSceMsgDialog", 'k'}, {"libSceNet", 'F'},
    {"libSceNetCtl", 'K'}, {"libSceNpAuth", 'Y'}, {"libSceNpCommerce", 'l'}, {"libSceNpCommon", 'R'},
    {"libSceNpManager", 'S'}, {"libSceNpMatching2", 'Q'}, {"libSceNpProfileDialog", 'i'},
    {"libSceNpScore", 'p'}, {"libSceNpSignaling", 'U'}, {"libSceNpTrophy", 'm'},
    {"libSceNpUtility", 'T'}, {"libSceNpWebApi", 'X'}, {"libScePad", 'B'}, {"libScePlayGo", 'o'},
    {"libSceRtc", 'I'}, {"libSceSaveData", 'P'}, {"libSceSaveDataDialog", 'e'},
    {"libSceScreenShot", 'h'}, {"libSceSsl", 'M'}, {"libSceSysmodule", 'N'},
    {"libSceSystemService", 'W'}, {"libSceUserService", 'E'}, {"libSceVideoOut", 'D'},
    {"libSceVideoRecording", 'g'}, {"libSceVoice", 'j'}, {"libSceWebBrowserDialog", 'b'},
    {"libc", 'q'}, {"libkernel", 'J'}
};

static std::string CanonicalImportName(
    const std::string& name,
    const std::unordered_map<std::string, std::pair<std::string, uint16_t>>& libraries,
    const std::unordered_map<std::string, std::pair<std::string, uint16_t>>& modules) {
    auto first = name.find('#');
    if (first == std::string::npos) return name;
    auto second = name.find('#', first + 1);
    if (second == std::string::npos) return name;

    std::string nid = name.substr(0, first);
    std::string lib_id = name.substr(first + 1, second - first - 1);
    std::string mod_id = name.substr(second + 1);

    auto lib_it = libraries.find(lib_id);
    auto mod_it = modules.find(mod_id);
    if (lib_it == libraries.end() || mod_it == modules.end()) return name;

    const std::string& lib_name = lib_it->second.first;
    const std::string& mod_name = mod_it->second.first;

    auto c_lib = kCanonicalLibraries.find(lib_name);
    auto c_mod = kCanonicalModules.find(mod_name);
    if (c_lib != kCanonicalLibraries.end() && c_mod != kCanonicalModules.end()) {
        return nid + "#" + c_lib->second + "#" + c_mod->second;
    }
    return nid + "#" + lib_name;
}

/* --------------------------------------------------------------------------
 * SELF / ELF Parser Structures
 * -------------------------------------------------------------------------- */

struct ParsedElf {
    std::vector<uint8_t> elf;
    Elf64_Ehdr header;
    std::vector<Elf64_Phdr> ph;
    std::vector<size_t> missing;
};

static ParsedElf ParseElf(const uint8_t* data, size_t size) {
    if (size < sizeof(Elf64_Ehdr)) {
        throw std::runtime_error("file truncated: smaller than ELF header");
    }
    Elf64_Ehdr ehdr;
    std::memcpy(&ehdr, data, sizeof(Elf64_Ehdr));

    if (std::memcmp(ehdr.e_ident, "\x7f\x45\x4c\x46\x02\x01\x01", 7) != 0 || ehdr.e_machine != EM_X86_64) {
        throw std::runtime_error("expected little-endian x86-64 ELF");
    }
    if (ehdr.e_phentsize != sizeof(Elf64_Phdr) || ehdr.e_phnum == 0 || ehdr.e_phnum >= 256) {
        throw std::runtime_error("unsupported program headers");
    }

    if (ehdr.e_phoff + ehdr.e_phnum * sizeof(Elf64_Phdr) > size) {
        throw std::runtime_error("program headers out of bounds");
    }

    std::vector<Elf64_Phdr> ph(ehdr.e_phnum);
    std::memcpy(ph.data(), data + ehdr.e_phoff, ehdr.e_phnum * sizeof(Elf64_Phdr));

    uint64_t end = ehdr.e_phoff + ehdr.e_phnum * sizeof(Elf64_Phdr);
    for (const auto& p : ph) {
        end = std::max(end, p.p_offset + p.p_filesz);
    }
    if (end > 512 * 1024 * 1024) {
        throw std::runtime_error("probe image exceeds 512 MiB limit");
    }
    if (end > size) {
        throw std::runtime_error("ELF payload out of bounds");
    }

    std::vector<uint8_t> elf(data, data + end);
    return ParsedElf{.elf = std::move(elf), .header = ehdr, .ph = std::move(ph), .missing = {}};
}

static ParsedElf ParseSelf(const uint8_t* data, size_t size) {
    if (size >= 4 && std::memcmp(data, "\x7f\x45\x4c\x46", 4) == 0) {
        return ParseElf(data, size);
    }
    if (size < 32 || std::memcmp(data, "\x4f\x15\x3d\x1d", 4) != 0) {
        throw std::runtime_error("expected PS4 SELF or ELF (eboot.bin is neither)");
    }

    uint16_t count = *reinterpret_cast<const uint16_t*>(data + 24);
    uint64_t base = 32 + static_cast<uint64_t>(count) * 32;
    if (base + sizeof(Elf64_Ehdr) > size) {
        throw std::runtime_error("out of bounds: SELF header table exceeds file");
    }

    Elf64_Ehdr header;
    std::memcpy(&header, data + base, sizeof(Elf64_Ehdr));
    if (std::memcmp(header.e_ident, "\x7f\x45\x4c\x46\x02\x01\x01", 7) != 0 || header.e_machine != EM_X86_64) {
        throw std::runtime_error("expected little-endian x86-64 ELF inside SELF");
    }
    if (header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 || header.e_phnum >= 256) {
        throw std::runtime_error("unsupported program headers");
    }

    if (base + header.e_phoff + header.e_phnum * sizeof(Elf64_Phdr) > size) {
        throw std::runtime_error("out of bounds: program headers exceed file");
    }

    std::vector<Elf64_Phdr> ph(header.e_phnum);
    std::memcpy(ph.data(), data + base + header.e_phoff, header.e_phnum * sizeof(Elf64_Phdr));

    uint64_t end = 0;
    for (const auto& p : ph) {
        end = std::max(end, p.p_offset + p.p_filesz);
    }
    if (end > 512 * 1024 * 1024) {
        throw std::runtime_error("probe image exceeds 512 MiB limit");
    }

    std::vector<uint8_t> elf(end, 0);
    uint64_t header_end = header.e_phoff + header.e_phnum * sizeof(Elf64_Phdr);
    if (base + header_end > size) {
        throw std::runtime_error("out of bounds: ELF headers exceed file");
    }
    std::memcpy(elf.data(), data + base, header_end);

    struct Span { uint64_t a, z; };
    std::vector<Span> covered = {{0, header_end}};

    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* entry = data + 32 + i * 32;
        uint64_t flags = *reinterpret_cast<const uint64_t*>(entry);
        uint64_t off = *reinterpret_cast<const uint64_t*>(entry + 8);
        uint64_t seg_size = *reinterpret_cast<const uint64_t*>(entry + 16);
        uint64_t mem = *reinterpret_cast<const uint64_t*>(entry + 24);

        if (!(flags & 0x800)) continue;
        if (flags & 10) {
            throw std::runtime_error("encrypted/compressed SELF segment is unsupported");
        }
        uint64_t index = (flags >> 20) & 4095;
        if (index >= ph.size()) {
            throw std::runtime_error("invalid segment index in SELF metadata");
        }
        const auto& p = ph[index];
        if (seg_size != p.p_filesz || seg_size != mem) {
            throw std::runtime_error("unsupported blocked segment layout");
        }
        if (off + seg_size > size || p.p_offset + seg_size > elf.size()) {
            throw std::runtime_error("out of bounds: segment copy exceeds buffer");
        }
        std::memcpy(&elf[p.p_offset], data + off, seg_size);
        covered.push_back({p.p_offset, p.p_offset + seg_size});
    }

    std::vector<size_t> missing;
    for (size_t i = 0; i < ph.size(); ++i) {
        const auto& p = ph[i];
        if (p.p_filesz) {
            bool is_covered = false;
            for (const auto& span : covered) {
                if (span.a <= p.p_offset && p.p_offset + p.p_filesz <= span.z) {
                    is_covered = true;
                    break;
                }
            }
            if (!is_covered) {
                missing.push_back(i);
                if (p.p_type == PT_LOAD || p.p_type == PT_DYNAMIC ||
                    p.p_type == PT_SCE_DYNLIBDATA || p.p_type == PT_SCE_MODULE_PARAM) {
                    throw std::runtime_error("required segment is unavailable");
                }
            }
        }
    }

    return ParsedElf{.elf = std::move(elf), .header = header, .ph = std::move(ph), .missing = std::move(missing)};
}

struct ParsedSymbol {
    std::string name;
    uint8_t type{0};
    uint8_t binding{0};
    uint16_t section{0};
    uint64_t value{0};
    uint64_t size{0};
    std::optional<std::tuple<std::string, std::pair<std::string, uint16_t>, std::pair<std::string, uint16_t>>> identity;
};

struct ParsedReloc {
    uint64_t target{0};
    uint32_t kind{0};
    uint32_t sym_id{0};
    int64_t addend{0};
};

struct ParsedModule {
    ParsedElf elf_info;
    std::unordered_map<uint64_t, uint64_t> tags;
    std::unordered_map<std::string, std::pair<std::string, uint16_t>> libraries;
    std::unordered_map<std::string, std::pair<std::string, uint16_t>> modules;
    std::vector<ParsedSymbol> symbols;
    std::vector<ParsedReloc> relocs;
};

static ParsedModule LoadModule(const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    size_t file_size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(file_size);
    f.read(reinterpret_cast<char*>(buffer.data()), file_size);

    ParsedElf parsed = ParseSelf(buffer.data(), buffer.size());

    // Find dynamic header
    const Elf64_Phdr* dp = nullptr;
    const Elf64_Phdr* lp = nullptr;
    for (const auto& p : parsed.ph) {
        if (p.p_type == PT_DYNAMIC) dp = &p;
        else if (p.p_type == PT_SCE_DYNLIBDATA) lp = &p;
    }
    if (!dp) throw std::runtime_error("missing PT_DYNAMIC header in " + path.string());
    if (!lp) throw std::runtime_error("missing PT_SCE_DYNLIBDATA header in " + path.string());

    std::unordered_map<uint64_t, uint64_t> tags;
    for (uint64_t pos = dp->p_offset; pos + 16 <= dp->p_offset + dp->p_filesz; pos += 16) {
        uint64_t tag = *reinterpret_cast<const uint64_t*>(&parsed.elf[pos]);
        uint64_t val = *reinterpret_cast<const uint64_t*>(&parsed.elf[pos + 8]);
        if (tag == 0) break;
        tags[tag] = val;
    }

    const uint8_t* blob = &parsed.elf[lp->p_offset];
    uint64_t blob_size = lp->p_filesz;

    uint64_t str_off = tags[DT_SCE_STRTAB];
    uint64_t str_sz = tags[DT_SCE_STRSZ];
    if (str_off + str_sz > blob_size) {
        throw std::runtime_error("invalid STRTAB layout in " + path.string());
    }
    const char* strtab = reinterpret_cast<const char*>(blob + str_off);

    auto GetString = [&](uint64_t off) -> std::string {
        if (off >= str_sz) return "";
        return std::string(strtab + off);
    };

    std::unordered_map<std::string, std::pair<std::string, uint16_t>> libraries;
    std::unordered_map<std::string, std::pair<std::string, uint16_t>> modules;

    for (const auto& [tag, value] : tags) {
        if (tag == DT_SCE_EXPORT_LIB || tag == DT_SCE_IMPORT_LIB ||
            tag == DT_SCE_ORIGINAL_FILENAME || tag == DT_SCE_NEEDED_MODULE) {
            bool is_lib = (tag == DT_SCE_EXPORT_LIB || tag == DT_SCE_IMPORT_LIB);
            std::string key = EncodeId(value >> 48);
            std::string name = GetString(value & 0xffffffff);
            uint16_t version = static_cast<uint16_t>((value >> 32) & 0xffff);
            if (is_lib) {
                libraries[key] = {name, version};
            } else {
                modules[key] = {name, version};
            }
        }
    }

    auto GetIdentity = [&](const std::string& name) -> std::optional<std::tuple<std::string, std::pair<std::string, uint16_t>, std::pair<std::string, uint16_t>>> {
        auto first = name.find('#');
        if (first == std::string::npos) return std::nullopt;
        auto second = name.find('#', first + 1);
        if (second == std::string::npos) return std::nullopt;

        std::string nid = name.substr(0, first);
        std::string lib_key = name.substr(first + 1, second - first - 1);
        std::string mod_key = name.substr(second + 1);

        auto lit = libraries.find(lib_key);
        auto mit = modules.find(mod_key);
        if (lit == libraries.end() || mit == modules.end()) return std::nullopt;
        return std::make_tuple(nid, lit->second, mit->second);
    };

    // Parse symbol table
    uint64_t sym_off = tags[DT_SCE_SYMTAB];
    uint64_t sym_sz = tags[DT_SCE_SYMTABSZ];
    if (sym_off + sym_sz > blob_size || sym_sz % 24 != 0) {
        throw std::runtime_error("invalid SYMTAB layout in " + path.string());
    }

    std::vector<ParsedSymbol> symbols;
    symbols.reserve(sym_sz / 24);
    for (uint64_t pos = 0; pos < sym_sz; pos += 24) {
        const uint8_t* p = blob + sym_off + pos;
        uint32_t name_off = *reinterpret_cast<const uint32_t*>(p);
        uint8_t info = p[4];
        uint8_t other = p[5];
        uint16_t section = *reinterpret_cast<const uint16_t*>(p + 6);
        uint64_t value = *reinterpret_cast<const uint64_t*>(p + 8);
        uint64_t sz = *reinterpret_cast<const uint64_t*>(p + 16);

        std::string name = GetString(name_off);
        auto ident = (!name.empty()) ? GetIdentity(name) : std::nullopt;
        symbols.push_back(ParsedSymbol{
            .name = std::move(name),
            .type = static_cast<uint8_t>(info & 0xf),
            .binding = static_cast<uint8_t>(info >> 4),
            .section = section,
            .value = value,
            .size = sz,
            .identity = std::move(ident)
        });
        (void)other;
    }

    // Parse relocations
    std::vector<ParsedReloc> relocs;
    for (auto [ot, st] : std::initializer_list<std::pair<uint64_t, uint64_t>>{
             {DT_SCE_RELA, DT_SCE_RELASZ}, {DT_SCE_JMPREL, DT_SCE_PLTRELSZ}}) {
        uint64_t table_off = tags[ot];
        uint64_t table_sz = tags[st];
        if (table_off + table_sz > blob_size || table_sz % 24 != 0) {
            throw std::runtime_error("invalid relocation table in " + path.string());
        }
        for (uint64_t pos = 0; pos < table_sz; pos += 24) {
            const uint8_t* p = blob + table_off + pos;
            uint64_t target = *reinterpret_cast<const uint64_t*>(p);
            uint64_t info = *reinterpret_cast<const uint64_t*>(p + 8);
            int64_t addend = *reinterpret_cast<const int64_t*>(p + 16);
            relocs.push_back(ParsedReloc{
                .target = target,
                .kind = static_cast<uint32_t>(info & 0xffffffff),
                .sym_id = static_cast<uint32_t>(info >> 32),
                .addend = addend
            });
        }
    }

    return ParsedModule{
        .elf_info = std::move(parsed),
        .tags = std::move(tags),
        .libraries = std::move(libraries),
        .modules = std::move(modules),
        .symbols = std::move(symbols),
        .relocs = std::move(relocs)
    };
}

static size_t PatchFsLoads(std::vector<uint8_t>& image, const std::vector<Elf64_Phdr>& ph, uint64_t base) {
    size_t patched = 0;
    for (const auto& p : ph) {
        if (p.p_type != PT_LOAD || !(p.p_flags & PF_X)) continue;
        uint64_t start = base + p.p_vaddr;
        uint64_t end = base + p.p_vaddr + p.p_filesz;
        if (end > image.size() || start + 9 > end) continue;

        for (uint64_t i = start; i + 9 <= end; ++i) {
            if (image[i] == 0x64 &&
                (image[i + 1] == 0x48 || image[i + 1] == 0x4c) &&
                image[i + 2] == 0x8b &&
                (image[i + 3] == 0x04 || image[i + 3] == 0x0c || image[i + 3] == 0x14 || image[i + 3] == 0x1c ||
                 image[i + 3] == 0x24 || image[i + 3] == 0x2c || image[i + 3] == 0x34 || image[i + 3] == 0x3c) &&
                image[i + 4] == 0x25 &&
                image[i + 5] == 0x00 && image[i + 6] == 0x00 && image[i + 7] == 0x00 && image[i + 8] == 0x00) {
                image[i] = 0x65; // Rewrite FS prefix to GS prefix
                patched++;
                i += 8;
            }
        }
    }
    return patched;
}

} // namespace

PrepareResult ModuleLinker::Link(
    const fs::path& game_dir,
    const fs::path& out_dir,
    const std::vector<std::string>& module_names) {
    PrepareResult res;
    try {
        fs::path eboot_path = game_dir / "eboot.bin";
        if (!fs::exists(eboot_path)) {
            res.error_message = "eboot.bin not found in " + game_dir.string();
            return res;
        }

        ParsedModule main = LoadModule(eboot_path);

        // Compute main program load size
        std::vector<const Elf64_Phdr*> loads;
        for (const auto& p : main.elf_info.ph) {
            if (p.p_type == PT_LOAD || p.p_type == PT_SCE_MODULE_PARAM) {
                loads.push_back(&p);
            }
        }
        if (loads.empty()) {
            res.error_message = "no loadable segments in eboot.bin";
            return res;
        }

        uint64_t main_size = 0;
        for (const auto* p : loads) {
            main_size = std::max(main_size, p->p_vaddr + p->p_memsz);
        }
        if (main_size > 512 * 1024 * 1024) {
            res.error_message = "eboot memory size exceeds 512 MiB limit";
            return res;
        }

        std::vector<uint8_t> image(main_size, 0);
        for (const auto* p : loads) {
            if (p->p_filesz > p->p_memsz) {
                res.error_message = "segment filesz exceeds memsz";
                return res;
            }
            std::memcpy(&image[p->p_vaddr], &main.elf_info.elf[p->p_offset], p->p_filesz);
        }

        struct OutputSegment {
            uint64_t address;
            uint64_t size;
            uint64_t flags;
        };
        std::vector<OutputSegment> segments;
        for (const auto* p : loads) {
            segments.push_back({p->p_vaddr, p->p_memsz, p->p_flags});
        }

        struct OutputReloc {
            uint64_t target;
            uint64_t kind;
            int64_t sym_or_target;
            int64_t addend;
        };
        std::vector<OutputReloc> relocs;
        std::vector<std::string> names;
        std::unordered_map<size_t, size_t> import_indices;

        auto IsMapped = [&](uint64_t addr, uint64_t sz = 8) {
            for (const auto* p : loads) {
                if (p->p_vaddr <= addr && addr + sz <= p->p_vaddr + p->p_memsz) return true;
            }
            return false;
        };

        // Relocate eboot
        for (const auto& r : main.relocs) {
            if (!IsMapped(r.target)) {
                res.error_message = "relocation outside mapped segments";
                return res;
            }
            if (r.kind == 8) { // R_X86_64_RELATIVE
                relocs.push_back({r.target, 0, r.addend, 0});
            } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
                if (r.sym_id >= main.symbols.size()) {
                    res.error_message = "invalid symbol id in relocation";
                    return res;
                }
                const auto& sym = main.symbols[r.sym_id];
                if (sym.section) {
                    if (sym.section == 0xfff1) { // SHN_ABS
                        *reinterpret_cast<uint64_t*>(&image[r.target]) = sym.value + r.addend;
                    } else {
                        relocs.push_back({r.target, 0, static_cast<int64_t>(sym.value + r.addend), 0});
                    }
                } else {
                    if (!import_indices.contains(r.sym_id)) {
                        import_indices[r.sym_id] = names.size();
                        names.push_back(sym.name);
                    }
                    uint64_t rk = (sym.type == 1) ? 2 : 1;
                    relocs.push_back({r.target, rk, static_cast<int64_t>(import_indices[r.sym_id]), r.addend});
                }
            }
        }

        // Module linking
        struct ModuleTableEntry {
            std::string file;
            uint64_t base;
            uint64_t size;
            uint64_t init;
            uint64_t tls_address;
            uint64_t tls_memsz;
            uint64_t tls_filesz;
            uint64_t tls_module;
        };
        std::vector<ModuleTableEntry> table;

        using IdentityKey = std::tuple<std::string, std::string, std::string>;
        auto MakeIdentKey = [](const auto& ident) -> IdentityKey {
            return {std::get<0>(ident), std::get<1>(ident).first, std::get<2>(ident).first};
        };

        std::unordered_map<IdentityKey, size_t, auto(*)(const IdentityKey&)->size_t> identities(
            16, [](const IdentityKey& k) -> size_t {
                return std::hash<std::string>()(std::get<0>(k) + "#" + std::get<1>(k) + "#" + std::get<2>(k));
            });

        for (size_t i = 0; i < names.size(); ++i) {
            auto first = names[i].find('#');
            if (first != std::string::npos) {
                auto second = names[i].find('#', first + 1);
                if (second != std::string::npos) {
                    std::string nid = names[i].substr(0, first);
                    std::string lk = names[i].substr(first + 1, second - first - 1);
                    std::string mk = names[i].substr(second + 1);
                    if (main.libraries.contains(lk) && main.modules.contains(mk)) {
                        identities[{nid, main.libraries[lk].first, main.modules[mk].first}] = i;
                    }
                }
            }
        }

        std::unordered_map<std::string, std::string> main_lib_keys;
        for (const auto& [k, v] : main.libraries) main_lib_keys[v.first] = k;
        std::unordered_map<std::string, std::string> main_mod_keys;
        for (const auto& [k, v] : main.modules) main_mod_keys[v.first] = k;

        auto GetOrCreateImport = [&](const ParsedSymbol& sym) -> size_t {
            if (!sym.identity) return 0;
            auto key = MakeIdentKey(*sym.identity);
            auto it = identities.find(key);
            if (it != identities.end()) return it->second;

            std::string n = std::get<0>(key);
            std::string lib = std::get<1>(key);
            std::string mod = std::get<2>(key);
            std::string import_name;
            if (main_lib_keys.contains(lib) && main_mod_keys.contains(mod)) {
                import_name = n + "#" + main_lib_keys[lib] + "#" + main_mod_keys[mod];
            } else {
                import_name = n + "#" + lib;
            }
            size_t idx = names.size();
            identities[key] = idx;
            names.push_back(import_name);
            return idx;
        };

        struct ExportValue {
            uint64_t address;
            uint64_t kind; // 2 for function, 1 for data
            std::string file;
        };
        std::unordered_map<IdentityKey, ExportValue, auto(*)(const IdentityKey&)->size_t> exports(
            16, [](const IdentityKey& k) -> size_t {
                return std::hash<std::string>()(std::get<0>(k) + "#" + std::get<1>(k) + "#" + std::get<2>(k));
            });

        std::unordered_map<std::string, std::vector<ExportValue>> by_nid_libc;

        uint64_t base = (main_size + 65535) & ~65535ULL;
        size_t fs_patched = PatchFsLoads(image, main.elf_info.ph, 0);
        uint64_t tls_module = 2;
        bool init_env_is_ret = false;

        for (const auto& filename : module_names) {
            fs::path mod_path = game_dir / "sce_module" / filename;
            if (!fs::exists(mod_path)) {
                continue;
            }
            ParsedModule m = LoadModule(mod_path);

            std::vector<const Elf64_Phdr*> mod_loads;
            for (const auto& p : m.elf_info.ph) {
                if (p.p_type == PT_LOAD || p.p_type == PT_SCE_MODULE_PARAM) {
                    mod_loads.push_back(&p);
                }
            }
            uint64_t modsize = 0;
            for (const auto* p : mod_loads) {
                modsize = std::max(modsize, p->p_vaddr + p->p_memsz);
            }
            if (base + modsize > 512 * 1024 * 1024) {
                res.error_message = "linked image exceeds 512 MiB probe limit";
                return res;
            }

            if (image.size() < base + modsize) {
                image.resize(base + modsize, 0);
            }

            for (const auto* p : mod_loads) {
                if (p->p_filesz > p->p_memsz) {
                    res.error_message = filename + ": invalid segment size";
                    return res;
                }
                std::memcpy(&image[base + p->p_vaddr], &m.elf_info.elf[p->p_offset], p->p_filesz);
                segments.push_back({base + p->p_vaddr, p->p_memsz, p->p_flags});
            }

            const Elf64_Phdr* tls = nullptr;
            for (const auto& p : m.elf_info.ph) {
                if (p.p_type == PT_TLS) { tls = &p; break; }
            }
            uint64_t current_tls_module = 0;
            if (tls) {
                current_tls_module = tls_module++;
            }

            // Relocations in module
            for (const auto& r : m.relocs) {
                uint64_t target = base + r.target;
                if (r.kind == 8) { // R_X86_64_RELATIVE
                    relocs.push_back({target, 0, static_cast<int64_t>(base + r.addend), 0});
                } else if (r.kind == 16) { // R_X86_64_DTPMOD64
                    *reinterpret_cast<uint64_t*>(&image[target]) = current_tls_module;
                } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
                    if (r.sym_id >= m.symbols.size()) {
                        res.error_message = filename + ": invalid symbol id";
                        return res;
                    }
                    const auto& sym = m.symbols[r.sym_id];
                    if (sym.section) {
                        if (sym.section == 0xfff1) {
                            *reinterpret_cast<uint64_t*>(&image[target]) = sym.value + r.addend;
                        } else {
                            relocs.push_back({target, 0, static_cast<int64_t>(base + sym.value + r.addend), 0});
                        }
                    } else if (!sym.identity && sym.binding == 2) {
                        *reinterpret_cast<uint64_t*>(&image[target]) = 0;
                    } else {
                        uint64_t rk = (sym.type == 1) ? 2 : 1;
                        relocs.push_back({target, rk, static_cast<int64_t>(GetOrCreateImport(sym)), r.addend});
                    }
                }
            }

            // Record exports
            for (const auto& s : m.symbols) {
                if (s.section && (s.binding == 1 || s.binding == 2) && s.identity && (s.type == 1 || s.type == 2)) {
                    auto key = MakeIdentKey(*s.identity);
                    ExportValue val{base + s.value, (s.type == 1 ? 2ULL : 1ULL), filename};
                    exports[key] = val;
                    if (std::get<1>(key) == "libc") {
                        by_nid_libc[std::get<0>(key)].push_back(val);
                    }
                }
            }

            // Inspect libc RET contract
            if (filename == "libc.prx") {
                std::string init_env_nid = CalculateSonyNid("_init_env");
                for (const auto& s : m.symbols) {
                    if (s.identity && std::get<0>(*s.identity) == init_env_nid) {
                        if (base + s.value < image.size() && image[base + s.value] == 0xc3) {
                            init_env_is_ret = true;
                        }
                        break;
                    }
                }
            }

            fs_patched += PatchFsLoads(image, m.elf_info.ph, base);

            uint64_t init_addr = (m.tags.contains(12)) ? base + m.tags[12] : 0;
            table.push_back(ModuleTableEntry{
                .file = filename,
                .base = base,
                .size = modsize,
                .init = init_addr,
                .tls_address = tls ? base + tls->p_vaddr : 0,
                .tls_memsz = tls ? tls->p_memsz : 0,
                .tls_filesz = tls ? tls->p_filesz : 0,
                .tls_module = current_tls_module
            });

            base = (base + modsize + 65535) & ~65535ULL;
        }

        // Resolve bindings
        struct Binding {
            uint64_t index;
            uint64_t address;
            uint64_t kind;
        };
        std::vector<Binding> bindings;
        for (const auto& [ident, index] : identities) {
            auto it = exports.find(ident);
            if (it != exports.end()) {
                bindings.push_back({index, it->second.address, it->second.kind});
            } else if (std::get<1>(ident) == "libSceLibcInternal") {
                auto libc_it = by_nid_libc.find(std::get<0>(ident));
                if (libc_it != by_nid_libc.end() && !libc_it->second.empty()) {
                    bindings.push_back({index, libc_it->second[0].address, libc_it->second[0].kind});
                }
            }
        }

        // Main TLS & ProcParam
        const Elf64_Phdr* main_tls = nullptr;
        const Elf64_Phdr* procparam = nullptr;
        for (const auto& p : main.elf_info.ph) {
            if (p.p_type == PT_TLS) main_tls = &p;
            else if (p.p_type == PT_SCE_PROCPARAM) procparam = &p;
        }

        uint64_t main_tls_values[4] = {
            main_tls ? main_tls->p_vaddr : 0,
            main_tls ? main_tls->p_filesz : 0,
            main_tls ? main_tls->p_memsz : 0,
            main_tls ? main_tls->p_align : 0
        };

        // Canonicalize import names
        for (auto& name : names) {
            name = CanonicalImportName(name, main.libraries, main.modules);
        }

        // Write boot-linked.bin (Format BBPROBE5)
        fs::create_directories(out_dir);
        fs::path boot_linked_path = out_dir / "boot-linked.bin";
        std::ofstream out(boot_linked_path, std::ios::binary);
        if (!out.is_open()) {
            res.error_message = "cannot write to " + boot_linked_path.string();
            return res;
        }

        uint64_t flags = init_env_is_ret ? 1 : 0;
        out.write("BBPROBE5", 8);
        uint64_t header_vals[] = {
            static_cast<uint64_t>(image.size()),
            main.elf_info.header.e_entry,
            static_cast<uint64_t>(segments.size()),
            static_cast<uint64_t>(relocs.size()),
            static_cast<uint64_t>(names.size()),
            flags,
            procparam ? procparam->p_vaddr : 0,
            main_tls_values[0], main_tls_values[1], main_tls_values[2], main_tls_values[3],
            static_cast<uint64_t>(table.size())
        };
        out.write(reinterpret_cast<const char*>(header_vals), sizeof(header_vals));

        for (const auto& t : table) {
            uint64_t mod_vals[] = {t.base, t.size, t.init, t.tls_address, t.tls_memsz, t.tls_filesz, t.tls_module};
            out.write(reinterpret_cast<const char*>(mod_vals), sizeof(mod_vals));
        }

        uint64_t nb = bindings.size();
        out.write(reinterpret_cast<const char*>(&nb), 8);
        for (const auto& b : bindings) {
            uint64_t b_vals[] = {b.index, b.address, b.kind};
            out.write(reinterpret_cast<const char*>(b_vals), sizeof(b_vals));
        }

        for (const auto& s : segments) {
            uint64_t s_vals[] = {s.address, s.size, s.flags};
            out.write(reinterpret_cast<const char*>(s_vals), sizeof(s_vals));
        }

        for (const auto& n : names) {
            char name_buf[128] = {0};
            std::strncpy(name_buf, n.c_str(), sizeof(name_buf) - 1);
            out.write(name_buf, sizeof(name_buf));
        }

        for (const auto& r : relocs) {
            uint64_t r_vals[] = {r.target, r.kind, static_cast<uint64_t>(r.sym_or_target), static_cast<uint64_t>(r.addend)};
            out.write(reinterpret_cast<const char*>(r_vals), sizeof(r_vals));
        }

        out.write(reinterpret_cast<const char*>(image.data()), image.size());

        res.success = true;
        res.image_size = image.size();
        res.entry_point = main.elf_info.header.e_entry;
        res.segment_count = segments.size();
        res.relocation_count = relocs.size();
        res.import_count = names.size();
        res.binding_count = bindings.size();
        res.fs_loads_patched = fs_patched;

    } catch (const std::exception& e) {
        res.success = false;
        res.error_message = e.what();
    }
    return res;
}

/* --------------------------------------------------------------------------
 * AppContent Profile Generation
 * -------------------------------------------------------------------------- */

bool ContentProfile::Generate(
    const fs::path& game_dir,
    const fs::path& out_file,
    std::string_view sku) {
    fs::path sfo_path = game_dir / "sce_sys" / "param.sfo";
    uint32_t params[4] = {0, 0, 0, 0};

    if (fs::exists(sfo_path)) {
        std::ifstream f(sfo_path, std::ios::binary);
        if (f.is_open()) {
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (data.size() >= 20 && std::memcmp(data.data(), "\0PSF", 4) == 0) {
                uint32_t keys_off = *reinterpret_cast<const uint32_t*>(&data[8]);
                uint32_t vals_off = *reinterpret_cast<const uint32_t*>(&data[12]);
                uint32_t count = *reinterpret_cast<const uint32_t*>(&data[16]);

                for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= data.size(); ++i) {
                    const uint8_t* e = &data[20 + i * 16];
                    uint16_t key_off = *reinterpret_cast<const uint16_t*>(e);
                    uint16_t format = *reinterpret_cast<const uint16_t*>(e + 2);
                    uint32_t length = *reinterpret_cast<const uint32_t*>(e + 4);
                    uint32_t offset = *reinterpret_cast<const uint32_t*>(e + 12);

                    if (keys_off + key_off >= data.size()) continue;
                    const char* key = reinterpret_cast<const char*>(&data[keys_off + key_off]);

                    for (int p = 1; p <= 4; ++p) {
                        std::string target = "USER_DEFINED_PARAM_" + std::to_string(p);
                        if (std::strcmp(key, target.c_str()) == 0 && format == 0x404 &&
                            vals_off + offset + 4 <= data.size() && length == 4) {
                            params[p - 1] = *reinterpret_cast<const uint32_t*>(&data[vals_off + offset]);
                        }
                    }
                }
            }
        }
    }

    fs::create_directories(out_file.parent_path());
    std::ofstream out(out_file, std::ios::binary);
    if (!out.is_open()) return false;

    uint32_t sku_val = (sku == "trial") ? 1 : 3;
    out.write("BBCONT01", 8);
    out.write(reinterpret_cast<const char*>(&sku_val), 4);
    out.write(reinterpret_cast<const char*>(params), sizeof(params));
    return true;
}

} // namespace bb::loader
