#include "level_script.hpp"

#include "../assets/ini_config.hpp"
#include "../assets/ue3_props.hpp"
#include "../assets/upk_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>

namespace me {

namespace {

constexpr float kSmall = 1.0e-4f;  // KINDA_SMALL_NUMBER

std::string prop_string(const UPropertyList& props, const char* name) {
    const UProperty* p = find_prop(props, name);
    return p ? p->s : std::string();
}

bool same(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return i == a.size() && b[i] == 0;
}

bool is_export(const UPKPackage& pkg, int32_t index) {
    return index > 0 && static_cast<size_t>(index) <= pkg.get_exports().size();
}

std::string class_of(const UPKPackage& pkg, int32_t index) {
    return is_export(pkg, index) ? pkg.get_export_class(pkg.get_exports()[static_cast<size_t>(index - 1)]) : std::string();
}

// "Group.Name" of a SoundCue reference, the form the audio engine files cues under.
std::string cue_name(const UPKPackage& pkg, int32_t object) {
    const std::string path = object_full_path(pkg, object);
    const size_t last = path.rfind('.');
    if (last == std::string::npos) return path;
    const size_t prev = path.rfind('.', last - 1);
    return prev == std::string::npos ? path.substr(last + 1) : path.substr(prev + 1);
}

std::string package_of(const UPKPackage& pkg, int32_t object) {
    const std::string path = object_full_path(pkg, object);
    return path.substr(0, path.find('.'));
}

std::string stem_of(const UPKPackage& pkg) {
    return to_lower(std::filesystem::path(pkg.get_file_path()).stem().string());
}

// The planes of a closed convex piece given as triangles: one per distinct face, outward.
std::vector<ScriptActor::Plane> hull_to_planes(const std::vector<Vec3>& tris) {
    std::vector<ScriptActor::Plane> planes;
    if (tris.size() < 12) return planes;
    Vec3 centroid(0.0f, 0.0f, 0.0f);
    for (const Vec3& v : tris) centroid = centroid + v;
    centroid = centroid * (1.0f / static_cast<float>(tris.size()));
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        Vec3 n = (tris[i + 1] - tris[i]).cross(tris[i + 2] - tris[i]);
        const float len = n.length();
        if (len < 1.0e-6f) continue;
        n = n * (1.0f / len);
        float d = n.dot(tris[i]);
        if (n.dot(centroid) > d) {
            n = n * -1.0f;
            d = -d;
        }
        bool known = false;
        for (const ScriptActor::Plane& p : planes) {
            if (p.normal.dot(n) > 0.9995f && std::abs(p.distance - d) < 0.5f) {
                known = true;
                break;
            }
        }
        if (!known) planes.push_back({n, d});
    }
    return planes;
}

}  // namespace

// --- the graph ------------------------------------------------------------------------------

int ScriptGraph::find_node(const std::string& package, int32_t export_index) const {
    const std::string low = to_lower(package);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const Node& n = nodes[i];
        if (n.export_index == export_index && packages[static_cast<size_t>(n.package)] == low) return static_cast<int>(i);
    }
    return -1;
}

int ScriptGraph::find_checkpoint_actor(const std::string& checkpoint_name) const {
    for (size_t i = 0; i < actors.size(); ++i) {
        if (actors[i].cls == "TdCheckpoint" && same(actors[i].checkpoint_name, checkpoint_name.c_str())) return static_cast<int>(i);
    }
    return -1;
}

int ScriptGraph::find_default_checkpoint_actor() const {
    for (size_t i = 0; i < actors.size(); ++i) {
        if (actors[i].cls == "TdCheckpoint" && actors[i].default_checkpoint) return static_cast<int>(i);
    }
    return -1;
}

