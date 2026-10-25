#pragma once

// collision_build.h -- first-launch collision builder (v171, shippable install).
//
// The C++ counterpart of prepare_collision.py and prepare_breakables.py: on the player's own
// machine, from the player's own Dark Souls Remastered files, it writes the caches Mario's
// terrain and breakable-object hits read (<map>.bin, <map>.brk).  Nothing game-derived ships
// with the mod.
//
//   <map>.bin = "MSC2" + u32 count + count x {u32 hit filter, f32[3][3] triangle} (world m)
//   <map>.brk = "MSB1" + u32 count + count x {f32[3] position, i32 model, i16 hp, 2 pad}
//
// Sources: map/MapStudio/<map>.msb (MSB1: collision parts, objects), map/<map>/h<map>.hkxbhd
// + .hkxbdt (BHF3/BDF3 binder of DCX-compressed Havok 2015 tagfiles), event/<map>.emevd.dcx
// and event/common.emevd.dcx (collisions a script can switch stay native-owned, as in the
// Python exporter), param/GameParam/GameParam.parambnd.dcx (ObjectParam hp > 0 = breakable).
//
// Pure C++ (no Windows calls) so it can be tested off the game; the host decides when to run.

#ifdef _WIN32
#include <windows.h>
#endif
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace collisionbuild {

using Bytes = std::vector<uint8_t>;

// Paths are UTF-8; on Windows they are opened as wide strings (installs under non-ASCII folders).
inline FILE *openUtf8(const std::string &path, const char *mode) {
#ifdef _WIN32
    wchar_t wp[1024], wm[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp, 1024)) return nullptr;
    if (!MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8)) return nullptr;
    return _wfopen(wp, wm);
#else
    return fopen(path.c_str(), mode);
#endif
}
inline bool removeUtf8(const std::string &path) {
#ifdef _WIN32
    wchar_t wp[1024];
    return MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp, 1024) && DeleteFileW(wp);
#else
    return remove(path.c_str()) == 0;
#endif
}
inline bool renameUtf8(const std::string &from, const std::string &to) {
#ifdef _WIN32
    wchar_t a[1024], b[1024];
    return MultiByteToWideChar(CP_UTF8, 0, from.c_str(), -1, a, 1024) && MultiByteToWideChar(CP_UTF8, 0, to.c_str(), -1, b, 1024) &&
           MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}
inline bool readFile(const std::string &path, Bytes &out) {
    FILE *f = openUtf8(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    out.resize(size_t(n));
    const bool ok = n == 0 || fread(out.data(), 1, size_t(n), f) == size_t(n);
    fclose(f);
    return ok;
}
inline bool writeFile(const std::string &path, const Bytes &data) {
    const std::string tmp = path + ".tmp";
    FILE *f = openUtf8(tmp, "wb");
    if (!f) return false;
    const bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok) { removeUtf8(tmp); return false; }
    return renameUtf8(tmp, path);
}

