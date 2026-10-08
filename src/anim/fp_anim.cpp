#include "fp_anim.hpp"

#include "anim_system.hpp"
#include "../assets/ue3_props.hpp"
#include "../assets/upk_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace me::fp {

namespace {

// UE3's ZERO_ANIMWEIGHT_THRESH: a node below this is not relevant and is not ticked.
constexpr float kZeroWeight = 0.00001f;

float bits_to_float(int32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

std::vector<float> float_array(const UProperty* p) {
    std::vector<float> out;
    if (p) {
        for (int32_t v : p->ints) out.push_back(bits_to_float(v));
    }
    return out;
}

// The NodeName of each slot, by CustomNodeType.
const char* const kSlotNames[static_cast<size_t>(Slot::Count)] = {
    "Custom_Canned", "Custom_CannedUpperBody", "Custom_FullBody", "Custom_FullBody_Dir", "Custom_UpperBody",
    "Custom_LowerBody", "Custom_Camera", "Custom_Weapon", "Custom_Face"};

TreeNode::Kind kind_of(const std::string& cls) {
    if (cls == "TdAnimNodeSequence" || cls == "AnimNodeSequence") return TreeNode::Kind::Sequence;
    if (cls == "TdAnimNodeSlot" || cls == "AnimNodeSlot") return TreeNode::Kind::Slot;
    if (cls == "AnimNodeBlendPerBone") return TreeNode::Kind::PerBone;
    if (cls == "TdAnimNodeBlendDirectional") return TreeNode::Kind::Directional;
    if (cls == "AnimNodeCrossfader") return TreeNode::Kind::Fixed;
    // The pose-offset nodes have one input and only move bones.
    if (cls == "AnimTree" || cls == "AnimNodeSynch" || cls == "TdAnimNodeAimOffset" || cls == "TdAnimNodeDirBone" || cls == "TdAnimNodeLandOffset" ||
        cls == "TdAnimNodeWeaponPoseOffset" || cls == "TdAnimNodeIKEffectorController" || cls == "TdAnimNodePoseOffset") {
        return TreeNode::Kind::Passthrough;
    }
    return TreeNode::Kind::List;
}

}  // namespace

bool AnimTree::load(const std::string& game_root, std::string& error) {
    nodes_.clear();
    order_.clear();
    walk_group_.clear();
    weapon_pose_profiles_.clear();
    root_ = -1;
    walk_synch_ = -1;
    std::fill(std::begin(slots_), std::end(slots_), -1);

    UPKPackage pkg(game_root + "/TdGame/CookedPC/Characters/AT_C1P.upk");
    if (!pkg.is_valid()) {
        error = "Characters/AT_C1P.upk not found";
        return false;
    }
    const auto& exports = pkg.get_exports();
    int32_t root_export = 0;
    for (size_t i = 0; i < exports.size(); ++i) {
        if (pkg.get_export_class(exports[i]) == "AnimTree") root_export = static_cast<int32_t>(i + 1);
    }
    if (root_export == 0) {
        error = "AT_C1P.upk has no AnimTree";
        return false;
    }

    // Every node reachable from the root, each once: a node can hang under several parents.
    std::unordered_map<int32_t, int> node_of;
    std::vector<int32_t> pending{root_export};
    std::vector<std::vector<int32_t>> child_exports;
    while (!pending.empty()) {
        const int32_t e = pending.back();
        pending.pop_back();
        if (node_of.count(e)) continue;
        const int index = static_cast<int>(nodes_.size());
        node_of[e] = index;
        nodes_.emplace_back();
        child_exports.emplace_back();
        TreeNode& n = nodes_.back();
        n.cls = pkg.get_export_class(exports[static_cast<size_t>(e - 1)]);
        n.kind = kind_of(n.cls);

        UPropertyList props;
        parse_export_properties(pkg, e, props);
        n.name = prop_name(props, "NodeName");
        if (n.name == "None") n.name.clear();
        if (const UProperty* children = find_prop(props, "Children")) {
            for (const auto& child : children->elements) {
                n.child_names.push_back(prop_name(child, "Name"));
                const int32_t anim = prop_object(child, "Anim");
                child_exports.back().push_back(anim);
                n.weight.push_back(prop_float(child, "Weight", 0.0f));
                if (anim > 0) pending.push_back(anim);
            }
        }
        if (n.kind == TreeNode::Kind::Sequence) {
            n.seq_name = prop_name(props, "AnimSeqName");
            if (n.seq_name == "None") n.seq_name.clear();
            n.rate = prop_float(props, "Rate", 1.0f);
            n.looping = prop_bool(props, "bLooping", false);
            n.playing = prop_bool(props, "bPlaying", false);
            // Class defaults of TdAnimNodeSequence: BaseSpeed 350, RateMax 2, bResetOnBecomeRelevant.
            n.base_speed = prop_float(props, "BaseSpeed", 350.0f);
            n.rate_min = prop_float(props, "RateMin", 0.0f);
            n.rate_max = prop_float(props, "RateMax", 2.0f);
            n.scale_rate_by_speed = prop_bool(props, "ScalePlayRateBySpeed", false);
            const std::string group = prop_name(props, "SynchGroupName");
            n.synchronize = !group.empty() && group != "None" && prop_bool(props, "bSynchronize", true);
            n.synch_offset = prop_float(props, "SynchPosOffset", 0.0f);
            n.reset_on_relevant = prop_bool(props, "bResetOnBecomeRelevant", true);
            n.start_position = prop_float(props, "NormalizedStartPosition", 0.0f);
            if (group == "Walk") walk_group_.push_back(index);
        }
        if (const UProperty* mapping = find_prop(props, "StateMapping")) n.state_mapping.assign(mapping->ints.begin(), mapping->ints.end());
        n.blend_in = float_array(find_prop(props, "BlendWeight"));
        n.blend_out = float_array(find_prop(props, "BlendOutWeight"));
        n.use_old_state = prop_bool(props, "bUseOldState", false);
        n.child2_weight = prop_float(props, "Child2Weight", 0.0f);
        n.bone_weight = float_array(find_prop(props, "Child2PerBoneWeight"));
        n.aim_from_legs = prop_bool(props, "bAimSourceIsLegRotation", false);
        if (n.cls == "TdAnimNodeWeaponPoseOffset") {
            if (const UProperty* profiles = find_prop(props, "Profiles")) {
                for (const auto& profile : profiles->elements) {
                    std::vector<int> bones;
                    if (const UProperty* indices = find_prop(profile, "BoneIndices")) bones.assign(indices->ints.begin(), indices->ints.end());
                    weapon_pose_profiles_.emplace_back(prop_name(profile, "Name"), std::move(bones));
                }
            }
        }
        if (const UProperty* profiles = find_prop(props, "Profiles"); profiles && !profiles->elements.empty()) {
            if (const UProperty* range = find_prop(profiles->elements[0], "HorizontalRange")) {
                if (range->v[0] != 0.0f) n.aim_range_neg = std::fabs(range->v[0]);
                if (range->v[1] != 0.0f) n.aim_range_pos = std::fabs(range->v[1]);
            }
            if (const UProperty* comps = find_prop(profiles->elements[0], "AimComponents")) {
                static const char* const kDirs[9] = {"LU", "LC", "LD", "CU", "CC", "CD", "RU", "RC", "RD"};
                for (const auto& comp : comps->elements) {
                    TreeNode::AimBone a;
                    a.bone = prop_name(comp, "BoneName");
                    for (int d = 0; d < 9; ++d) {
                        a.rot[d][0] = a.rot[d][1] = a.rot[d][2] = 0.0f;
                        a.rot[d][3] = 1.0f;
                        a.trans[d][0] = a.trans[d][1] = a.trans[d][2] = 0.0f;
                        const UProperty* t = find_prop(comp, kDirs[d]);
                        if (!t) continue;
                        if (const UProperty* q = find_prop(t->fields, "Quaternion")) std::memcpy(a.rot[d], q->v, sizeof a.rot[d]);
                        if (const UProperty* tr = find_prop(t->fields, "Translation")) std::memcpy(a.trans[d], tr->v, sizeof a.trans[d]);
                    }
                    n.aim.push_back(a);
                }
            }
        }
        n.active = prop_int(props, "ActiveChildIndex", 0);
    }
    for (size_t i = 0; i < nodes_.size(); ++i) {
        TreeNode& n = nodes_[i];
        for (int32_t e : child_exports[i]) {
            auto it = node_of.find(e);
            n.children.push_back(it == node_of.end() ? -1 : it->second);
        }
        n.target = n.weight;
        for (size_t s = 0; s < static_cast<size_t>(Slot::Count); ++s) {
            if (n.kind == TreeNode::Kind::Slot && n.name == kSlotNames[s]) slots_[s] = static_cast<int>(i);
        }
    }
    root_ = node_of[root_export];
    // The retail recorder's list leaves out what plays on the Camera and Canned slots.
    for (Slot hidden : {Slot::Camera, Slot::Canned}) {
        const int index = slots_[static_cast<size_t>(hidden)];
        if (index < 0) continue;
        const TreeNode& slot = nodes_[static_cast<size_t>(index)];
        for (size_t c = 1; c < slot.children.size(); ++c) {
            if (slot.children[c] >= 0) nodes_[static_cast<size_t>(slot.children[c])].unlisted = true;
        }
    }
    in_walk_group_.assign(nodes_.size(), 0);
    for (int i : walk_group_) in_walk_group_[static_cast<size_t>(i)] = 1;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].cls == "AnimNodeSynch" && nodes_[i].name == "MasterSync") walk_synch_ = static_cast<int>(i);
    }

    // Parents before children: a node is ticked once, after everything above it.
    std::vector<int> parents_left(nodes_.size(), 0);
    for (const TreeNode& n : nodes_) {
        for (int c : n.children) {
            if (c >= 0) ++parents_left[static_cast<size_t>(c)];
        }
    }
    std::vector<int> ready{root_};
    while (!ready.empty()) {
        const int i = ready.back();
        ready.pop_back();
        order_.push_back(i);
        for (int c : nodes_[static_cast<size_t>(i)].children) {
            if (c >= 0 && --parents_left[static_cast<size_t>(c)] == 0) ready.push_back(c);
        }
    }
    reset();
    return true;
}

