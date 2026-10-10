#include "level_intro.hpp"

#include "ue3_props.hpp"
#include "upk_loader.hpp"
#include "../anim/anim_system.hpp"

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <iostream>

namespace me {

namespace {

using Packages = std::vector<std::shared_ptr<UPKPackage>>;

// The animation's own space (X left, Y down, Z forward of the placed actor) to the level's.
struct RigFrame {
    Vec3 origin;
    Vec3 forward;
    Vec3 left;
    Vec3 up{0.0f, 0.0f, 1.0f};

    RigFrame(const Vec3& location, float yaw_deg) : origin(location) {
        const float yaw = yaw_deg * DEG2RAD;
        forward = Vec3(std::cos(yaw), std::sin(yaw), 0.0f);
        left = Vec3(std::sin(yaw), -std::cos(yaw), 0.0f);
    }
    [[nodiscard]] Vec3 dir(const Vec3& v) const { return left * v.x + up * (-v.y) + forward * v.z; }
    [[nodiscard]] Vec3 pos(const Vec3& p) const { return origin + dir(p); }
    // An offset in the actor's UE axes (X forward, Y right, Z up): an InterpTrackMove relative to its start.
    [[nodiscard]] Vec3 actor_offset(const Vec3& o) const { return forward * o.x + left * (-o.y) + up * o.z; }
};

// The pawn's Location rides this far above the animation's root. Measured in retail: 94.0 in every
// intro (the pawn against the placed actor in Cranes, the camera against the animation in Boat).
constexpr float kPawnAboveRoot = 94.0f;

// An FInterpCurveVector: each segment is evaluated the way its first key's InterpMode says.
struct KeyedVector {
    enum class Mode { Linear, Constant, Curve };
    struct Key {
        float time = 0.0f;
        Vec3 value{0.0f, 0.0f, 0.0f};
        Vec3 arrive{0.0f, 0.0f, 0.0f};
        Vec3 leave{0.0f, 0.0f, 0.0f};
        Mode mode = Mode::Linear;
    };
    std::vector<Key> keys;