template <typename T> inline bool rd(const Bytes &b, size_t at, T &v) {
    if (at > b.size() || b.size() - at < sizeof(T)) return false;
    memcpy(&v, b.data() + at, sizeof(T));
    return true;
}
template <typename T> inline T get(const Bytes &b, size_t at) {
    T v{};
    rd(b, at, v);
    return v;
}
inline uint32_t be32(const Bytes &b, size_t at) {
    if (at + 4 > b.size()) return 0;
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}
inline std::string cstr(const Bytes &b, size_t at) {
    std::string s;
    while (at < b.size() && b[at]) s.push_back(char(b[at++]));
    return s;
}
inline std::string lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// ---- Inflate (RFC 1951; the zlib streams inside DCX "DFLT") ----
struct Inflater {
    const uint8_t *in;
    size_t len, pos;
    uint32_t bitbuf;
    int bitcnt;
    Bytes *out;
    bool bad;
    int bits(int need) {
        uint32_t val = bitbuf;
        while (bitcnt < need) {
            if (pos >= len) { bad = true; return 0; }
            val |= uint32_t(in[pos++]) << bitcnt;
            bitcnt += 8;
        }
        bitbuf = val >> need;
        bitcnt -= need;
        return int(val & ((1u << need) - 1));
    }
    struct Huff { short count[16]; short symbol[320]; };
    int decode(const Huff &h) {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16; ++l) {
            code |= bits(1);
            if (bad) return -1;
            const int count = h.count[l];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        bad = true;
        return -1;
    }
    static int construct(Huff &h, const short *length, int n) {
        for (int l = 0; l < 16; ++l) h.count[l] = 0;
        for (int s = 0; s < n; ++s) h.count[length[s]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int l = 1; l < 16; ++l) {
            left <<= 1;
            left -= h.count[l];
            if (left < 0) return left;
        }
        short offs[16];
        offs[1] = 0;
        for (int l = 1; l < 15; ++l) offs[l + 1] = short(offs[l] + h.count[l]);
        for (int s = 0; s < n; ++s)
            if (length[s] != 0) h.symbol[offs[length[s]]++] = short(s);
        return left;
    }
    bool codes(const Huff &lencode, const Huff &distcode) {
        static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = decode(lencode);
            if (symbol < 0 || bad) return false;
            if (symbol < 256) {
                out->push_back(uint8_t(symbol));
            } else if (symbol == 256) {
                return true;
            } else {
                symbol -= 257;
                if (symbol >= 29) return false;
                const int l = lbase[symbol] + bits(lext[symbol]);
                const int ds = decode(distcode);
                if (ds < 0 || ds >= 30 || bad) return false;
                const size_t dist = size_t(dbase[ds] + bits(dext[ds]));
                if (bad || dist > out->size()) return false;
                const size_t from = out->size() - dist;
                for (int k = 0; k < l; ++k) out->push_back((*out)[from + size_t(k)]);
            }
        }
    }
    bool stored() {
        bitbuf = 0;
        bitcnt = 0;
        if (pos + 4 > len) return false;
        const unsigned n = in[pos] | in[pos + 1] << 8;
        const unsigned nc = in[pos + 2] | in[pos + 3] << 8;
        pos += 4;
        if (n != (~nc & 0xffff) || pos + n > len) return false;
        out->insert(out->end(), in + pos, in + pos + n);
        pos += n;
        return true;
    }
    bool fixed() {
        static Huff lencode, distcode;
        static bool made = false;
        if (!made) {
            short lengths[320];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < 288; ++s) lengths[s] = 8;
            construct(lencode, lengths, 288);
            for (s = 0; s < 30; ++s) lengths[s] = 5;
            construct(distcode, lengths, 30);
            made = true;
        }
        return codes(lencode, distcode);
    }
    bool dynamic() {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (bad || nlen > 286 || ndist > 30) return false;
        short lengths[320];
        int index;
        for (index = 0; index < ncode; ++index) lengths[order[index]] = short(bits(3));
        for (; index < 19; ++index) lengths[order[index]] = 0;
        Huff lencode, distcode;
        if (construct(lencode, lengths, 19) != 0) return false;
        index = 0;
        while (index < nlen + ndist) {
            int symbol = decode(lencode);
            if (symbol < 0 || bad) return false;
            if (symbol < 16) {
                lengths[index++] = short(symbol);
            } else {
                short len2 = 0;
                if (symbol == 16) {
                    if (index == 0) return false;
                    len2 = lengths[index - 1];
                    symbol = 3 + bits(2);
                } else if (symbol == 17) {
                    symbol = 3 + bits(3);
                } else {
                    symbol = 11 + bits(7);
                }
                if (index + symbol > nlen + ndist) return false;
                while (symbol--) lengths[index++] = len2;
            }
        }
        if (lengths[256] == 0) return false;
        const int e1 = construct(lencode, lengths, nlen);
        if (e1 < 0 || (e1 > 0 && nlen - lencode.count[0] != 1)) return false;
        const int e2 = construct(distcode, lengths + nlen, ndist);
        if (e2 < 0 || (e2 > 0 && ndist - distcode.count[0] != 1)) return false;
        return codes(lencode, distcode);
    }
    bool run() {
        int last;
        do {
            last = bits(1);
            const int type = bits(2);
            if (bad) return false;
            bool ok = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : false;
            if (!ok) return false;
        } while (!last);
        return true;
    }
};