void AnimTree::reset() {
    for (TreeNode& n : nodes_) {
        n.total = n.incoming = 0.0f;
        n.relevant = false;
        n.blend_to_go = 0.0f;
        n.pending_blend_out = -1.0f;
        n.hold = -1.0f;
        n.last_state = -1;
        n.dir_side = 0.0f;
        n.dir_forward = 1.0f;
        n.going_forward = true;
        n.time = 0.0f;
        n.seq = nullptr;
        if (n.kind == TreeNode::Kind::List || n.kind == TreeNode::Kind::Slot) {
            n.active = 0;
            for (size_t c = 0; c < n.weight.size(); ++c) n.weight[c] = n.target[c] = c == 0 ? 1.0f : 0.0f;
        }
    }
    walk_master_ = -1;
    land_amount_ = 0.0f;
    land_time_ = -1.0f;
    leg_yaw_set_ = false;
    turning_ = false;
}

namespace {
float wrap180(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}
}  // namespace

// TdAnimNodeTurn: Default, Turn Left 45, Turn Left 90, Turn Right 45, Turn Right 90. Measured on
// the recordings (76 turns; the rule makes 27 of the 30 that follow another, on the right side and
// within three frames): standing, the legs stay where they are while the view turns, never more
// than a quarter turn behind it; once it is more than 65 degrees off them the quarter-turn step of
// that side plays, bringing the legs round 90 degrees over its 0.8 s whatever the view does
// meanwhile, and blending in and out over 0.2 s. The 45 degree steps are never chosen.
void AnimTree::tick_turn(TreeNode& n, const PawnAnimState& pawn, float dt, bool became_relevant) {
    if (n.weight.size() < 5) return;
    const float view = pawn.yaw_deg;
    float d = wrap180(view - leg_yaw_);
    if (std::fabs(d) > 90.0f) {
        leg_yaw_ = view - (d > 0.0f ? 90.0f : -90.0f);
        d = d > 0.0f ? 90.0f : -90.0f;
    }
    if (became_relevant && !turning_) set_active(n, 0, 0.0f);
    if (turning_) {
        leg_yaw_ += turn_side_ * (90.0f / 0.8f) * dt;
        turn_time_ += dt;
        if (turn_time_ >= 0.8f) {
            turning_ = false;
            set_active(n, 0, 0.2f);
        }
    } else if (std::fabs(d) > 65.0f) {
        turn_side_ = d > 0.0f ? 1.0f : -1.0f;
        const int child = d > 0.0f ? 4 : 2;
        if (n.children[static_cast<size_t>(child)] >= 0) {
            TreeNode& seq = nodes_[static_cast<size_t>(n.children[static_cast<size_t>(child)])];
            seq.time = 0.0f;
            seq.playing = true;
        }
        set_active(n, child, 0.2f);
        turning_ = true;
        turn_time_ = dt;
        leg_yaw_ += turn_side_ * (90.0f / 0.8f) * dt;
    }
}

