#pragma once
// The field names of a .proto file, for the Protobuf ZMQ formats (issue #118,
// phase 2). With libprotobuf (WVT_WITH_PROTOBUF), a format file can give
// `proto = FILE` and `message = NAME`, and then name its fields
// (`iq.rate_hz`) in place of numbers and types. This is used only while a
// format file is read: the decoder works with the numbers (net/zmq_format.h).
//
// Without WVT_WITH_PROTOBUF, available() is false and load() fails with a
// message that says so.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/zmq_format.h"

// A field that a path names.
struct ProtoSchemaField {
    std::vector<uint32_t> path;  // the field numbers
    std::string name;            // "iq.rate_hz"
    ProtoType type = ProtoType::Double;  // a message field: Bytes (its wire type is len)
    bool message = false;
    bool repeated = false;
    bool packed = false;  // a repeated number field that the .proto packs
};

class ProtoSchema {
public:
    ProtoSchema();
    ~ProtoSchema();
    ProtoSchema(const ProtoSchema&) = delete;
    ProtoSchema& operator=(const ProtoSchema&) = delete;

    // True when wvt is built with libprotobuf.
    static bool available();

    // Reads `file` (and the files that it imports, from its folder) and
    // selects the message `message`: a full name ("wvt.example.Envelope"), a
    // name in the package of the file ("Envelope"), or empty when the file
    // has one top-level message.
    bool load(const std::string& file, const std::string& message, std::string& err);
    // The full name of the selected message.
    std::string messageName() const;

    // A path of field names or numbers, from the selected message:
    // "iq.rate_hz", "2.3", "iq.3".
    bool resolve(const std::string& path, ProtoSchemaField& out, std::string& err) const;
    // The number of the value `name` of the enum field at `path`.
    bool enumValue(const ProtoSchemaField& field, const std::string& name, int64_t& value, std::string& err) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
