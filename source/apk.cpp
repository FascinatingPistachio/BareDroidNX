#include "apk.h"
#include <minizip/unzip.h>
#include <dirent.h>
#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// Minimal Android Binary XML (AXML) parser
// We only need: manifest/@package and application/@android:label
// ---------------------------------------------------------------------------

static uint16_t r16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t r32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static std::vector<std::string> parseStringPool(const uint8_t* chunk, size_t chunkSize) {
    std::vector<std::string> out;
    if (chunkSize < 28) return out;

    uint32_t count       = r32(chunk + 8);
    uint32_t flags       = r32(chunk + 16);
    uint32_t stringsStart = r32(chunk + 20);
    bool utf8 = (flags & 0x100) != 0;

    const uint8_t* offsets = chunk + 28;
    const uint8_t* strBase = chunk + stringsStart;

    for (uint32_t i = 0; i < count; i++) {
        if (28 + i * 4 + 4 > chunkSize) break;
        uint32_t off = r32(offsets + i * 4);
        const uint8_t* sp = strBase + off;

        std::string s;
        if (utf8) {
            // UTF-16 length (skip it)
            uint32_t u16 = *sp++;
            if (u16 & 0x80) { u16 = ((u16 & 0x7F) << 8) | *sp++; }
            // UTF-8 byte length
            uint32_t u8 = *sp++;
            if (u8 & 0x80) { u8 = ((u8 & 0x7F) << 8) | *sp++; }
            s.assign((const char*)sp, u8);
        } else {
            uint32_t len = r16(sp); sp += 2;
            if (len & 0x8000) { len = ((len & 0x7FFF) << 16) | r16(sp); sp += 2; }
            for (uint32_t j = 0; j < len; j++) {
                uint16_t ch = r16(sp + j * 2);
                s += (ch < 128 && ch > 0) ? (char)ch : '?';
            }
        }
        out.push_back(s);
    }
    return out;
}

struct AXMLResult { std::string packageName, appLabel; };

static AXMLResult parseAXML(const std::vector<uint8_t>& data) {
    AXMLResult res;
    const uint8_t* p = data.data();
    size_t size = data.size();
    if (size < 8) return res;

    std::vector<std::string> strings;
    bool inManifest = false, inApplication = false;

    size_t pos = 8; // skip outer file chunk header
    while (pos + 8 <= size) {
        uint16_t type      = r16(p + pos);
        uint32_t chunkSize = r32(p + pos + 4);
        if (chunkSize < 8 || pos + chunkSize > size) break;

        if (type == 0x0001) {
            // String pool
            strings = parseStringPool(p + pos, chunkSize);

        } else if (type == 0x0102 && !strings.empty()) {
            // Start element
            uint32_t nameIdx   = r32(p + pos + 20);
            uint16_t attrStart = r16(p + pos + 24);
            uint16_t attrSize  = r16(p + pos + 26);
            uint16_t attrCount = r16(p + pos + 28);

            std::string elemName = nameIdx < strings.size() ? strings[nameIdx] : "";

            if (elemName == "manifest")     inManifest     = true;
            if (elemName == "application")  inApplication  = true;

            size_t attrBase = (pos + 16) + attrStart;
            for (uint16_t i = 0; i < attrCount; i++) {
                size_t ap = attrBase + i * attrSize;
                if (ap + 20 > size) break;

                uint32_t attrNameIdx = r32(p + ap + 4);
                uint8_t  dataType    = p[ap + 15];
                uint32_t dataValue   = r32(p + ap + 16);

                std::string attr = attrNameIdx < strings.size() ? strings[attrNameIdx] : "";

                // package= on <manifest>
                if (inManifest && !inApplication &&
                    attr == "package" && dataType == 0x03 &&
                    dataValue < strings.size()) {
                    res.packageName = strings[dataValue];
                }
                // label= on <manifest> or <application> (application wins)
                if (attr == "label" && dataType == 0x03 && dataValue < strings.size()) {
                    res.appLabel = strings[dataValue];
                }
            }

        } else if (type == 0x0103 && !strings.empty()) {
            // End element
            uint32_t nameIdx = r32(p + pos + 20);
            std::string name = nameIdx < strings.size() ? strings[nameIdx] : "";
            if (name == "application") inApplication = false;
            if (name == "manifest")    break; // done
        }

        pos += chunkSize;
    }
    return res;
}

// ---------------------------------------------------------------------------
// ZIP helpers
// ---------------------------------------------------------------------------

static std::vector<uint8_t> readZipEntry(unzFile zf, const char* filename) {
    if (unzLocateFile(zf, filename, 0) != UNZ_OK) return {};
    unz_file_info fi;
    if (unzGetCurrentFileInfo(zf, &fi, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK) return {};
    std::vector<uint8_t> buf(fi.uncompressed_size);
    if (unzOpenCurrentFile(zf) != UNZ_OK) return {};
    int n = unzReadCurrentFile(zf, buf.data(), (unsigned)buf.size());
    unzCloseCurrentFile(zf);
    if (n < 0) return {};
    return buf;
}

static const char* ICON_CANDIDATES[] = {
    "res/mipmap-xxxhdpi-v4/ic_launcher.png",
    "res/mipmap-xxhdpi-v4/ic_launcher.png",
    "res/mipmap-xhdpi-v4/ic_launcher.png",
    "res/mipmap-hdpi-v4/ic_launcher.png",
    "res/mipmap-mdpi-v4/ic_launcher.png",
    "res/mipmap-xxxhdpi/ic_launcher.png",
    "res/mipmap-xxhdpi/ic_launcher.png",
    "res/mipmap-xhdpi/ic_launcher.png",
    "res/mipmap-hdpi/ic_launcher.png",
    "res/drawable-xxxhdpi/ic_launcher.png",
    "res/drawable-xxhdpi/ic_launcher.png",
    "res/drawable-xhdpi/ic_launcher.png",
    "res/drawable-hdpi/ic_launcher.png",
    "res/drawable/ic_launcher.png",
    nullptr
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ApkInfo parseApk(const std::string& path) {
    ApkInfo info;
    info.path = path;

    size_t slash = path.rfind('/');
    info.filename = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    // Filename stem as fallback app name
    info.appName = info.filename.size() > 4
        ? info.filename.substr(0, info.filename.size() - 4)
        : info.filename;

    unzFile zf = unzOpen(path.c_str());
    if (!zf) return info;

    // Manifest
    auto manifest = readZipEntry(zf, "AndroidManifest.xml");
    if (!manifest.empty()) {
        auto ax = parseAXML(manifest);
        if (!ax.packageName.empty()) info.packageName = ax.packageName;
        if (!ax.appLabel.empty())    info.appName     = ax.appLabel;
    }

    // Icon — try candidates in order, use first PNG found
    for (int i = 0; ICON_CANDIDATES[i]; i++) {
        auto icon = readZipEntry(zf, ICON_CANDIDATES[i]);
        if (!icon.empty()) {
            info.iconPng = std::move(icon);
            break;
        }
    }

    unzClose(zf);
    return info;
}

std::vector<ApkInfo> scanApks(const std::string& dir) {
    std::vector<ApkInfo> result;
    DIR* d = opendir(dir.c_str());
    if (!d) return result;

    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string name = ent->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".apk") == 0)
            result.push_back(parseApk(dir + "/" + name));
    }
    closedir(d);

    std::sort(result.begin(), result.end(),
        [](const ApkInfo& a, const ApkInfo& b) { return a.appName < b.appName; });
    return result;
}