void AnimTree::set_landed(float amount) {
    land_amount_ = amount;
    land_time_ = 0.0f;
}

void AnimTree::resolve(TreeNode& n) const {
    n.seq_set = nullptr;
    n.seq = (lookup_ && !n.seq_name.empty()) ? lookup_(n.seq_name, &n.seq_set) : nullptr;
}

void AnimTree::invalidate_sequences() {
    for (TreeNode& n : nodes_) {
        n.seq = nullptr;
        n.seq_set = nullptr;
    }
}

int AnimTree::slot_node(Slot slot) const { return slots_[static_cast<size_t>(slot)]; }

// AnimNodeBlendList.SetActiveChild.
void AnimTree::set_active(TreeNode& n, int child, float blend_time) {
    if (child < 0 || static_cast<size_t>(child) >= n.weight.size()) child = 0;
    for (size_t c = 0; c < n.target.size(); ++c) n.target[c] = static_cast<int>(c) == child ? 1.0f : 0.0f;
    n.active = child;
    // A slot's time is for a whole blend, and a channel that already has weight has that much less
    // to go. Measured: StopCustomAnim(FullBody_Dir, 0.3) on an animation 0.56 of the way in took
    // 0.167 s. The state nodes do not shorten theirs (scaling them too loses 0.3% of the
    // walking frames).
    if (blend_time > 0.0f && n.kind == TreeNode::Kind::Slot) blend_time *= 1.0f - std::clamp(n.weight[static_cast<size_t>(child)], 0.0f, 1.0f);
    n.blend_to_go = blend_time;
    if (blend_time <= 0.0f) n.weight = n.target;
}

void AnimTree::play_custom_anim(Slot slot, const std::string& name, float rate, float blend_in, float blend_out, bool looping,
                                bool override_playing, bool root_motion) {
    const int index = slot_node(slot);
    if (index < 0) return;
    TreeNode& n = nodes_[static_cast<size_t>(index)];
    if (n.children.size() < 3) return;
    if (!override_playing && n.active > 0) {
        const TreeNode& playing = nodes_[static_cast<size_t>(n.children[static_cast<size_t>(n.active)])];
        if (playing.playing && playing.seq_name == name) return;
    }
    // The channel that is not the one playing, so two custom animations can cross-fade.
    const int channel = n.active == 1 ? 2 : 1;
    TreeNode& seq = nodes_[static_cast<size_t>(n.children[static_cast<size_t>(channel)])];
    seq.seq_name = name;
    resolve(seq);
    seq.rate = rate;
    seq.looping = looping;
    seq.playing = true;
    // TdAnimNodeSequence.OnBecomeRelevant: one played backwards starts at its end.
    seq.time = (rate < 0.0f && seq.seq) ? seq.seq->length : 0.0f;
    seq.root_motion = root_motion;
    set_active(n, channel, blend_in);
    // With no time to blend out in, it holds its last frame until something else takes the slot.
    n.pending_blend_out = (looping || blend_out <= 0.0f) ? -1.0f : blend_out;
}