bool ScriptGraph::load(const std::vector<std::shared_ptr<UPKPackage>>& level_packages,
                       const std::unordered_map<std::string, int>& cutscene_of, std::vector<std::string>& warnings) {
    packages.clear();
    nodes.clear();
    actors.clear();
    matinees.clear();
    remote_events.clear();

    struct Key {
        int package;
        int32_t export_index;
        bool operator==(const Key& o) const { return package == o.package && export_index == o.export_index; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const { return std::hash<int64_t>()((static_cast<int64_t>(k.package) << 32) ^ k.export_index); }
    };
    std::unordered_map<Key, int, KeyHash> node_of;
    std::unordered_map<Key, int, KeyHash> actor_of;
    std::unordered_map<Key, int, KeyHash> matinee_of;
    std::vector<Key> export_of;

    // An actor of the level the graph refers to, read once.
    auto actor_for = [&](int package, int32_t export_index) -> int {
        const UPKPackage& pkg = *level_packages[static_cast<size_t>(package)];
        if (!is_export(pkg, export_index)) return -1;
        const Key key{package, export_index};
        auto it = actor_of.find(key);
        if (it != actor_of.end()) return it->second;
        const std::string cls = class_of(pkg, export_index);
        // Only placed actors: a component or a sequence object is not one.
        if (cls.empty() || cls.find("Component") != std::string::npos || cls.rfind("Seq", 0) == 0 || cls == "InterpData") return -1;
        ScriptActor a;
        a.package = packages[static_cast<size_t>(package)];
        a.export_index = export_index;
        a.cls = cls;
        a.name = export_object_name(pkg, export_index);
        UPropertyList props;
        parse_export_properties(pkg, export_index, props);
        if (const UProperty* loc = find_prop(props, "Location")) a.location = Vec3(loc->v[0], loc->v[1], loc->v[2]);
        if (const UProperty* rot = find_prop(props, "Rotation")) {
            a.yaw_deg = static_cast<float>(rot->vi[1]) * (360.0f / 65536.0f);
            a.pitch_deg = static_cast<float>(rot->vi[0]) * (360.0f / 65536.0f);
        }
        a.collide_actors = prop_bool(props, "bCollideActors", true);
        a.bounds.min_pt = a.location;
        a.bounds.max_pt = a.location;
        const int32_t cylinder = prop_object(props, "CylinderComponent") ? prop_object(props, "CylinderComponent") : prop_object(props, "CollisionComponent");
        if (is_export(pkg, cylinder) && class_of(pkg, cylinder) == "CylinderComponent") {
            UPropertyList cp;
            parse_export_properties(pkg, cylinder, cp);
            a.has_cylinder = true;
            // Trigger's CollisionCylinder: CollisionRadius 40, CollisionHeight 40 by default (Engine.Trigger)
            a.radius = prop_float(cp, "CollisionRadius", 40.0f);
            a.half_height = prop_float(cp, "CollisionHeight", 40.0f);
            a.bounds.min_pt = a.location - Vec3(a.radius, a.radius, a.half_height);
            a.bounds.max_pt = a.location + Vec3(a.radius, a.radius, a.half_height);
        }
        if (prop_object(props, "BrushComponent") > 0) {
            std::vector<std::vector<Vec3>> hulls;
            read_actor_brush_hulls(pkg, export_index, hulls);
            for (const auto& hull : hulls) {
                std::vector<ScriptActor::Plane> planes = hull_to_planes(hull);
                if (planes.size() < 4) continue;
                for (const Vec3& v : hull) a.bounds.expand(v);
                a.hull_planes.push_back(std::move(planes));
            }
            if (a.hull_planes.empty()) warnings.push_back(a.name + " has no brush hull");
        }
        if (cls == "TdCheckpoint") {
            a.checkpoint_name = prop_string(props, "CheckpointName");
            a.default_checkpoint = prop_bool(props, "DefaultCheckpoint", false);
        }
        const int index = static_cast<int>(actors.size());
        actors.push_back(std::move(a));
        actor_of[key] = index;
        return index;
    };

    // Every package's Main_Sequence and the sequences under it.
    for (size_t p = 0; p < level_packages.size(); ++p) {
        const UPKPackage& pkg = *level_packages[p];
        packages.push_back(stem_of(pkg));
        const auto& exports = pkg.get_exports();
        int32_t root = 0;
        for (size_t i = 0; i < exports.size(); ++i) {
            if (pkg.get_export_class(exports[i]) == "Sequence" &&
                export_object_name(pkg, static_cast<int32_t>(i + 1)) == "Main_Sequence") {
                root = static_cast<int32_t>(i + 1);
                break;
            }
        }
        if (root <= 0) continue;
        const int package = static_cast<int>(p);
        std::function<void(int32_t, int)> collect = [&](int32_t sequence_export, int sequence_node) {
            UPropertyList sp;
            parse_export_properties(pkg, sequence_export, sp);
            const UProperty* objects = find_prop(sp, "SequenceObjects");
            if (!objects) return;
            for (int32_t e : objects->ints) {
                const Key key{package, e};
                if (!is_export(pkg, e) || node_of.count(key)) continue;
                const std::string cls = class_of(pkg, e);
                if (cls.rfind("SequenceFrame", 0) == 0) continue;
                Node n;
                n.package = package;
                n.export_index = e;
                n.cls = cls;
                n.name = export_object_name(pkg, e);
                n.sequence = sequence_node;
                const int index = static_cast<int>(nodes.size());
                nodes.push_back(std::move(n));
                node_of[key] = index;
                export_of.push_back(key);
                if (cls == "Sequence") collect(e, index);
            }
        };
        collect(root, -1);
    }

    auto node_for = [&](int package, int32_t e) {
        auto it = node_of.find(Key{package, e});
        return it == node_of.end() ? -1 : it->second;
    };

    for (size_t ni = 0; ni < nodes.size(); ++ni) {
        Node& n = nodes[ni];
        const UPKPackage& pkg = *level_packages[static_cast<size_t>(n.package)];
        UPropertyList props;
        parse_export_properties(pkg, n.export_index, props);

        if (const UProperty* outs = find_prop(props, "OutputLinks")) {
            for (const auto& el : outs->elements) {
                Output o;
                o.desc = prop_string(el, "LinkDesc");
                o.delay = prop_float(el, "ActivateDelay", 0.0f);
                o.disabled = prop_bool(el, "bDisabled", false);
                o.linked_op = node_for(n.package, prop_object(el, "LinkedOp"));
                if (const UProperty* links = find_prop(el, "Links")) {
                    for (const auto& l : links->elements) {
                        Link link;
                        link.op = node_for(n.package, prop_object(l, "LinkedOp"));
                        link.input = prop_int(l, "InputLinkIdx", 0);
                        if (link.op >= 0) o.links.push_back(link);
                    }
                }
                n.outputs.push_back(std::move(o));
            }
        }
        if (const UProperty* ins = find_prop(props, "InputLinks")) {
            for (const auto& el : ins->elements) {
                Input in;
                in.desc = prop_string(el, "LinkDesc");
                in.delay = prop_float(el, "ActivateDelay", 0.0f);
                in.disabled = prop_bool(el, "bDisabled", false);
                in.linked_op = node_for(n.package, prop_object(el, "LinkedOp"));
                n.inputs.push_back(std::move(in));
            }
        }
        if (const UProperty* vars = find_prop(props, "VariableLinks")) {
            for (const auto& el : vars->elements) {
                VarLink v;
                v.desc = prop_string(el, "LinkDesc");
                if (const UProperty* linked = find_prop(el, "LinkedVariables")) {
                    for (int32_t e : linked->ints) {
                        const int var = node_for(n.package, e);
                        if (var >= 0) v.vars.push_back(var);
                        else if (class_of(pkg, e) == "InterpData" && n.cls == "SeqAct_Interp") {
                            // The InterpData is not always listed among the sequence's objects.
                            v.vars.push_back(-1 - e);  // resolved below
                        }
                    }
                }
                n.vars.push_back(std::move(v));
            }
        }

        const std::string& c = n.cls;
        const bool is_event = c.rfind("SeqEvent", 0) == 0 || c.rfind("SeqEvt", 0) == 0;
        if (is_event) {
            n.max_trigger = prop_int(props, "MaxTriggerCount", 1);
            n.retrigger = prop_float(props, "ReTriggerDelay", c == "SeqEvent_Touch" || c == "SeqEvent_TdTouch" ? 0.1f : 0.0f);
            n.enabled = prop_bool(props, "bEnabled", true);
            n.player_only = prop_bool(props, "bPlayerOnly", true);
            n.originator = actor_for(n.package, prop_object(props, "Originator"));
            if (c == "SeqEvent_RemoteEvent") {
                n.label = prop_name(props, "EventName");
                if (!n.label.empty()) remote_events[to_lower(n.label)].push_back(static_cast<int>(ni));
            } else if (c == "SeqEvent_SequenceActivated") {
                n.label = prop_string(props, "InputLabel");
                n.max_trigger = prop_int(props, "MaxTriggerCount", 0);
            }
            if (c == "SeqEvent_TdTouch") n.momentum = prop_float(props, "Momentum", 0.0f);
            if (c == "SeqEvent_Touch" || c == "SeqEvent_TdTouch") {
                // ClassProximityTypes (default Pawn): a trigger for the bots alone does not answer the player.
                if (const UProperty* types = find_prop(props, "ClassProximityTypes")) {
                    bool player = types->ints.empty();
                    for (int32_t ref : types->ints) {
                        const std::string path = to_lower(object_full_path(pkg, ref));
                        const std::string cls = path.substr(path.rfind('.') == std::string::npos ? 0 : path.rfind('.') + 1);
                        if (cls == "pawn" || cls == "tdpawn" || cls == "tdplayerpawn" || cls == "actor" || cls == "gamepawn") player = true;
                    }
                    n.player_touches = player;
                }
            }
            if (c == "SeqEvent_TakeDamage" || c == "SeqEvent_Death") {
                // What the event does not save is its class's (Engine.u).
                const auto defaults = script_default_chain("Default__" + c);
                const auto saved_or_default = [&](const char* name) -> const UProperty* {
                    if (const UProperty* p = find_prop(props, name)) return p;
                    for (const auto* list : defaults) {
                        if (const UProperty* p = find_prop(*list, name)) return p;
                    }
                    return nullptr;
                };
                if (const UProperty* p = saved_or_default("bPlayerOnly")) n.player_only = p->b;
                if (const UProperty* p = saved_or_default("MaxTriggerCount")) n.max_trigger = p->i;
                n.f = 100.0f;  // DamageThreshold
                n.f2 = 0.0f;   // MinDamageAmount
                if (const UProperty* p = saved_or_default("DamageThreshold")) n.f = p->f;
                if (const UProperty* p = saved_or_default("MinDamageAmount")) n.f2 = p->f;
                const auto class_names = [&](const char* name, std::vector<std::string>& out) {
                    const UProperty* list = find_prop(props, name);
                    if (!list) return;
                    for (int32_t ref : list->ints) {
                        const std::string path = to_lower(object_full_path(pkg, ref));
                        out.push_back(path.substr(path.rfind('.') == std::string::npos ? 0 : path.rfind('.') + 1));
                    }
                };
                class_names("DamageTypes", n.damage_types);
                class_names("IgnoreDamageTypes", n.ignore_damage_types);
                if (c == "SeqEvent_TakeDamage" && n.originator >= 0) damage_events.push_back(static_cast<int>(ni));
            }
            if (c == "SeqEvent_LOS") {
                // Engine.SeqEvent_LOS defaults
                n.los_distance = prop_float(props, "TriggerDistance", 2048.0f);
                n.los_screen = prop_float(props, "ScreenCenterDistance", 50.0f);
                n.los_obstructions = prop_bool(props, "bCheckForObstructions", true);
            }
        } else if (c == "SeqAct_ActivateRemoteEvent") {
            n.label = prop_name(props, "EventName");
        } else if (c == "SeqAct_FinishSequence") {
            n.label = prop_string(props, "OutputLabel");
        } else if (c == "SeqVar_Named") {
            n.label = prop_name(props, "FindVarName");
        } else if (c == "SeqVar_External") {
            n.label = prop_string(props, "VariableLabel");
        } else if (c.rfind("SeqVar_", 0) == 0) {
            n.label = prop_name(props, "VarName");
            if (c == "SeqVar_Float") n.f = prop_float(props, "FloatValue", 0.0f);
            if (c == "SeqVar_Int") n.i = prop_int(props, "IntValue", 0);
            if (c == "SeqVar_Bool") n.b = prop_int(props, "bValue", 0) != 0 || prop_bool(props, "bValue", false);
            if (c == "SeqVar_String") n.text = prop_string(props, "StrValue");
            if (c == "SeqVar_Object") {
                const int32_t obj = prop_object(props, "ObjValue");
                n.actor = actor_for(n.package, obj);
                n.text = is_export(pkg, obj) ? class_of(pkg, obj) : std::string();
            }
        } else if (c == "SeqAct_ActorFactory") {
            // An emitter's factory is followed (its particle system is made); the others are not.
            const int32_t factory = prop_object(props, "Factory");
            if (is_export(pkg, factory) && class_of(pkg, factory) == "ActorFactoryEmitter") n.i = factory;
        } else if (c == "SeqAct_Delay") {
            n.f = prop_float(props, "Duration", 1.0f);
        } else if (c == "SeqAct_Gate") {
            n.b = prop_bool(props, "bOpen", true);
            n.i = prop_int(props, "AutoCloseCount", 0);
        } else if (c == "SeqAct_Switch") {
            n.i = prop_int(props, "IncrementAmount", 1);
            n.b = prop_bool(props, "bLooping", false);
            n.b2 = prop_bool(props, "bAutoDisableLinks", false);
        } else if (c == "SeqAct_RandomSwitch") {
            n.i = prop_int(props, "LinkCount", static_cast<int>(n.outputs.size()));
            n.b = prop_bool(props, "bLooping", false);
            n.b2 = prop_bool(props, "bAutoDisableLinks", false);
        } else if (c == "SeqAct_SetInt") {
            n.i = prop_int(props, "Value", 0);
        } else if (c == "SeqAct_SetFloat") {
            n.f = prop_float(props, "Value", 0.0f);
        } else if (c == "SeqAct_SetBool") {
            n.b = prop_bool(props, "bValue", false);
        } else if (c == "SeqAct_AddFloat" || c == "SeqAct_SubtractFloat" || c == "SeqCond_CompareFloat" || c == "SeqAct_MultFloat") {
            n.f = prop_float(props, "ValueA", 0.0f);
            n.f2 = prop_float(props, "ValueB", 0.0f);
        } else if (c == "SeqAct_AddInt" || c == "SeqAct_SubtractInt" || c == "SeqCond_CompareInt" || c == "SeqAct_MultInt") {
            n.i = prop_int(props, "ValueA", 0);
            n.f2 = static_cast<float>(prop_int(props, "ValueB", 0));
        } else if (c == "SeqAct_Interp") {
            n.f = prop_float(props, "PlayRate", 1.0f);
            n.b = prop_bool(props, "bLooping", false);
            n.b2 = prop_bool(props, "bRewindOnPlay", false);
            n.b3 = prop_bool(props, "bRewindIfAlreadyPlaying", false);
            n.b4 = prop_bool(props, "bIsSkippable", false);
        } else if (c == "SeqAct_TdCheckpoint") {
            n.b = prop_bool(props, "teleportPawnToCheckpoint", false);
            n.b2 = prop_bool(props, "skipSaveToDisk", false);
        } else if (c == "SeqAct_TdLevelCompleted") {
            n.text = prop_string(props, "NextLevelName");
            n.text2 = prop_string(props, "NextCheckpointName");
        } else if (c == "SeqAct_TdTutorialMessage") {
            n.text = prop_string(props, "TutorialMessage");
            n.text2 = prop_string(props, "CustomButtonCallOut");
            n.f = prop_float(props, "Duration", 3.0f);
            n.b = prop_bool(props, "bReplaceCurrentMessage", false);
            n.b2 = prop_bool(props, "bRequireAccept", true);
            n.b3 = prop_bool(props, "bTriggerSlomo", false);
            n.b4 = prop_bool(props, "bPauseGame", false);
        } else if (c == "SeqAct_TdSupersMessage") {
            n.text = prop_string(props, "SupersMessage");
            n.f = prop_float(props, "Duration", 3.0f);
        } else if (c == "SeqAct_TdTriggerSubtitle") {
            n.f = prop_float(props, "Duration", 5.0f);
            if (const UProperty* subs = find_prop(props, "Subtitles")) {
                for (const auto& el : subs->elements) {
                    if (!n.text.empty()) n.text += "\n";
                    n.text += prop_string(el, "Text");
                }
            }
        } else if (c == "SeqAct_TdTriggerSplashHint") {
            n.i = prop_int(props, "HintNumber", 0);
        } else if (c == "SeqAct_TdFadeEffect") {
            n.f = prop_float(props, "FadeTime", 1.0f);
            n.b = prop_name(props, "FadeEffect") == "FadeOut";
            // SeqAct_TdFadeEffect divides each channel by 255 as integers: 1 at 255, else 0.
            if (const UProperty* col = find_prop(props, "FadeColor")) {
                n.color = Vec3(col->v[0] >= 0.999f ? 1.0f : 0.0f, col->v[1] >= 0.999f ? 1.0f : 0.0f, col->v[2] >= 0.999f ? 1.0f : 0.0f);
            } else {
                n.color = Vec3(0.0f, 0.0f, 0.0f);
            }
        } else if (c == "SeqAct_TdDisablePlayerInput") {
            n.b = prop_bool(props, "bDisablePlayerMoveInput", true);
            n.b2 = prop_bool(props, "bDisablePlayerLookInput", true);
            n.b3 = prop_bool(props, "bSetCinematicMode", true);
            n.b4 = prop_bool(props, "bDisableSkipCutscenes", false);
        } else if (c == "SeqAct_DisableLoadFromLastCheckpoint") {
            n.b3 = prop_bool(props, "bShouldBeDisabled", true);
        } else if (c == "SeqAct_TdPlaySound" || c == "SeqAct_PlaySound") {
            const int32_t cue = prop_object(props, "PlaySound");
            if (cue != 0) {
                n.cue = cue_name(pkg, cue);
                n.cue_bank = package_of(pkg, cue);
                n.cue_voice = to_lower(object_full_path(pkg, cue)).find("a_vo_") != std::string::npos;
            }
            n.f = prop_float(props, "VolumeMultiplier", 1.0f);
        } else if (c == "SeqAct_CauseDamage") {
            n.f = prop_float(props, "DamageAmount", 0.0f);
            if (const int32_t type = prop_object(props, "DamageType")) {
                const std::string path = to_lower(object_full_path(pkg, type));
                n.damage_types.push_back(path.substr(path.rfind('.') == std::string::npos ? 0 : path.rfind('.') + 1));
            }
        } else if (c == "SeqAct_MultiLevelStreaming") {
            if (const UProperty* levels = find_prop(props, "Levels")) {
                for (const auto& el : levels->elements) {
                    const std::string name = prop_name(el, "LevelName");
                    if (!name.empty()) n.level_names.push_back(name);
                }
            }
        } else if (c == "SeqAct_LevelStreaming") {
            const std::string name = prop_name(props, "LevelName");
            if (!name.empty()) n.level_names.push_back(name);
        } else if (c == "SeqAct_StreamingZone") {
            if (const UProperty* levels = find_prop(props, "StreamingLevels")) {
                for (int32_t ref : levels->ints) {
                    if (!is_export(pkg, ref)) continue;
                    UPropertyList lp;
                    parse_export_properties(pkg, ref, lp);
                    const std::string name = prop_name(lp, "PackageName");
                    if (!name.empty()) n.level_names.push_back(name);
                }
            }
        } else if (c == "SeqAct_ChangeCollision") {
            const std::string ctype = prop_name(props, "CollisionType");
            bool collide = prop_bool(props, "bCollideActors", false);
            bool block = prop_bool(props, "bBlockActors", false);
            if (ctype == "COLLIDE_NoCollision") {
                collide = false;
                block = false;
            } else if (ctype == "COLLIDE_BlockAll" || ctype == "COLLIDE_BlockAllButWeapons") {
                collide = true;
                block = true;
            } else if (ctype == "COLLIDE_TouchAll" || ctype == "COLLIDE_TouchAllButWeapons") {
                collide = true;
                block = false;
            }
            n.b = collide;
            n.b2 = block;
        }
    }

    // A SeqAct_Interp plays the InterpData behind its "Data" link: its length, event keys and sound keys.
    for (size_t ni = 0; ni < nodes.size(); ++ni) {
        Node& n = nodes[ni];
        if (n.cls != "SeqAct_Interp") continue;
        const UPKPackage& pkg = *level_packages[static_cast<size_t>(n.package)];
        int32_t data = 0;
        for (VarLink& v : n.vars) {
            for (size_t k = 0; k < v.vars.size();) {
                if (v.vars[k] < -0) {  // a raw InterpData export, stored as -1 - export
                    data = -1 - v.vars[k];
                    v.vars.erase(v.vars.begin() + static_cast<std::ptrdiff_t>(k));
                } else {
                    const Node& var = nodes[static_cast<size_t>(v.vars[k])];
                    if (var.cls == "InterpData") data = var.export_index;
                    ++k;
                }
            }
        }
        if (data <= 0) {
            warnings.push_back(n.name + " has no Matinee data");
            continue;
        }
        const Key key{n.package, data};
        auto it = matinee_of.find(key);
        if (it == matinee_of.end()) {
            ScriptMatinee m;
            m.name = export_object_name(pkg, data);
            UPropertyList dp;
            parse_export_properties(pkg, data, dp);
            m.length = prop_float(dp, "InterpLength", 5.0f);  // the class default, left out when unchanged
            if (const UProperty* groups = find_prop(dp, "InterpGroups")) {
                for (int32_t g : groups->ints) {
                    if (!is_export(pkg, g)) continue;
                    UPropertyList gp;
                    parse_export_properties(pkg, g, gp);
                    const UProperty* tracks = find_prop(gp, "InterpTracks");
                    if (!tracks) continue;
                    const std::string group_name = prop_name(gp, "GroupName", "InterpGroup");
                    for (int32_t t : tracks->ints) {
                        const std::string cls = class_of(pkg, t);
                        if (cls.empty()) continue;
                        UPropertyList tp;
                        parse_export_properties(pkg, t, tp);
                        if (cls == "InterpTrackEvent") {
                            if (const UProperty* keys = find_prop(tp, "EventTrack")) {
                                for (const auto& k : keys->elements) m.events.emplace_back(prop_float(k, "Time", 0.0f), prop_name(k, "EventName"));
                            }
                        } else if (cls == "InterpTrackToggle") {
                            if (const UProperty* keys = find_prop(tp, "ToggleTrack")) {
                                for (const auto& k : keys->elements) {
                                    ScriptMatinee::Toggle toggle;
                                    toggle.time = prop_float(k, "Time", 0.0f);
                                    toggle.group = group_name;
                                    // ETrackToggleAction; ETTA_Off is the first and is not saved.
                                    const std::string action = prop_name(k, "ToggleAction", "ETTA_Off");
                                    toggle.action = action == "ETTA_On" ? 0 : (action == "ETTA_Toggle" ? 2 : 1);
                                    m.toggles.push_back(std::move(toggle));
                                }
                            }
                        } else if (cls.find("InterpTrackSound") != std::string::npos) {
                            if (const UProperty* keys = find_prop(tp, "Sounds")) {
                                for (const auto& k : keys->elements) {
                                    const int32_t cue = prop_object(k, "Sound");
                                    if (cue == 0) continue;
                                    ScriptSound s;
                                    s.time = prop_float(k, "Time", 0.0f);
                                    s.cue = cue_name(pkg, cue);
                                    s.bank = package_of(pkg, cue);
                                    s.voice = to_lower(object_full_path(pkg, cue)).find("a_vo_") != std::string::npos;
                                    m.sounds.push_back(std::move(s));
                                }
                            }
                        }
                    }
                }
            }
            std::stable_sort(m.events.begin(), m.events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            std::stable_sort(m.sounds.begin(), m.sounds.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
            it = matinee_of.emplace(key, static_cast<int>(matinees.size())).first;
            matinees.push_back(std::move(m));
        }
        n.matinee = it->second;
        auto cs = cutscene_of.find(packages[static_cast<size_t>(n.package)] + ":" + std::to_string(n.export_index));
        if (cs != cutscene_of.end()) {
            // One Matinee per cutscene: the first SeqAct_Interp that plays it owns it.
            matinees[static_cast<size_t>(n.matinee)].player_cutscene = cs->second;
        }
    }
    return valid();
}

// --- running ----------------------------------------------------------------------------------

void LevelScript::init(std::shared_ptr<const ScriptGraph> graph, ScriptHost host) {
    graph_ = std::move(graph);
    host_ = std::move(host);
    loaded_.clear();
    reset_state();
}

void LevelScript::reset_state() {
    state_.clear();
    active_.clear();
    sequence_nodes_.clear();
    sequence_package_.clear();
    sequence_slot_.clear();
    delayed_.clear();
    time_ = 0.0;
    tick_ = 0;
    input_move_disabled_ = false;
    input_look_disabled_ = false;
    cinematic_mode_ = false;
    skip_disabled_ = false;
    load_checkpoint_disabled_ = false;
    playing_cutscene_ = -1;
    playing_cutscene_node_ = -1;
    tutorial_node_ = -1;
    into_cutscene_node_ = -1;
    spawn_pending_ = 2;
    if (!graph_) return;
    state_.resize(graph_->nodes.size());
    package_loaded_.assign(graph_->packages.size(), loaded_.empty());
    level_loaded_fired_.assign(graph_->packages.size(), false);
    for (size_t p = 0; p < graph_->packages.size(); ++p) {
        active_.emplace_back();
        sequence_nodes_.push_back(-1);
        sequence_package_.push_back(static_cast<int>(p));
    }
    for (size_t i = 0; i < graph_->nodes.size(); ++i) {
        const ScriptGraph::Node& n = graph_->nodes[i];
        State& s = state_[i];
        s.f = n.f;
        s.i = n.i;
        s.b = n.b;
        s.open = n.b;
        s.enabled = n.enabled;
        s.auto_close_left = n.i;
        if (n.cls == "Sequence") {
            sequence_slot_[static_cast<int>(i)] = static_cast<int>(active_.size());
            active_.emplace_back();
            sequence_nodes_.push_back(static_cast<int>(i));
            sequence_package_.push_back(n.package);
        }
    }
    for (const std::string& stem : loaded_) {
        for (size_t p = 0; p < graph_->packages.size(); ++p) {
            if (same(graph_->packages[p], stem.c_str())) package_loaded_[p] = true;
        }
    }
    // The persistent map (the first package) is always loaded.
    if (!package_loaded_.empty()) package_loaded_[0] = true;
}

void LevelScript::log(const std::string& line) const {
    if (host_.log) host_.log(line);
}

bool LevelScript::package_loaded(int package) const {
    return package < 0 || static_cast<size_t>(package) >= package_loaded_.size() || package_loaded_[static_cast<size_t>(package)];
}

void LevelScript::set_loaded_packages(const std::vector<std::string>& stems) {
    loaded_ = stems;
    if (!graph_) return;
    std::vector<bool> now(graph_->packages.size(), stems.empty());
    for (const std::string& stem : stems) {
        for (size_t p = 0; p < graph_->packages.size(); ++p) {
            if (same(graph_->packages[p], stem.c_str())) now[p] = true;
        }
    }
    // The persistent map (the first package) is always loaded.
    if (!now.empty()) now[0] = true;
    for (size_t p = 0; p < now.size(); ++p) {
        const bool was = package_loaded_[p];
        package_loaded_[p] = now[p];
        if (now[p] && !was && !level_loaded_fired_[p]) {
            level_loaded_fired_[p] = true;
            for (size_t i = 0; i < graph_->nodes.size(); ++i) {
                const ScriptGraph::Node& n = graph_->nodes[i];
                if (n.package == static_cast<int>(p) && n.cls == "SeqEvent_LevelLoaded") check_activate(static_cast<int>(i));
            }
        }
    }
}

const ScriptActor* LevelScript::begin_play(const std::string& checkpoint_name) {
    if (!graph_) return nullptr;
    reset_state();
    int cp = checkpoint_name.empty() ? -1 : graph_->find_checkpoint_actor(checkpoint_name);
    if (cp < 0) cp = graph_->find_default_checkpoint_actor();
    start_checkpoint_ = cp;
    active_checkpoint_ = cp;

    // SeqEvent_LevelLoaded of every loaded level, the persistent map's first.
    for (size_t p = 0; p < graph_->packages.size(); ++p) {
        if (!package_loaded_[p]) continue;
        level_loaded_fired_[p] = true;
        for (size_t i = 0; i < graph_->nodes.size(); ++i) {
            const ScriptGraph::Node& n = graph_->nodes[i];
            if (n.package == static_cast<int>(p) && (n.cls == "SeqEvent_LevelLoaded" || (n.cls == "SeqEvent_SequenceActivated" && n.sequence < 0))) {
                check_activate(static_cast<int>(i));
            }
        }
    }
    // Then the checkpoint's own events (TdSPStoryGame.TriggerEventsOnLevelReload: the
    // SeqEvt_TdCheckpointLoaded and SeqEvt_TdCheckpointActivated of the active checkpoint).
    if (cp >= 0) {
        fire_checkpoint_events(cp, true);
        fire_checkpoint_events(cp, false);
        std::string loaded_list;
        for (size_t p = 0; p < graph_->packages.size(); ++p) {
            if (package_loaded_[p]) loaded_list += (loaded_list.empty() ? "" : " ") + graph_->packages[p];
        }
        log("[Script] level loaded at checkpoint '" + graph_->actors[static_cast<size_t>(cp)].checkpoint_name + "'; sequences running: " + loaded_list);
    } else {
        log("[Script] level loaded; no checkpoint of that name" + (checkpoint_name.empty() ? std::string() : " ('" + checkpoint_name + "')"));
    }
    // The emitters the script can make (SeqAct_ActorFactory with an ActorFactoryEmitter).
    size_t factories = 0, placed = 0;
    for (size_t i = 0; i < graph_->nodes.size(); ++i) {
        const ScriptGraph::Node& n = graph_->nodes[i];
        if (n.cls != "SeqAct_ActorFactory" || n.i <= 0) continue;
        ++factories;
        bool point = false;
        for (int t : linked_vars(static_cast<int>(i), "Spawn Point")) point = point || graph_->nodes[static_cast<size_t>(t)].actor >= 0;
        placed += point ? 1 : 0;
    }
    if (factories > 0) log("[Script] emitter factories: " + std::to_string(factories) + ", " + std::to_string(placed) + " with a spawn point");
    return cp >= 0 ? &graph_->actors[static_cast<size_t>(cp)] : nullptr;
}

void LevelScript::fire_checkpoint_events(int checkpoint_actor, bool loaded) {
    const ScriptActor& want = graph_->actors[static_cast<size_t>(checkpoint_actor)];
    const char* cls = loaded ? "SeqEvt_TdCheckpointLoaded" : "SeqEvt_TdCheckpointActivated";
    for (size_t i = 0; i < graph_->nodes.size(); ++i) {
        const ScriptGraph::Node& n = graph_->nodes[i];
        if (n.cls != cls || n.originator < 0 || !package_loaded(n.package)) continue;
        const ScriptActor& a = graph_->actors[static_cast<size_t>(n.originator)];
        if (n.originator == checkpoint_actor || (!want.checkpoint_name.empty() && same(a.checkpoint_name, want.checkpoint_name.c_str()))) {
            check_activate(static_cast<int>(i));
        }
    }
}

void LevelScript::reload_checkpoint() {
    if (!graph_) return;
    const int cp = active_checkpoint_;
    if (host_.stop_cutscene && playing_cutscene_ >= 0) host_.stop_cutscene();
    if (host_.hide_tutorial) host_.hide_tutorial();
    begin_play(cp >= 0 ? graph_->actors[static_cast<size_t>(cp)].checkpoint_name : std::string());
}

// USequenceEvent::CheckActivate, then USequence::QueueSequenceOp(this, false): an event goes to
// the bottom of the list, so it runs after the ops that were already ticking.
bool LevelScript::check_activate(int event) {
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(event)];
    State& s = state_[static_cast<size_t>(event)];
    if (!s.enabled || !package_loaded(n.package)) return false;
    if (n.max_trigger > 0 && s.trigger_count >= n.max_trigger) return false;
    if (n.retrigger > 0.0f && time_ - s.last_trigger < static_cast<double>(n.retrigger)) return false;
    ++s.trigger_count;
    s.last_trigger = time_;
    queue(event, false);
    return true;
}

// Actor.TakeDamage: every SeqEvent_TakeDamage on the actor adds the damage up and fires when it
// reaches its threshold (SeqEvent_TakeDamage.HandleDamage, IsValidDamageType).
bool LevelScript::damage_actor(const std::string& package, const std::string& name, float amount, bool by_player,
                               const std::string& damage_type) {
    if (!graph_) return false;
    // ClassIsChildOf: the classes the game deals here (bullet, barge, blow) each stand right under DamageType's line.
    const auto is_a = [&](const std::string& cls) { return cls == damage_type || cls == "damagetype" || cls == "tddamagetype"; };
    bool fired = false;
    for (int ev : graph_->damage_events) {
        const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(ev)];
        const ScriptActor& a = graph_->actors[static_cast<size_t>(n.originator)];
        if (a.name != name || a.package != package) continue;
        State& s = state_[static_cast<size_t>(ev)];
        if (!n.damage_types.empty() && std::none_of(n.damage_types.begin(), n.damage_types.end(), is_a)) continue;
        if (std::any_of(n.ignore_damage_types.begin(), n.ignore_damage_types.end(), is_a)) continue;
        if (!s.enabled || amount < n.f2 || (n.player_only && !by_player)) continue;
        s.damage += amount;
        if (s.damage < n.f) continue;
        if (!check_activate(ev)) continue;
        for (int v : linked_vars(ev, "Damage Taken")) state_[static_cast<size_t>(v)].f = s.damage;
        log("[Script] " + a.name + " took " + std::to_string(static_cast<int>(s.damage)) + " damage -> " + n.name);
        s.damage = n.f <= 0.0f ? 0.0f : s.damage - n.f;
        fired = true;
    }
    return fired;
}

void LevelScript::fire_remote_event(const std::string& name) {
    if (!graph_) return;
    auto it = graph_->remote_events.find(to_lower(name));
    if (it == graph_->remote_events.end()) return;
    bool any = false;
    for (int ev : it->second) any = check_activate(ev) || any;
    if (!any) {
        std::string where;
        for (int ev : it->second) {
            const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(ev)];
            where += (where.empty() ? "" : ", ") + graph_->packages[static_cast<size_t>(n.package)] +
                     (package_loaded(n.package) ? "" : " (not loaded)");
        }
        log("[Script] remote event '" + name + "' reached no listener (" + where + ")");
    }
}