// A DCX file ("DCX\0" ... "DCA\0" + zlib stream, DFLT) or a bare zlib stream -> bytes.
inline bool dcx(const Bytes &file, Bytes &out) {
    out.clear();
    size_t at = 0;
    if (file.size() >= 4 && memcmp(file.data(), "DCX\0", 4) == 0) {
        size_t dca = std::string::npos;
        for (size_t i = 4; i + 8 <= file.size() && i < 0x100; ++i)
            if (memcmp(file.data() + i, "DCA\0", 4) == 0) { dca = i; break; }
        if (dca == std::string::npos) return false;
        at = dca + 8;
        uint32_t usize = be32(file, 0x1C);
        if (usize && usize < (512u << 20)) out.reserve(usize);
    }
    if (at + 2 > file.size() || (file[at] & 0x0F) != 8) return false;  // zlib: deflate method
    Inflater z{file.data() + at + 2, file.size() - at - 2, 0, 0, 0, &out, false};
    return z.run();
}

// ---- BND3 / BHF3+BDF3 binders: (name, data) entries, data DCX-decompressed when it is DCX ----
struct Entry { std::string name; Bytes data; };
inline bool bnd3(const Bytes &b, std::vector<Entry> &out) {
    if (b.size() < 0x20 || memcmp(b.data(), "BND3", 4) != 0) return false;
    const uint32_t n = get<uint32_t>(b, 0x10);
    for (uint32_t i = 0; i < n; ++i) {
        const size_t e = 0x20 + size_t(i) * 0x18;
        const uint32_t size = get<uint32_t>(b, e + 4), off = get<uint32_t>(b, e + 8), noff = get<uint32_t>(b, e + 16);
        if (size_t(off) + size > b.size()) return false;
        Entry en;
        en.name = cstr(b, noff);
        en.data.assign(b.begin() + off, b.begin() + off + size);
        out.push_back(std::move(en));
    }
    return true;
}
// Index only (name, offset, size) for the split binder; data is read per entry.
struct BhfEntry { std::string name; uint32_t off, size; };
inline bool bhf3(const Bytes &bhd, std::vector<BhfEntry> &out) {
    if (bhd.size() < 0x20 || memcmp(bhd.data(), "BHF3", 4) != 0) return false;
    const uint32_t n = get<uint32_t>(bhd, 0x10);  // (+0x0C is the format byte)
    for (uint32_t i = 0; i < n; ++i) {
        const size_t e = 0x20 + size_t(i) * 0x18;
        if (e + 0x18 > bhd.size()) return false;
        out.push_back({cstr(bhd, get<uint32_t>(bhd, e + 16)), get<uint32_t>(bhd, e + 8), get<uint32_t>(bhd, e + 4)});
    }
    return true;
}