void AnimTree::stop_custom_anim(Slot slot, float blend_out) {
    const int index = slot_node(slot);
    if (index < 0) return;
    TreeNode& n = nodes_[static_cast<size_t>(index)];
    if (n.active == 0) return;
    set_active(n, 0, blend_out);
    n.pending_blend_out = -1.0f;
}

bool AnimTree::custom_anim_playing(Slot slot, std::string* name) const {
    const int index = slot_node(slot);
    if (index < 0) return false;
    const TreeNode& n = nodes_[static_cast<size_t>(index)];
    if (n.active == 0 || n.children.size() < 3) return false;
    const TreeNode& seq = nodes_[static_cast<size_t>(n.children[static_cast<size_t>(n.active)])];
    if (name) *name = seq.seq_name;
    return seq.playing;
}

float AnimTree::custom_anim_time(Slot slot) const {
    const int index = slot_node(slot);
    if (index < 0) return 0.0f;
    const TreeNode& n = nodes_[static_cast<size_t>(index)];
    if (n.active == 0 || n.children.size() < 3) return 0.0f;
    return nodes_[static_cast<size_t>(n.children[static_cast<size_t>(n.active)])].time;
}

void AnimTree::activate_custom_blend(const std::string& node_name, float amount, float duration, float blend_in, float blend_out) {
    for (TreeNode& n : nodes_) {
        if (n.cls != "TdAnimNodeCustomBlend" || n.name != node_name || n.weight.size() < 2) continue;
        n.target[0] = 1.0f - amount;
        n.target[1] = amount;
        n.blend_to_go = blend_in;
        n.active = 1;
        n.hold = duration;
        n.hold_blend_out = blend_out;
    }
}

float AnimTree::walk_cycle() const {
    if (walk_master_ < 0) return -1.0f;
    const TreeNode& m = nodes_[static_cast<size_t>(walk_master_)];
    return (m.seq && m.seq->length > 0.0f) ? m.time / m.seq->length : -1.0f;
}

// AnimNodeSynch.TickAnim for the "Walk" group: the heaviest member leads at its own rate, and
// every other member is put at the same place in its own cycle (its SynchPosOffset apart), so a
// walk and a run of different lengths stay in step and blending between them keeps the feet.
void AnimTree::tick_walk_group(const PawnAnimState& pawn, float dt) {
    int master = walk_master_;
    float best = master >= 0 ? nodes_[static_cast<size_t>(master)].total : 0.0f;
    for (int i : walk_group_) {
        if (nodes_[static_cast<size_t>(i)].total > best) {
            best = nodes_[static_cast<size_t>(i)].total;
            master = i;
        }
    }
    walk_master_ = master;
    if (master < 0) return;
    TreeNode& m = nodes_[static_cast<size_t>(master)];
    if (!m.seq && !m.seq_name.empty()) resolve(m);
    if (!m.seq || m.seq->length <= 0.0f) return;
    advance(m, pawn, dt);
    float rel = m.time / m.seq->length - m.synch_offset;
    rel -= std::floor(rel);
    for (int i : walk_group_) {
        if (i == master) continue;
        TreeNode& slave = nodes_[static_cast<size_t>(i)];
        if (!slave.seq && !slave.seq_name.empty()) resolve(slave);
        if (!slave.seq || slave.seq->length <= 0.0f) continue;
        float at = rel + slave.synch_offset;
        at -= std::floor(at);
        slave.time = at * slave.seq->length;
    }
}

namespace {
// TdMove.AnimBlendTime of the move a movement state belongs to (Default__TdMove_*; 0 where the class leaves it).
float move_blend_time(int state) {
    switch (static_cast<EMovement>(state)) {
        case EMovement::MOVE_Slide:
        case EMovement::MOVE_RumpSlide:
            return 0.5f;
        case EMovement::MOVE_Balance:
            return 0.4f;
        case EMovement::MOVE_Crouch:
            return 0.25f;
        case EMovement::MOVE_Falling:
        case EMovement::MOVE_WallRunningLeft:
        case EMovement::MOVE_WallRunningRight:
            return 0.2f;
        case EMovement::MOVE_Swing:
            return 0.15f;
        case EMovement::MOVE_Grabbing:
            return 0.1f;
        default:
            return 0.0f;
    }
}
}  // namespace