void LevelScript::player_died() {
    if (!graph_) return;
    for (size_t i = 0; i < graph_->nodes.size(); ++i) {
        if (graph_->nodes[i].cls == "SeqEvt_TdPlayerDeath") check_activate(static_cast<int>(i));
    }
}

void LevelScript::queue(int node, bool top) {
    const int seq = graph_->nodes[static_cast<size_t>(node)].sequence;
    const size_t slot = seq < 0 ? static_cast<size_t>(graph_->nodes[static_cast<size_t>(node)].package)
                                : static_cast<size_t>(sequence_slot_.at(seq));
    std::vector<int>& list = active_[slot];
    if (std::find(list.begin(), list.end(), node) != list.end()) return;
    if (top) list.push_back(node);
    else list.insert(list.begin(), node);
}

int LevelScript::resolve_var(int var) const {
    for (int guard = 0; guard < 8 && var >= 0; ++guard) {
        const ScriptGraph::Node& v = graph_->nodes[static_cast<size_t>(var)];
        if (v.cls == "SeqVar_Named") {
            int found = -1;
            for (size_t i = 0; i < graph_->nodes.size(); ++i) {
                const ScriptGraph::Node& c = graph_->nodes[i];
                if (c.package != v.package || c.cls.rfind("SeqVar_", 0) != 0 || c.cls == "SeqVar_Named" || c.cls == "SeqVar_External") continue;
                if (!c.label.empty() && same(c.label, v.label.c_str())) {
                    found = static_cast<int>(i);
                    break;
                }
            }
            var = found;
        } else if (v.cls == "SeqVar_External") {
            int found = -1;
            if (v.sequence >= 0) {
                const ScriptGraph::Node& seq = graph_->nodes[static_cast<size_t>(v.sequence)];
                for (const ScriptGraph::VarLink& l : seq.vars) {
                    if (same(l.desc, v.label.c_str()) && !l.vars.empty()) found = l.vars.front();
                }
            }
            var = found;
        } else {
            return var;
        }
    }
    return var;
}

