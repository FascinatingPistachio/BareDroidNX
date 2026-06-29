#pragma once
#include <string>
#include <vector>

struct ApkInfo {
    std::string filename;    // just the .apk filename
    std::string path;        // full sdmc: path
    std::string appName;     // from android:label, or filename stem as fallback
    std::string packageName; // from manifest package= attribute
    std::vector<uint8_t> iconPng; // raw PNG bytes, empty if not found
};

ApkInfo    parseApk (const std::string& path);
std::vector<ApkInfo> scanApks(const std::string& dir);