// A state node's choice of child for this frame.
void AnimTree::update_list(TreeNode& n, const PawnAnimState& pawn, bool became_relevant) {
    int want = n.active;
    float blend = 0.2f;
    if (n.cls == "TdAnimNodeCustomBlend") {
        // Moved only by Activate and its timer. TdAnimNodeCustomBlend.OnBecomeRelevant: with no
        // Duration left it is back on its first child at once.
        if (became_relevant && n.hold <= 0.0f) set_active(n, 0, 0.0f);
        return;
    }
    if (n.cls == "TdAnimNodeMovementState") {
        // The state SetAnimationMovementState forces, else the pawn's own (its last one for bUseOldState).
        int state = static_cast<int>(n.use_old_state ? pawn.old_movement : pawn.movement);
        if (!n.use_old_state && pawn.animation_movement != EMovement::MOVE_None) state = static_cast<int>(pawn.animation_movement);
        want = 0;
        for (size_t k = 0; k < n.state_mapping.size(); ++k) {
            if (n.state_mapping[k] == state) want = static_cast<int>(k) + 1;
        }
        // bUseCustomBlend: the time is GetBlendValue(new state, previous state), which is native,
        // not BlendWeight (whose 0.5 would fall on the right wall run and not on the left).
        // Measured on the recordings: 0.4 s on to a beam (TdMove_Balance.AnimBlendTime) and about
        // that off it, 0.2 into a jump, a fall or either wall run and back to the ground. So: the
        // longer of the two moves' TdMove.AnimBlendTime, and never under 0.2 s. (Twice the first
        // blend after a slide, AnimBlendTime 0.5, took 0.5 s with the slide two moves back and the
        // node without weight all through it. That is not reproduced: the previous state here is
        // the one the node last blended to.)
        if (became_relevant) {
            set_active(n, want, 0.0f);  // TdAnimNodeState.OnBecomeRelevant
        } else if (want != n.active) {
            set_active(n, want, std::max({0.2f, move_blend_time(state), move_blend_time(n.last_state)}));
            n.last_state = state;
        }
        return;
    } else if (n.cls == "TdAnimNodeWalkingState") {
        want = 0;
        for (size_t k = 0; k < n.state_mapping.size(); ++k) {
            if (n.state_mapping[k] == static_cast<int>(pawn.walking_state)) want = static_cast<int>(k) + 1;
        }
    } else if (n.cls == "TdAnimNodeWeaponState") {
        // Default (ready, reloading, throwing: the weapon up), then Relaxed, Unarmed, HeavyArmed by
        // TdPawn.WeaponAnimState.
        want = 0;
        for (size_t k = 0; k < n.state_mapping.size(); ++k) {
            if (n.state_mapping[k] == pawn.weapon_state) want = static_cast<int>(k) + 1;
        }
    } else if (n.cls == "TdAnimNodeWeaponTypeState") {
        // Default, then "Heavy".
        want = (pawn.heavy_weapon && n.weight.size() > 1) ? 1 : 0;
    } else if (n.cls == "TdAnimNodeSwing") {
        // Front, Middle, back. Measured on the recordings (the node is native; three swings, 2143
        // frames, to 0.005): by the swing's angle alone, a quarter turn ahead of the bar all Front,
        // a quarter turn behind all back, the Middle in between, with nothing smoothing it.
        if (n.weight.size() >= 3) {
            const float a = std::clamp(pawn.swing_angle / (PI * 0.5f), -1.0f, 1.0f);
            n.weight[0] = std::max(a, 0.0f);
            n.weight[1] = 1.0f - std::fabs(a);
            n.weight[2] = std::max(-a, 0.0f);
            n.target = n.weight;
            n.blend_to_go = 0.0f;
        }
        return;
    } else if (n.cls == "TdAnimNodeGrabSlope") {
        // Hang, Hang45 (hang45right), Hang45m (hang45left): the level hang and the two it leans
        // into on a ledge that runs up to one side. In hang45right the right hand is 42 uu under
        // the left with the hands 42 apart, so it is the hang from a ledge falling 45 degrees to
        // her right, and hang45left its mirror. The node is native and in no recording: taken as
        // straight in the slope between the level pose and those.
        if (n.weight.size() >= 3) {
            const float a = std::clamp(pawn.grab_slope_deg / 45.0f, -1.0f, 1.0f);
            n.weight[0] = 1.0f - std::fabs(a);
            n.weight[1] = std::max(-a, 0.0f);
            n.weight[2] = std::max(a, 0.0f);
            n.target = n.weight;
            n.blend_to_go = 0.0f;
        }
        return;
    } else if (n.cls == "TdAnimNodeTurn") {
        return;  // tick_turn
    } else if (n.cls == "TdAnimNodeGrabbing") {
        // Hang, HangFree, then the looking-back idles (left, right, and their extremes).
        // TdMove_IntoGrab: GrabAnimNode.SetActiveMove(legs on the wall ? 0 : 1).
        // Measured on the recordings (the node is native; every frame it has weight fits to 0.005):
        // turned past a quarter turn with the turn started, the idle of that side shows at once,
        // shared with its extreme by how far past ((|d| - 90) / 90); with no turn it goes back to
        // the hang over 0.2 s; in between (the turn's end playing) it stays as it is.
        const int hang = pawn.hanging_free ? 1 : 0;
        const float off = std::fabs(pawn.grab_turn_deg);
        if (became_relevant) {
            set_active(n, hang, 0.0f);
        } else if (pawn.grab_turn_type >= 2 && off > 90.0f && n.weight.size() >= 6) {
            const size_t side = pawn.grab_turn_deg < 0.0f ? 0 : 1;
            const float extreme = std::min((off - 90.0f) / 90.0f, 1.0f);
            std::fill(n.weight.begin(), n.weight.end(), 0.0f);
            n.weight[2 + side] = 1.0f - extreme;
            n.weight[4 + side] = extreme;
            n.target = n.weight;
            n.active = static_cast<int>(2 + side);
            n.blend_to_go = 0.0f;
        } else if (pawn.grab_turn_type <= 1 && (n.active != hang || n.target[static_cast<size_t>(hang)] < 1.0f)) {
            set_active(n, hang, 0.2f);
        }
        return;
    } else if (n.cls == "TdAnimNodeClimb") {
        // LadderLeft, LadderRight, LadderSlide, PipeLeft, PipeRight, PipeSlide, TurnLeftIdle, TurnRightIdle.
        // Looking back past a quarter turn (TdMove_Climb.StartTurningAngle) the look idle of that side
        // shows: measured on a ladder, on at 90.2 to 92.8 degrees and off again under 90, either
        // way over the node's 0.2 s.
        const int base = pawn.climbing_pipe ? 3 : 0;
        if (pawn.look_deg < -90.0f && n.weight.size() > 6) want = 6;
        else if (pawn.look_deg > 90.0f && n.weight.size() > 7) want = 7;
        else want = pawn.climb_sliding ? base + 2 : base + (pawn.climb_hand ? 0 : 1);
    } else if (n.cls == "TdAnimNodeBalanceWalk") {
        // Danger Left, Default, Danger Right, Crouch. Retail on a beam: the lose-balance pose of the
        // side she is going over comes in over 0.4 s and, countered, goes out over 0.6 (the node's
        // BlendWeight, as measured); it is at full weight for the last 0.4 s before she falls.
        want = pawn.balance_danger < 0 ? 0 : pawn.balance_danger > 0 ? 2 : 1;
        blend = want == 1 ? 0.6f : 0.4f;
    } else if (n.cls == "TdAnimNodeBalanceBlend") {
        // Left, Middle, Right: by how far she leans off the beam.
        if (n.weight.size() >= 3) {
            const float lean = std::clamp(pawn.balance_lean, -1.0f, 1.0f);
            n.weight[0] = std::max(0.0f, -lean);
            n.weight[1] = 1.0f - std::fabs(lean);
            n.weight[2] = std::max(0.0f, lean);
            n.target = n.weight;
            n.blend_to_go = 0.0f;
        }
        return;
    } else if (n.cls == "TdAnimNodeLedgeWalk" || n.cls == "TdAnimNodeDirSwitch") {
        // Right / Left along a ledge, Forward / Backward crouched: by the way she is going, and the
        // last way while she is still.
        const float yaw = pawn.yaw_deg * DEG2RAD;
        const float forward = std::cos(yaw) * pawn.velocity.x + std::sin(yaw) * pawn.velocity.y;
        const float right = -std::sin(yaw) * pawn.velocity.x + std::cos(yaw) * pawn.velocity.y;
        const float along = n.cls == "TdAnimNodeLedgeWalk" ? right : forward;
        if (std::fabs(along) > 1.0f) want = along >= 0.0f ? 0 : 1;
    } else {
        // Driven by nothing here: the node stays on its first child.
        want = 0;
    }
    if (static_cast<size_t>(want) < n.blend_in.size()) blend = n.blend_in[static_cast<size_t>(want)];
    // Measured (470 changes of walking state): into Idle and Sneak takes 0.2 s where BlendWeight
    // says 0.15 and 0.1; Walk 0.35, Jog 0.3, Run 0.6 and Sprint 0.8 are BlendWeight's. No blend of
    // a state node is seen to take less than 0.2 s (TdAnimNodeTurn's 0.1 is 0.2 too).
    if (n.cls == "TdAnimNodeWalkingState") blend = std::max(blend, 0.2f);
    if (became_relevant) {
        set_active(n, want, 0.0f);  // TdAnimNodeState.OnBecomeRelevant
    } else if (want != n.active) {
        set_active(n, want, blend);
    }
}

