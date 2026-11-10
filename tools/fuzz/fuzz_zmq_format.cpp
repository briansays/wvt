// libFuzzer target: ZMQ message formats from files (issue #101). The format
// files are user files, and the subscriber decodes every message that
// reaches its socket with one of them, so both parsers must reject all bad
// input. The input is parsed as a format file, and decoded as a message with
// a few fixed formats (and with the format from the input, if it parses).
// A message that decodes is encoded again (the publisher's side) and decoded
// a second time: the count and the samples must stay the same.
// The Protobuf formats (issue #118) go through the same steps, so the input
// is also read as a Protobuf message with nested fields, arrays and matches.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "net/zmq_format.h"

namespace {

StreamFormat make(const char* text) {
    StreamFormat f;
    std::vector<std::string> w;
    std::string err;
    if (!parseStreamFormat(text, "fixed.ini", f, w, err)) __builtin_trap();
    return f;
}

const std::vector<StreamFormat>& fixedFormats() {
    static const std::vector<StreamFormat> list = {
        builtinStreamFormat(),
        make("[socket]\ntype = sub\n[header]\nbytes = 32\nmatch.magic = \"AB\" @ 0\nmatch.t = u8 @ 2 == 1\n"
             "seq = u32be @ 4\nfc = f64be @ 8\nrate = u32be @ 16 * 1e3\ntime_ns = u64be @ 20\ngain = i16le @ 28 * 0.5\n"
             "[samples]\nformat = ci16\nendian = big\n"),
        make("[socket]\ntype = pull\n[header]\nbytes = 2\ncount = u16le @ 0\n[samples]\nformat = cu8\norder = qi\n"
             "[stream]\nrate = 1e6\n"),
        make("[socket]\ntype = sub\n[header]\nbytes = 8\nindex = u64le @ 0\n[samples]\nformat = cf64\nendian = big\n"
             "[stream]\nrate = 1e6\n"),
        make("[socket]\ntype = sub\n[samples]\nformat = ci8\n[stream]\nrate = 1e6\n"),
        make("[socket]\ntype = sub\n[samples]\nformat = cf32\n[stream]\nrate = 1e6\n"),
        // Protobuf: the example of config/, i and q arrays, bytes, unpacked varints.
        make("[format]\nencoding = protobuf\n[socket]\ntype = sub\n[protobuf]\nmatch.iq = present 2\n"
             "seq = uint64 2.1\nfc = double 2.2\nrate = double 2.3\ngain = float 2.4\ntime_ns = fixed64 2.5\n"
             "samples = float 2.6\n"),
        make("[format]\nencoding = protobuf\n[socket]\ntype = pull\n[protobuf]\nmatch.kind = uint32 1 == 3\n"
             "match.name = string 6.1 == \"rx1\"\ni = sint32 2\nq = sint32 3\nrate = double 4\nindex = int64 5\n"
             "fc = uint32 6.2 * 1e3\n"),
        make("[format]\nencoding = protobuf\n[socket]\ntype = sub\n[protobuf]\nsamples = bytes 1\n"
             "rate = float 2 * 1e6\n[samples]\nformat = ci16\nendian = big\norder = qi\n"),
        make("[format]\nencoding = protobuf\n[socket]\ntype = dish\n[protobuf]\nsamples = unpacked int32 1\n"
             "seq = sint64 3\n[samples]\nformat = cu8\n[stream]\nrate = 1e6\n"),
        make("[format]\nencoding = protobuf\n[socket]\ntype = sub\n[protobuf]\ni = double 1.1.1\nq = double 1.1.2\n"
             "[stream]\nrate = 1e6\n"),
    };
    return list;
}

void decodeWith(const StreamFormat& f, const uint8_t* data, size_t size) {
    StreamMessage m;
    FormatExtra x;
    std::vector<uint8_t> scratch;
    std::string why;
    if (!decodeStreamMessage(f, data, size, m, x, scratch, why)) {
        if (why.empty()) __builtin_trap();  // a rejection always says why
        return;
    }
    if (f.builtin) return;  // fuzz_zmq_wire checks parseStreamMessage
    // The samples are in the input or in scratch, and are whole samples of the ring format.
    const bool inInput = m.samples >= data && m.samples + m.sampleBytes <= data + size;
    const bool inScratch = m.samples >= scratch.data() && m.samples + m.sampleBytes <= scratch.data() + scratch.size();
    if (m.sampleBytes && !inInput && !inScratch) __builtin_trap();
    if (m.sampleBytes != (size_t)m.h.count * (size_t)formatBytesPerSample(m.h.fmt)) __builtin_trap();
    if (m.h.fmt != streamRingFormat(f)) __builtin_trap();
    if (!(m.h.rate > 0) || m.h.rate > 1e10 || m.h.fc < 0 || m.h.fc > 1e12) __builtin_trap();

    // Encode it again, from the ring format, and decode that.
    std::string err;
    if (!checkPublishFormat(f, err)) return;
    const size_t n = std::min<size_t>(m.h.count, publishMaxSamples(f));
    const std::vector<uint8_t> ring(m.samples, m.samples + n * (size_t)formatBytesPerSample(m.h.fmt));
    std::vector<uint8_t> out, scratch2;
    if (!encodeStreamMessage(f, m.h, x.seq, m.h.fmt, ring.data(), n, out, why)) {
        if (why.empty()) __builtin_trap();
        return;  // a value that does not fit after the scale (rounding)
    }
    if (f.encoding == FormatEncoding::Binary && out.size() != f.headerBytes + n * (size_t)formatBytesPerSample(f.sampleFmt))
        __builtin_trap();
    if (n == publishMaxSamples(f) && out.size() > publishMaxBytes(f)) __builtin_trap();
    StreamMessage m2;
    FormatExtra x2;
    // A match that overlaps a field can change the field, so the decode may fail; it must not crash.
    if (!decodeStreamMessage(f, out.data(), out.size(), m2, x2, scratch2, why)) return;
    if (m2.h.count != n) __builtin_trap();
    // The same samples, unless a value was NaN (cf32 and cf64 keep it, the others make it 0).
    // No samples: m2.samples is null, and memcmp must not get a null pointer, even for 0 bytes.
    if (!ring.empty() && m2.h.fmt == m.h.fmt && memcmp(m2.samples, ring.data(), ring.size()) != 0) {
        bool nan = false;
        if (m.h.fmt == SampleFormat::CF32)
            for (size_t i = 0; i + 4 <= ring.size(); i += 4) {
                float v = 0;
                memcpy(&v, ring.data() + i, 4);
                nan = nan || std::isnan(v);
            }
        if (!nan) __builtin_trap();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    for (const StreamFormat& f : fixedFormats()) decodeWith(f, data, size);

    StreamFormat f;
    std::vector<std::string> w;
    std::string err;
    if (parseStreamFormat(std::string((const char*)data, size), "fuzz.ini", f, w, err)) {
        // A format that parses has its fields and matches inside the header, and a rate.
        auto bytes = [](FieldType t) -> size_t {
            switch (t) {
                case FieldType::U8:
                case FieldType::I8: return 1;
                case FieldType::U16:
                case FieldType::I16: return 2;
                case FieldType::U32:
                case FieldType::I32:
                case FieldType::F32: return 4;
                default: return 8;
            }
        };
        auto inside = [&](const FormatField& fld) { return !fld.set || fld.offset + bytes(fld.type) <= f.headerBytes; };
        for (const FormatField* fld : {&f.fc, &f.rate, &f.gain, &f.timeNs, &f.index, &f.seq, &f.count})
            if (!inside(*fld)) __builtin_trap();
        for (const FormatMatch& m : f.matches)
            if (m.offset + m.bytes.size() > f.headerBytes || m.bytes.empty()) __builtin_trap();
        if (f.encoding == FormatEncoding::Protobuf) {
            // A protobuf format has no header, a rate, the samples, and paths of 1 to 16 fields.
            const ProtoLayout& L = f.proto;
            if (f.headerBytes || f.fc.set || f.rate.set || !f.matches.empty()) __builtin_trap();
            if (!L.rate.set && !(f.fixedRate > 0)) __builtin_trap();
            if (L.samples.set == (L.i.set && L.q.set) || L.i.set != L.q.set) __builtin_trap();
            for (const ProtoField* fld : {&L.fc, &L.rate, &L.gain, &L.timeNs, &L.index, &L.seq, &L.samples, &L.i, &L.q})
                if (fld->set && (fld->path.empty() || fld->path.size() > 16)) __builtin_trap();
            for (const ProtoMatch& m : L.matches)
                if (m.path.empty() || m.path.size() > 16) __builtin_trap();
        } else {
            if (!f.rate.set && !(f.fixedRate > 0)) __builtin_trap();
        }
        if (f.headerBytes >= f.maxBytes) __builtin_trap();
        decodeWith(f, data, size);
    } else if (err.empty()) {
        __builtin_trap();  // a rejection always says why
    }
    return 0;
}
