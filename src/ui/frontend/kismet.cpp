#include "kismet.hpp"

#include "../../assets/ue3_props.hpp"
#include "../../assets/upk_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace me::fe {

namespace {

constexpr float kSmall = 1.0e-4f;  // KINDA_SMALL_NUMBER
constexpr float kRadToDeg = 57.29577951f;

CurveMode curve_mode(const std::string& s) {
    if (s == "CIM_CurveAuto" || s == "CIM_CurveAutoClamped") return CurveMode::CurveAuto;
    if (s == "CIM_Constant") return CurveMode::Constant;
    if (s == "CIM_CurveUser") return CurveMode::CurveUser;
    if (s == "CIM_CurveBreak") return CurveMode::CurveBreak;
    return CurveMode::Linear;
}

Vec3 curve_value(const UProperty& p) {
    if (p.type == "FloatProperty") return Vec3{p.f, 0.0f, 0.0f};
    return Vec3{p.v[0], p.v[1], p.v[2]};
}

void read_curve(const UPropertyList& track, const std::string& name, Curve& out) {
    const UProperty* curve = find_prop(track, name);
    if (!curve) return;
    const UProperty* points = find_prop(curve->fields, "Points");
    if (!points) return;
    for (const auto& el : points->elements) {
        CurveKey k;
        for (const UProperty& f : el) {
            if (f.name == "InVal") k.t = f.f;
            else if (f.name == "OutVal") k.v = curve_value(f);
            else if (f.name == "ArriveTangent") k.arrive = curve_value(f);
            else if (f.name == "LeaveTangent") k.leave = curve_value(f);
            else if (f.name == "InterpMode") k.mode = curve_mode(f.s);
        }
        out.keys.push_back(k);
    }
}

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

bool load_matinee_data(const UPKPackage& pkg, int32_t index, MatineeData& out) {
    UPropertyList props;
    parse_export_properties(pkg, index, props);
    out = MatineeData{};
    out.name = export_object_name(pkg, index);
    out.length = prop_float(props, "InterpLength", 5.0f);
    const UProperty* groups = find_prop(props, "InterpGroups");
    if (!groups) return false;
    for (int32_t g : groups->ints) {
        if (g <= 0) continue;
        UPropertyList gp;
        parse_export_properties(pkg, g, gp);
        MatineeGroup group;
        group.name = prop_name(gp, "GroupName");
        if (const UProperty* tracks = find_prop(gp, "InterpTracks")) {
            for (int32_t t : tracks->ints) {
                if (t <= 0) continue;
                const std::string cls = object_class_name(pkg, t);
                UPropertyList tp;
                parse_export_properties(pkg, t, tp);
                if (cls == "InterpTrackMove") {
                    group.has_move = true;
                    read_curve(tp, "PosTrack", group.pos);
                    read_curve(tp, "EulerTrack", group.euler);
                    if (prop_name(tp, "RotMode") == "IMR_LookAtGroup") group.look_at = prop_name(tp, "LookAtGroupName");
                } else if (cls == "InterpTrackFloatProp" && prop_name(tp, "PropertyName") == "FOVAngle") {
                    group.has_fov = true;
                    read_curve(tp, "FloatTrack", group.fov);
                } else if (cls == "InterpTrackDirector") {
                    if (const UProperty* cut = find_prop(tp, "CutTrack")) {
                        for (const auto& el : cut->elements) out.cuts.emplace_back(prop_float(el, "Time", 0.0f), prop_name(el, "TargetCamGroup"));
                    }
                } else if (cls == "InterpTrackEvent") {
                    if (const UProperty* ev = find_prop(tp, "EventTrack")) {
                        for (const auto& el : ev->elements) out.events.emplace_back(prop_float(el, "Time", 0.0f), prop_name(el, "EventName"));
                    }
                }
            }
        }
        out.groups.push_back(std::move(group));
    }
    return out.length > 0.0f;
}

}  // namespace