    [[nodiscard]] Vec3 at(float t) const {
        if (keys.empty()) return Vec3(0.0f, 0.0f, 0.0f);
        if (t <= keys.front().time) return keys.front().value;
        for (size_t i = 1; i < keys.size(); ++i) {
            if (t > keys[i].time) continue;
            const Key& a = keys[i - 1];
            const Key& b = keys[i];
            const float span = b.time - a.time;
            if (span <= 1e-6f || a.mode == Mode::Constant) return span <= 1e-6f ? b.value : a.value;
            const float u = (t - a.time) / span;
            if (a.mode == Mode::Linear) return a.value + (b.value - a.value) * u;
            // Cubic Hermite; the tangents are per unit of time.
            const float u2 = u * u;
            const float u3 = u2 * u;
            return a.value * (2.0f * u3 - 3.0f * u2 + 1.0f) + a.leave * (span * (u3 - 2.0f * u2 + u)) +
                   b.arrive * (span * (u3 - u2)) + b.value * (-2.0f * u3 + 3.0f * u2);
        }
        return keys.back().value;
    }
};

KeyedVector read_vector_curve(const UPropertyList& props, const char* name) {
    KeyedVector curve;
    const UProperty* c = find_prop(props, name);
    const UProperty* points = c ? find_prop(c->fields, "Points") : nullptr;
    if (!points) return curve;
    const auto vec = [](const UPropertyList& el, const char* field) {
        const UProperty* p = find_prop(el, field);
        return p ? Vec3(p->v[0], p->v[1], p->v[2]) : Vec3(0.0f, 0.0f, 0.0f);
    };
    for (const auto& el : points->elements) {
        KeyedVector::Key key;
        key.time = prop_float(el, "InVal", 0.0f);
        key.value = vec(el, "OutVal");
        key.arrive = vec(el, "ArriveTangent");
        key.leave = vec(el, "LeaveTangent");
        const std::string mode = to_lower(prop_name(el, "InterpMode"));  // absent = CIM_Linear, the enum's first
        key.mode = mode == "cim_constant" ? KeyedVector::Mode::Constant
                   : mode.rfind("cim_curve", 0) == 0 ? KeyedVector::Mode::Curve : KeyedVector::Mode::Linear;
        curve.keys.push_back(key);
    }
    return curve;
}

std::string prop_string(const UPropertyList& props, const char* name) {
    const UProperty* p = find_prop(props, name);
    return p ? p->s : std::string();
}

bool is_export(const UPKPackage& pkg, int32_t index) {
    return index > 0 && static_cast<size_t>(index) <= pkg.get_exports().size();
}

std::string class_of(const UPKPackage& pkg, int32_t index) {
    return is_export(pkg, index) ? pkg.get_export_class(pkg.get_exports()[static_cast<size_t>(index - 1)]) : std::string();
}

// "Group.Name" of a SoundCue reference: the form the audio engine files cues under.
std::string cue_name(const UPKPackage& pkg, int32_t object) {
    const std::string path = object_full_path(pkg, object);
    const size_t last = path.rfind('.');
    if (last == std::string::npos) return path;
    const size_t prev = path.rfind('.', last - 1);
    return prev == std::string::npos ? path.substr(last + 1) : path.substr(prev + 1);
}

// The content package of an object reference: the head of its path ("A_Props_Interactive").
std::string package_name(const UPKPackage& pkg, int32_t object) {
    const std::string path = object_full_path(pkg, object);
    return path.substr(0, path.find('.'));
}

void collect_matinee(const Packages& packages, const UPKPackage& pkg, const UPropertyList& interp_props, float time,
                     LevelIntroSequence& out, int depth);

IntroSoundEvent cue_event(const UPKPackage& pkg, int32_t cue, float time) {
    IntroSoundEvent ev;
    ev.time = time;
    ev.cue = cue_name(pkg, cue);
    ev.bank = package_name(pkg, cue);
    ev.voice = to_lower(object_full_path(pkg, cue)).find("a_vo_") != std::string::npos;
    return ev;
}

// The sounds an output of a Kismet op leads to: sound actions linked to it directly, behind a
// SeqAct_Delay, behind a remote event it activates, or in and behind another Matinee it plays (the
// door Faith kicks open has one of its own). A link or an input the level has disabled is not
// followed, and either end of a link can add a delay. `link` empty = every output.
void collect_sounds(const Packages& packages, const UPKPackage& pkg, const UPropertyList& op_props, const std::string& link,
                    float time, LevelIntroSequence& out, int depth) {
    const UProperty* outputs = find_prop(op_props, "OutputLinks");
    if (!outputs || depth > 6) return;
    for (const auto& output : outputs->elements) {
        if (!link.empty() && to_lower(prop_string(output, "LinkDesc")) != to_lower(link)) continue;
        if (prop_bool(output, "bDisabled", false)) continue;
        const UProperty* links = find_prop(output, "Links");
        if (!links) continue;
        for (const auto& l : links->elements) {
            const int32_t op = prop_object(l, "LinkedOp");
            const int32_t input = prop_int(l, "InputLinkIdx", 0);
            const std::string cls = class_of(pkg, op);
            if (cls.empty()) continue;
            UPropertyList props;
            parse_export_properties(pkg, op, props);
            float at = time + prop_float(output, "ActivateDelay", 0.0f);
            if (const UProperty* inputs = find_prop(props, "InputLinks");
                inputs && input >= 0 && static_cast<size_t>(input) < inputs->elements.size()) {
                const auto& in = inputs->elements[static_cast<size_t>(input)];
                if (prop_bool(in, "bDisabled", false)) continue;
                at += prop_float(in, "ActivateDelay", 0.0f);
            }
            if (input != 0) continue;  // every op followed here starts on its first input (Play, Start)
            if (cls.find("PlaySound") != std::string::npos) {
                if (const int32_t cue = prop_object(props, "PlaySound")) out.sounds.push_back(cue_event(pkg, cue, at));
            } else if (cls == "SeqAct_Delay") {
                collect_sounds(packages, pkg, props, "Finished", at + prop_float(props, "Duration", 0.0f), out, depth + 1);
            } else if (cls == "SeqAct_Interp") {
                collect_matinee(packages, pkg, props, at, out, depth + 1);
            } else if (cls == "SeqAct_ActivateRemoteEvent") {
                const std::string event = to_lower(prop_name(props, "EventName"));
                if (event.empty()) continue;
                for (const auto& other : packages) {
                    const auto& exports = other->get_exports();
                    for (size_t i = 0; i < exports.size(); ++i) {
                        if (other->get_export_class(exports[i]) != "SeqEvent_RemoteEvent") continue;
                        UPropertyList ev_props;
                        parse_export_properties(*other, static_cast<int32_t>(i) + 1, ev_props);
                        if (to_lower(prop_name(ev_props, "EventName")) != event) continue;
                        collect_sounds(packages, *other, ev_props, "", at, out, depth + 1);
                    }
                }
            }
        }
    }
}

// The screen fades an output of a Kismet op leads to: SeqAct_TdFadeEffect actions linked to it
// directly, behind a SeqAct_Delay, behind another fade (the action is latent: its Completed
// output fires when the fade has run), behind a remote event it activates (in any of the
// level's packages), or behind an op that passes the impulse straight on (the player-input
// switches). A SeqAct_TdStartMovementChallenge is followed to the SeqEvt_TdMovementChallengeStarted
// events of its challenge: the training area's challenge manager fires them as the challenge
// starts, and that is where its opening pan's FadeIn sits. `link` empty = every output.
void collect_fades(const Packages& packages, const UPKPackage& pkg, const UPropertyList& op_props, const std::string& link,
                   float time, LevelIntroSequence& out, int depth) {
    const UProperty* outputs = find_prop(op_props, "OutputLinks");
    if (!outputs || depth > 6) return;
    for (const auto& output : outputs->elements) {
        if (!link.empty() && to_lower(prop_string(output, "LinkDesc")) != to_lower(link)) continue;
        if (prop_bool(output, "bDisabled", false)) continue;
        const UProperty* links = find_prop(output, "Links");
        if (!links) continue;
        for (const auto& l : links->elements) {
            const int32_t op = prop_object(l, "LinkedOp");
            const int32_t input = prop_int(l, "InputLinkIdx", 0);
            const std::string cls = class_of(pkg, op);
            const bool relays = cls == "SeqAct_ActivateRemoteEvent" || cls == "SeqAct_TdStartMovementChallenge";
            const bool passes = cls == "SeqAct_TdEnablePlayerInput" || cls == "SeqAct_TdDisablePlayerInput";
            if (input != 0 || (cls != "SeqAct_TdFadeEffect" && cls != "SeqAct_Delay" && !relays && !passes)) continue;
            UPropertyList props;
            parse_export_properties(pkg, op, props);
            float at = time + prop_float(output, "ActivateDelay", 0.0f);
            if (const UProperty* inputs = find_prop(props, "InputLinks"); inputs && !inputs->elements.empty()) {
                if (prop_bool(inputs->elements[0], "bDisabled", false)) continue;
                at += prop_float(inputs->elements[0], "ActivateDelay", 0.0f);
            }
            if (cls == "SeqAct_Delay") {
                collect_fades(packages, pkg, props, "Finished", at + prop_float(props, "Duration", 0.0f), out, depth + 1);
                continue;
            }
            if (passes) {
                collect_fades(packages, pkg, props, "Out", at, out, depth + 1);
                continue;
            }
            if (relays) {
                // The events the op fires, in any package: remote events by EventName; the
                // challenge-started events by challenge (the action spells its property
                // "MovementChallege" in the cooked data).
                const bool challenge = cls == "SeqAct_TdStartMovementChallenge";
                std::string name = to_lower(prop_name(props, challenge ? "MovementChallege" : "EventName"));
                if (challenge && name.empty()) name = to_lower(prop_name(props, "MovementChallenge"));
                if (name.empty()) continue;
                const char* event_class = challenge ? "SeqEvt_TdMovementChallengeStarted" : "SeqEvent_RemoteEvent";
                const char* event_field = challenge ? "MovementChallenge" : "EventName";
                for (const auto& other : packages) {
                    const auto& exports = other->get_exports();
                    for (size_t i = 0; i < exports.size(); ++i) {
                        if (other->get_export_class(exports[i]) != event_class) continue;
                        UPropertyList ev_props;
                        parse_export_properties(*other, static_cast<int32_t>(i) + 1, ev_props);
                        if (to_lower(prop_name(ev_props, event_field)) != name) continue;
                        collect_fades(packages, *other, ev_props, "", at, out, depth + 1);
                    }
                }
                continue;
            }
            IntroFadeEvent ev;
            ev.time = at;
            ev.fade_out = prop_name(props, "FadeEffect") == "FadeOut";
            ev.duration = prop_float(props, "FadeTime", 0.5f);
            // SeqAct_TdFadeEffect divides each channel by 255 as integers: 1 at 255, else 0.
            if (const UProperty* c = find_prop(props, "FadeColor")) {
                ev.color = Vec3(c->v[0] >= 0.999f ? 1.0f : 0.0f, c->v[1] >= 0.999f ? 1.0f : 0.0f, c->v[2] >= 0.999f ? 1.0f : 0.0f);
            }
            bool known = false;
            for (const IntroFadeEvent& have : out.fades) {
                known = known || (std::abs(have.time - ev.time) < 1e-3f && have.fade_out == ev.fade_out &&
                                  std::abs(have.duration - ev.duration) < 1e-3f);
            }
            if (known) continue;
            out.fades.push_back(ev);
            collect_fades(packages, pkg, props, "Completed", at + std::max(ev.duration, 0.0166f), out, depth + 1);
        }
    }
}

// What the Matinee's Completed output stops (input 1 of a sound action): the long tracks it
// started, which must not play on when the intro is skipped.
void collect_stop_cues(const UPKPackage& pkg, const UPropertyList& interp_props, LevelIntroSequence& out) {
    const UProperty* outputs = find_prop(interp_props, "OutputLinks");
    if (!outputs) return;
    for (const auto& output : outputs->elements) {
        if (to_lower(prop_string(output, "LinkDesc")) != "completed") continue;
        const UProperty* links = find_prop(output, "Links");
        if (!links) continue;
        for (const auto& l : links->elements) {
            const int32_t op = prop_object(l, "LinkedOp");
            if (prop_int(l, "InputLinkIdx", 0) != 1 || class_of(pkg, op).find("PlaySound") == std::string::npos) continue;
            UPropertyList op_props;
            parse_export_properties(pkg, op, op_props);
            if (const int32_t cue = prop_object(op_props, "PlaySound")) out.stop_cues.push_back(cue_name(pkg, cue));
        }
    }
}

// The ops of `pkg` with an output linked to the first input of `target`.
std::vector<int32_t> feeders_of(const UPKPackage& pkg, int32_t target) {
    std::vector<int32_t> found;
    const auto& exports = pkg.get_exports();
    for (size_t i = 0; i < exports.size(); ++i) {
        if (pkg.get_export_class(exports[i]).compare(0, 3, "Seq") != 0) continue;
        UPropertyList props;
        parse_export_properties(pkg, static_cast<int32_t>(i) + 1, props);
        const UProperty* outputs = find_prop(props, "OutputLinks");
        if (!outputs) continue;
        bool feeds = false;
        for (const auto& output : outputs->elements) {
            const UProperty* links = find_prop(output, "Links");
            if (!links || prop_bool(output, "bDisabled", false)) continue;
            for (const auto& l : links->elements) {
                feeds = feeds || (prop_object(l, "LinkedOp") == target && prop_int(l, "InputLinkIdx", 0) == 0);
            }
        }
        if (feeds) found.push_back(static_cast<int32_t>(i) + 1);
    }
    return found;
}

// The fades that start with the intro: what plays its Matinee (a remote event, as a rule) fades the
// picture in on another of its links, and so can what leads to it: the op that activates that
// remote event, the checkpoint event behind that. Three steps back at most.
void collect_start_fades(const Packages& packages, const UPKPackage& pkg, int32_t target, LevelIntroSequence& out, int depth = 0) {
    if (depth > 3) return;
    for (int32_t feeder : feeders_of(pkg, target)) {
        UPropertyList props;
        parse_export_properties(pkg, feeder, props);
        collect_fades(packages, pkg, props, "", 0.0f, out, 0);
        collect_start_fades(packages, pkg, feeder, out, depth + 1);
        if (class_of(pkg, feeder) != "SeqEvent_RemoteEvent") continue;
        const std::string event = to_lower(prop_name(props, "EventName"));
        if (event.empty()) continue;
        for (const auto& other : packages) {
            const auto& exports = other->get_exports();
            for (size_t i = 0; i < exports.size(); ++i) {
                if (other->get_export_class(exports[i]) != "SeqAct_ActivateRemoteEvent") continue;
                UPropertyList activate;
                parse_export_properties(*other, static_cast<int32_t>(i) + 1, activate);
                if (to_lower(prop_name(activate, "EventName")) != event) continue;
                collect_start_fades(packages, *other, static_cast<int32_t>(i) + 1, out, depth + 1);
            }
        }
    }
}

// Whether the level starts this op by itself as it loads: a SeqEvent_LevelLoaded or a checkpoint's
// loaded event feeds it, directly or through the ops between and the remote events they activate
// (TdSPStoryGame.TriggerEventsOnLevelReload fires those events). A few steps back at most.
bool started_at_level_load(const Packages& packages, const UPKPackage& pkg, int32_t target, int depth = 0) {
    if (depth > 4) return false;
    const std::vector<int32_t> feeders = feeders_of(pkg, target);
    for (int32_t feeder : feeders) {
        const std::string cls = class_of(pkg, feeder);
        if (cls == "SeqEvent_LevelLoaded" || cls == "SeqEvt_TdCheckpointLoaded" || cls == "SeqEvt_TdCheckpointActivated") return true;
    }
    for (int32_t feeder : feeders) {
        if (started_at_level_load(packages, pkg, feeder, depth + 1)) return true;
        if (class_of(pkg, feeder) != "SeqEvent_RemoteEvent") continue;
        UPropertyList props;
        parse_export_properties(pkg, feeder, props);
        const std::string event = to_lower(prop_name(props, "EventName"));
        if (event.empty()) continue;
        for (const auto& other : packages) {
            const auto& exports = other->get_exports();
            for (size_t i = 0; i < exports.size(); ++i) {
                if (other->get_export_class(exports[i]) != "SeqAct_ActivateRemoteEvent") continue;
                UPropertyList activate;
                parse_export_properties(*other, static_cast<int32_t>(i) + 1, activate);
                if (to_lower(prop_name(activate, "EventName")) != event) continue;
                if (started_at_level_load(packages, *other, static_cast<int32_t>(i) + 1, depth + 1)) return true;
            }
        }
    }
    return false;
}

// The doors a Matinee's movement tracks turn: for each group whose actor is an InterpActor, its
// rotation track's yaw sampled into out.door_swings, as degrees from the actor's closed rotation.
void collect_door_swings(const UPKPackage& pkg, const UPropertyList& interp_props, const UPropertyList& data_props, float time,
                         float rate, LevelIntroSequence& out) {
    const UProperty* groups = find_prop(data_props, "InterpGroups");
    const UProperty* var_links = find_prop(interp_props, "VariableLinks");
    if (!groups || !var_links) return;
    for (int32_t g : groups->ints) {
        if (!is_export(pkg, g)) continue;
        UPropertyList gp;
        parse_export_properties(pkg, g, gp);
        const UProperty* tracks = find_prop(gp, "InterpTracks");
        if (!tracks) continue;
        const std::string group_name = to_lower(prop_name(gp, "GroupName"));
        int32_t actor = 0;
        for (const auto& vl : var_links->elements) {
            if (to_lower(prop_string(vl, "LinkDesc")) != group_name) continue;
            const UProperty* vars = find_prop(vl, "LinkedVariables");
            if (!vars) continue;
            for (int32_t v : vars->ints) {
                if (class_of(pkg, v) != "SeqVar_Object") continue;
                UPropertyList vp;
                parse_export_properties(pkg, v, vp);
                const int32_t object = prop_object(vp, "ObjValue");
                if (class_of(pkg, object) == "InterpActor") actor = object;
            }
        }
        if (actor == 0) continue;
        UPropertyList ap;
        parse_export_properties(pkg, actor, ap);
        Vec3 hinge(0.0f, 0.0f, 0.0f);
        float closed_yaw = 0.0f;
        if (const UProperty* lp = find_prop(ap, "Location")) hinge = Vec3(lp->v[0], lp->v[1], lp->v[2]);
        if (const UProperty* rp = find_prop(ap, "Rotation")) closed_yaw = Rotator(rp->vi[0], rp->vi[1], rp->vi[2]).to_degrees().y;

        for (int32_t t : tracks->ints) {
            if (class_of(pkg, t) != "InterpTrackMove") continue;
            UPropertyList tp;
            parse_export_properties(pkg, t, tp);
            const KeyedVector euler = read_vector_curve(tp, "EulerTrack");  // roll, pitch, yaw in degrees
            if (euler.keys.size() < 2) continue;
            // A track that only slides its actor (the opening's title lettering) turns no door.
            float low = euler.keys.front().value.z;
            float high = low;
            for (const KeyedVector::Key& key : euler.keys) {
                low = std::min(low, key.value.z);
                high = std::max(high, key.value.z);
            }
            if (high - low < 0.5f) continue;
            // IMF_World (the default) keys hold the actor's rotation itself; relative ones its turn.
            // Whole turns between the two mean nothing and are taken out once, so the samples stay
            // continuous.
            const bool relative = to_lower(prop_name(tp, "MoveFrame")) == "imf_relativetoinitial";
            const float first = euler.keys.front().value.z - (relative ? 0.0f : closed_yaw);
            const float unwind = std::remainder(first, 360.0f) - first;
            IntroDoorSwing* swing = nullptr;
            for (IntroDoorSwing& known : out.door_swings) {
                if ((known.hinge - hinge).length() < 4.0f) swing = &known;
            }
            if (!swing) {
                out.door_swings.emplace_back();
                swing = &out.door_swings.back();
                swing->hinge = hinge;
            }
            const float t0 = euler.keys.front().time;
            const float t1 = euler.keys.back().time;
            const int steps = std::max(1, static_cast<int>(std::ceil((t1 - t0) * 60.0f)));
            for (int i = 0; i <= steps; ++i) {
                const float key_time = t0 + (t1 - t0) * static_cast<float>(i) / static_cast<float>(steps);
                const float yaw = euler.at(key_time).z - (relative ? 0.0f : closed_yaw) + unwind;
                swing->yaw_keys.emplace_back(time + key_time / rate, yaw);
            }
        }
    }
}

// A Matinee started from Kismet at `time`: the cues on its sound tracks, what its event keys
// trigger, the doors it turns, and what its completion does.
void collect_matinee(const Packages& packages, const UPKPackage& pkg, const UPropertyList& interp_props, float time,
                     LevelIntroSequence& out, int depth) {
    int32_t data = 0;
    if (const UProperty* var_links = find_prop(interp_props, "VariableLinks")) {
        for (const auto& vl : var_links->elements) {
            const UProperty* vars = find_prop(vl, "LinkedVariables");
            if (!vars) continue;
            for (int32_t v : vars->ints) {
                if (class_of(pkg, v) == "InterpData") data = v;
            }
        }
    }
    if (data == 0) return;
    UPropertyList data_props;
    parse_export_properties(pkg, data, data_props);
    const float rate = std::max(0.01f, prop_float(interp_props, "PlayRate", 1.0f));
    if (const UProperty* groups = find_prop(data_props, "InterpGroups")) {
        for (int32_t g : groups->ints) {
            if (!is_export(pkg, g)) continue;
            UPropertyList gp;
            parse_export_properties(pkg, g, gp);
            const UProperty* tracks = find_prop(gp, "InterpTracks");
            if (!tracks) continue;
            for (int32_t t : tracks->ints) {
                const std::string cls = class_of(pkg, t);
                if (cls.empty()) continue;
                UPropertyList tp;
                parse_export_properties(pkg, t, tp);
                if (cls == "InterpTrackEvent") {
                    if (const UProperty* keys = find_prop(tp, "EventTrack")) {
                        for (const auto& k : keys->elements) {
                            collect_sounds(packages, pkg, interp_props, prop_name(k, "EventName"),
                                           time + prop_float(k, "Time", 0.0f) / rate, out, depth);
                            collect_fades(packages, pkg, interp_props, prop_name(k, "EventName"),
                                          time + prop_float(k, "Time", 0.0f) / rate, out, depth);
                        }
                    }
                } else if (cls.find("InterpTrackSound") != std::string::npos) {
                    if (const UProperty* keys = find_prop(tp, "Sounds")) {
                        for (const auto& k : keys->elements) {
                            if (const int32_t cue = prop_object(k, "Sound")) {
                                out.sounds.push_back(cue_event(pkg, cue, time + prop_float(k, "Time", 0.0f) / rate));
                            }
                        }
                    }
                }
            }
        }
    }
    collect_door_swings(pkg, interp_props, data_props, time, rate, out);
    collect_sounds(packages, pkg, interp_props, "Completed", time + prop_float(data_props, "InterpLength", 0.0f) / rate, out, depth);
    collect_fades(packages, pkg, interp_props, "Completed", time + prop_float(data_props, "InterpLength", 0.0f) / rate, out, depth);
}

// The sounds the animation itself asks for: AnimNotify_Sound, AnimNotify_Footstep and
// TdAnimNotify_CharacterSound entries of the sequence's Notifies, up to its length.
//
// Retail starts every AnimNotify_Sound cue twice in the same frame (its sound log has each of them
// doubled through all ten intros, and no footstep or clothing notify), so a cue with a random node
// plays two of its variants together. They are asked for twice here too.
void collect_notifies(const UPKPackage& pkg, int32_t sequence, float sequence_length, float start_time,
                      std::vector<IntroSoundEvent>& out) {
    UPropertyList props;
    parse_export_properties(pkg, sequence, props);
    const UProperty* notifies = find_prop(props, "Notifies");
    if (!notifies) return;
    for (const auto& n : notifies->elements) {
        const float t = prop_float(n, "Time", 0.0f);
        const int32_t notify = prop_object(n, "Notify");
        if (t > sequence_length + 1e-3f || !is_export(pkg, notify)) continue;
        const std::string cls = class_of(pkg, notify);
        UPropertyList np;
        parse_export_properties(pkg, notify, np);
        IntroSoundEvent ev;
        ev.time = start_time + t;
        ev.from_notify = true;
        if (cls == "AnimNotify_Sound") {
            const int32_t cue = prop_object(np, "SoundCue");
            if (cue == 0) continue;
            ev.cue = cue_name(pkg, cue);
            ev.bank = package_name(pkg, cue);
            out.push_back(ev);
        } else if (cls == "AnimNotify_Footstep") {
            // FootDown: the footstep cue's number (1 Sneak, 2 Walk, 3 Run, .. 10 LandHard), negative for the left foot.
            ev.footstep = std::abs(prop_int(np, "FootDown", 0));
            if (ev.footstep == 0) continue;
            // 34 is not a step on a surface but the body's roll (retail plays Body.Roll for it).
            if (ev.footstep == 34) {
                ev.footstep = 0;
                ev.cue = "Body.Roll";
                ev.bank = "A_Character_Female_01";
            }
        } else if (cls == "TdAnimNotify_CharacterSound") {
            // ECSClothing_Run -> A_Character_Female_01.Cloth.Run
            const std::string trigger = prop_name(np, "TriggerType");
            static const std::string kClothing = "ECSClothing_";
            if (trigger.compare(0, kClothing.size(), kClothing) != 0) continue;
            ev.cue = "Cloth." + trigger.substr(kClothing.size());
            ev.bank = "A_Character_Female_01";
        } else {
            continue;
        }
        out.push_back(std::move(ev));
    }
}

struct IntroMatinee {
    const UPKPackage* pkg = nullptr;
    int32_t interp = 0;       // SeqAct_Interp
    int32_t data = 0;         // InterpData
    int32_t actor = 0;        // the placed first-person body
    std::string group;        // the group linked to the local pawn
};

// The Matinee that drives the local pawn together with a placed skeletal mesh, if this
// SeqAct_Interp is one.
bool find_pawn_matinee(const UPKPackage& pkg, int32_t interp, const UPropertyList& interp_props, IntroMatinee& m) {
    const UProperty* var_links = find_prop(interp_props, "VariableLinks");
    if (!var_links) return false;
    m = IntroMatinee{};
    m.pkg = &pkg;
    m.interp = interp;
    for (const auto& link : var_links->elements) {
        const UProperty* vars = find_prop(link, "LinkedVariables");
        if (!vars) continue;
        bool pawn = false;
        int32_t actor = 0;
        for (int32_t v : vars->ints) {
            const std::string cls = class_of(pkg, v);
            if (cls == "InterpData") m.data = v;
            if (cls == "SeqVar_TdLocalPawn") pawn = true;
            if (cls != "SeqVar_Object") continue;
            UPropertyList vp;
            parse_export_properties(pkg, v, vp);
            const int32_t obj = prop_object(vp, "ObjValue");
            const std::string ocls = class_of(pkg, obj);
            if (ocls == "SkeletalMeshActorMAT" || ocls == "SkeletalMeshActor") actor = obj;
        }
        if (pawn && actor > 0) {
            m.actor = actor;
            m.group = prop_string(link, "LinkDesc");
        }
    }
    return m.data > 0 && m.actor > 0;
}

// One animation of the pawn's group, baked: the first-person view per animation frame in the
// rig's own space, and the full-body root it rides on when the level ships one.
struct BakedAnim {
    int32_t seq_export = 0;
    std::string name;
    float length = 0.0f;
    size_t tracks = 0;
    bool on_full_body_root = false;
    float root_apart = 0.0f;
    std::vector<AnimSystem::CannedCameraFrame> frames;
};

// The sequence of that name that animates the first-person skeleton. A level can carry several
// with one name (a second body, a prop); the one that drives the most first-person bones, EyeJoint
// among them, is the view's.
//
// Edge and Boat also ship the full-body version (89 tracks to the first-person 82), and that is
// the one the pawn's own mesh plays: its root is what moves the pawn, and the first-person body
// rides on the pawn. The two roots are not always the same - in Edge they part by up to 29 uu and
// 14 degrees for a few seconds, and retail's pawn and camera follow the full-body one.
bool bake_anim(const std::string& game_root, const UPKPackage& pkg, const std::string& anim_name, BakedAnim& out) {
    const auto& exports = pkg.get_exports();
    int32_t best_seq = 0;
    int best_score = 0;
    size_t best_tracks = 0;
    AnimSetAsset best_set;
    struct Body {
        int32_t seq = 0;
        AnimSetAsset set;
    };
    std::vector<Body> bodies;  // every sequence of that name that animates a body
    for (size_t e = 0; e < exports.size(); ++e) {
        if (pkg.get_export_class(exports[e]) != "AnimSequence") continue;
        UPropertyList sp;
        parse_export_properties(pkg, static_cast<int32_t>(e) + 1, sp);
        if (to_lower(prop_name(sp, "SequenceName")) != to_lower(anim_name)) continue;
        AnimSetAsset set;
        if (!AnimSystem::parse_single_anim_sequence(pkg, static_cast<int32_t>(e) + 1, set) || set.sequences.empty()) continue;
        const size_t tracks = set.track_bone_names.size();
        if (set.bone_to_track.count("root") && set.bone_to_track.count("hips")) {
            bodies.push_back({static_cast<int32_t>(e) + 1, set});
        }
        bool has_eye = false;
        const int score = AnimSystem::count_first_person_tracks(game_root, set, &has_eye);
        if (!has_eye) continue;
        if (score > best_score || (score == best_score && tracks < best_tracks)) {
            best_seq = static_cast<int32_t>(e) + 1;
            best_score = score;
            best_tracks = tracks;
            best_set = std::move(set);
        }
    }
    if (best_seq <= 0) return false;
    const AnimSequenceAsset& seq = best_set.sequences.begin()->second;

    // The full-body root, when there is one and it is a different track from the view's own.
    const AnimTrack* pawn_root = nullptr;
    float root_apart = 0.0f;
    size_t body_tracks = best_tracks;
    const Body* full_body = nullptr;
    for (const Body& b : bodies) {
        if (b.seq == best_seq || b.set.track_bone_names.size() <= body_tracks) continue;
        body_tracks = b.set.track_bone_names.size();
        full_body = &b;
    }
    if (full_body) {
        const AnimSetAsset& body_set = full_body->set;
        const AnimSequenceAsset& body_seq = body_set.sequences.begin()->second;
        const size_t body_root = static_cast<size_t>(body_set.bone_to_track.at("root"));
        const auto own = best_set.bone_to_track.find("root");
        if (body_root < body_seq.tracks.size() && own != best_set.bone_to_track.end() &&
            static_cast<size_t>(own->second) < seq.tracks.size() && body_seq.num_frames == seq.num_frames) {
            const AnimTrack& theirs = body_seq.tracks[body_root];
            const AnimTrack& ours = seq.tracks[static_cast<size_t>(own->second)];
            if (theirs.positions.size() == ours.positions.size()) {
                for (size_t k = 0; k < ours.positions.size(); ++k) {
                    root_apart = std::max(root_apart, (theirs.positions[k] - ours.positions[k]).length());
                }
            }
            pawn_root = &theirs;
        }
    }

    out = BakedAnim{};
    if (!AnimSystem::bake_canned_camera(game_root, best_set, seq, out.frames, pawn_root) || out.frames.size() < 2) return false;
    out.seq_export = best_seq;
    out.name = seq.name;
    out.length = seq.length;
    out.tracks = best_tracks;
    out.on_full_body_root = pawn_root != nullptr;
    out.root_apart = root_apart;
    return true;
}

struct AnimKey {
    std::string name;
    float start = 0.0f;  // StartTime in the Matinee
};

// Bakes one pawn Matinee into `out`. With `intro_rule` the pawn group's first animation whose
// name contains "intro" is the one and only segment, as the intro has always been read; otherwise
// every animation of the group's AnimControl track is a segment, baked onto one timeline at the
// first segment's frame rate (between segments the pose holds).
bool bake_pawn_matinee(const std::string& game_root, const Packages& packages, const IntroMatinee& m, bool intro_rule,
                       LevelIntroSequence& out) {
    const UPKPackage& pkg = *m.pkg;
    out = LevelIntroSequence{};
    UPropertyList interp_props;
    parse_export_properties(pkg, m.interp, interp_props);
    UPropertyList data_props;
    parse_export_properties(pkg, m.data, data_props);
    const UProperty* groups = find_prop(data_props, "InterpGroups");
    if (!groups) return false;

    // The pawn's group: its animations, where in the Matinee they start, and how the actor moves.
    std::vector<AnimKey> anims;
    KeyedVector move;
    std::vector<std::pair<float, std::string>> events;  // every group's event keys
    for (int32_t g : groups->ints) {
        if (!is_export(pkg, g)) continue;
        UPropertyList gp;
        parse_export_properties(pkg, g, gp);
        const bool pawn_group = to_lower(prop_name(gp, "GroupName")) == to_lower(m.group);
        const UProperty* tracks = find_prop(gp, "InterpTracks");
        if (!tracks) continue;
        for (int32_t t : tracks->ints) {
            const std::string cls = class_of(pkg, t);
            if (cls.empty()) continue;
            UPropertyList tp;
            parse_export_properties(pkg, t, tp);
            if (cls == "InterpTrackEvent") {
                if (const UProperty* keys = find_prop(tp, "EventTrack")) {
                    for (const auto& k : keys->elements) {
                        events.emplace_back(prop_float(k, "Time", 0.0f), prop_name(k, "EventName"));
                    }
                }
            } else if (pawn_group && cls == "InterpTrackAnimControl" && anims.empty()) {
                if (const UProperty* seqs = find_prop(tp, "AnimSeqs")) {
                    for (const auto& k : seqs->elements) {
                        const std::string name = prop_name(k, "AnimSeqName");
                        if (name.empty()) continue;
                        if (intro_rule && to_lower(name).find("intro") == std::string::npos) continue;
                        anims.push_back({name, prop_float(k, "StartTime", 0.0f)});
                        if (intro_rule) break;
                    }
                }
            } else if (pawn_group && cls == "InterpTrackMove") {
                move = read_vector_curve(tp, "PosTrack");
            }
        }
    }
    if (anims.empty()) return false;
    std::stable_sort(anims.begin(), anims.end(), [](const AnimKey& a, const AnimKey& b) { return a.start < b.start; });

    std::vector<BakedAnim> baked;
    for (const AnimKey& key : anims) {
        BakedAnim b;
        if (!bake_anim(game_root, pkg, key.name, b)) {
            if (baked.empty()) return false;  // the view is not in it
            continue;                          // a later segment without an eye: the pose holds
        }
        baked.push_back(std::move(b));
        out.segments.push_back({baked.back().name, baked.back().seq_export, key.start, baked.back().length});
    }

    UPropertyList actor_props;
    parse_export_properties(pkg, m.actor, actor_props);
    Vec3 actor_loc(0.0f, 0.0f, 0.0f);
    float actor_yaw = 90.0f;
    if (const UProperty* lp = find_prop(actor_props, "Location")) actor_loc = Vec3(lp->v[0], lp->v[1], lp->v[2]);
    if (const UProperty* rp = find_prop(actor_props, "Rotation")) {
        actor_yaw = Rotator(rp->vi[0], rp->vi[1], rp->vi[2]).to_degrees().y;
    }

    // Where the animation's root sits against the placed actor:
    // SkeletalMeshActorMAT (CINE_Female1p) always has its pivot on the floor (root height), even
    // when UnrealEd inserted a default constant (0,0,0) InterpTrackMove track. Only 3P
    // SkeletalMeshActor (SK_TKY_Crim_Fixer, e.g. Boat sp07_intro / sp07_truck) is placed at
    // pawn capsule center (+94 uu above the floor).
    const bool pawn_at_actor = class_of(pkg, m.actor) == "SkeletalMeshActor";
    Vec3 anim_origin = actor_loc;
    if (pawn_at_actor) anim_origin.z -= kPawnAboveRoot;

    // One timeline: the first segment's frames as they are; later segments resampled onto the
    // same spacing, the pose held through any gap. A single segment is left frame for frame.
    const float first_start = out.segments.front().start_sec;
    float span = 0.0f;
    for (const auto& seg : out.segments) span = std::max(span, seg.start_sec + seg.length_sec - first_start);
    const float frame_dt = baked.front().length / static_cast<float>(baked.front().frames.size() - 1);
    const size_t frame_count = baked.size() == 1 ? baked.front().frames.size()
                                                 : static_cast<size_t>(std::ceil(span / frame_dt)) + 1;
    const auto sample = [&](float t) -> AnimSystem::CannedCameraFrame {
        // The last segment started by t; before the first, the first's first frame.
        size_t si = 0;
        for (size_t k = 0; k < out.segments.size(); ++k) {
            if (out.segments[k].start_sec <= t + 1e-4f) si = k;
        }
        const BakedAnim& b = baked[si];
        const float local = std::clamp(t - out.segments[si].start_sec, 0.0f, b.length);
        const float b_dt = b.length / static_cast<float>(b.frames.size() - 1);
        const float fidx = std::clamp(local / std::max(1e-6f, b_dt), 0.0f, static_cast<float>(b.frames.size() - 1));
        const size_t i0 = std::min(static_cast<size_t>(fidx), b.frames.size() - 1);
        const size_t i1 = std::min(i0 + 1, b.frames.size() - 1);
        const float a = fidx - static_cast<float>(i0);
        AnimSystem::CannedCameraFrame f;
        f.eye_pos = b.frames[i0].eye_pos + (b.frames[i1].eye_pos - b.frames[i0].eye_pos) * a;
        f.forward = (b.frames[i0].forward + (b.frames[i1].forward - b.frames[i0].forward) * a).normalized();
        f.up = (b.frames[i0].up + (b.frames[i1].up - b.frames[i0].up) * a).normalized();
        f.root_pos = b.frames[i0].root_pos + (b.frames[i1].root_pos - b.frames[i0].root_pos) * a;
        return f;
    };

    out.cam_pos.reserve(frame_count);
    for (size_t f = 0; f < frame_count; ++f) {
        const float t = first_start + static_cast<float>(f) * frame_dt;
        const AnimSystem::CannedCameraFrame frame = baked.size() == 1 ? baked.front().frames[f] : sample(t);
        // An InterpTrackMove carries the pawn, and the animation with it, from where it started.
        RigFrame rig(anim_origin, actor_yaw);
        rig.origin = anim_origin + rig.actor_offset(move.at(t));
        out.cam_pos.push_back(rig.pos(frame.eye_pos));
        out.cam_forward.push_back(rig.dir(frame.forward).normalized());
        out.cam_up.push_back(rig.dir(frame.up).normalized());
        out.root_pos.push_back(rig.pos(frame.root_pos));
    }

    for (const auto& [time, name] : events) {
        collect_sounds(packages, pkg, interp_props, name, time, out, 0);
        collect_fades(packages, pkg, interp_props, name, time, out, 0);
    }
    collect_fades(packages, pkg, interp_props, "Completed", prop_float(data_props, "InterpLength", 0.0f), out, 0);
    for (size_t k = 0; k < baked.size(); ++k) {
        collect_notifies(pkg, baked[k].seq_export, baked[k].length, out.segments[k].start_sec, out.sounds);
    }
    // The doors the Matinee itself turns (the Mall's), beside those behind its events.
    collect_door_swings(pkg, interp_props, data_props, 0.0f, 1.0f, out);
    for (IntroDoorSwing& swing : out.door_swings) {
        std::stable_sort(swing.yaw_keys.begin(), swing.yaw_keys.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    collect_stop_cues(pkg, interp_props, out);
    std::stable_sort(out.sounds.begin(), out.sounds.end(),
                     [](const IntroSoundEvent& a, const IntroSoundEvent& b) { return a.time < b.time; });
    collect_start_fades(packages, pkg, m.interp, out);
    std::stable_sort(out.fades.begin(), out.fades.end(),
                     [](const IntroFadeEvent& a, const IntroFadeEvent& b) { return a.time < b.time; });

    const Vec3 end_fwd = out.cam_forward.back();
    out.valid = true;
    out.seq_name = baked.front().name;
    out.package_path = pkg.get_file_path();
    out.anim_export_index_1 = baked.front().seq_export;
    out.actor_location = actor_loc;
    out.actor_yaw_deg = actor_yaw;
    out.start_offset_sec = first_start;
    out.duration_sec = baked.size() == 1 ? baked.front().length : span;
    out.matinee_length_sec = std::max(prop_float(data_props, "InterpLength", 0.0f), first_start + out.duration_sec);
    out.start_feet_pos = out.root_pos.front();
    out.end_feet_pos = out.root_pos.back();
    out.end_yaw_deg = std::atan2(end_fwd.y, end_fwd.x) * RAD2DEG;
    out.interp_package = to_lower(std::filesystem::path(pkg.get_file_path()).stem().string());
    out.interp_export_index_1 = m.interp;
    out.skippable = prop_bool(interp_props, "bIsSkippable", false);

    int voices = 0;
    for (const auto& s : out.sounds) voices += s.voice ? 1 : 0;
    std::cout << "[Level] " << (intro_rule ? "Level intro '" : "Player cutscene '") << out.seq_name << "' (" << out.duration_sec
              << " s animation from " << first_start << " s of a " << out.matinee_length_sec << " s Matinee, " << frame_count
              << " frames, " << baked.front().tracks << " tracks";
    if (baked.size() > 1) std::cout << ", " << baked.size() << " segments";
    std::cout << "): start=(" << int(out.start_feet_pos.x) << "," << int(out.start_feet_pos.y) << "," << int(out.start_feet_pos.z)
              << ") -> end=(" << int(out.end_feet_pos.x) << "," << int(out.end_feet_pos.y) << "," << int(out.end_feet_pos.z)
              << ", yaw=" << int(out.end_yaw_deg) << "); " << (pawn_at_actor ? "movement track, root below the placed actor; " : "");
    if (!out.door_swings.empty()) std::cout << out.door_swings.size() << " door(s) swung; ";
    if (baked.front().on_full_body_root) std::cout << "on the full-body root (up to " << baked.front().root_apart << " uu from its own); ";
    std::cout << out.sounds.size() << " sounds, " << voices << " of them voice lines" << std::endl;
    return true;
}

// --- a Matinee seen through a placed camera ------------------------------------------------------
//
// The training area opens on one: no first-person animation, but a Matinee whose director track
// cuts to a CameraActor's group as it starts, and whose movement track carries that camera across
// the level. The view is the camera for the Matinee's length; the pawn stands where it spawned.

struct DirectorMatinee {
    const UPKPackage* pkg = nullptr;
    int32_t interp = 0;      // SeqAct_Interp
    int32_t data = 0;        // InterpData
    std::string cam_group;   // the group the director cuts to at the start
    int32_t camera = 0;      // its CameraActor
};

// The Matinee whose director cuts to a CameraActor as it starts, if this SeqAct_Interp is one.
bool find_director_matinee(const UPKPackage& pkg, int32_t interp, const UPropertyList& interp_props, DirectorMatinee& m) {
    m = DirectorMatinee{};
    m.pkg = &pkg;
    m.interp = interp;
    const UProperty* var_links = find_prop(interp_props, "VariableLinks");
    if (!var_links) return false;
    for (const auto& link : var_links->elements) {
        const UProperty* vars = find_prop(link, "LinkedVariables");
        if (!vars) continue;
        for (int32_t v : vars->ints) {
            if (class_of(pkg, v) == "InterpData") m.data = v;
        }
    }
    if (m.data <= 0) return false;
    UPropertyList data_props;
    parse_export_properties(pkg, m.data, data_props);
    const UProperty* groups = find_prop(data_props, "InterpGroups");
    if (!groups) return false;
    // The director's first cut, which must come with the start.
    float first_cut = 1.0e9f;
    for (int32_t g : groups->ints) {
        if (class_of(pkg, g) != "InterpGroupDirector") continue;
        UPropertyList gp;
        parse_export_properties(pkg, g, gp);
        const UProperty* tracks = find_prop(gp, "InterpTracks");
        if (!tracks) continue;
        for (int32_t t : tracks->ints) {
            if (class_of(pkg, t) != "InterpTrackDirector") continue;
            UPropertyList tp;
            parse_export_properties(pkg, t, tp);
            const UProperty* cuts = find_prop(tp, "CutTrack");
            if (!cuts) continue;
            for (const auto& cut : cuts->elements) {
                const float at = prop_float(cut, "Time", 0.0f);
                if (at < first_cut) {
                    first_cut = at;
                    m.cam_group = prop_name(cut, "TargetCamGroup");
                }
            }
        }
    }
    if (m.cam_group.empty() || first_cut > 0.05f) return false;
    // The group's actor, through the variable link that carries the group's name.
    for (const auto& link : var_links->elements) {
        if (to_lower(prop_string(link, "LinkDesc")) != to_lower(m.cam_group)) continue;
        const UProperty* vars = find_prop(link, "LinkedVariables");
        if (!vars) continue;
        for (int32_t v : vars->ints) {
            if (class_of(pkg, v) != "SeqVar_Object") continue;
            UPropertyList vp;
            parse_export_properties(pkg, v, vp);
            const int32_t obj = prop_object(vp, "ObjValue");
            if (class_of(pkg, obj) == "CameraActor") m.camera = obj;
        }
    }
    return m.camera > 0;
}

// A placed actor's frame, FRotationTranslationMatrix(Rotation, Location): the reference an
// IMF_RelativeToInitial movement track moves in (UInterpTrackInstMove::CalcInitialTransform).
struct ActorFrame {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Rotator rotation;
    Vec3 x{1.0f, 0.0f, 0.0f};
    Vec3 y{0.0f, 1.0f, 0.0f};
    Vec3 z{0.0f, 0.0f, 1.0f};

    void set(const UPropertyList& props) {
        if (const UProperty* lp = find_prop(props, "Location")) origin = Vec3(lp->v[0], lp->v[1], lp->v[2]);
        if (const UProperty* rp = find_prop(props, "Rotation")) {
            // Whole turns taken out (a cooked yaw can be several turns round), for the float trigonometry.
            const auto wind = [](float v) { return std::remainder(v, 65536.0f); };
            rotation = Rotator(wind(static_cast<float>(rp->vi[0])), wind(static_cast<float>(rp->vi[1])),
                               wind(static_cast<float>(rp->vi[2])));
        }
        x = rotation.forward();
        y = rotation.right();
        z = rotation.up();
    }
    [[nodiscard]] Vec3 dir(const Vec3& v) const { return x * v.x + y * v.y + z * v.z; }
    [[nodiscard]] Vec3 pos(const Vec3& v) const { return origin + dir(v); }
};

// One group's actor and the movement track that drives it, read the way
// UInterpTrackMove::GetLocationAtTime evaluates them (MirrorsEdge.exe 0x00e41340): the track's
// position and Euler rotation at the time, composed with the reference frame of its MoveFrame.
struct MovingActor {
    ActorFrame initial;
    KeyedVector pos;        // PosTrack
    KeyedVector euler;      // EulerTrack: roll, pitch, yaw in degrees
    bool relative = false;  // IMF_RelativeToInitial; else IMF_World
    bool look_at = false;   // IMR_LookAtGroup: the rotation is the line to another group's actor
    std::string look_at_group;

    [[nodiscard]] Vec3 position(float t) const {
        if (pos.keys.empty()) return initial.origin;  // no keys: the actor stays where it is
        const Vec3 rel = pos.at(t);
        return relative ? initial.pos(rel) : rel;
    }
    void orientation(float t, Vec3& forward, Vec3& up) const {
        if (euler.keys.empty()) {
            forward = initial.x;
            up = initial.z;
            return;
        }
        const Vec3 e = euler.at(t);
        const Rotator rel = Rotator::from_degrees(e.y, e.z, e.x);
        forward = relative ? initial.dir(rel.forward()) : rel.forward();
        up = relative ? initial.dir(rel.up()) : rel.up();
    }
};

// Where the level's pawn spawns (TdSPStoryGame.FindPlayerStart): its default TdCheckpoint, else
// its first, else a player start.
bool find_player_start(const Packages& packages, Vec3& location, float& yaw_deg) {
    int best = 0;  // 3 the default checkpoint, 2 a checkpoint, 1 a player start
    for (const auto& pkg_ptr : packages) {
        const UPKPackage& pkg = *pkg_ptr;
        const auto& exports = pkg.get_exports();
        for (size_t i = 0; i < exports.size(); ++i) {
            const std::string cls = pkg.get_export_class(exports[i]);
            int rank = cls == "TdCheckpoint" ? 2 : (cls == "PlayerStart" || cls == "TdTutorialStart") ? 1 : 0;
            if (rank == 0 || rank < best) continue;
            UPropertyList props;
            parse_export_properties(pkg, static_cast<int32_t>(i) + 1, props);
            if (rank == 2 && prop_bool(props, "DefaultCheckpoint", false)) rank = 3;
            const UProperty* lp = find_prop(props, "Location");
            if (rank <= best || !lp) continue;
            best = rank;
            location = Vec3(lp->v[0], lp->v[1], lp->v[2]);
            yaw_deg = 0.0f;
            if (const UProperty* rp = find_prop(props, "Rotation")) yaw_deg = Rotator(rp->vi[0], rp->vi[1], rp->vi[2]).to_degrees().y;
        }
    }
    return best > 0;
}

// Bakes a director Matinee into `out`: the camera's position and view at 60 Hz through the
// Matinee, the sounds and fades behind its event keys and its completion, and the pawn's place,
// which does not change.
bool bake_director_matinee(const Packages& packages, const DirectorMatinee& m, LevelIntroSequence& out) {
    const UPKPackage& pkg = *m.pkg;
    out = LevelIntroSequence{};
    UPropertyList interp_props;
    parse_export_properties(pkg, m.interp, interp_props);
    UPropertyList data_props;
    parse_export_properties(pkg, m.data, data_props);
    const UProperty* groups = find_prop(data_props, "InterpGroups");
    const UProperty* var_links = find_prop(interp_props, "VariableLinks");
    const float length = prop_float(data_props, "InterpLength", 0.0f);
    if (!groups || !var_links || length <= 0.0f) return false;

    // Every group with an actor: the actor as placed and its movement track. The camera's group is
    // one; the group it looks at, if any, another.
    std::unordered_map<std::string, MovingActor> movers;  // by group name, lower case
    std::vector<std::pair<float, std::string>> events;    // every group's event keys
    for (int32_t g : groups->ints) {
        if (!is_export(pkg, g)) continue;
        UPropertyList gp;
        parse_export_properties(pkg, g, gp);
        const UProperty* tracks = find_prop(gp, "InterpTracks");
        if (!tracks) continue;
        const std::string group_name = to_lower(prop_name(gp, "GroupName"));
        int32_t actor = 0;
        for (const auto& vl : var_links->elements) {
            if (to_lower(prop_string(vl, "LinkDesc")) != group_name) continue;
            const UProperty* vars = find_prop(vl, "LinkedVariables");
            if (!vars) continue;
            for (int32_t v : vars->ints) {
                if (class_of(pkg, v) != "SeqVar_Object") continue;
                UPropertyList vp;
                parse_export_properties(pkg, v, vp);
                if (const int32_t obj = prop_object(vp, "ObjValue"); is_export(pkg, obj)) actor = obj;
            }
        }
        MovingActor mover;
        if (actor > 0) {
            UPropertyList ap;
            parse_export_properties(pkg, actor, ap);
            mover.initial.set(ap);
        }
        bool moved = false;
        for (int32_t t : tracks->ints) {
            const std::string cls = class_of(pkg, t);
            if (cls.empty()) continue;
            UPropertyList tp;
            parse_export_properties(pkg, t, tp);
            if (cls == "InterpTrackEvent") {
                if (const UProperty* keys = find_prop(tp, "EventTrack")) {
                    for (const auto& k : keys->elements) {
                        events.emplace_back(prop_float(k, "Time", 0.0f), prop_name(k, "EventName"));
                    }
                }
            } else if (cls == "InterpTrackMove" && !moved) {
                moved = true;
                mover.pos = read_vector_curve(tp, "PosTrack");
                mover.euler = read_vector_curve(tp, "EulerTrack");
                mover.relative = to_lower(prop_name(tp, "MoveFrame")) == "imf_relativetoinitial";
                mover.look_at = to_lower(prop_name(tp, "RotMode")) == "imr_lookatgroup";
                mover.look_at_group = to_lower(prop_name(tp, "LookAtGroupName"));
            }
        }
        if (actor > 0 && !group_name.empty()) movers[group_name] = std::move(mover);
    }
    const auto cam_it = movers.find(to_lower(m.cam_group));
    if (cam_it == movers.end()) return false;
    const MovingActor& cam = cam_it->second;
    const MovingActor* target = nullptr;
    if (cam.look_at) {
        const auto it = movers.find(cam.look_at_group);
        if (it != movers.end()) target = &it->second;
    }

    // The camera at 60 Hz. A look-at rotation is the line to the other actor, with no roll
    // (FRotator from a direction); otherwise the Euler track's.
    const size_t frame_count = static_cast<size_t>(std::lround(length * 60.0f)) + 1;
    const float frame_dt = length / static_cast<float>(frame_count - 1);
    Vec3 feet(0.0f, 0.0f, 0.0f);
    float feet_yaw = 0.0f;
    const bool spawn_known = find_player_start(packages, feet, feet_yaw);
    out.cam_pos.reserve(frame_count);
    for (size_t f = 0; f < frame_count; ++f) {
        const float t = static_cast<float>(f) * frame_dt;
        const Vec3 eye = cam.position(t);
        Vec3 forward;
        Vec3 up;
        if (target) {
            forward = (target->position(t) - eye).normalized();
            const float yaw = std::atan2(forward.y, forward.x) * RAD2DEG;
            const float pitch = std::asin(std::clamp(forward.z, -1.0f, 1.0f)) * RAD2DEG;
            up = Rotator::from_degrees(pitch, yaw, 0.0f).up();
        } else {
            cam.orientation(t, forward, up);
        }
        out.cam_pos.push_back(eye);
        out.cam_forward.push_back(forward);
        out.cam_up.push_back(up);
        out.root_pos.push_back(feet);
    }

    for (const auto& [time, name] : events) {
        collect_sounds(packages, pkg, interp_props, name, time, out, 0);
        collect_fades(packages, pkg, interp_props, name, time, out, 0);
    }
    collect_fades(packages, pkg, interp_props, "Completed", length, out, 0);
    collect_door_swings(pkg, interp_props, data_props, 0.0f, 1.0f, out);
    for (IntroDoorSwing& swing : out.door_swings) {
        std::stable_sort(swing.yaw_keys.begin(), swing.yaw_keys.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    collect_stop_cues(pkg, interp_props, out);
    std::stable_sort(out.sounds.begin(), out.sounds.end(),
                     [](const IntroSoundEvent& a, const IntroSoundEvent& b) { return a.time < b.time; });
    collect_start_fades(packages, pkg, m.interp, out);
    std::stable_sort(out.fades.begin(), out.fades.end(),
                     [](const IntroFadeEvent& a, const IntroFadeEvent& b) { return a.time < b.time; });

    out.valid = true;
    out.seq_name = m.cam_group;
    out.package_path = pkg.get_file_path();
    out.anim_export_index_1 = 0;  // no first-person animation: the camera alone
    out.actor_location = cam.initial.origin;
    out.actor_yaw_deg = cam.initial.rotation.to_degrees().y;
    out.start_offset_sec = 0.0f;
    out.duration_sec = length;
    out.matinee_length_sec = length;
    out.start_feet_pos = feet;
    out.end_feet_pos = feet;
    out.end_yaw_deg = feet_yaw;
    out.interp_package = to_lower(std::filesystem::path(pkg.get_file_path()).stem().string());
    out.interp_export_index_1 = m.interp;
    out.skippable = prop_bool(interp_props, "bIsSkippable", false);

    int voices = 0;
    for (const auto& s : out.sounds) voices += s.voice ? 1 : 0;
    const Vec3& first = out.cam_pos.front();
    const Vec3& last = out.cam_pos.back();
    std::cout << "[Level] Level intro '" << out.seq_name << "' (" << length << " s camera pan of " << export_object_name(pkg, m.camera)
              << (target ? ", looking at '" + cam.look_at_group + "'" : std::string()) << ", " << frame_count << " frames): camera ("
              << int(first.x) << "," << int(first.y) << "," << int(first.z) << ") -> (" << int(last.x) << "," << int(last.y) << ","
              << int(last.z) << "); pawn " << (spawn_known ? "at its start (" : "at (") << int(feet.x) << "," << int(feet.y) << ","
              << int(feet.z) << "); " << out.sounds.size() << " sounds, " << voices << " of them voice lines, " << out.fades.size()
              << " fades" << (out.skippable ? "" : ", not skippable") << std::endl;
    return true;
}

}  // namespace

void extract_level_intro(const std::string& game_root, const Packages& packages, LevelIntroSequence& out) {
    out = LevelIntroSequence{};

    for (const auto& pkg_ptr : packages) {
        const UPKPackage& pkg = *pkg_ptr;
        const auto& exports = pkg.get_exports();
        for (size_t i = 0; i < exports.size() && !out.valid; ++i) {
            if (pkg.get_export_class(exports[i]) != "SeqAct_Interp") continue;
            UPropertyList interp_props;
            parse_export_properties(pkg, static_cast<int32_t>(i) + 1, interp_props);
            IntroMatinee m;
            if (!find_pawn_matinee(pkg, static_cast<int32_t>(i) + 1, interp_props, m)) continue;
            LevelIntroSequence baked;
            if (bake_pawn_matinee(game_root, packages, m, /*intro_rule=*/true, baked)) out = std::move(baked);
        }
        if (out.valid) break;
    }
    if (out.valid) return;

    // No first-person intro: the level may still open through a placed camera, a Matinee it starts
    // by itself as it loads whose director cuts to a CameraActor (the training area's pan).
    for (const auto& pkg_ptr : packages) {
        const UPKPackage& pkg = *pkg_ptr;
        const auto& exports = pkg.get_exports();
        for (size_t i = 0; i < exports.size() && !out.valid; ++i) {
            if (pkg.get_export_class(exports[i]) != "SeqAct_Interp") continue;
            const int32_t interp = static_cast<int32_t>(i) + 1;
            UPropertyList interp_props;
            parse_export_properties(pkg, interp, interp_props);
            DirectorMatinee m;
            if (!find_director_matinee(pkg, interp, interp_props, m) || !started_at_level_load(packages, pkg, interp)) continue;
            LevelIntroSequence baked;
            if (bake_director_matinee(packages, m, baked)) out = std::move(baked);
        }
        if (out.valid) break;
    }
}

void extract_player_cutscenes(const std::string& game_root, const Packages& packages, const LevelIntroSequence& intro,
                              std::vector<LevelIntroSequence>& out, std::unordered_map<std::string, int>& cutscene_of) {
    out.clear();
    cutscene_of.clear();
    for (const auto& pkg_ptr : packages) {
        const UPKPackage& pkg = *pkg_ptr;
        const std::string stem = to_lower(std::filesystem::path(pkg.get_file_path()).stem().string());
        const auto& exports = pkg.get_exports();
        for (size_t i = 0; i < exports.size(); ++i) {
            if (pkg.get_export_class(exports[i]) != "SeqAct_Interp") continue;
            const int32_t interp = static_cast<int32_t>(i) + 1;
            UPropertyList interp_props;
            parse_export_properties(pkg, interp, interp_props);
            IntroMatinee m;
            if (!find_pawn_matinee(pkg, interp, interp_props, m)) continue;
            LevelIntroSequence baked;
            if (intro.valid && intro.interp_export_index_1 == interp && intro.interp_package == stem) {
                baked = intro;
            } else if (!bake_pawn_matinee(game_root, packages, m, /*intro_rule=*/false, baked)) {
                std::cout << "[Level] Player Matinee " << export_object_name(pkg, interp) << " in " << stem
                          << " has no first-person animation to bake; it runs without a camera" << std::endl;
                continue;
            }
            cutscene_of[stem + ":" + std::to_string(interp)] = static_cast<int>(out.size());
            out.push_back(std::move(baked));
        }
    }
}

}  // namespace me
