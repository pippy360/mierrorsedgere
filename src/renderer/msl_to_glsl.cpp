// Placeholder: replaced by the real translator (see msl_to_glsl.hpp).
#include "msl_to_glsl.hpp"

namespace me {

struct MslToGlsl::Impl {
    std::vector<GlslStaticSampler> static_samplers;
};

MslToGlsl::MslToGlsl() : impl_(std::make_unique<Impl>()) {}
MslToGlsl::~MslToGlsl() = default;

bool MslToGlsl::add_source(const std::string&, std::string* error) {
    if (error) *error = "MslToGlsl: not implemented";
    return false;
}

GlslShader MslToGlsl::emit(const std::string& entry, int) const {
    GlslShader s;
    s.entry = entry;
    s.error = "MslToGlsl: not implemented";
    return s;
}

GlslShader MslToGlsl::emit_from(const std::string&, const std::string& entry, int) const {
    return emit(entry, 1);
}

const std::vector<GlslStaticSampler>& MslToGlsl::static_samplers() const { return impl_->static_samplers; }

}  // namespace me