int LevelScript::linked_var(int node, const char* desc) const {
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    for (const ScriptGraph::VarLink& l : n.vars) {
        if (same(l.desc, desc) && !l.vars.empty()) return resolve_var(l.vars.front());
    }
    return -1;
}

std::vector<int> LevelScript::linked_vars(int node, const char* desc) const {
    std::vector<int> out;
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    for (const ScriptGraph::VarLink& l : n.vars) {
        if (!same(l.desc, desc)) continue;
        for (int v : l.vars) {
            const int r = resolve_var(v);
            if (r >= 0) out.push_back(r);
        }
    }
    return out;
}

float LevelScript::read_float(int node, const char* desc, float fallback) const {
    const int v = linked_var(node, desc);
    return v >= 0 ? state_[static_cast<size_t>(v)].f : fallback;
}

int LevelScript::read_int(int node, const char* desc, int fallback) const {
    const int v = linked_var(node, desc);
    return v >= 0 ? state_[static_cast<size_t>(v)].i : fallback;
}

bool LevelScript::read_bool(int node, const char* desc, bool fallback) const {
    const int v = linked_var(node, desc);
    return v >= 0 ? state_[static_cast<size_t>(v)].b : fallback;
}

bool LevelScript::var_is_player(int var) const {
    if (var < 0) return false;
    const std::string& c = graph_->nodes[static_cast<size_t>(var)].cls;
    return c == "SeqVar_Player" || c == "SeqVar_TdLocalPawn";
}

void LevelScript::fire_output(int node, const char* desc) {
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    for (size_t o = 0; o < n.outputs.size() && o < 32; ++o) {
        if (same(n.outputs[o].desc, desc)) state_[static_cast<size_t>(node)].out |= 1u << o;
    }
}