// TdAnimNodeBlendDirectional: Forward, ForwardRight, ForwardLeft, Backward, BackWardRight, BackWardLeft.
// Measured on the recordings. Direction is this frame's velocity in the pawn's frame with its two
// parts scaled to add up to 1 (the package saves it so: 0.639, -0.361), and the sideways child
// takes the sideways part: half at 45 degrees, 0.156 at 10 (the angle over 90 would be 0.11),
// 0.712 at 68, never more than 0.9 (straight sideways is 0.9 and 0.1). Which three play,
// forward or backward, is sticky: it changes only once the forward part is more than 0.163 the
// other way (9 changes: none at 0.1605, the first at 0.1657; about 11 degrees past sideways), and
// ForwardBlend then moves over ForwardInterpTime 0.4. OnBecomeRelevant starts it on the sign of
// Direction.Y. DirInterpTime is 0.1.
void AnimTree::update_directional(TreeNode& n, const PawnAnimState& pawn, float dt, bool became_relevant) {
    if (n.weight.size() < 6) return;
    constexpr float kMaxSide = 0.9f, kTurnBack = 0.163f;
    const float yaw = pawn.yaw_deg * DEG2RAD;
    const float forward = std::cos(yaw) * pawn.velocity.x + std::sin(yaw) * pawn.velocity.y;
    const float right = -std::sin(yaw) * pawn.velocity.x + std::cos(yaw) * pawn.velocity.y;
    if (forward * forward + right * right > 1.0f) {
        const float sum = std::fabs(forward) + std::fabs(right);
        n.dir_side = right / sum;
        n.dir_forward = forward / sum;
    }
    if (became_relevant) n.going_forward = n.dir_forward > 0.0f;
    else if (n.going_forward ? n.dir_forward < -kTurnBack : n.dir_forward > kTurnBack) n.going_forward = !n.going_forward;
    const float side_target = std::min(std::fabs(n.dir_side), kMaxSide);
    const float forward_target = n.going_forward ? 1.0f : 0.0f;
    if (became_relevant) {
        n.forward_blend = forward_target;
        n.side_blend = side_target;
    } else {
        auto approach = [dt](float value, float target, float time) {
            const float step = time > 0.0f ? dt / time : 1.0f;
            return value < target ? std::min(value + step, target) : std::max(value - step, target);
        };
        n.forward_blend = approach(n.forward_blend, forward_target, 0.4f);
        n.side_blend = approach(n.side_blend, side_target, 0.1f);
    }
    const int side = n.dir_side >= 0.0f ? 1 : 2;
    std::fill(n.weight.begin(), n.weight.end(), 0.0f);
    n.weight[0] = n.forward_blend * (1.0f - n.side_blend);
    n.weight[static_cast<size_t>(side)] = n.forward_blend * n.side_blend;
    n.weight[3] = (1.0f - n.forward_blend) * (1.0f - n.side_blend);
    n.weight[static_cast<size_t>(3 + side)] = (1.0f - n.forward_blend) * n.side_blend;
}

