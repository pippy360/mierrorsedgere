#pragma once

// -----------------------------------------------------------------------------
// The menu level's Kismet (Maps/Menu/TdMainMenu.me1, Main_Sequence) and its Matinees, read out
// of the package and run the way the engine runs them. It is what moves the camera behind every
// front-end screen, fades the screen, and lights a district on the chapter-select screen: the UI
// scenes only fire named level events ("panel2", "VideoButton_Clicked", "LoadLevel_Edge").
//
// docs/MAIN_MENU_SYSTEM_RE.md, section 6, describes the graph.
// -----------------------------------------------------------------------------

#include "curve.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace me {
class UPKPackage;
}

namespace me::fe {

// One InterpGroup: the actor it moves comes from the SeqAct_Interp variable link of the same name.
struct MatineeGroup {
    std::string name;
    bool has_move = false;
    Curve pos;            // InterpTrackMove.PosTrack, world space
    Curve euler;          // InterpTrackMove.EulerTrack: roll, pitch, yaw in degrees
    std::string look_at;  // LookAtGroupName when RotMode is IMR_LookAtGroup
    bool has_fov = false;
    Curve fov;            // InterpTrackFloatProp "FOVAngle"
};

struct MatineeData {
    std::string name;
    float length = 0.0f;
    std::vector<MatineeGroup> groups;                     // in InterpGroups order, which is update order
    std::vector<std::pair<float, std::string>> cuts;      // InterpTrackDirector.CutTrack: time, TargetCamGroup
    std::vector<std::pair<float, std::string>> events;    // InterpTrackEvent: time, EventName
};

// A CameraActor or InterpActor of the level, as the Matinees leave it.
struct KismetActor {
    std::string name;
    Vec3 pos{0.0f, 0.0f, 0.0f};
    Vec3 euler{0.0f, 0.0f, 0.0f};  // roll, pitch, yaw in degrees
    float fov = 90.0f;
};

// The sequence graph. Nodes are every SequenceObject under Main_Sequence: ops and variables.
struct KismetGraph {
    struct Link {
        int op = -1;
        int input = 0;
    };
    struct Output {
        std::string desc;
        float delay = 0.0f;
        std::vector<Link> links;
        int linked_op = -1;  // a Sequence's output: the SeqAct_FinishSequence behind it
    };
    struct Input {
        std::string desc;
        int linked_op = -1;  // a Sequence's input: the SeqEvent_SequenceActivated behind it
    };
    struct VarLink {
        std::string desc;
        std::vector<int> vars;
    };
    struct Node {
        std::string cls;
        std::string name;
        int sequence = -1;  // the Sequence node that holds it; -1 for Main_Sequence's own
        std::vector<Input> inputs;
        std::vector<Output> outputs;
        std::vector<VarLink> vars;

        std::string label;       // EventName, VarName, FindVarName, InputLabel, OutputLabel, VariableLabel, LevelName
        std::string param;       // ParamName
        std::string object;      // ObjValue / MatInst: the object's name ("CameraActor_0", "MI_SP03_01")
        float f = 0.0f;          // FloatValue, Duration, FadeTime, ScalarValue
        float value_b = 0.0f;    // ValueB of the float ops
        int i = 0;               // IntValue, Value (SetInt), IncrementAmount
        bool b = false;          // bValue, FadeOut
        int max_trigger = 0;     // events: 0 = no limit
        float retrigger = 0.0f;  // events: ReTriggerDelay
        bool looping = false;    // SeqAct_Interp
        bool rewind_on_play = false;
        bool rewind_if_playing = false;
        int matinee = -1;        // SeqAct_Interp: index into matinees (through its Data link)
    };

    std::vector<Node> nodes;
    std::vector<MatineeData> matinees;
    std::vector<KismetActor> actors;         // start poses
    std::vector<std::string> material_names; // every MaterialInstanceConstant a node names

    bool load(const UPKPackage& level, std::vector<std::string>& warnings);
    [[nodiscard]] bool valid() const { return !nodes.empty(); }
    [[nodiscard]] int find_matinee(const std::string& name) const;
};

// Runs a KismetGraph. One instance per front end.
class KismetRunner {
public:
    void init(const KismetGraph* graph);

    // The level has loaded: SeqEvent_LevelLoaded and the root's SeqEvent_SequenceActivated fire.
    void begin_play();
    // TdUIScene.ActivateLevelEvent: every SeqEvent_RemoteEvent of that name is queued.
    void fire_event(const std::string& name);
    void update(float dt);

    // The actor the player looks through (the last Director cut), or null before the first.
    [[nodiscard]] const KismetActor* view() const { return view_ >= 0 ? &actors_[static_cast<size_t>(view_)] : nullptr; }
    [[nodiscard]] float fade() const { return fade_; }  // SeqAct_TdFadeEffect: 0 clear, 1 the fade colour (white)
    // A material instance's scalar parameter as Kismet last set it; `fallback` if it never did.
    [[nodiscard]] float material_param(const std::string& material, const std::string& param, float fallback) const;
    // Level names SeqAct_LevelStreaming asked to load since the last call.
    std::vector<std::string> take_streamed_levels();

    // For tests and the doc: the Matinees playing now, oldest first, as "InterpData_18@12.3".
    [[nodiscard]] std::string playing() const;

private:
    struct State {
        bool active = false;      // bActive: a latent op between Activated and DeActivated
        uint32_t impulses = 0;    // input links with bHasImpulse
        uint32_t out = 0;         // output links with bHasImpulse
        int trigger_count = 0;
        double last_trigger = -1.0e9;
        // variables
        float f = 0.0f;
        int i = 0;
        bool b = false;
        // SeqAct_Delay
        float remaining = 0.0f;
        bool delay_running = false;
        uint64_t started_tick = 0;
        // SeqAct_Interp
        float position = 0.0f;
        bool playing = false;
        bool initialised = false;  // InitInterp ran and TermInterp has not
        // SeqAct_Gate / SeqAct_Switch
        bool open = true;
        int index = 1;
    };

    void queue(int node, bool top);
    void run_sequence(int sequence, float dt);
    bool step_op(int node, float dt, bool newly_active);  // true when the op has finished
    void deactivated(int node);
    int resolve_var(int node, int var) const;             // through SeqVar_Named / SeqVar_External
    int linked_var(int node, const char* desc) const;
    float read_float(int node, const char* desc, float fallback) const;
    void fire_output(int node, const char* desc);
    void check_activate(int event);

    void interp_play(int node);
    void interp_step(int node, float dt);
    void interp_update(int node, float position, bool jump, float previous);
    int actor_for_group(int node, const std::string& group) const;

    const KismetGraph* graph_ = nullptr;
    std::vector<State> state_;
    std::vector<KismetActor> actors_;
    std::unordered_map<std::string, int> actor_index_;
    std::unordered_map<std::string, float> material_params_;  // "MI_SP03_01.Selected"
    std::vector<std::vector<int>> active_;   // ActiveSequenceOps per sequence; [0] is Main_Sequence
    std::vector<int> sequence_nodes_;        // sequence slot -> its Sequence node (-1 for the root)
    std::unordered_map<int, int> sequence_slot_;  // Sequence node -> slot
    struct Delayed {
        int op;
        int input;
        float remaining;
    };
    std::vector<Delayed> delayed_;
    std::vector<std::string> streamed_;
    int view_ = -1;
    float fade_ = 0.0f;
    float fade_rate_ = 0.0f;
    int fade_node_ = -1;
    double time_ = 0.0;
    uint64_t tick_ = 0;
};

}  // namespace me::fe