void LevelScript::fire_output_index(int node, size_t index) {
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    if (index < n.outputs.size() && index < 32) state_[static_cast<size_t>(node)].out |= 1u << index;
}

// Trigger: cylinder against the pawn's cylinder. Volume: the pawn's cylinder against each convex
// piece, as the distance of its centre to every plane against the cylinder's extent along that
// plane's normal.
bool LevelScript::player_touches(const ScriptActor& actor, const ScriptPlayerState& player) const {
    if (!actor.collide_actors) return false;
    const Vec3 centre = player.position + Vec3(0.0f, 0.0f, player.half_height);
    if (actor.has_cylinder && actor.hull_planes.empty()) {
        if (std::abs(centre.z - actor.location.z) > actor.half_height + player.half_height) return false;
        const float r = actor.radius + player.radius;
        return (centre - actor.location).length_xy_sq() <= r * r;
    }
    if (actor.hull_planes.empty()) return false;
    AABB grown = actor.bounds;
    grown.expand(std::max(player.radius, player.half_height));
    if (!grown.contains(centre)) return false;
    for (const auto& planes : actor.hull_planes) {
        bool inside = true;
        for (const ScriptActor::Plane& p : planes) {
            const float extent = player.radius * std::sqrt(p.normal.x * p.normal.x + p.normal.y * p.normal.y) +
                                 player.half_height * std::abs(p.normal.z);
            if (p.normal.dot(centre) - p.distance > extent) {
                inside = false;
                break;
            }
        }
        if (inside) return true;
    }
    return false;
}

// Trigger_LOS: the actor within TriggerDistance, within ScreenCenterDistance pixels of the
// screen's centre (a 1280-wide picture at the player's field of view), and in view.
bool LevelScript::player_sees(const ScriptGraph::Node& ev, const ScriptActor& actor, const ScriptPlayerState& player) const {
    const Vec3 to = actor.location - player.eye;
    const float dist = to.length();
    if (dist > ev.los_distance || dist < 1.0f) return false;
    const Vec3 dir = to * (1.0f / dist);
    const float cos_angle = dir.dot(player.view_dir);
    if (cos_angle <= 0.01f) return false;
    const float tan_angle = std::sqrt(std::max(0.0f, 1.0f - cos_angle * cos_angle)) / cos_angle;
    const float focal = 640.0f / std::tan(std::clamp(player.fov_deg, 30.0f, 150.0f) * 0.5f * DEG2RAD);
    if (tan_angle * focal > ev.los_screen) return false;
    if (ev.los_obstructions && host_.line_clear && !host_.line_clear(player.eye, actor.location)) return false;
    return true;
}

// The events that watch the player, once a frame.
void LevelScript::check_events(const ScriptPlayerState& player) {
    for (size_t i = 0; i < graph_->nodes.size(); ++i) {
        const ScriptGraph::Node& n = graph_->nodes[i];
        if (n.originator < 0) continue;
        State& s = state_[i];
        if (!s.enabled || !package_loaded(n.package)) continue;
        const ScriptActor& actor = graph_->actors[static_cast<size_t>(n.originator)];
        if (n.cls == "SeqEvent_Touch" || n.cls == "SeqEvent_TdTouch" || n.cls == "SeqEvt_TdTutorialCheckpointCompleted") {
            // (A TdTutorialCheckpoint counts as completed when the pawn reaches it: the nearest
            // reading of it without the training area's challenge system.)
            bool now = n.player_touches && !player.dead && player_touches(actor, player) && (n.momentum <= 0.0f || player.speed >= n.momentum);
            // UE3 generates Touch from a move: a pawn that spawns inside a trigger touches it the
            // first time it moves, not before.
            if (now && spawn_pending_ && (player.position - spawn_pos_).length() < 4.0f) now = false;
            if (now && !s.touching) {
                s.touching = true;
                if (check_activate(static_cast<int>(i))) {
                    s.impulses |= 1u;  // Touched
                    log("[Script] touched " + actor.name + " -> " + n.name);
                }
            } else if (!now && s.touching) {
                s.touching = false;
                // UnTouched is not held to the trigger count or the re-trigger delay.
                if (n.outputs.size() > 1 && !n.outputs[1].links.empty()) {
                    queue(static_cast<int>(i), false);
                    s.impulses |= 2u;
                }
            }
        } else if (n.cls == "SeqEvent_LOS") {
            if (!player.dead && player_sees(n, actor, player)) {
                if (check_activate(static_cast<int>(i))) s.impulses |= 1u;
            }
        } else if (n.cls == "SeqEvent_Used" || n.cls == "SeqEvent_TdUsed") {
            if (!player.use_pressed || player.dead) continue;
            const Vec3 to = actor.location - player.eye;
            if (to.length() > 200.0f || to.normalized().dot(player.view_dir) < 0.3f) continue;
            if (check_activate(static_cast<int>(i))) {
                s.impulses |= 1u;  // 'Out' or 'Start'
                // 'Finished' once the press is done (a button's animation takes about this long).
                for (size_t o = 1; o < n.outputs.size(); ++o) {
                    if (!same(n.outputs[o].desc, "Finished")) continue;
                    for (const ScriptGraph::Link& l : n.outputs[o].links) delayed_.push_back(Delayed{l.op, l.input, 0.8f + n.outputs[o].delay});
                }
                log("[Script] used " + actor.name + " -> " + n.name);
            }
        }
    }
}

void LevelScript::update(float dt, const ScriptPlayerState& player) {
    if (!graph_) return;
    ++tick_;
    time_ += static_cast<double>(dt);

    if (spawn_pending_ == 2) {
        spawn_pos_ = player.position;
        spawn_pending_ = 1;
    } else if (spawn_pending_ == 1 && (player.position - spawn_pos_).length() >= 4.0f) {
        spawn_pending_ = 0;
    }
    check_events(player);

    for (size_t k = 0; k < delayed_.size();) {
        delayed_[k].remaining -= dt;
        if (delayed_[k].remaining <= 0.0f) {
            queue(delayed_[k].op, true);
            state_[static_cast<size_t>(delayed_[k].op)].impulses |= 1u << (delayed_[k].input & 31);
            delayed_.erase(delayed_.begin() + static_cast<std::ptrdiff_t>(k));
        } else {
            ++k;
        }
    }
    for (size_t slot = 0; slot < active_.size(); ++slot) {
        if (!package_loaded(sequence_package_[slot])) continue;
        run_sequence(static_cast<int>(slot), dt);
    }
}

// USequence::ExecuteActiveOps: ops are popped off the end of the list; what an op activates is
// pushed so that its links run in link order, depth first, before whatever was already waiting.
void LevelScript::run_sequence(int slot, float dt) {
    std::vector<int>& list = active_[static_cast<size_t>(slot)];
    std::vector<int> latent;
    int steps = 0;
    while (!list.empty() && steps++ < 20000) {
        const int op = list.back();
        list.pop_back();
        State& s = state_[static_cast<size_t>(op)];
        const bool newly = !s.active;
        s.active = true;
        const bool done = step_op(op, dt, newly);
        if (done) {
            deactivated(op);
            s.active = false;
        } else if (std::find(latent.begin(), latent.end(), op) == latent.end()) {
            latent.push_back(op);
        }

        const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(op)];
        std::vector<ScriptGraph::Link> pending;
        for (size_t o = 0; o < n.outputs.size() && o < 32; ++o) {
            if (!(s.out & (1u << o)) || n.outputs[o].disabled) continue;
            for (const ScriptGraph::Link& link : n.outputs[o].links) {
                const ScriptGraph::Node& target = graph_->nodes[static_cast<size_t>(link.op)];
                float delay = n.outputs[o].delay;
                if (link.input >= 0 && static_cast<size_t>(link.input) < target.inputs.size()) {
                    if (target.inputs[static_cast<size_t>(link.input)].disabled) continue;
                    delay += target.inputs[static_cast<size_t>(link.input)].delay;
                }
                if (delay > 0.0f) delayed_.push_back(Delayed{link.op, link.input, delay});
                else pending.push_back(link);
            }
        }
        s.impulses = 0;
        s.out = 0;
        for (size_t k = pending.size(); k-- > 0;) {
            queue(pending[k].op, true);
            state_[static_cast<size_t>(pending[k].op)].impulses |= 1u << (pending[k].input & 31);
        }
    }
    while (!latent.empty()) {
        const int op = latent.back();
        latent.pop_back();
        if (std::find(list.begin(), list.end(), op) == list.end()) list.push_back(op);
    }
}

// USeqAct_Interp::Play
void LevelScript::interp_play(int node) {
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    State& s = state_[static_cast<size_t>(node)];
    if (n.b2 && (!s.playing || n.b3)) s.position = 0.0f;
    const bool was_playing = s.playing;
    s.playing = true;
    s.aborted = false;
    if (n.matinee < 0) return;
    const ScriptMatinee& m = graph_->matinees[static_cast<size_t>(n.matinee)];
    if (m.player_cutscene >= 0 && !was_playing) {
        playing_cutscene_ = m.player_cutscene;
        playing_cutscene_node_ = node;
        log("[Script] " + n.name + " plays the player's cutscene '" + m.name + "' (" + std::to_string(m.length) + " s at " +
            std::to_string(n.f) + "x)");
        if (host_.play_cutscene) host_.play_cutscene(m.player_cutscene, n.f);
    }
}

void LevelScript::interp_stop(int node, bool aborted) {
    State& s = state_[static_cast<size_t>(node)];
    if (!s.playing) return;
    s.playing = false;
    s.aborted = aborted;
    if (playing_cutscene_node_ == node) {
        playing_cutscene_ = -1;
        playing_cutscene_node_ = -1;
        if (host_.stop_cutscene) host_.stop_cutscene();
    }
}

void LevelScript::fire_matinee_keys(int node, float previous, float position) {
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    const ScriptMatinee& m = graph_->matinees[static_cast<size_t>(n.matinee)];
    for (const auto& [when, name] : m.events) {
        if (when > previous && when <= position) fire_output(node, name.c_str());
    }
    // The toggle tracks switch the emitters and lens flares of their groups.
    if (host_.toggle_effect) {
        for (const ScriptMatinee::Toggle& toggle : m.toggles) {
            if (!(toggle.time > previous && toggle.time <= position)) continue;
            for (int v : linked_vars(node, toggle.group.c_str())) {
                const int actor = graph_->nodes[static_cast<size_t>(v)].actor;
                if (actor >= 0) host_.toggle_effect(graph_->actors[static_cast<size_t>(actor)], toggle.action);
            }
        }
    }
    // A player cutscene's sound track plays from the player; another Matinee's would play from
    // its actors, positioned, which is not done here (an ambient loop would otherwise be heard
    // everywhere at full volume).
    if (m.player_cutscene < 0) return;
    for (const ScriptSound& snd : m.sounds) {
        if (snd.time > previous && snd.time <= position && host_.play_sound) host_.play_sound(snd.cue, snd.bank, snd.voice, 1.0f, nullptr);
    }
}