void AnimTree::advance(TreeNode& n, const PawnAnimState& pawn, float dt) {
    if (!n.seq && !n.seq_name.empty()) resolve(n);
    if (!n.seq || !n.playing) return;
    const float length = n.seq->length;
    if (length <= 0.0f) return;
    float rate = n.rate * n.seq->rate_scale;
    if (n.scale_rate_by_speed && n.base_speed > 0.0f) {
        // Measured on the recordings (2,900 frames with the game's own frame time known): the speed
        // is the frame before's, to 1 uu/s; this frame's is 3 to 25 uu/s out while she speeds up.
        const Vec3& v = pawn.last_velocity;
        const float speed = std::sqrt(v.x * v.x + v.y * v.y);
        rate *= std::clamp(speed / n.base_speed, n.rate_min, n.rate_max);
    }
    n.time += rate * dt;
    if (n.looping) {
        n.time = std::fmod(n.time, length);
        if (n.time < 0.0f) n.time += length;
    } else if (n.time >= length) {
        n.time = length;
        n.playing = false;
    } else if (n.time < 0.0f) {
        n.time = 0.0f;
        n.playing = false;
    }
}

void AnimTree::tick(const PawnAnimState& pawn, float dt) {
    if (root_ < 0) return;
    // TdAnimNodeLandOffset (native): into the landing pose over LandInto, back out over LandOut.
    float land = 0.0f;
    if (land_time_ >= 0.0f) {
        // Measured against retail's camera through landings: 0.1 s in and 0.3 s back out (the node's
        // LandOut says 0.4; LandOverlap and OverlapSize are not accounted for).
        constexpr float kLandInto = 0.1f, kLandOut = 0.3f;
        land_time_ += dt;
        if (land_time_ < kLandInto) land = land_amount_ * land_time_ / kLandInto;
        else if (land_time_ < kLandInto + kLandOut) land = land_amount_ * (1.0f - (land_time_ - kLandInto) / kLandOut);
        else land_time_ = -1.0f;
    }
    // Moving, the legs point the way she goes (the other way when she goes backward).
    {
        const float speed_sq = pawn.velocity.x * pawn.velocity.x + pawn.velocity.y * pawn.velocity.y;
        if (!leg_yaw_set_) {
            leg_yaw_ = pawn.yaw_deg;
            leg_yaw_set_ = true;
        }
        if (speed_sq > 100.0f && pawn.walking_state != kWasIdle) {
            const float yaw = pawn.yaw_deg * DEG2RAD;
            const float forward = std::cos(yaw) * pawn.velocity.x + std::sin(yaw) * pawn.velocity.y;
            float travel = std::atan2(pawn.velocity.y, pawn.velocity.x) / DEG2RAD;
            if (forward < 0.0f) travel += 180.0f;
            // They come round to it, not at once: after half a second of a slow sidestep retail's
            // are about 40 degrees round (the rate is a rough fit).
            const float off = wrap180(travel - leg_yaw_);
            const float step = 2.4f * std::sqrt(speed_sq) * dt;  // 83 degrees a second at 35 uu/s
            leg_yaw_ = wrap180(leg_yaw_ + std::clamp(off, -step, step));
            turning_ = false;
        }
    }
    nodes_[static_cast<size_t>(root_)].incoming = 1.0f;
    for (int index : order_) {
        TreeNode& n = nodes_[static_cast<size_t>(index)];
        n.total = std::min(n.incoming, 1.0f);
        n.incoming = 0.0f;
        const bool was_relevant = n.relevant;
        n.relevant = n.total > kZeroWeight;
        const bool became_relevant = n.relevant && !was_relevant;
        if (!n.relevant) continue;

        switch (n.kind) {
            case TreeNode::Kind::Sequence:
                if (became_relevant && n.reset_on_relevant && !n.seq_name.empty()) {
                    // TdAnimNodeSequence.OnBecomeRelevant.
                    if (!n.seq) resolve(n);
                    const float length = n.seq ? n.seq->length : 0.0f;
                    n.time = (n.synchronize ? n.synch_offset : n.start_position) * length;
                    n.playing = true;
                }
                // A member of the walk group is moved by the group.
                if (!in_walk_group_[static_cast<size_t>(index)] || walk_synch_ < 0) advance(n, pawn, dt);
                break;
            case TreeNode::Kind::Slot: {
                // A custom animation that is not looping blends back out as it runs down.
                if (n.active > 0 && n.pending_blend_out >= 0.0f) {
                    const TreeNode& seq = nodes_[static_cast<size_t>(n.children[static_cast<size_t>(n.active)])];
                    if (seq.seq && seq.seq->length > 0.0f) {
                        const float signed_rate = seq.rate * seq.seq->rate_scale;
                        const float rate = std::fabs(signed_rate);
                        const float left = rate > 0.0f ? (signed_rate > 0.0f ? seq.seq->length - seq.time : seq.time) / rate : 0.0f;
                        if (left <= n.pending_blend_out) {
                            set_active(n, 0, left);
                            n.pending_blend_out = -1.0f;
                        }
                    }
                }
                break;
            }
            case TreeNode::Kind::List:
                if (n.cls == "TdAnimNodeTurn") tick_turn(n, pawn, dt, became_relevant);
                update_list(n, pawn, became_relevant);
                if (n.hold >= 0.0f) {
                    n.hold -= dt;
                    if (n.hold < 0.0f) set_active(n, 0, n.hold_blend_out);
                }
                break;
            case TreeNode::Kind::PerBone:
                // A bone mask: the source is always whole underneath.
                if (n.weight.size() >= 2) {
                    n.weight[0] = 1.0f;
                    n.weight[1] = n.name == "ArmedLeft" ? pawn.armed_left : (n.name == "ArmedRight" ? pawn.armed_right : n.child2_weight);
                }
                break;
            case TreeNode::Kind::Passthrough:
                if (!n.weight.empty()) n.weight[0] = 1.0f;
                if (n.cls == "TdAnimNodeLandOffset") n.aim_y = land;
                // The standing legs' twist: how far the legs are off the body, a quarter turn to the unit.
                if (n.aim_from_legs) n.aim_x = wrap180(leg_yaw_ - pawn.yaw_deg) / 90.0f;
                if (n.cls == "TdAnimNodeDirBone" && n.name == "1pAim") {
                    // The hips (and the spine against them) turn the legs the way she is going, a
                    // quarter turn at most: the profile's left and right poses are 90 degrees. Going
                    // backward they turn off the opposite of it.
                    float want = 0.0f;
                    if (pawn.movement == EMovement::MOVE_Walking || pawn.movement == EMovement::MOVE_Crouch) {
                        const float yaw = pawn.yaw_deg * DEG2RAD;
                        const float forward = std::cos(yaw) * pawn.velocity.x + std::sin(yaw) * pawn.velocity.y;
                        const float right = -std::sin(yaw) * pawn.velocity.x + std::cos(yaw) * pawn.velocity.y;
                        if (forward * forward + right * right > 100.0f) {
                            float angle = std::atan2(right, forward) / DEG2RAD;
                            if (angle > 90.0f) angle -= 180.0f;
                            else if (angle < -90.0f) angle += 180.0f;
                            want = angle / 90.0f;
                        }
                    }
                    const float step = dt / 0.1f;  // DirInterpTime
                    n.aim_x = n.aim_x < want ? std::min(n.aim_x + step, want) : std::max(n.aim_x - step, want);
                }
                if (index == walk_synch_) tick_walk_group(pawn, dt);
                break;
            case TreeNode::Kind::Directional:
                update_directional(n, pawn, dt, became_relevant);
                break;
            case TreeNode::Kind::Fixed:
                break;
        }

        // AnimNodeBlendList.TickAnim: the weights close on their targets over BlendTimeToGo.
        if (n.kind == TreeNode::Kind::List || n.kind == TreeNode::Kind::Slot) {
            if (n.blend_to_go > dt) {
                for (size_t c = 0; c < n.weight.size(); ++c) n.weight[c] += (n.target[c] - n.weight[c]) / n.blend_to_go * dt;
                n.blend_to_go -= dt;
            } else {
                n.weight = n.target;
                n.blend_to_go = 0.0f;
            }
        }
        if (n.cls == "TdAnimNodeWeaponState" && !n.weight.empty()) weapon_ready_ = pawn.weapon_state == 0 ? 0.0f : n.weight[0];
        for (size_t c = 0; c < n.children.size(); ++c) {
            if (n.children[c] >= 0) nodes_[static_cast<size_t>(n.children[c])].incoming += n.total * n.weight[c];
        }
    }
}

void AnimTree::leaves(std::vector<Leaf>& out, size_t limit) const {
    out.clear();
    for (const TreeNode& n : nodes_) {
        if (n.kind != TreeNode::Kind::Sequence || n.seq_name.empty() || n.total <= 0.005f || n.unlisted) continue;
        // The carriers of the footstep and breathing notifies draw nothing.
        if (n.seq_name.compare(0, 13, "notifierdummy") == 0) continue;
        out.push_back(Leaf{n.seq_name, n.time, n.total});
    }
    std::stable_sort(out.begin(), out.end(), [](const Leaf& a, const Leaf& b) { return a.weight > b.weight; });
    if (out.size() > limit) out.resize(limit);
}

bool AnimTree::left_leg_forward() const {
    const float at = walk_cycle();
    return at > 0.5f;
}

}  // namespace me::fp