// ---- Havok 2015 tagfile: the mesh subparts of a map collision (MeshSubpartStorage) ----
struct Mesh { std::vector<float> v; std::vector<uint32_t> tri; };  // v: xyz, tri: 3 per face
struct TagType {
    std::string name;
    int parent = 0;
    uint32_t size = 0;
    struct Member { std::string name; uint32_t offset; int type; };
    std::vector<Member> members;
};
inline bool hkxMeshes(const Bytes &h, std::vector<Mesh> &meshes, std::string *why = nullptr) {
    auto fail = [&](const char *w) { if (why) *why = w; return false; };
    size_t pos = 0;
    // Section: u32 BE (size | flags in the top 2 bits) + 4-char magic.
    auto section = [&](size_t at, size_t &size, char magic[5]) -> bool {
        if (at + 8 > h.size()) return false;
        size = (be32(h, at) & 0x3FFFFFFFu);
        memcpy(magic, h.data() + at + 4, 4);
        magic[4] = 0;
        return size >= 8 && at + size <= h.size();
    };
    size_t sz;
    char mg[5];
    if (!section(0, sz, mg) || strcmp(mg, "TAG0") != 0) return fail("not a TAG0 tagfile");
    const size_t end = sz;
    pos = 8;
    size_t dataStart = 0;
    std::vector<std::string> typeNames, memberNames;
    std::vector<TagType> types;
    struct Item { uint32_t type, off, count; };
    std::vector<Item> items;
    auto varint = [&](size_t &p) -> uint64_t {
        if (p >= h.size()) return 0;
        const uint8_t b1 = h[p];
        if (!(b1 & 0x80)) { ++p; return b1 & 0x7F; }
        if (b1 == 0xC3) { const uint64_t v = uint64_t(h[p + 1]) << 8 | h[p + 2]; p += 3; return v; }
        const uint8_t marker = b1 >> 3;
        if (marker >= 0x10 && marker < 0x18) { const uint64_t v = 0x3FFF & (uint64_t(b1) << 8 | h[p + 1]); p += 2; return v; }
        if (marker >= 0x18 && marker < 0x1C) {
            const uint64_t v = 0x1FFFFF & (uint64_t(b1) << 16 | uint64_t(h[p + 1]) << 8 | h[p + 2]);
            p += 3;
            return v;
        }
        if (marker == 0x1C) { const uint64_t v = get<uint32_t>(h, p) & 0x07FFFFFFu; p += 4; return v; }
        if (marker == 0x1D) {
            const uint64_t v = 0x07FFFFFFFFull & (uint64_t(b1) << 32 | uint64_t(h[p + 1]) << 24 | uint64_t(h[p + 2]) << 16 |
                                                  uint64_t(h[p + 3]) << 8 | h[p + 4]);
            p += 5;
            return v;
        }
        if (marker == 0x1E) { const uint64_t v = get<uint64_t>(h, p) & 0x07FFFFFFFFFFFFFFull; p += 8; return v; }
        p = h.size();
        return 0;
    };
    auto strings = [&](size_t at, size_t size, std::vector<std::string> &out) {
        std::string cur;
        for (size_t i = at; i < at + size && i < h.size(); ++i) {
            if (h[i]) cur.push_back(char(h[i]));
            else { out.push_back(cur); cur.clear(); }
        }
    };
    while (pos + 8 <= end) {
        if (!section(pos, sz, mg)) return fail("bad section");
        const size_t body = pos + 8, bend = pos + sz;
        if (!strcmp(mg, "DATA")) {
            dataStart = body;
        } else if (!strcmp(mg, "TYPE")) {
            size_t p2 = body;
            while (p2 + 8 <= bend) {
                size_t s2;
                char m2[5];
                if (!section(p2, s2, m2)) return fail("bad type section");
                const size_t b2 = p2 + 8, e2 = p2 + s2;
                if (!strcmp(m2, "TSTR")) strings(b2, s2 - 8, typeNames);
                else if (!strcmp(m2, "FSTR")) strings(b2, s2 - 8, memberNames);
                else if (!strcmp(m2, "TNAM") || !strcmp(m2, "TNA1")) {
                    size_t q = b2;
                    const uint64_t count = varint(q);
                    if (count > 100000) return fail("type count");
                    types.assign(size_t(count), TagType{});
                    for (uint64_t t = 1; t < count; ++t) {
                        const uint64_t ni = varint(q), templates = varint(q);
                        types[size_t(t)].name = ni < typeNames.size() ? typeNames[size_t(ni)] : "";
                        for (uint64_t k = 0; k < templates; ++k) { varint(q); varint(q); }
                    }
                } else if (!strcmp(m2, "TBOD") || !strcmp(m2, "TBDY")) {
                    size_t q = b2;
                    while (q < e2) {
                        const uint64_t ti = varint(q);
                        if (ti == 0) continue;
                        if (ti >= types.size()) return fail("type index");
                        TagType &t = types[size_t(ti)];
                        t.parent = int(varint(q));
                        const uint64_t ff = varint(q);
                        uint64_t typeFlags = 0;
                        if (ff & 1) typeFlags = varint(q);
                        if ((ff & 2) && (typeFlags & 0xF) >= 6) varint(q);
                        if (ff & 4) varint(q);
                        if (ff & 8) { t.size = uint32_t(varint(q)); varint(q); }
                        if (ff & 16) varint(q);
                        if (ff & 32) {
                            const uint64_t mc = varint(q);
                            for (uint64_t k = 0; k < mc && q < e2; ++k) {
                                const uint64_t mn = varint(q);
                                varint(q);  // flags
                                const uint64_t off = varint(q), mt = varint(q);
                                t.members.push_back({mn < memberNames.size() ? memberNames[size_t(mn)] : "", uint32_t(off), int(mt)});
                            }
                        }
                        if (ff & 64) {
                            const uint64_t ic = varint(q);
                            for (uint64_t k = 0; k < ic; ++k) { varint(q); varint(q); }
                        }
                        if (ff & 128) return fail("unknown type flag");
                    }
                }
                p2 = e2;
            }
        } else if (!strcmp(mg, "INDX")) {
            size_t p2 = body;
            while (p2 + 8 <= bend) {
                size_t s2;
                char m2[5];
                if (!section(p2, s2, m2)) return fail("bad index section");
                if (!strcmp(m2, "ITEM")) {
                    for (size_t q = p2 + 8; q + 12 <= p2 + s2; q += 12)
                        items.push_back({get<uint32_t>(h, q) & 0x00FFFFFFu, get<uint32_t>(h, q + 4), get<uint32_t>(h, q + 8)});
                }
                p2 += s2;
            }
        }
        pos = bend;
    }
    if (!dataStart || types.empty() || items.empty()) return fail("missing DATA/TYPE/INDX");
    auto memberOf = [&](int ti, const char *name, uint32_t &off) -> bool {
        for (int guard = 0; ti > 0 && ti < int(types.size()) && guard < 32; ++guard) {
            for (const auto &m : types[size_t(ti)].members)
                if (m.name == name) { off = m.offset; return true; }
            ti = types[size_t(ti)].parent;
        }
        return false;
    };
    auto arrayAt = [&](size_t at, Item &it) -> bool {
        const uint32_t idx = get<uint32_t>(h, at);
        if (!idx || idx >= items.size()) return false;
        it = items[idx];
        return true;
    };
    for (const Item &it : items) {
        if (!it.type || it.type >= types.size()) continue;
        const TagType &t = types[it.type];
        if (t.name.size() < 18 || t.name.compare(t.name.size() - 18, 18, "MeshSubpartStorage") != 0) continue;
        if (!t.size) continue;
        uint32_t oV, o8 = 0, o16 = 0, o32 = 0;
        if (!memberOf(int(it.type), "vertices", oV)) continue;
        const bool h8 = memberOf(int(it.type), "indices8", o8), h16 = memberOf(int(it.type), "indices16", o16),
                   h32 = memberOf(int(it.type), "indices32", o32);
        for (uint32_t e = 0; e < it.count; ++e) {
            const size_t base = dataStart + it.off + size_t(e) * t.size;
            Item vi;
            if (!arrayAt(base + oV, vi)) continue;
            Mesh m;
            const size_t vat = dataStart + vi.off;
            if (vat + size_t(vi.count) * 16 > h.size()) return fail("vertex array out of range");
            m.v.resize(size_t(vi.count) * 3);
            for (uint32_t k = 0; k < vi.count; ++k)
                for (int c = 0; c < 3; ++c) m.v[size_t(k) * 3 + size_t(c)] = get<float>(h, vat + size_t(k) * 16 + size_t(c) * 4);
            Item ii;
            int width = 0;
            if (h16 && arrayAt(base + o16, ii) && ii.count) width = 2;
            else if (h32 && arrayAt(base + o32, ii) && ii.count) width = 4;
            else if (h8 && arrayAt(base + o8, ii) && ii.count) width = 1;
            if (!width) continue;
            if (ii.count % 4) return fail("index count not a multiple of 4");
            const size_t iat = dataStart + ii.off;
            if (iat + size_t(ii.count) * size_t(width) > h.size()) return fail("index array out of range");
            for (uint32_t k = 0; k < ii.count; k += 4)
                for (int c = 0; c < 3; ++c) {
                    const size_t at = iat + size_t(k + uint32_t(c)) * size_t(width);
                    const uint32_t ix = width == 2 ? get<uint16_t>(h, at) : width == 4 ? get<uint32_t>(h, at) : h[at];
                    if (ix >= vi.count) return fail("vertex index out of range");
                    m.tri.push_back(ix);
                }
            meshes.push_back(std::move(m));
        }
    }
    if (meshes.empty()) return fail("no mesh subparts");
    return true;
}