// USeqAct_Interp::StepInterp: the position advances by dt * PlayRate; the event keys passed fire
// their outputs; at the end a looping Matinee starts over and any other stops.
void LevelScript::interp_step(int node, float dt) {
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    State& s = state_[static_cast<size_t>(node)];
    if (!s.playing || n.matinee < 0) return;
    const float length = graph_->matinees[static_cast<size_t>(n.matinee)].length;
    if (length <= kSmall) {
        // An empty Matinee (a few are cooked that way) is over the moment it starts.
        fire_matinee_keys(node, -1.0f, 0.0f);
        s.position = length;
        s.playing = false;
        if (playing_cutscene_node_ == node) {
            playing_cutscene_ = -1;
            playing_cutscene_node_ = -1;
        }
        return;
    }
    float next = s.position + dt * std::max(0.01f, n.f);
    bool stop = false;
    if (next > length) {
        if (n.b) {
            fire_matinee_keys(node, s.position, length);
            s.position = 0.0f;
            next = std::fmod(next, length);
        } else {
            next = length;
            stop = true;
        }
    }
    fire_matinee_keys(node, s.position, next);
    s.position = next;
    if (stop) {
        s.playing = false;
        if (playing_cutscene_node_ == node) {
            playing_cutscene_ = -1;
            playing_cutscene_node_ = -1;
        }
    }
}

// SkipCutscene (TdPlayerController, native): a skippable Matinee is set to its end
// (USeqAct_Interp::SetPosition(InterpLength, bJump)): the event keys in between do not fire, and
// the op, no longer playing, fires Completed (the Edge intro's "Land closes the gate that leads
// to the teleport" pattern only works this way; Aborted is the Stop input's).
void LevelScript::skip_cutscene() {
    if (playing_cutscene_node_ < 0 || skip_disabled_) return;
    const int node = playing_cutscene_node_;
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    State& s = state_[static_cast<size_t>(node)];
    log("[Script] cutscene skipped: " + n.name);
    playing_cutscene_ = -1;
    playing_cutscene_node_ = -1;
    if (host_.stop_cutscene) host_.stop_cutscene();
    if (n.matinee >= 0) s.position = graph_->matinees[static_cast<size_t>(n.matinee)].length;
    s.playing = false;
    s.aborted = false;
}

void LevelScript::cutscene_finished() {
    // The op reaches its length on its own in the same frame; nothing to do unless the host's
    // playback ran ahead of the op (a long first frame).
    if (playing_cutscene_node_ < 0) return;
    State& s = state_[static_cast<size_t>(playing_cutscene_node_)];
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(playing_cutscene_node_)];
    if (n.matinee >= 0 && s.playing) {
        const float length = graph_->matinees[static_cast<size_t>(n.matinee)].length;
        fire_matinee_keys(playing_cutscene_node_, s.position, length);
        s.position = length;
        s.playing = false;
    }
    playing_cutscene_ = -1;
    playing_cutscene_node_ = -1;
}

void LevelScript::accept_message() {
    if (tutorial_node_ >= 0) state_[static_cast<size_t>(tutorial_node_)].waiting = false;
}

void LevelScript::into_cutscene_finished() {
    if (into_cutscene_node_ >= 0) state_[static_cast<size_t>(into_cutscene_node_)].waiting = false;
}

