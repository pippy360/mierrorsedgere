#include "level_intro.hpp"

#include "ue3_props.hpp"
#include "upk_loader.hpp"
#include "../anim/anim_system.hpp"

#include <algorithm>
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
            const UProperty* var_links = find_prop(interp_props, "VariableLinks");
            if (!var_links) continue;

            // The Matinee that drives the local pawn together with a placed skeletal mesh.
            IntroMatinee m;
            m.pkg = &pkg;
            m.interp = static_cast<int32_t>(i) + 1;
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
            if (m.data <= 0 || m.actor <= 0) continue;

            UPropertyList data_props;
            parse_export_properties(pkg, m.data, data_props);
            const UProperty* groups = find_prop(data_props, "InterpGroups");
            if (!groups) continue;

            // The pawn's group: its animation, where in the Matinee it starts, and how the actor moves.
            std::string anim_name;
            float anim_start = 0.0f;
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
                    } else if (pawn_group && cls == "InterpTrackAnimControl" && anim_name.empty()) {
                        if (const UProperty* seqs = find_prop(tp, "AnimSeqs")) {
                            for (const auto& k : seqs->elements) {
                                const std::string name = prop_name(k, "AnimSeqName");
                                if (to_lower(name).find("intro") == std::string::npos) continue;
                                anim_name = name;
                                anim_start = prop_float(k, "StartTime", 0.0f);
                                break;
                            }
                        }
                    } else if (pawn_group && cls == "InterpTrackMove") {
                        move = read_vector_curve(tp, "PosTrack");
                    }
                }
            }
            if (anim_name.empty()) continue;

            // The sequence of that name that animates the first-person skeleton. A level can carry
            // several with one name (a second body, a prop); the one that drives the most
            // first-person bones, EyeJoint among them, is the view's.
            //
            // Edge and Boat also ship the full-body version (89 tracks to the first-person 82), and
            // that is the one the pawn's own mesh plays: its root is what moves the pawn, and the
            // first-person body rides on the pawn. The two roots are not always the same - in Edge
            // they part by up to 29 uu and 14 degrees for a few seconds, and retail's pawn and camera
            // follow the full-body one.
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
            if (best_seq <= 0) continue;
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

            std::vector<AnimSystem::CannedCameraFrame> rig_frames;
            if (!AnimSystem::bake_canned_camera(game_root, best_set, seq, rig_frames, pawn_root) || rig_frames.size() < 2) continue;

            UPropertyList actor_props;
            parse_export_properties(pkg, m.actor, actor_props);
            Vec3 actor_loc(0.0f, 0.0f, 0.0f);
            float actor_yaw = 90.0f;
            if (const UProperty* lp = find_prop(actor_props, "Location")) actor_loc = Vec3(lp->v[0], lp->v[1], lp->v[2]);
            if (const UProperty* rp = find_prop(actor_props, "Rotation")) {
                actor_yaw = Rotator(rp->vi[0], rp->vi[1], rp->vi[2]).to_degrees().y;
            }

            // Where the animation's root sits against the placed actor. Retail, from its pawn and
            // camera through all ten intros: the root is at the actor, so the pawn's Location rides
            // kPawnAboveRoot over it, floor under the actor or not (Jacknife's is in mid-air). Boat is
            // the exception, and the one intro whose pawn group has a movement track: there the
            // pawn's Location itself is at the actor, which the level places that far above the deck,
            // and the root is kPawnAboveRoot below.
            const bool pawn_at_actor = !move.keys.empty();
            Vec3 anim_origin = actor_loc;
            if (pawn_at_actor) anim_origin.z -= kPawnAboveRoot;

            out.cam_pos.reserve(rig_frames.size());
            const float frame_dt = seq.length / static_cast<float>(rig_frames.size() - 1);
            for (size_t f = 0; f < rig_frames.size(); ++f) {
                // An InterpTrackMove carries the pawn, and the animation with it, from where it started.
                RigFrame rig(anim_origin, actor_yaw);
                rig.origin = anim_origin + rig.actor_offset(move.at(anim_start + static_cast<float>(f) * frame_dt));
                out.cam_pos.push_back(rig.pos(rig_frames[f].eye_pos));
                out.cam_forward.push_back(rig.dir(rig_frames[f].forward).normalized());
                out.cam_up.push_back(rig.dir(rig_frames[f].up).normalized());
                out.root_pos.push_back(rig.pos(rig_frames[f].root_pos));
            }

            for (const auto& [time, name] : events) {
                collect_sounds(packages, pkg, interp_props, name, time, out, 0);
            }
            collect_notifies(pkg, best_seq, seq.length, anim_start, out.sounds);
            // The doors the intro's own Matinee turns (the Mall's), beside those behind its events.
            collect_door_swings(pkg, interp_props, data_props, 0.0f, 1.0f, out);
            for (IntroDoorSwing& swing : out.door_swings) {
                std::stable_sort(swing.yaw_keys.begin(), swing.yaw_keys.end(),
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
            }
            // What the Matinee's Completed output stops (input 1 of a sound action): the long tracks
            // it started, which must not play on when the intro is skipped.
            if (const UProperty* outputs = find_prop(interp_props, "OutputLinks")) {
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
            std::stable_sort(out.sounds.begin(), out.sounds.end(),
                             [](const IntroSoundEvent& a, const IntroSoundEvent& b) { return a.time < b.time; });

            const Vec3 end_fwd = out.cam_forward.back();
            out.valid = true;
            out.seq_name = seq.name;
            out.package_path = pkg.get_file_path();
            out.anim_export_index_1 = best_seq;
            out.actor_location = actor_loc;
            out.actor_yaw_deg = actor_yaw;
            out.start_offset_sec = anim_start;
            out.duration_sec = seq.length;
            out.matinee_length_sec = std::max(prop_float(data_props, "InterpLength", 0.0f), anim_start + seq.length);
            out.start_feet_pos = out.root_pos.front();
            out.end_feet_pos = out.root_pos.back();
            out.end_yaw_deg = std::atan2(end_fwd.y, end_fwd.x) * RAD2DEG;

            int voices = 0;
            for (const auto& s : out.sounds) voices += s.voice ? 1 : 0;
            std::cout << "[Level] Level intro '" << seq.name << "' (" << seq.length << " s animation from "
                      << anim_start << " s of a " << out.matinee_length_sec << " s Matinee, " << rig_frames.size()
                      << " frames, " << best_tracks << " tracks): start=(" << int(out.start_feet_pos.x) << ","
                      << int(out.start_feet_pos.y) << "," << int(out.start_feet_pos.z) << ") -> end=("
                      << int(out.end_feet_pos.x) << "," << int(out.end_feet_pos.y) << "," << int(out.end_feet_pos.z)
                      << ", yaw=" << int(out.end_yaw_deg) << "); "
                      << (pawn_at_actor ? "movement track, root below the placed actor; " : "");
            if (!out.door_swings.empty()) std::cout << out.door_swings.size() << " door(s) swung; ";
            if (pawn_root) std::cout << "on the full-body root (up to " << root_apart << " uu from its own); ";
            std::cout
                      << out.sounds.size() << " sounds, " << voices
                      << " of them voice lines" << std::endl;
        }
        if (out.valid) break;
    }
}

}  // namespace me