// ---- MSB1 ----
struct Part {
    std::string name, model;
    int type = -1, entityId = -1, hitFilter = 8;
    float pos[3] = {}, rot[3] = {};
};
inline bool msbParts(const Bytes &d, std::vector<Part> &parts) {
    std::vector<std::string> models;
    size_t off = 0;
    for (int guard = 0; guard < 64; ++guard) {
        const int32_t nameOff = get<int32_t>(d, off + 4), cnt = get<int32_t>(d, off + 8);
        if (cnt < 1 || cnt > 100000 || off + 12 + size_t(cnt) * 4 > d.size()) return false;
        const std::string list = cstr(d, size_t(nameOff));
        std::vector<int32_t> offs(static_cast<size_t>(cnt));
        for (int32_t i = 0; i < cnt; ++i) offs[size_t(i)] = get<int32_t>(d, off + 12 + size_t(i) * 4);
        const size_t entries = size_t(cnt) - 1;
        if (list == "MODEL_PARAM_ST") {
            for (size_t i = 0; i < entries; ++i) {
                const size_t o = size_t(offs[i]);
                models.push_back(cstr(d, o + size_t(get<int32_t>(d, o))));
            }
        } else if (list == "PARTS_PARAM_ST") {
            for (size_t i = 0; i < entries; ++i) {
                const size_t o = size_t(offs[i]);
                Part p;
                p.name = cstr(d, o + size_t(get<int32_t>(d, o)));
                p.type = get<int32_t>(d, o + 4);
                const int32_t mi = get<int32_t>(d, o + 0x0C);
                if (mi >= 0 && size_t(mi) < models.size()) p.model = models[size_t(mi)];
                for (int c = 0; c < 3; ++c) {
                    p.pos[c] = get<float>(d, o + 0x14 + size_t(c) * 4);
                    p.rot[c] = get<float>(d, o + 0x20 + size_t(c) * 4);
                }
                const int32_t superOff = get<int32_t>(d, o + 0x58), subOff = get<int32_t>(d, o + 0x5C);
                p.entityId = get<int32_t>(d, o + size_t(superOff));
                if (p.type == 5) p.hitFilter = d.size() > o + size_t(subOff) ? d[o + size_t(subOff)] : 8;
                parts.push_back(std::move(p));
            }
        }
        const int32_t next = offs.back();
        if (!next) return !parts.empty();
        off = size_t(next);
    }
    return false;
}