// Activated() and UpdateOp() of one op. True when it has finished (it is not latent, or is done).
bool LevelScript::step_op(int node, float dt, bool newly) {
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    State& s = state_[static_cast<size_t>(node)];
    const std::string& c = n.cls;
    const uint32_t all_outputs = n.outputs.size() >= 32 ? 0xFFFFFFFFu : ((1u << n.outputs.size()) - 1u);

    // --- latent ops ---
    if (c == "SeqAct_Interp") {
        if (newly && (s.impulses & 0x13u)) s.initialised = true;
        if (s.impulses & 1u) interp_play(node);
        else if (s.impulses & 4u) interp_stop(node, true);  // Stop
        else if (s.impulses & 8u) s.playing = false;        // Pause
        else if (s.impulses & 2u) interp_stop(node, false); // Reverse: not played backwards here
        interp_step(node, dt);
        return !s.playing;
    }
    if (c == "SeqAct_Delay") {
        if (s.impulses & 2u) {
            s.delay_running = false;
            fire_output(node, "Aborted");
            return true;
        }
        if (s.impulses & 4u) {  // Pause
            return s.delay_running;
        }
        if (s.impulses & 1u) {
            if (!s.delay_running) {
                s.delay_running = true;
                s.remaining = read_float(node, "Duration", n.f);
                s.started_tick = tick_;
            }
        }
        if (!s.delay_running) return true;
        if (s.started_tick != tick_) {
            s.remaining -= dt;
            if (s.remaining <= 0.0f) {
                s.delay_running = false;
                fire_output(node, "Finished");
                return true;
            }
        }
        return false;
    }
    if (c == "SeqAct_TdFadeEffect") {
        if (newly) {
            s.remaining = std::max(n.f, 0.0f);
            s.started_tick = tick_;
            if (host_.fade) host_.fade(n.b, n.f, n.color);
            log(std::string("[Script] fade ") + (n.b ? "out" : "in") + " over " + std::to_string(n.f) + " s (" + n.name + ")");
        }
        if (s.started_tick != tick_) s.remaining -= dt;
        if (s.remaining <= 0.0f) {
            s.out = all_outputs;
            return true;
        }
        return false;
    }
    if (c == "SeqAct_TdPlaySound" || c == "SeqAct_PlaySound") {
        if (s.impulses & 2u) {  // Stop
            if (host_.stop_sound && !n.cue.empty()) host_.stop_sound(n.cue);
            s.sound_playing = false;
            fire_output(node, "Stopped");
            return true;
        }
        if (s.impulses & 1u) {
            float length = 0.0f;
            // The Target the sound plays from: the player (SeqVar_Player / TdLocalPawn, or none)
            // or a placed actor, positioned.
            const Vec3* at = nullptr;
            for (int t : linked_vars(node, "Target")) {
                const int actor = graph_->nodes[static_cast<size_t>(t)].actor;
                if (actor >= 0 && !var_is_player(t)) at = &graph_->actors[static_cast<size_t>(actor)].location;
            }
            if (host_.play_sound && !n.cue.empty()) length = host_.play_sound(n.cue, n.cue_bank, n.cue_voice, n.f, at);
            s.remaining = length;
            s.sound_playing = length > 0.0f;
            s.started_tick = tick_;
            fire_output(node, "Out");
            if (!s.sound_playing) {
                fire_output(node, "Finished");
                return true;
            }
        }
        if (!s.sound_playing) return true;
        if (s.started_tick != tick_) s.remaining -= dt;
        if (s.remaining <= 0.0f) {
            s.sound_playing = false;
            fire_output(node, "Finished");
            return true;
        }
        return false;
    }
    if (c == "SeqAct_TdTutorialMessage") {
        if (newly) {
            if (tutorial_node_ >= 0 && tutorial_node_ != node) state_[static_cast<size_t>(tutorial_node_)].waiting = false;
            tutorial_node_ = node;
            s.waiting = true;
            s.remaining = n.f;
            s.started_tick = tick_;
            if (host_.show_tutorial) host_.show_tutorial(n.text, n.f, n.b);
        }
        if (s.started_tick != tick_) s.remaining -= dt;
        if (!s.waiting || s.remaining <= 0.0f) {
            if (tutorial_node_ == node) {
                tutorial_node_ = -1;
                if (host_.hide_tutorial) host_.hide_tutorial();
            }
            s.waiting = false;
            s.out = all_outputs;  // Success
            return true;
        }
        return false;
    }
    if (c == "SeqAct_TdIntoCutscene") {
        if (newly) {
            s.waiting = false;
            into_cutscene_node_ = node;
            const int dest = linked_var(node, "Destination");
            const int actor = dest >= 0 ? graph_->nodes[static_cast<size_t>(dest)].actor : -1;
            if (actor >= 0 && host_.into_cutscene) {
                s.waiting = true;
                const ScriptActor& a = graph_->actors[static_cast<size_t>(actor)];
                host_.into_cutscene(a.location, a.yaw_deg);
            }
        }
        if (s.waiting) return false;
        into_cutscene_node_ = -1;
        fire_output(node, "Finished");
        return true;
    }
    if (c == "SeqAct_MultiLevelStreaming" || c == "SeqAct_LevelStreaming" || c == "SeqAct_StreamingZone") {
        if (newly) {
            const bool load = !(s.impulses & 2u);  // Load / Unload (StreamingZone: Activated)
            if (host_.stream_levels && !n.level_names.empty()) host_.stream_levels(n.level_names, load);
            s.started_tick = tick_;
            return false;  // Finished the tick after, once the levels are in
        }
        fire_output(node, "Finished");
        return true;
    }

    // --- events ---
    if (c.rfind("SeqEvent_", 0) == 0 || c.rfind("SeqEvt_", 0) == 0) {
        if (c == "SeqEvent_Touch" || c == "SeqEvent_TdTouch" || c == "SeqEvent_Used" || c == "SeqEvent_TdUsed") {
            s.out = s.impulses ? s.impulses : 1u;  // Touched / UnTouched as queued
            if (c == "SeqEvent_TdUsed" && (s.out & 1u)) {
                // 'Start' and, for a plain button, 'Out'; 'Finished' comes from check_events.
                for (size_t o = 0; o < n.outputs.size(); ++o) {
                    if (same(n.outputs[o].desc, "Start") || same(n.outputs[o].desc, "Out")) s.out |= 1u << o;
                }
            }
            s.out &= all_outputs;
        } else {
            s.out = all_outputs;
        }
        return true;
    }

    // --- the rest finishes in the step it is activated ---
    if (c == "Sequence") {
        for (size_t i = 0; i < n.inputs.size() && i < 32; ++i) {
            if ((s.impulses & (1u << i)) && n.inputs[i].linked_op >= 0) check_activate(n.inputs[i].linked_op);
        }
    } else if (c == "SeqAct_FinishSequence") {
        if (n.sequence >= 0) {
            const ScriptGraph::Node& seq = graph_->nodes[static_cast<size_t>(n.sequence)];
            for (const ScriptGraph::Output& o : seq.outputs) {
                if (o.linked_op != node) continue;
                for (size_t k = o.links.size(); k-- > 0;) {
                    queue(o.links[k].op, true);
                    state_[static_cast<size_t>(o.links[k].op)].impulses |= 1u << (o.links[k].input & 31);
                }
            }
        }
    } else if (c == "SeqAct_Switch") {
        const int index = read_int(node, "Index", s.index);
        if (index >= 1 && static_cast<size_t>(index) <= n.outputs.size()) s.out |= 1u << (index - 1);
        int next = index + n.i;
        if (n.b && static_cast<size_t>(next) > n.outputs.size()) next = 1;
        s.index = next;
        const int iv = linked_var(node, "Index");
        if (iv >= 0) state_[static_cast<size_t>(iv)].i = next;
    } else if (c == "SeqAct_RandomSwitch") {
        const int count = std::max(1, std::min<int>(n.i, static_cast<int>(n.outputs.size())));
        // xorshift32, seeded per level
        random_ ^= random_ << 13;
        random_ ^= random_ >> 17;
        random_ ^= random_ << 5;
        const int pick = static_cast<int>(random_ % static_cast<uint32_t>(count));
        s.out |= 1u << pick;
        const int av = linked_var(node, "Active Link");
        if (av >= 0) state_[static_cast<size_t>(av)].i = pick + 1;
    } else if (c == "SeqAct_Gate") {
        if (s.impulses & 2u) s.open = true;
        else if (s.impulses & 4u) s.open = false;
        else if (s.impulses & 8u) s.open = !s.open;
        if (s.open && (s.impulses & 1u)) {
            s.out = all_outputs;
            if (n.i > 0 && --s.auto_close_left <= 0) {
                s.open = false;
                s.auto_close_left = n.i;
            }
        }
    } else if (c == "SeqAct_SetBool") {
        const int value = linked_var(node, "Value");
        const bool v = value >= 0 ? state_[static_cast<size_t>(value)].b : n.b;
        for (int t : linked_vars(node, "Target")) state_[static_cast<size_t>(t)].b = v;
        s.out = all_outputs;
    } else if (c == "SeqAct_SetInt") {
        const int value = linked_var(node, "Value");
        const int v = value >= 0 ? state_[static_cast<size_t>(value)].i : n.i;
        for (int t : linked_vars(node, "Target")) state_[static_cast<size_t>(t)].i = v;
        s.out = all_outputs;
    } else if (c == "SeqAct_SetFloat") {
        const int value = linked_var(node, "Value");
        const float v = value >= 0 ? state_[static_cast<size_t>(value)].f : n.f;
        for (int t : linked_vars(node, "Target")) state_[static_cast<size_t>(t)].f = v;
        s.out = all_outputs;
    } else if (c == "SeqCond_CompareBool") {
        fire_output(node, read_bool(node, "Bool", false) ? "True" : "False");
    } else if (c == "SeqCond_CompareFloat" || c == "SeqCond_CompareInt") {
        const float a = c == "SeqCond_CompareInt" ? static_cast<float>(read_int(node, "A", n.i)) : read_float(node, "A", n.f);
        const float b = c == "SeqCond_CompareInt" ? static_cast<float>(read_int(node, "B", static_cast<int>(n.f2))) : read_float(node, "B", n.f2);
        if (a <= b) fire_output(node, "A <= B");
        if (a > b) fire_output(node, "A > B");
        if (a == b) fire_output(node, "A == B");
        if (a < b) fire_output(node, "A < B");
        if (a >= b) fire_output(node, "A >= B");
    } else if (c == "SeqAct_AddFloat" || c == "SeqAct_SubtractFloat" || c == "SeqAct_MultFloat") {
        const float a = read_float(node, "A", n.f);
        const float b = read_float(node, "B", n.f2);
        const float r = c == "SeqAct_AddFloat" ? a + b : c == "SeqAct_SubtractFloat" ? a - b : a * b;
        for (int t : linked_vars(node, "FloatResult")) state_[static_cast<size_t>(t)].f = r;
        s.out = all_outputs;
    } else if (c == "SeqAct_AddInt" || c == "SeqAct_SubtractInt" || c == "SeqAct_MultInt") {
        const int a = read_int(node, "A", n.i);
        const int b = read_int(node, "B", static_cast<int>(n.f2));
        const int r = c == "SeqAct_AddInt" ? a + b : c == "SeqAct_SubtractInt" ? a - b : a * b;
        for (int t : linked_vars(node, "IntResult")) state_[static_cast<size_t>(t)].i = r;
        s.out = all_outputs;
    } else if (c == "SeqAct_Toggle") {
        // Turn On / Turn Off / Toggle: the events linked as targets are enabled or disabled; a
        // trigger actor's collision likewise. Lights, emitters and the like are not the script's.
        for (int t : linked_vars(node, "Target")) {
            State& ts = state_[static_cast<size_t>(t)];
            const ScriptGraph::Node& tn = graph_->nodes[static_cast<size_t>(t)];
            if (tn.cls.rfind("SeqEvent", 0) == 0 || tn.cls.rfind("SeqEvt", 0) == 0) {
                if (s.impulses & 1u) ts.enabled = true;
                else if (s.impulses & 2u) ts.enabled = false;
                else if (s.impulses & 4u) ts.enabled = !ts.enabled;
            }
        }
        // SequenceEvents are also linked straight from the actor: the Toggle's targets include
        // the actor, and every event whose Originator it is follows.
        for (int t : linked_vars(node, "Target")) {
            const int actor = graph_->nodes[static_cast<size_t>(t)].actor;
            if (actor < 0) continue;
            const ScriptActor& sa = graph_->actors[static_cast<size_t>(actor)];
            const std::string& acls = sa.cls;
            if (acls.find("Emitter") != std::string::npos || acls == "LensFlareSource") {
                // Emitter.OnToggle, LensFlareSource.OnToggle: the system or the flare goes on or off.
                if (host_.toggle_effect) host_.toggle_effect(sa, (s.impulses & 1u) ? 0 : ((s.impulses & 2u) ? 1 : 2));
                continue;
            }
            if (acls.find("Trigger") == std::string::npos && acls.find("Volume") == std::string::npos) continue;
            for (size_t i = 0; i < graph_->nodes.size(); ++i) {
                if (graph_->nodes[i].originator != actor) continue;
                State& es = state_[i];
                if (s.impulses & 1u) es.enabled = true;
                else if (s.impulses & 2u) es.enabled = false;
                else if (s.impulses & 4u) es.enabled = !es.enabled;
            }
            if (acls.find("Volume") != std::string::npos && host_.change_collision) {
                if (s.impulses & 1u) {
                    host_.change_collision(sa, true, acls == "BlockingVolume");
                } else if (s.impulses & 2u) {
                    host_.change_collision(sa, false, false);
                }
            }
        }
        s.out = all_outputs;
    } else if (c == "SeqAct_ChangeCollision") {
        for (int t : linked_vars(node, "Target")) {
            const int actor = graph_->nodes[static_cast<size_t>(t)].actor;
            if (actor < 0) continue;
            const ScriptActor& sa = graph_->actors[static_cast<size_t>(actor)];
            log("[Script] change collision on " + sa.package + "." + sa.name +
                " collide=" + (n.b ? "1" : "0") + " block=" + (n.b2 ? "1" : "0"));
            if (host_.change_collision) {
                host_.change_collision(sa, n.b, n.b2);
            }
        }
        s.out = all_outputs;
    } else if (c == "SeqAct_ActivateRemoteEvent") {
        fire_remote_event(n.label);
        s.out = all_outputs;
    } else if (c == "SeqAct_TdCheckpoint") {
        const int var = linked_var(node, "Checkpoint");
        const int actor = var >= 0 ? graph_->nodes[static_cast<size_t>(var)].actor : -1;
        if (actor >= 0) {
            active_checkpoint_ = actor;
            const ScriptActor& a = graph_->actors[static_cast<size_t>(actor)];
            log("[Script] checkpoint '" + a.checkpoint_name + "'" + (n.b2 ? " (soft)" : " (saved)") + (n.b ? ", pawn teleported" : ""));
            if (host_.set_checkpoint) host_.set_checkpoint(a, n.b, !n.b2);
            // TdCheckpoint.OnTdCheckpoint: the checkpoint's SeqEvt_TdCheckpointActivated events fire.
            fire_checkpoint_events(actor, false);
        }
        s.out = all_outputs;
    } else if (c == "SeqAct_TdLevelCompleted") {
        log("[Script] level completed -> " + n.text + " at '" + n.text2 + "'");
        if (host_.level_completed) host_.level_completed(n.text, n.text2);
        s.out = all_outputs;
    } else if (c == "SeqAct_TdSupersMessage") {
        if (host_.show_supers) host_.show_supers(n.text, n.f);
        s.out = all_outputs;
    } else if (c == "SeqAct_TdTriggerSubtitle") {
        if (host_.show_subtitle) host_.show_subtitle(n.text, n.f);
        s.out = all_outputs;
    } else if (c == "SeqAct_TdTriggerSplashHint") {
        if (host_.splash_hint) host_.splash_hint(n.i);
        s.out = all_outputs;
    } else if (c == "SeqAct_TdDisablePlayerInput") {
        input_move_disabled_ = input_move_disabled_ || n.b;
        input_look_disabled_ = input_look_disabled_ || n.b2;
        cinematic_mode_ = cinematic_mode_ || n.b3;
        skip_disabled_ = skip_disabled_ || n.b4;
        if (host_.disable_input) host_.disable_input(n.b, n.b2, n.b3, n.b4);
        s.out = all_outputs;
    } else if (c == "SeqAct_TdEnablePlayerInput") {
        input_move_disabled_ = false;
        input_look_disabled_ = false;
        cinematic_mode_ = false;
        skip_disabled_ = false;
        if (host_.enable_input) host_.enable_input();
        s.out = all_outputs;
    } else if (c == "SeqAct_DisableLoadFromLastCheckpoint") {
        load_checkpoint_disabled_ = n.b3;
        s.out = all_outputs;
    } else if (c == "SeqAct_Teleport" || c == "SeqAct_TdTeleport") {
        bool player = false;
        for (int t : linked_vars(node, "Target")) player = player || var_is_player(t);
        const int dest = linked_var(node, "Destination");
        const int actor = dest >= 0 ? graph_->nodes[static_cast<size_t>(dest)].actor : -1;
        if (player && actor >= 0 && host_.teleport_player) {
            const ScriptActor& a = graph_->actors[static_cast<size_t>(actor)];
            log("[Script] player teleported to " + a.name);
            host_.teleport_player(a.location, a.yaw_deg);
        }
        s.out = all_outputs;
    } else if (c == "SeqAct_TdPlayerFail") {
        if (host_.player_fail) host_.player_fail();
        s.out = all_outputs;
    } else if (c == "SeqAct_CauseDamage") {
        bool player = false;
        const float amount = read_float(node, "Amount", n.f);
        for (int t : linked_vars(node, "Target")) {
            if (var_is_player(t)) {
                player = true;
                continue;
            }
            // Any other actor: its own damage events hear of it (a pane breaks its broken twin).
            const int actor = graph_->nodes[static_cast<size_t>(t)].actor;
            if (actor < 0) continue;
            const ScriptActor& sa = graph_->actors[static_cast<size_t>(actor)];
            damage_actor(sa.package, sa.name, amount, false, n.damage_types.empty() ? std::string("damagetype") : n.damage_types.front());
        }
        if (player && host_.damage_player) host_.damage_player(amount);
        s.out = all_outputs;
    } else if (c == "SeqAct_ShowLoading" || c == "SeqAct_HideLoading") {
        if (host_.loading_indicator) host_.loading_indicator(c == "SeqAct_ShowLoading");
        s.out = all_outputs;
    } else if (c == "SeqAct_TdPhysXGate") {
        // No hardware PhysX here: the branch for its absence.
        bool fired = false;
        for (size_t o = 0; o < n.outputs.size() && o < 32; ++o) {
            const std::string d = to_lower(n.outputs[o].desc);
            if (d.find("off") != std::string::npos || d.find("no") == 0 || d.find("disabled") != std::string::npos) {
                s.out |= 1u << o;
                fired = true;
            }
        }
        if (!fired && n.outputs.size() > 1) s.out |= 1u << (n.outputs.size() - 1);
    } else if (c == "SeqAct_ToggleHidden" || c == "SeqAct_Destroy") {
        // Hide / UnHide / Toggle, or gone for good: an emitter or a lens flare stops, any other actor
        // (a pane of glass, its broken twin) is no longer drawn.
        for (int t : linked_vars(node, "Target")) {
            const int actor = graph_->nodes[static_cast<size_t>(t)].actor;
            if (actor < 0) continue;
            const ScriptActor& sa = graph_->actors[static_cast<size_t>(actor)];
            if (sa.cls.find("Emitter") == std::string::npos && sa.cls != "LensFlareSource") {
                if (!host_.hide_actor) continue;
                const int action = c == "SeqAct_Destroy" ? 3 : (s.impulses & 1u) ? 0 : (s.impulses & 2u) ? 1 : (s.impulses & 4u) ? 2 : -1;
                if (action < 0) continue;
                host_.hide_actor(sa, action);
                static const char* const kWhat[4] = {"hidden", "shown", "hidden or shown", "destroyed"};
                log("[Script] " + sa.name + " " + kWhat[action] + " (" + n.name + ")");
                continue;
            }
            if (!host_.hide_effect) continue;
            if (c == "SeqAct_Destroy" || (s.impulses & 1u)) {
                host_.hide_effect(sa, true);
            } else if (s.impulses & 2u) {
                host_.hide_effect(sa, false);
            }
        }
        s.out = all_outputs;
    } else if (c == "SeqAct_ActorFactory" && n.i > 0) {
        // "Spawn Actor" (the first input): the factory's emitter at every spawn point.
        if ((s.impulses & 1u) && host_.spawn_effect) {
            int made = 0;
            for (int t : linked_vars(node, "Spawn Point")) {
                const int actor = graph_->nodes[static_cast<size_t>(t)].actor;
                if (actor < 0) continue;
                host_.spawn_effect(graph_->packages[static_cast<size_t>(n.package)], n.i, graph_->actors[static_cast<size_t>(actor)]);
                ++made;
            }
            log("[Script] " + n.name + " makes its emitter at " + std::to_string(made) + " spawn point(s)");
        }
        fire_output(node, "Finished");
    } else if (c == "SeqAct_TdDisarmRopeburn") {
        // The Ropeburn counter-disarm is not played here: the fight is given to the player so the
        // chapter goes on.
        fire_output(node, "Succeeded");
    } else {
        // Actor factories, AI directives, lights, emitters, materials, stats, music: not the
        // script's to do here. The op finishes at once through the outputs that mean "done"
        // (Out, Finished, Completed, Spawned ..), not through its branches.
        bool fired = false;
        for (size_t o = 0; o < n.outputs.size() && o < 32; ++o) {
            const std::string d = to_lower(n.outputs[o].desc);
            if (d == "out" || d == "finished" || d == "completed" || d == "done" || d == "success" || d.rfind("spawned", 0) == 0) {
                s.out |= 1u << o;
                fired = true;
            }
        }
        if (!fired && !n.outputs.empty()) s.out |= 1u;
    }
    (void)dt;
    return true;
}

