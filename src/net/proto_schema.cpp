#include "net/proto_schema.h"

#ifdef WVT_HAVE_PROTOBUF

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace gp = google::protobuf;

namespace {

// The errors of the .proto parser, as "file:line:column: message" lines.
class Errors : public gp::compiler::MultiFileErrorCollector {
public:
    std::string text;
#if __has_include(<absl/strings/string_view.h>)
    // libprotobuf 22 and later
    void RecordError(absl::string_view file, int line, int column, absl::string_view message) override {
        add(std::string(file), line, column, std::string(message));
    }
#else
    void AddError(const std::string& file, int line, int column, const std::string& message) override {
        add(file, line, column, message);
    }
#endif

private:
    void add(const std::string& file, int line, int column, const std::string& message) {
        if (!text.empty()) text += "; ";
        text += file;
        if (line >= 0) text += ":" + std::to_string(line + 1) + ":" + std::to_string(column + 1);
        text += ": " + message;
    }
};

std::string dirOf(const std::string& path) {
    const size_t s = path.rfind('/');
    return s == std::string::npos ? "." : s == 0 ? "/" : path.substr(0, s);
}

std::string fileOf(const std::string& path) {
    const size_t s = path.rfind('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

bool isNumber(const std::string& s) {
    if (s.empty() || s.size() > 10) return false;
    for (char c : s)
        if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

// "seq, fc_hz, rate_hz ...": the fields of a message, for the messages.
std::string fieldNames(const gp::Descriptor* d) {
    std::string s;
    const int n = d->field_count();
    for (int i = 0; i < n && i < 12; ++i) s += (i ? ", " : "") + std::string(d->field(i)->name());
    if (n > 12) s += ", ...";
    return n ? s : "none";
}

bool typeOf(const gp::FieldDescriptor* f, ProtoType& t) {
    using FD = gp::FieldDescriptor;
    switch (f->type()) {
        case FD::TYPE_DOUBLE: t = ProtoType::Double; return true;
        case FD::TYPE_FLOAT: t = ProtoType::Float; return true;
        case FD::TYPE_INT64: t = ProtoType::Int64; return true;
        case FD::TYPE_UINT64: t = ProtoType::Uint64; return true;
        case FD::TYPE_INT32: t = ProtoType::Int32; return true;
        case FD::TYPE_FIXED64: t = ProtoType::Fixed64; return true;
        case FD::TYPE_FIXED32: t = ProtoType::Fixed32; return true;
        case FD::TYPE_BOOL: t = ProtoType::Bool; return true;
        case FD::TYPE_STRING: t = ProtoType::String; return true;
        case FD::TYPE_BYTES:
        case FD::TYPE_MESSAGE: t = ProtoType::Bytes; return true;
        case FD::TYPE_UINT32: t = ProtoType::Uint32; return true;
        case FD::TYPE_ENUM: t = ProtoType::Enum; return true;
        case FD::TYPE_SFIXED32: t = ProtoType::Sfixed32; return true;
        case FD::TYPE_SFIXED64: t = ProtoType::Sfixed64; return true;
        case FD::TYPE_SINT32: t = ProtoType::Sint32; return true;
        case FD::TYPE_SINT64: t = ProtoType::Sint64; return true;
        default: return false;  // groups
    }
}

}  // namespace

struct ProtoSchema::Impl {
    gp::compiler::DiskSourceTree tree;
    Errors errors;
    std::unique_ptr<gp::compiler::Importer> importer;
    const gp::Descriptor* message = nullptr;
};

ProtoSchema::ProtoSchema() : impl_(std::make_unique<Impl>()) {}
ProtoSchema::~ProtoSchema() = default;

bool ProtoSchema::available() { return true; }

bool ProtoSchema::load(const std::string& file, const std::string& message, std::string& err) {
    impl_ = std::make_unique<Impl>();
    Impl& m = *impl_;
    m.tree.MapPath("", dirOf(file));
    m.importer = std::make_unique<gp::compiler::Importer>(&m.tree, &m.errors);
    const gp::FileDescriptor* fd = m.importer->Import(fileOf(file));
    if (!fd) {
        err = m.errors.text.empty() ? "cannot read " + file : m.errors.text;
        return false;
    }
    const gp::DescriptorPool* pool = m.importer->pool();
    if (message.empty()) {
        if (fd->message_type_count() != 1) {
            err = "set message = NAME: " + fileOf(file) + " has " + std::to_string(fd->message_type_count()) +
                  " top-level messages";
            return false;
        }
        m.message = fd->message_type(0);
        return true;
    }
    m.message = pool->FindMessageTypeByName(message);
    if (!m.message && !std::string(fd->package()).empty())
        m.message = pool->FindMessageTypeByName(std::string(fd->package()) + "." + message);
    if (!m.message) {
        std::string names;
        for (int i = 0; i < fd->message_type_count(); ++i)
            names += (i ? ", " : "") + std::string(fd->message_type(i)->full_name());
        err = "no message '" + message + "' in " + fileOf(file) + " (" + (names.empty() ? "it has none" : names) + ")";
        return false;
    }
    return true;
}

std::string ProtoSchema::messageName() const {
    return impl_->message ? std::string(impl_->message->full_name()) : std::string();
}

bool ProtoSchema::resolve(const std::string& path, ProtoSchemaField& out, std::string& err) const {
    out = ProtoSchemaField{};
    const gp::Descriptor* d = impl_->message;
    if (!d) {
        err = "no message is loaded";
        return false;
    }
    std::vector<std::string> parts;
    std::stringstream in(path);
    for (std::string p; std::getline(in, p, '.');) parts.push_back(p);
    if (parts.empty() || path.back() == '.') {
        err = "'" + path + "' is not a field";
        return false;
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& p = parts[i];
        const gp::FieldDescriptor* f =
            isNumber(p) ? d->FindFieldByNumber((int)std::min<unsigned long>(std::strtoul(p.c_str(), nullptr, 10), 0x7fffffff))
                        : d->FindFieldByName(p);
        if (!f) {
            err = std::string(d->full_name()) + " has no field '" + p + "' (" + fieldNames(d) + ")";
            return false;
        }
        out.name += (i ? "." : "") + std::string(f->name());
        out.path.push_back((uint32_t)f->number());
        if (f->is_map()) {
            err = out.name + " is a map: wvt does not read maps";
            return false;
        }
        if (!typeOf(f, out.type)) {
            err = out.name + " is a group: wvt does not read groups";
            return false;
        }
        const bool last = i + 1 == parts.size();
        if (!last) {
            if (f->type() != gp::FieldDescriptor::TYPE_MESSAGE) {
                err = out.name + " is not a message, so it has no field '" + parts[i + 1] + "'";
                return false;
            }
            d = f->message_type();
            continue;
        }
        out.message = f->type() == gp::FieldDescriptor::TYPE_MESSAGE;
        out.repeated = f->is_repeated();
        out.packed = f->is_packed();
    }
    return true;
}

bool ProtoSchema::enumValue(const ProtoSchemaField& field, const std::string& name, int64_t& value,
                            std::string& err) const {
    // Walk the path again to the enum field.
    const gp::Descriptor* d = impl_->message;
    const gp::FieldDescriptor* f = nullptr;
    for (size_t i = 0; d && i < field.path.size(); ++i) {
        f = d->FindFieldByNumber((int)field.path[i]);
        d = f && f->type() == gp::FieldDescriptor::TYPE_MESSAGE ? f->message_type() : nullptr;
    }
    if (!f || !f->enum_type()) {
        err = field.name + " is not an enum";
        return false;
    }
    const gp::EnumValueDescriptor* v = f->enum_type()->FindValueByName(name);
    if (!v) {
        std::string names;
        for (int i = 0; i < f->enum_type()->value_count() && i < 12; ++i)
            names += (i ? ", " : "") + std::string(f->enum_type()->value(i)->name());
        err = std::string(f->enum_type()->full_name()) + " has no value '" + name + "' (" + names + ")";
        return false;
    }
    value = v->number();
    return true;
}

#else  // no libprotobuf

struct ProtoSchema::Impl {};

ProtoSchema::ProtoSchema() = default;
ProtoSchema::~ProtoSchema() = default;

bool ProtoSchema::available() { return false; }

bool ProtoSchema::load(const std::string&, const std::string&, std::string& err) {
    err = "this wvt is built without WVT_WITH_PROTOBUF, so it cannot read .proto files: "
          "remove proto and message, and give each field as TYPE NUMBER (for example double 2.3)";
    return false;
}

std::string ProtoSchema::messageName() const { return {}; }

bool ProtoSchema::resolve(const std::string&, ProtoSchemaField&, std::string& err) const {
    err = "no .proto file";
    return false;
}

bool ProtoSchema::enumValue(const ProtoSchemaField&, const std::string&, int64_t&, std::string& err) const {
    err = "no .proto file";
    return false;
}

#endif