// ---- EMEVD (DS1R, 32-bit): collisions EMEVD 2011[01]/[02] can switch ----
// Header ints: [4] events, [5] events offset, [6] instructions, [7] instructions offset, [12]
// parameters, [13] parameters offset, [17] arguments offset.  Event: 7 ints (id, instruction
// count, instruction offset (bytes into the instruction table), parameter count, parameter
// offset, rest, pad).  Instruction: 6 ints (bank, id, argument length, argument offset, layer,
// pad).  Parameter: 5 ints (instruction index in the event, target byte, source byte, length,
// pad).  A collision id taken from an event argument (target byte 0) could be any collision:
// then every collision in the map stays native (the Python exporter's rule).
inline void controlledCollisions(const Bytes &e, std::unordered_set<int> &ids, bool &all) {
    if (e.size() < 0x50 || memcmp(e.data(), "EVD\0", 4) != 0) return;
    const uint32_t ne = get<uint32_t>(e, 16), eo = get<uint32_t>(e, 20), io = get<uint32_t>(e, 28);
    const uint32_t po = get<uint32_t>(e, 52), ao = get<uint32_t>(e, 68);
    for (uint32_t k = 0; k < ne; ++k) {
        const size_t ev = eo + size_t(k) * 28;
        const uint32_t ic = get<uint32_t>(e, ev + 4), iOff = get<uint32_t>(e, ev + 8), pc = get<uint32_t>(e, ev + 12),
                       pOff = get<uint32_t>(e, ev + 16);
        for (uint32_t j = 0; j < ic; ++j) {
            const size_t in = io + iOff + size_t(j) * 24;
            const uint32_t bank = get<uint32_t>(e, in), id = get<uint32_t>(e, in + 4), aOff = get<uint32_t>(e, in + 12);
            if (bank != 2011 || (id != 1 && id != 2)) continue;
            bool replaced = false;
            for (uint32_t q = 0; q < pc; ++q) {
                const size_t pr = po + pOff + size_t(q) * 20;
                if (get<uint32_t>(e, pr) == j && get<uint32_t>(e, pr + 4) == 0) replaced = true;
            }
            if (replaced) all = true;
            else ids.insert(get<int32_t>(e, ao + aOff));
        }
    }
}