// USeqAct_Interp::DeActivated: "Completed" if it stopped at the end, "Aborted" if it was stopped
// part way. An op that never initialised has no data and fires nothing.
void LevelScript::deactivated(int node) {
    const ScriptGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    if (n.cls != "SeqAct_Interp") return;
    State& s = state_[static_cast<size_t>(node)];
    if (!s.initialised || n.matinee < 0) return;
    const float length = graph_->matinees[static_cast<size_t>(n.matinee)].length;
    if (s.aborted) {
        if (n.outputs.size() > 1) s.out |= 2u;
    } else if (s.position > length - kSmall) {
        if (!n.outputs.empty()) s.out |= 1u;
    }
    s.initialised = false;
    s.aborted = false;
}

std::string LevelScript::playing() const {
    std::string out;
    if (!graph_) return out;
    for (size_t i = 0; i < graph_->nodes.size(); ++i) {
        const ScriptGraph::Node& n = graph_->nodes[i];
        const State& s = state_[i];
        if (n.cls != "SeqAct_Interp" || !s.playing || n.matinee < 0) continue;
        if (!out.empty()) out += " ";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "@%.2f", static_cast<double>(s.position));
        out += n.name + buf;
    }
    return out;
}

// --- localized strings --------------------------------------------------------------------------

void LocalizedStrings::load(const std::string& game_root) {
    strings_.clear();
    const std::string dir = get_localization_path(game_root, "", "INT");
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(dir).parent_path(), ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = to_lower(entry.path().extension().string());
        if (ext != ".int") continue;
        const std::string file = to_lower(entry.path().stem().string());
        // The voice-over files are the subtitle tables (Subtitles.INT, A_VO_*.INT): not strings.
        if (file.rfind("a_vo_", 0) == 0) continue;
        IniConfig ini;
        if (!ini.load_file(entry.path().string())) continue;
        for (const std::string& section : ini.get_section_names()) {
            // "[SP01a UIDataProvider_TdMaps]": the section's name is its first word.
            std::string sec = section.substr(0, section.find(' '));
            for (const auto& [key, values] : ini.get_section_keys(section)) {
                if (values.empty()) continue;
                std::string value = values.back();
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
                strings_[file + "." + to_lower(sec) + "." + to_lower(key)] = value;
            }
        }
    }
}

std::string LocalizedStrings::lookup(const std::string& file, const std::string& section, const std::string& key) const {
    auto it = strings_.find(to_lower(file) + "." + to_lower(section) + "." + to_lower(key));
    return it == strings_.end() ? std::string() : it->second;
}

// "<Strings:TdGameUI.TdSupersMessage.SP01A>" and "<StringAliasBindings:GBA_Jump>" markup, resolved;
// anything else is literal.
std::string LocalizedStrings::resolve(const std::string& text) const {
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t open = text.find('<', pos);
        if (open == std::string::npos) {
            out += text.substr(pos);
            break;
        }
        const size_t close = text.find('>', open);
        if (close == std::string::npos) {
            out += text.substr(pos);
            break;
        }
        out += text.substr(pos, open - pos);
        const std::string tag = text.substr(open + 1, close - open - 1);
        const size_t colon = tag.find(':');
        const std::string kind = colon == std::string::npos ? std::string() : to_lower(tag.substr(0, colon));
        const std::string ref = colon == std::string::npos ? tag : tag.substr(colon + 1);
        if (kind == "strings") {
            const size_t a = ref.find('.');
            const size_t b = a == std::string::npos ? std::string::npos : ref.find('.', a + 1);
            std::string value;
            if (a != std::string::npos && b != std::string::npos) value = lookup(ref.substr(0, a), ref.substr(a + 1, b - a - 1), ref.substr(b + 1));
            out += value.empty() ? ref : value;
        } else if (kind == "stringaliasbindings" || kind == "stringaliasmap") {
            // The key the game binding is on (Config/DefaultInput.ini): [GBA_Jump] -> [SPACE]
            static const std::pair<const char*, const char*> kKeys[] = {
                {"GBA_Jump", "SPACE"}, {"GBA_Crouch", "LEFT SHIFT"}, {"GBA_Fire", "LEFT MOUSE"}, {"GBA_SwitchWeapon", "RIGHT MOUSE"},
                {"GBA_Use", "E"}, {"GBA_LookAt", "LEFT ALT"}, {"GBA_ReactionTime", "R"}, {"GBA_LookBehind", "Q"},
                {"GBA_InGameMenu", "TAB"}, {"GBA_MoveForward", "W"}, {"GBA_Backward", "S"}, {"GBA_MoveBackward", "S"},
                {"GBA_StrafeLeft", "A"}, {"GBA_StrafeRight", "D"}, {"GBA_ZoomWeapon", "F"}, {"GBA_WalkMod", "LEFT CTRL"},
                {"GBA_Strafe", ""}, {"GBA_TurnLeft", ""}, {"GBA_TurnRight", ""}, {"GBA_LookUp", ""}, {"GBA_LookDown", ""},
                {"GBA_Look_Gamepad", ""}, {"GBA_Look_MouseX", "MOUSE"}, {"GBA_Look_MouseY", ""},
            };
            std::string value = ref;
            for (const auto& [name, key] : kKeys) {
                if (same(ref, name)) {
                    value = key;
                    break;
                }
            }
            if (!value.empty()) out += "[" + value + "] ";
        } else {
            out += text.substr(open, close - open + 1);
        }
        pos = close + 1;
    }
    // "%SixAxis_BalanceWalk%" and the like are the PS3's; the PC shows nothing for them.
    for (size_t p = out.find('%'); p != std::string::npos; p = out.find('%')) {
        const size_t q = out.find('%', p + 1);
        if (q == std::string::npos) break;
        out.erase(p, q - p + 1);
    }
    // Doubled spaces from empty aliases
    for (size_t p = out.find("  "); p != std::string::npos; p = out.find("  ")) out.erase(p, 1);
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string LocalizedStrings::tutorial_message(const std::string& key) const {
    const std::string raw = lookup("TdGameUI", "TdTutorialMessages", key);
    return raw.empty() ? key : resolve(raw);
}

std::string LocalizedStrings::splash_hint(int number) const {
    return resolve(lookup("TdGameUI", "TdSplashHints", "SplashHint" + std::to_string(number)));
}

}  // namespace me
