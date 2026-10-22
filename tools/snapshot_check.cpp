// Plot snapshot self-test (issue #13): file names and numbering, the crop, and
// the PNG writer (chunk layout, CRCs, size and colour type). Exit status is
// nonzero on failure.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "snapshot.h"

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xffffffffu;
}

std::vector<uint8_t> readAll(const std::string& path) {
    std::vector<uint8_t> v;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        uint8_t b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
        fclose(f);
    }
    return v;
}

void touch(const std::string& path) {
    if (FILE* f = fopen(path.c_str(), "wb")) fclose(f);
}

void testNames() {
    printf("names\n");
    check(snapshotBase("/data/caps/cap_915MHz.sigmf-meta") == "cap_915MHz", "SigMF meta: folder and extension removed");
    check(snapshotBase("cap.sigmf-data") == "cap", "SigMF data");
    check(snapshotBase("/x/gqrx_20261020_1_100000000_2400000_fc.raw") == "gqrx_20261020_1_100000000_2400000_fc",
          "raw file");
    check(snapshotBase("/x/rec.v2.cf32") == "rec.v2", "only the last extension goes");
    check(snapshotBase("") == "live", "no file: live");
    check(snapshotPath("/tmp/a", "cap", SnapPlot::Spectrum, 3) == "/tmp/a/cap_capture_spectrum_3.png",
          "path: <dir>/<base>_capture_<plot>_<n>.png");
    check(snapshotPath("/tmp/a/", "cap", SnapPlot::IQ, 1) == "/tmp/a/cap_capture_iq_1.png", "a folder with a slash");
    bool unique = true;
    for (int i = 0; i < kSnapPlots; ++i)
        for (int j = i + 1; j < kSnapPlots; ++j)
            unique = unique && strcmp(snapPlotName((SnapPlot)i), snapPlotName((SnapPlot)j)) != 0;
    check(unique, "plot names are unique");
}

void testIndex(const std::string& dir) {
    printf("numbering\n");
    check(nextSnapshotIndex(dir, "cap") == 1, "empty folder: 1");
    touch(snapshotPath(dir, "cap", SnapPlot::Waterfall, 1));
    check(nextSnapshotIndex(dir, "cap") == 2, "one plot of set 1 exists: 2 for all plots");
    touch(snapshotPath(dir, "cap", SnapPlot::PM, 2));
    check(nextSnapshotIndex(dir, "cap") == 3, "set 2 used by another plot: 3");
    check(nextSnapshotIndex(dir, "other") == 1, "another recording starts at 1");
}

void testCrop() {
    printf("crop\n");
    RgbImage src;
    src.w = 10;
    src.h = 8;
    src.px.resize(10 * 8 * 3);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 10; ++x) {
            uint8_t* p = &src.px[(y * 10 + x) * 3];
            p[0] = (uint8_t)x;
            p[1] = (uint8_t)y;
            p[2] = 7;
        }
    RgbImage c = cropImage(src, {2, 3, 6, 5});
    bool ok = c.w == 4 && c.h == 2;
    for (int y = 0; ok && y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) ok = ok && c.px[(y * c.w + x) * 3] == x + 2 && c.px[(y * c.w + x) * 3 + 1] == y + 3;
    check(ok, "4x2 crop at (2, 3) has the right pixels");
    c = cropImage(src, {-5, -5, 3, 2});
    check(c.w == 3 && c.h == 2 && c.px[0] == 0 && c.px[1] == 0, "a crop past the top left is clipped");
    c = cropImage(src, {8, 6, 50, 50});
    check(c.w == 2 && c.h == 2 && c.px[0] == 8 && c.px[1] == 6, "a crop past the bottom right is clipped");
    check(cropImage(src, {20, 0, 30, 5}).px.empty(), "a crop outside the image is empty");
}

void testPng(const std::string& dir) {
    printf("PNG\n");
    RgbImage img;
    img.w = 300;
    img.h = 120;
    img.px.resize((size_t)img.w * img.h * 3);
    for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = (uint8_t)((i / 3) % 251);
    const std::string path = dir + "/t.png";
    std::string err;
    check(writePng(path, img, err), "write " + path + (err.empty() ? "" : ": " + err));
    const auto v = readAll(path);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    check(v.size() > 8 && memcmp(v.data(), sig, 8) == 0, "PNG signature");
    size_t pos = 8;
    bool crcOk = true, ihdr = false, idat = false, iend = false;
    while (pos + 12 <= v.size()) {
        const uint32_t len = be32(&v[pos]);
        if (pos + 12 + len > v.size()) {
            crcOk = false;
            break;
        }
        const char* type = (const char*)&v[pos + 4];
        crcOk = crcOk && crc32(&v[pos + 4], len + 4) == be32(&v[pos + 8 + len]);
        if (!memcmp(type, "IHDR", 4)) {
            const uint8_t* d = &v[pos + 8];
            ihdr = be32(d) == 300 && be32(d + 4) == 120 && d[8] == 8 && d[9] == 2;
        }
        idat = idat || !memcmp(type, "IDAT", 4);
        if (!memcmp(type, "IEND", 4)) {
            iend = pos + 12 + len == v.size();
            break;
        }
        pos += 12 + len;
    }
    check(ihdr, "IHDR: 300x120, 8-bit RGB");
    check(idat && iend, "IDAT, then IEND at the end of the file");
    check(crcOk, "all chunk CRCs are correct");
    check(v.size() < img.px.size(), "compressed (" + std::to_string(v.size()) + " bytes)");
    bool exists = false;
    check(!writePng(path, img, err, &exists) && exists && readAll(path) == v,
          "an existing file is not replaced: " + err);
    check(!writePng(dir + "/no/such/folder/t.png", img, err, &exists) && !exists, "a missing folder fails: " + err);
    RgbImage empty;
    check(!writePng(dir + "/e.png", empty, err), "an empty image fails");
}
}  // namespace

int main() {
    char tmpl[] = "/tmp/wvt_snapshot_check_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) {
        perror("mkdtemp");
        return 2;
    }
    testNames();
    testIndex(dir);
    testCrop();
    testPng(dir);
    std::string cmd = std::string("rm -rf '") + dir + "'";
    if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", dir);
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