// ---- One map's terrain cache ----
// rot: degrees, applied as Ry * Rz * Rx (FromSoft XZY).
inline void rotate(const float r[3], const float in[3], double out[3]) {
    const double k = 3.14159265358979323846 / 180.0;
    const double sx = std::sin(r[0] * k), cx = std::cos(r[0] * k), sy = std::sin(r[1] * k), cy = std::cos(r[1] * k),
                 sz = std::sin(r[2] * k), cz = std::cos(r[2] * k);
    const double a[3] = {in[0], cx * in[1] - sx * in[2], sx * in[1] + cx * in[2]};          // Rx
    const double b[3] = {cz * a[0] - sz * a[1], sz * a[0] + cz * a[1], a[2]};                // Rz
    out[0] = cy * b[0] + sy * b[2]; out[1] = b[1]; out[2] = -sy * b[0] + cy * b[2];          // Ry
}

struct MapResult { size_t triangles = 0, nativeOwned = 0, failed = 0, breakables = 0; std::string error; };

// game: install folder ("/"-or-"\\" ending not needed); outDir: cache folder.
inline MapResult buildMap(const std::string &game, const std::string &mapId, const std::string &outDir,
                          const std::unordered_map<int, int> &objectHp, const Bytes &commonEmevd) {
    MapResult r;
    const std::string sep = "/";
    Bytes msbData;
    if (!readFile(game + sep + "map/MapStudio/" + mapId + ".msb", msbData)) { r.error = "no MSB"; return r; }
    std::vector<Part> parts;
    if (!msbParts(msbData, parts)) { r.error = "bad MSB"; return r; }
    // Breakables (prepare_breakables.py): object parts whose model oNNNN has ObjectParam hp > 0.
    {
        Bytes brk = {'M', 'S', 'B', '1', 0, 0, 0, 0};
        uint32_t n = 0;
        for (const Part &p : parts) {
            if (p.type != 1 || p.model.size() < 5 || p.model[0] != 'o') continue;
            bool digits = true;
            for (int c = 1; c < 5; ++c) digits = digits && p.model[size_t(c)] >= '0' && p.model[size_t(c)] <= '9';
            if (!digits) continue;
            const int number = (p.model[1] - '0') * 1000 + (p.model[2] - '0') * 100 + (p.model[3] - '0') * 10 + (p.model[4] - '0');
            auto it = objectHp.find(number);
            if (it == objectHp.end() || it->second <= 0) continue;
            uint8_t rec[20] = {};
            memcpy(rec, p.pos, 12);
            memcpy(rec + 12, &number, 4);
            const int16_t hp = int16_t(it->second);
            memcpy(rec + 16, &hp, 2);
            brk.insert(brk.end(), rec, rec + 20);
            ++n;
        }
        memcpy(brk.data() + 4, &n, 4);
        r.breakables = n;
        writeFile(outDir + sep + mapId + ".brk", brk);
    }
    std::unordered_set<int> controlled;
    bool allControlled = false;
    {
        Bytes ev, raw;
        if (readFile(game + sep + "event/" + mapId + ".emevd.dcx", ev) && dcx(ev, raw)) controlledCollisions(raw, controlled, allControlled);
        controlledCollisions(commonEmevd, controlled, allControlled);
    }
    const std::string stem = game + sep + "map/" + mapId + "/h" + mapId.substr(1);
    Bytes bhd, bdt;
    if (!readFile(stem + ".hkxbhd", bhd) || !readFile(stem + ".hkxbdt", bdt)) { r.error = "no collision binder"; return r; }
    std::vector<BhfEntry> entries;
    if (!bhf3(bhd, entries)) { r.error = "bad collision binder"; return r; }
    std::unordered_map<std::string, size_t> byName;
    for (size_t i = 0; i < entries.size(); ++i) {
        std::string n = entries[i].name;
        const size_t slash = n.find_last_of("\\/");
        if (slash != std::string::npos) n = n.substr(slash + 1);
        byName[lower(n)] = i;
    }
    Bytes out = {'M', 'S', 'C', '2', 0, 0, 0, 0};
    uint32_t count = 0;
    std::unordered_map<size_t, std::vector<Mesh>> cache;
    for (const Part &p : parts) {
        if (p.type != 5) continue;
        if (allControlled || controlled.count(p.entityId)) { ++r.nativeOwned; continue; }
        std::string key = lower(p.model) + "a" + mapId.substr(1, 2) + ".hkx.dcx";
        auto it = byName.find(key);
        if (it == byName.end()) {
            size_t hits = 0, which = 0;
            const std::string pre = lower(p.model);
            for (const auto &kv : byName)
                if (kv.first.compare(0, pre.size(), pre) == 0) { ++hits; which = kv.second; }
            if (hits != 1) { ++r.failed; continue; }
            it = byName.find(lower(entries[which].name.substr(entries[which].name.find_last_of("\\/") + 1)));
            if (it == byName.end()) { ++r.failed; continue; }
        }
        auto ci = cache.find(it->second);
        if (ci == cache.end()) {
            const BhfEntry &en = entries[it->second];
            std::vector<Mesh> meshes;
            if (size_t(en.off) + en.size <= bdt.size()) {
                Bytes comp(bdt.begin() + en.off, bdt.begin() + en.off + en.size), hkx;
                if (dcx(comp, hkx)) hkxMeshes(hkx, meshes);
            }
            ci = cache.emplace(it->second, std::move(meshes)).first;
        }
        if (ci->second.empty()) { ++r.failed; continue; }
        const uint32_t filter = uint32_t(p.hitFilter);
        for (const Mesh &m : ci->second) {
            for (size_t f = 0; f + 2 < m.tri.size(); f += 3) {
                float t[9];
                bool ok = true;
                for (int c = 0; c < 3; ++c) {
                    const float *v = &m.v[size_t(m.tri[f + size_t(c)]) * 3];
                    double w[3];
                    rotate(p.rot, v, w);
                    for (int a = 0; a < 3; ++a) {
                        t[c * 3 + a] = float(w[a] + double(p.pos[a]));
                        ok = ok && std::isfinite(t[c * 3 + a]);
                    }
                }
                if (!ok) continue;
                const size_t at = out.size();
                out.resize(at + 40);
                memcpy(out.data() + at, &filter, 4);
                memcpy(out.data() + at + 4, t, 36);
                ++count;
            }
        }
    }
    memcpy(out.data() + 4, &count, 4);
    r.triangles = count;
    if (!count) { r.error = "no triangles"; return r; }
    if (!writeFile(outDir + sep + mapId + ".bin", out)) r.error = "write failed";
    return r;
}

// ObjectParam (GameParam.parambnd.dcx) row id -> hp (int16 at +0).
inline bool objectParamHp(const std::string &game, std::unordered_map<int, int> &hp) {
    Bytes file, raw;
    if (!readFile(game + "/param/GameParam/GameParam.parambnd.dcx", file) || !dcx(file, raw)) return false;
    std::vector<Entry> entries;
    if (!bnd3(raw, entries)) return false;
    for (const Entry &e : entries) {
        if (e.name.find("ObjectParam.param") == std::string::npos) continue;
        const Bytes &b = e.data;
        const uint16_t rows = get<uint16_t>(b, 0x0A);
        for (uint16_t j = 0; j < rows; ++j) {
            const int32_t rid = get<int32_t>(b, 0x30 + size_t(j) * 12);
            const uint32_t off = get<uint32_t>(b, 0x30 + size_t(j) * 12 + 4);
            hp[rid] = get<int16_t>(b, off);
        }
        return true;
    }
    return false;
}

}  // namespace collisionbuild