int KismetGraph::find_matinee(const std::string& name) const {
    for (size_t i = 0; i < matinees.size(); ++i) {
        if (matinees[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

bool KismetGraph::load(const UPKPackage& level, std::vector<std::string>& warnings) {
    nodes.clear();
    matinees.clear();
    actors.clear();
    material_names.clear();

    const auto& exports = level.get_exports();
    int32_t root = 0;
    for (size_t i = 0; i < exports.size(); ++i) {
        if (level.get_export_class(exports[i]) == "Sequence" && export_object_name(level, static_cast<int32_t>(i + 1)) == "Main_Sequence") {
            root = static_cast<int32_t>(i + 1);
            break;
        }
    }
    if (root <= 0) {
        warnings.push_back("the menu level has no Main_Sequence: no camera");
        return false;
    }

    std::unordered_map<int32_t, int> node_of;
    std::vector<int32_t> export_of;
    std::function<void(int32_t, int)> collect = [&](int32_t sequence_export, int sequence_node) {
        UPropertyList sp;
        parse_export_properties(level, sequence_export, sp);
        const UProperty* objects = find_prop(sp, "SequenceObjects");
        if (!objects) return;
        for (int32_t e : objects->ints) {
            if (e <= 0 || node_of.count(e)) continue;
            const std::string cls = object_class_name(level, e);
            if (cls.rfind("SequenceFrame", 0) == 0) continue;
            Node n;
            n.cls = cls;
            n.name = export_object_name(level, e);
            n.sequence = sequence_node;
            const int index = static_cast<int>(nodes.size());
            nodes.push_back(std::move(n));
            node_of[e] = index;
            export_of.push_back(e);
            if (cls == "Sequence") collect(e, index);
        }
    };
    collect(root, -1);

    auto node_for = [&](int32_t e) {
        auto it = node_of.find(e);
        return it == node_of.end() ? -1 : it->second;
    };
    auto object_name = [&](int32_t e) { return e > 0 ? export_object_name(level, e) : std::string(); };

    std::unordered_map<int, int> matinee_of_node;
    for (size_t ni = 0; ni < nodes.size(); ++ni) {
        Node& n = nodes[ni];
        UPropertyList props;
        parse_export_properties(level, export_of[ni], props);

        if (const UProperty* outs = find_prop(props, "OutputLinks")) {
            for (const auto& el : outs->elements) {
                Output o;
                o.desc = prop_string(el, "LinkDesc");
                o.delay = prop_float(el, "ActivateDelay", 0.0f);
                o.linked_op = node_for(prop_object(el, "LinkedOp"));
                if (const UProperty* links = find_prop(el, "Links")) {
                    for (const auto& l : links->elements) {
                        Link link;
                        link.op = node_for(prop_object(l, "LinkedOp"));
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
                in.linked_op = node_for(prop_object(el, "LinkedOp"));
                n.inputs.push_back(std::move(in));
            }
        }
        if (const UProperty* vars = find_prop(props, "VariableLinks")) {
            for (const auto& el : vars->elements) {
                VarLink v;
                v.desc = prop_string(el, "LinkDesc");
                if (const UProperty* linked = find_prop(el, "LinkedVariables")) {
                    for (int32_t e : linked->ints) {
                        const int var = node_for(e);
                        if (var >= 0) v.vars.push_back(var);
                    }
                }
                n.vars.push_back(std::move(v));
            }
        }

        const std::string& c = n.cls;
        if (c == "SeqEvent_RemoteEvent" || c == "SeqAct_ActivateRemoteEvent") {
            n.label = prop_name(props, "EventName");
            n.max_trigger = prop_int(props, "MaxTriggerCount", 1);
            n.retrigger = prop_float(props, "ReTriggerDelay", 0.0f);
        } else if (c == "SeqEvent_SequenceActivated") {
            n.label = prop_string(props, "InputLabel");
            n.max_trigger = prop_int(props, "MaxTriggerCount", 0);
        } else if (c == "SeqEvent_LevelLoaded") {
            n.max_trigger = prop_int(props, "MaxTriggerCount", 1);
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
            if (c == "SeqVar_Object") n.object = object_name(prop_object(props, "ObjValue"));
        } else if (c == "InterpData") {
            MatineeData m;
            if (load_matinee_data(level, export_of[ni], m)) {
                matinee_of_node[static_cast<int>(ni)] = static_cast<int>(matinees.size());
                matinees.push_back(std::move(m));
            }
        } else if (c == "SeqAct_Delay") {
            n.f = prop_float(props, "Duration", 1.0f);
        } else if (c == "SeqAct_TdFadeEffect") {
            n.f = prop_float(props, "FadeTime", 1.0f);
            n.b = prop_name(props, "FadeEffect") == "FadeOut";
        } else if (c == "SeqAct_SetMatInstScalarParam") {
            n.param = prop_name(props, "ParamName");
            n.f = prop_float(props, "ScalarValue", 0.0f);
            n.object = object_name(prop_object(props, "MatInst"));
        } else if (c == "SeqAct_SetInt") {
            n.i = prop_int(props, "Value", 0);
        } else if (c == "SeqAct_AddFloat" || c == "SeqAct_SubtractFloat" || c == "SeqCond_CompareFloat") {
            n.f = prop_float(props, "ValueA", 0.0f);
            n.value_b = prop_float(props, "ValueB", 0.0f);
        } else if (c == "SeqAct_Switch") {
            n.i = prop_int(props, "IncrementAmount", 1);
            n.looping = prop_bool(props, "bLooping", false);
        } else if (c == "SeqAct_Gate") {
            n.b = prop_bool(props, "bOpen", true);
        } else if (c == "SeqAct_Interp") {
            n.looping = prop_bool(props, "bLooping", false);
            n.rewind_on_play = prop_bool(props, "bRewindOnPlay", false);
            n.rewind_if_playing = prop_bool(props, "bRewindIfAlreadyPlaying", false);
        } else if (c == "SeqAct_LevelStreaming") {
            n.label = prop_name(props, "LevelName");
        }
        if (!n.object.empty() && n.object.rfind("MI_", 0) == 0 &&
            std::find(material_names.begin(), material_names.end(), n.object) == material_names.end()) {
            material_names.push_back(n.object);
        }
    }

    // A SeqAct_Interp plays the InterpData behind its "Data" link.
    for (Node& n : nodes) {
        if (n.cls != "SeqAct_Interp") continue;
        for (const VarLink& v : n.vars) {
            if (!same(v.desc, "Data") || v.vars.empty()) continue;
            auto it = matinee_of_node.find(v.vars.front());
            if (it != matinee_of_node.end()) n.matinee = it->second;
        }
        if (n.matinee < 0) warnings.push_back(n.name + " has no Matinee data");
    }

    for (size_t i = 0; i < exports.size(); ++i) {
        const std::string cls = level.get_export_class(exports[i]);
        if (cls != "CameraActor" && cls != "InterpActor") continue;
        UPropertyList props;
        parse_export_properties(level, static_cast<int32_t>(i + 1), props);
        KismetActor a;
        a.name = export_object_name(level, static_cast<int32_t>(i + 1));
        if (const UProperty* loc = find_prop(props, "Location")) a.pos = Vec3{loc->v[0], loc->v[1], loc->v[2]};
        if (const UProperty* rot = find_prop(props, "Rotation")) {
            const float to_deg = 360.0f / 65536.0f;
            a.euler = Vec3{static_cast<float>(rot->vi[2]) * to_deg, static_cast<float>(rot->vi[0]) * to_deg, static_cast<float>(rot->vi[1]) * to_deg};
        }
        a.fov = prop_float(props, "FOVAngle", 90.0f);
        actors.push_back(std::move(a));
    }
    return valid();
}

// --- running --------------------------------------------------------------------------------

void KismetRunner::init(const KismetGraph* graph) {
    graph_ = graph;
    state_.clear();
    actors_.clear();
    actor_index_.clear();
    material_params_.clear();
    active_.clear();
    sequence_nodes_.clear();
    sequence_slot_.clear();
    delayed_.clear();
    streamed_.clear();
    view_ = -1;
    fade_ = 0.0f;
    fade_rate_ = 0.0f;
    fade_node_ = -1;
    time_ = 0.0;
    tick_ = 0;
    if (!graph_) return;
    state_.resize(graph_->nodes.size());
    actors_ = graph_->actors;
    for (size_t i = 0; i < actors_.size(); ++i) actor_index_[actors_[i].name] = static_cast<int>(i);
    active_.emplace_back();
    sequence_nodes_.push_back(-1);
    for (size_t i = 0; i < graph_->nodes.size(); ++i) {
        const KismetGraph::Node& n = graph_->nodes[i];
        State& s = state_[i];
        s.f = n.f;
        s.i = n.i;
        s.b = n.b;
        s.open = n.b;
        if (n.cls == "Sequence") {
            sequence_slot_[static_cast<int>(i)] = static_cast<int>(active_.size());
            active_.emplace_back();
            sequence_nodes_.push_back(static_cast<int>(i));
        }
    }
}

void KismetRunner::begin_play() {
    if (!graph_) return;
    for (size_t i = 0; i < graph_->nodes.size(); ++i) {
        const KismetGraph::Node& n = graph_->nodes[i];
        if (n.cls == "SeqEvent_LevelLoaded" || (n.cls == "SeqEvent_SequenceActivated" && n.sequence < 0)) check_activate(static_cast<int>(i));
    }
}

void KismetRunner::fire_event(const std::string& name) {
    if (!graph_) return;
    for (size_t i = 0; i < graph_->nodes.size(); ++i) {
        const KismetGraph::Node& n = graph_->nodes[i];
        if (n.cls == "SeqEvent_RemoteEvent" && same(n.label, name.c_str())) check_activate(static_cast<int>(i));
    }
}

// USequenceEvent::CheckActivate, then USequence::QueueSequenceOp(this, false): an event goes to
// the bottom of the list, so it runs after the ops that were already ticking.
void KismetRunner::check_activate(int event) {
    const KismetGraph::Node& n = graph_->nodes[static_cast<size_t>(event)];
    State& s = state_[static_cast<size_t>(event)];
    if (n.max_trigger > 0 && s.trigger_count >= n.max_trigger) return;
    if (n.retrigger > 0.0f && time_ - s.last_trigger < static_cast<double>(n.retrigger)) return;
    ++s.trigger_count;
    s.last_trigger = time_;
    queue(event, false);
}

void KismetRunner::queue(int node, bool top) {
    const int seq = graph_->nodes[static_cast<size_t>(node)].sequence;
    std::vector<int>& list = active_[seq < 0 ? 0 : static_cast<size_t>(sequence_slot_.at(seq))];
    if (std::find(list.begin(), list.end(), node) != list.end()) return;
    if (top) list.push_back(node);
    else list.insert(list.begin(), node);
}

std::vector<std::string> KismetRunner::take_streamed_levels() {
    std::vector<std::string> out;
    out.swap(streamed_);
    return out;
}

float KismetRunner::material_param(const std::string& material, const std::string& param, float fallback) const {
    auto it = material_params_.find(material + "." + param);
    return it == material_params_.end() ? fallback : it->second;
}

std::string KismetRunner::playing() const {
    std::string out;
    if (!graph_ || active_.empty()) return out;
    const std::vector<int>& list = active_[0];
    for (size_t k = list.size(); k-- > 0;) {
        const KismetGraph::Node& n = graph_->nodes[static_cast<size_t>(list[k])];
        const State& s = state_[static_cast<size_t>(list[k])];
        if (n.cls != "SeqAct_Interp" || !s.playing || n.matinee < 0) continue;
        if (!out.empty()) out += " ";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "@%.2f", static_cast<double>(s.position));
        out += graph_->matinees[static_cast<size_t>(n.matinee)].name + buf;
    }
    return out;
}

int KismetRunner::resolve_var(int node, int var) const {
    (void)node;
    for (int guard = 0; guard < 8 && var >= 0; ++guard) {
        const KismetGraph::Node& v = graph_->nodes[static_cast<size_t>(var)];
        if (v.cls == "SeqVar_Named") {
            int found = -1;
            for (size_t i = 0; i < graph_->nodes.size(); ++i) {
                const KismetGraph::Node& c = graph_->nodes[i];
                if (c.cls.rfind("SeqVar_", 0) != 0 || c.cls == "SeqVar_Named" || c.cls == "SeqVar_External") continue;
                if (!c.label.empty() && same(c.label, v.label.c_str())) {
                    found = static_cast<int>(i);
                    break;
                }
            }
            var = found;
        } else if (v.cls == "SeqVar_External") {
            int found = -1;
            if (v.sequence >= 0) {
                const KismetGraph::Node& seq = graph_->nodes[static_cast<size_t>(v.sequence)];
                for (const KismetGraph::VarLink& l : seq.vars) {
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

int KismetRunner::linked_var(int node, const char* desc) const {
    const KismetGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    for (const KismetGraph::VarLink& l : n.vars) {
        if (same(l.desc, desc) && !l.vars.empty()) return resolve_var(node, l.vars.front());
    }
    return -1;
}

float KismetRunner::read_float(int node, const char* desc, float fallback) const {
    const int v = linked_var(node, desc);
    return v >= 0 ? state_[static_cast<size_t>(v)].f : fallback;
}

void KismetRunner::fire_output(int node, const char* desc) {
    const KismetGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    for (size_t o = 0; o < n.outputs.size() && o < 32; ++o) {
        if (same(n.outputs[o].desc, desc)) state_[static_cast<size_t>(node)].out |= 1u << o;
    }
}

void KismetRunner::update(float dt) {
    if (!graph_) return;
    ++tick_;
    time_ += static_cast<double>(dt);

    for (size_t k = 0; k < delayed_.size();) {
        delayed_[k].remaining -= dt;
        if (delayed_[k].remaining <= 0.0f) {
            queue(delayed_[k].op, true);
            state_[static_cast<size_t>(delayed_[k].op)].impulses |= 1u << delayed_[k].input;
            delayed_.erase(delayed_.begin() + static_cast<std::ptrdiff_t>(k));
        } else {
            ++k;
        }
    }
    for (size_t slot = 0; slot < active_.size(); ++slot) run_sequence(static_cast<int>(slot), dt);
}

// USequence::ExecuteActiveOps: ops are popped off the end of the list; what an op activates is
// pushed so that its links run in link order, depth first, before whatever was already waiting.
void KismetRunner::run_sequence(int slot, float dt) {
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

        const KismetGraph::Node& n = graph_->nodes[static_cast<size_t>(op)];
        std::vector<KismetGraph::Link> pending;
        for (size_t o = 0; o < n.outputs.size() && o < 32; ++o) {
            if (!(s.out & (1u << o))) continue;
            for (const KismetGraph::Link& link : n.outputs[o].links) {
                if (n.outputs[o].delay > 0.0f) delayed_.push_back(Delayed{link.op, link.input, n.outputs[o].delay});
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

int KismetRunner::actor_for_group(int node, const std::string& group) const {
    const int v = linked_var(node, group.c_str());
    if (v < 0) return -1;
    auto it = actor_index_.find(graph_->nodes[static_cast<size_t>(v)].object);
    return it == actor_index_.end() ? -1 : it->second;
}

// USeqAct_Interp::Play.
void KismetRunner::interp_play(int node) {
    const KismetGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    State& s = state_[static_cast<size_t>(node)];
    if (n.rewind_on_play && (!s.playing || n.rewind_if_playing)) interp_update(node, 0.0f, true, 0.0f);
    s.playing = true;
}

// USeqAct_Interp::StepInterp.
void KismetRunner::interp_step(int node, float dt) {
    const KismetGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    State& s = state_[static_cast<size_t>(node)];
    if (!s.playing || n.matinee < 0) return;
    const float length = graph_->matinees[static_cast<size_t>(n.matinee)].length;
    float next = s.position + dt;
    bool stop = false;
    if (next > length) {
        if (n.looping) {
            interp_update(node, length, false, s.position);
            interp_update(node, 0.0f, true, 0.0f);
            while (next > length) next -= length;
        } else {
            next = length;
            stop = true;
        }
    }
    interp_update(node, next, false, s.position);
    if (stop) s.playing = false;
}

// USeqAct_Interp::UpdateInterp: every group's tracks at `position`.
void KismetRunner::interp_update(int node, float position, bool jump, float previous) {
    const KismetGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    State& s = state_[static_cast<size_t>(node)];
    s.position = position;
    if (n.matinee < 0) return;
    const MatineeData& m = graph_->matinees[static_cast<size_t>(n.matinee)];

    if (!jump) {
        for (const auto& [when, name] : m.events) {
            if (when > previous && when <= position) fire_output(node, name.c_str());
        }
    }
    for (const MatineeGroup& g : m.groups) {
        const int ai = actor_for_group(node, g.name);
        if (ai < 0) continue;
        KismetActor& actor = actors_[static_cast<size_t>(ai)];
        if (g.has_move) {
            const Vec3 pos = g.pos.eval(position, actor.pos);
            Vec3 euler = g.euler.eval(position, actor.euler);
            // UInterpTrackMove::GetLocationAtTime: the keyed rotation stands unless the look-at
            // group has an actor, and the direction is taken from where this actor is now, before
            // this update moves it.
            if (!g.look_at.empty()) {
                bool has_group = false;
                for (const MatineeGroup& other : m.groups) has_group = has_group || other.name == g.look_at;
                const int target = has_group ? actor_for_group(node, g.look_at) : -1;
                if (target >= 0) {
                    const Vec3 d = actors_[static_cast<size_t>(target)].pos - actor.pos;
                    const float flat = std::sqrt(d.x * d.x + d.y * d.y);
                    if (flat > 1.0e-6f || std::fabs(d.z) > 1.0e-6f) {
                        euler = Vec3{0.0f, std::atan2(d.z, flat) * kRadToDeg, std::atan2(d.y, d.x) * kRadToDeg};
                    }
                }
            }
            actor.pos = pos;
            actor.euler = euler;
        }
        if (g.has_fov) actor.fov = g.fov.eval(position, Vec3{actor.fov, 0.0f, 0.0f}).x;
    }
    // InterpTrackDirector: the player looks through the group the last cut names.
    const std::string* cut = nullptr;
    for (const auto& [when, group] : m.cuts) {
        if (when <= position || !cut) cut = &group;
    }
    if (cut) {
        const int ai = actor_for_group(node, *cut);
        if (ai >= 0) view_ = ai;
    }
}

// Activated() and UpdateOp() of one op. True when it has finished (it is not latent, or is done).
bool KismetRunner::step_op(int node, float dt, bool newly) {
    const KismetGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    State& s = state_[static_cast<size_t>(node)];
    const std::string& c = n.cls;
    const uint32_t all_outputs = n.outputs.size() >= 32 ? 0xFFFFFFFFu : ((1u << n.outputs.size()) - 1u);

    if (c == "SeqAct_Interp") {
        if (newly && (s.impulses & 0x13u)) {
            s.initialised = true;
            if (s.impulses & 1u) interp_play(node);
        }
        if (s.impulses & 1u) interp_play(node);
        else if (s.impulses & 4u) s.playing = false;
        else if (s.impulses & 8u) s.playing = false;
        interp_step(node, dt);
        return !s.playing;
    }
    if (c == "SeqAct_Delay") {
        if (s.impulses & 2u) {
            s.delay_running = false;
            fire_output(node, "Aborted");
            return true;
        }
        if (s.impulses & 1u) {
            if (!s.delay_running) {
                s.delay_running = true;
                s.remaining = n.f;
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
            const float t = n.f > 1.0e-4f ? n.f : 1.0e-4f;
            if (n.b) {
                fade_rate_ = 1.0f / t;
            } else {
                fade_ = 1.0f;
                fade_rate_ = -1.0f / t;
            }
            fade_node_ = node;
        }
        if (fade_node_ != node) return true;  // a later fade took over
        fade_ += fade_rate_ * dt;
        if ((fade_rate_ > 0.0f && fade_ >= 1.0f) || (fade_rate_ < 0.0f && fade_ <= 0.0f)) {
            fade_ = fade_rate_ > 0.0f ? 1.0f : 0.0f;
            fade_rate_ = 0.0f;
            fade_node_ = -1;
            s.out = all_outputs;
            return true;
        }
        return false;
    }

    // Everything else finishes in the step it is activated.
    if (c.rfind("SeqEvent_", 0) == 0) {
        s.out = all_outputs;
    } else if (c == "Sequence") {
        for (size_t i = 0; i < n.inputs.size() && i < 32; ++i) {
            if ((s.impulses & (1u << i)) && n.inputs[i].linked_op >= 0) check_activate(n.inputs[i].linked_op);
        }
    } else if (c == "SeqAct_FinishSequence") {
        if (n.sequence >= 0) {
            const KismetGraph::Node& seq = graph_->nodes[static_cast<size_t>(n.sequence)];
            for (const KismetGraph::Output& o : seq.outputs) {
                if (o.linked_op != node) continue;
                for (size_t k = o.links.size(); k-- > 0;) {
                    queue(o.links[k].op, true);
                    state_[static_cast<size_t>(o.links[k].op)].impulses |= 1u << (o.links[k].input & 31);
                }
            }
        }
    } else if (c == "SeqAct_Switch") {
        if (s.index >= 1 && static_cast<size_t>(s.index) <= n.outputs.size()) s.out |= 1u << (s.index - 1);
        s.index += n.i;
        if (n.looping && static_cast<size_t>(s.index) > n.outputs.size()) s.index = 1;
    } else if (c == "SeqAct_SetBool") {
        const int target = linked_var(node, "Target");
        const int value = linked_var(node, "Value");
        if (target >= 0) state_[static_cast<size_t>(target)].b = value >= 0 ? state_[static_cast<size_t>(value)].b : false;
        s.out = all_outputs;
    } else if (c == "SeqAct_SetInt") {
        const int target = linked_var(node, "Target");
        const int value = linked_var(node, "Value");
        if (target >= 0) state_[static_cast<size_t>(target)].i = value >= 0 ? state_[static_cast<size_t>(value)].i : n.i;
        s.out = all_outputs;
    } else if (c == "SeqCond_CompareBool") {
        const int v = linked_var(node, "Bool");
        fire_output(node, (v >= 0 && state_[static_cast<size_t>(v)].b) ? "True" : "False");
    } else if (c == "SeqCond_CompareFloat") {
        const float a = read_float(node, "A", n.f);
        const float b = read_float(node, "B", n.value_b);
        if (a <= b) fire_output(node, "A <= B");
        if (a > b) fire_output(node, "A > B");
        if (a == b) fire_output(node, "A == B");
        if (a < b) fire_output(node, "A < B");
        if (a >= b) fire_output(node, "A >= B");
    } else if (c == "SeqCond_TdCaseInt") {
        const int in = linked_var(node, "Int");
        const int va = linked_var(node, "A");
        const int vb = linked_var(node, "B");
        const int input = in >= 0 ? state_[static_cast<size_t>(in)].i : 0;
        if (va >= 0 && input == state_[static_cast<size_t>(va)].i) s.out |= 1u;
        else if (vb >= 0 && input == state_[static_cast<size_t>(vb)].i) s.out |= 2u;
        else s.out |= 4u;
        s.out |= 8u;
        s.out &= all_outputs;
    } else if (c == "SeqAct_AddFloat" || c == "SeqAct_SubtractFloat") {
        const float a = read_float(node, "A", n.f);
        const float b = read_float(node, "B", n.value_b);
        const float r = c == "SeqAct_AddFloat" ? a + b : a - b;
        for (const KismetGraph::VarLink& l : n.vars) {
            if (!same(l.desc, "FloatResult")) continue;
            for (int v : l.vars) {
                const int rv = resolve_var(node, v);
                if (rv >= 0) state_[static_cast<size_t>(rv)].f = r;
            }
        }
        s.out = all_outputs;
    } else if (c == "SeqAct_SetMatInstScalarParam") {
        const int mv = linked_var(node, "MatInst");
        const std::string& material = mv >= 0 ? graph_->nodes[static_cast<size_t>(mv)].object : n.object;
        if (!material.empty()) material_params_[material + "." + n.param] = read_float(node, "ScalarValue", n.f);
        s.out = all_outputs;
    } else if (c == "SeqAct_Gate") {
        if (s.impulses & 2u) s.open = true;
        else if (s.impulses & 4u) s.open = false;
        else if (s.impulses & 8u) s.open = !s.open;
        if (s.open && (s.impulses & 1u)) s.out = all_outputs;
    } else if (c == "SeqAct_ActivateRemoteEvent") {
        fire_event(n.label);
        s.out = all_outputs;
    } else if (c == "SeqAct_LevelStreaming") {
        if (s.impulses & 1u) streamed_.push_back(n.label);
        s.out = all_outputs;
    } else {
        // SeqAct_ToggleCinematicMode, UIAction_OpenScene, SeqAct_CrossFadeMusicTracks: nothing to do here.
        s.out = all_outputs;
    }
    (void)dt;
    return true;
}

// USeqAct_Interp::DeActivated: "Completed" if it stopped at the end, "Reversed" if at the start,
// nothing if it was stopped part way. An op that never initialised has no data and fires nothing.
void KismetRunner::deactivated(int node) {
    const KismetGraph::Node& n = graph_->nodes[static_cast<size_t>(node)];
    if (n.cls != "SeqAct_Interp") return;
    State& s = state_[static_cast<size_t>(node)];
    if (!s.initialised || n.matinee < 0) return;
    const float length = graph_->matinees[static_cast<size_t>(n.matinee)].length;
    if (s.position < kSmall) {
        if (n.outputs.size() > 1) s.out |= 2u;
    } else if (s.position > length - kSmall) {
        if (!n.outputs.empty()) s.out |= 1u;
    }
    s.initialised = false;
}

}  // namespace me::fe
