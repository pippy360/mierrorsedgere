#pragma once

// -----------------------------------------------------------------------------
// How loud a sound is at a distance: the arithmetic of the retail game's sound nodes.
//
// Retail's OpenAL does no distance attenuation of its own (the audio device's init calls
// alDistanceModel(AL_NONE), MirrorsEdge.exe 0x010D97EC). On every audio update the cue's node graph
// is walked for each playing sound (USoundNode::ParseNodes), and each attenuation node on the way
// down to a wave multiplies the volume by a factor of the distance between the listener and the
// sound. The source then plays at exactly that volume. docs/AUDIO_SYSTEM_RE.md section 3.2.
// -----------------------------------------------------------------------------

#include <cmath>
#include <cstdint>
#include <vector>

namespace me {

// UAudioDevice::LocationIsAudible (0x00B653D0) takes a MaxAudibleDistance of WORLD_MAX as "anywhere";
// USoundCue::CalculateMaxAudibleDistance (0x00B76C80) gives it to a cue none of whose nodes has a radius.
inline constexpr float kSoundWorldMax = 524288.0f;

// USoundNodeAttenuation::ParseNodes (0x00B7DD60), the factor a node with bAttenuate multiplies into
// the volume. `distance`, `min_radius` and `max_radius` are in unreal units, the radii as drawn for
// this play; `model` is the node's SoundDistanceModel. The order of the tests is the game's: at or
// beyond MaxRadius the sound is silent (so a node whose radii are equal, or the wrong way round, is
// full volume inside and nothing outside, and the divisions below never see a zero span), at or
// inside MinRadius it is untouched, and only between the two does the model apply.
//
// The linear and inverse models are single-precision SSE in the game and are written the same way
// here, operation for operation. The other three run on the x87 stack and store a float at the end;
// they are done in double and rounded once. The constants are the executable's: 1.0 (0x01A94A98),
// 0.25 (0x01AA689C), -1.0 (0x01A9970C), 0.02 (0x01B08128), 0.05 (0x01AA2E60), 10.0 (0x01B56BF0).
inline float attenuation_gain(uint8_t model, float distance, float min_radius, float max_radius, float db_at_max) {
    if (distance >= max_radius) return 0.0f;
    if (distance <= min_radius) return 1.0f;
    const double d = distance, mn = min_radius, mx = max_radius;
    switch (model) {
        case 0:  // ATTENUATION_Linear (0x00B7DF1C)
            return 1.0f - (distance - min_radius) / (max_radius - min_radius);
        case 1: {  // ATTENUATION_Logarithmic (0x00B7DF48): 1 at MinRadius, 0 at MaxRadius, steepest near the source
            const double k = (min_radius == 0.0f) ? 0.25 : -1.0 / std::log(mn / mx);
            const double v = -(std::log(d / mx) * k);
            return 1.0 < v ? 1.0f : static_cast<float>(v);
        }
        case 2: {  // ATTENUATION_Inverse (0x00B7DFA1): still 0.02 * Max / Min just inside MaxRadius, then cut
            const float k = (min_radius == 0.0f) ? 1.0f : max_radius / min_radius;
            const float v = 0.02f / (distance / max_radius) * k;
            return 1.0f >= v ? v : 1.0f;
        }
        case 3: {  // ATTENUATION_LogReverse (0x00B7DFF4): a small step down at MinRadius, 0 before MaxRadius
            const double k = (min_radius == 0.0f) ? 0.25 : -1.0 / std::log(mn / mx);
            const double v = 1.0 - std::log(1.0 / (1.0 - d / mx)) * k;
            return v < 0.0 ? 0.0f : static_cast<float>(v);
        }
        case 4:  // ATTENUATION_NaturalSound (0x00B7E07B): a straight line in decibels down to dBAttenuationAtMax
            return static_cast<float>(std::pow(10.0, (d - mn) / (mx - mn) * static_cast<double>(db_at_max) * static_cast<double>(0.05f)));
        default:  // any other value (0x00B7E075): untouched
            return 1.0f;
    }
}

// A radius drawn for one play from the pair its cooked table holds. The pair is not always the
// lower value first (three of the game's tables are 5000 then 800, 5000 then 3000, 1500 then 400).
inline float draw_radius(const float pair[2], float random01) {
    return pair[0] + (pair[1] - pair[0]) * random01;
}

// One attenuation node on the way from a cue's first node down to a wave, as one play uses it: its
// radii are drawn the first time the node is parsed for that play and kept (the node's payload in
// the UAudioComponent), the distance is taken anew on every update.
struct DrawnAttenuation {
    uint8_t model = 0;
    bool attenuate = true;
    float min_radius = 400.0f;
    float max_radius = 5000.0f;
    float db_at_max = -60.0f;
    // UTdSoundNodeAttenuation::ParseNodes (0x0122AE80) attenuates linearly on the distance its first
    // parse found, whatever its DistanceModel, and then calls its parent's, which applies the model
    // to the distance now: this is the first factor, fixed for the play (1 for any other node).
    float first_distance_gain = 1.0f;
};

// Everything the attenuation nodes above one wave do to it. A cue can have several such nodes: on
// the parallel branches of a mixer, each with its own radii, and in 17 cues two on one branch.
struct VoiceAttenuation {
    std::vector<DrawnAttenuation> nodes;  // root first
    // CurrentUseSpatialization: each node on the way ORs its bSpatialize in; a wave with no such
    // node above it plays on a source-relative source at the listener (0x010DB137).
    bool spatialize = false;

    // The product of the nodes' factors (each one is multiplied into CurrentVolume, and the child is
    // parsed afterwards).
    [[nodiscard]] float gain(float distance) const {
        float g = 1.0f;
        for (const DrawnAttenuation& n : nodes) {
            if (!n.attenuate) continue;
            g *= n.first_distance_gain * attenuation_gain(n.model, distance, n.min_radius, n.max_radius, n.db_at_max);
        }
        return g;
    }
};

}  // namespace me
