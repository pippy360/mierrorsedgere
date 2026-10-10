#pragma once

// -----------------------------------------------------------------------------
// The level's Kismet (the Main_Sequence of the persistent map and of every streamed sublevel),
// read out of the cooked packages and run the way the engine runs it: trigger volumes, remote
// events across packages, delays, gates, switches, checkpoints, the player's cutscenes, the
// text put on the screen, and the action that ends a chapter.
//
// What the graph does is the game's own logic; what this runtime adds is the engine's
// semantics for it: USequence::ExecuteActiveOps ordering, SequenceEvent trigger counts and
// re-trigger delays, link activation delays, and the latent actions (Delay, Interp, PlaySound,
// FadeEffect, TutorialMessage). The frontend's menu Kismet (src/ui/frontend/kismet.cpp) runs
// the same way; this one knows the level's actors and the player.
//
// docs/KISMET_SEQUENCE_GRAPHS_RE.md and docs/GAMEPLAY_SCRIPTING_RE.md describe the graphs.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace me {

class UPKPackage;

// An actor a sequence event listens on, or an action points at: a Trigger (cylinder), a
// TriggerVolume (brush), a TdCheckpoint, a Trigger_LOS, a destination for a teleport.
struct ScriptActor {
    std::string package;      // package stem, lower case
    int32_t export_index = 0; // 1-based
    std::string cls;
    std::string name;         // "Trigger_16"
    Vec3 location{0.0f, 0.0f, 0.0f};
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    // Trigger: CylinderComponent
    bool has_cylinder = false;
    float radius = 40.0f;
    float half_height = 40.0f;
    // Volume: BrushComponent.BrushAggGeom, world space, one plane set per convex piece
    // (normal outward: inside when dot(normal, p) <= distance for every plane).
    struct Plane {
        Vec3 normal{0.0f, 0.0f, 1.0f};
        float distance = 0.0f;
    };
    std::vector<std::vector<Plane>> hull_planes;
    AABB bounds;
    bool collide_actors = true;  // bCollideActors; a Toggle on the actor flips it
    // TdCheckpoint
    std::string checkpoint_name;
    bool default_checkpoint = false;
};

// One sound key of a Matinee's InterpTrackSound.
struct ScriptSound {
    float time = 0.0f;
    std::string cue;   // "Group.Name"
    std::string bank;  // the content package the cue lives in
    bool voice = false;
};

// An InterpData: what the runtime needs of it. The pawn's animation is baked separately
// (LevelScene::cutscenes) and only referred to here.
struct ScriptMatinee {
    std::string name;
    float length = 0.0f;
    std::vector<std::pair<float, std::string>> events;  // InterpTrackEvent keys of every group
    std::vector<ScriptSound> sounds;                    // InterpTrackSound keys of every group
    // InterpTrackToggle keys: at `time` the actors of the Matinee's group `group` (those linked to
    // the SeqAct_Interp's variable link of that name) are switched: 0 on, 1 off, 2 the other way.
    struct Toggle {
        float time = 0.0f;
        std::string group;
        int action = 0;
    };
    std::vector<Toggle> toggles;
    int player_cutscene = -1;                           // index into LevelScene::cutscenes, -1 if the pawn is not in it
};

struct ScriptGraph {
    struct Link {
        int op = -1;
        int input = 0;
    };
    struct Output {
        std::string desc;
        float delay = 0.0f;
        bool disabled = false;
        std::vector<Link> links;
        int linked_op = -1;  // a Sequence's output: the SeqAct_FinishSequence behind it
    };
    struct Input {
        std::string desc;
        float delay = 0.0f;
        bool disabled = false;
        int linked_op = -1;  // a Sequence's input: the SeqEvent_SequenceActivated behind it
    };
    struct VarLink {
        std::string desc;
        std::vector<int> vars;
    };
    struct Node {
        int package = -1;          // index into ScriptGraph::packages
        int32_t export_index = 0;  // 1-based, in that package
        std::string cls;
        std::string name;
        int sequence = -1;         // the Sequence node that holds it; -1 for a package's Main_Sequence
        std::vector<Input> inputs;
        std::vector<Output> outputs;
        std::vector<VarLink> vars;

        // SequenceEvent
        int originator = -1;       // index into actors
        int max_trigger = 1;       // MaxTriggerCount, 0 = no limit
        float retrigger = 0.0f;    // ReTriggerDelay
        bool enabled = true;       // bEnabled
        bool player_only = true;   // bPlayerOnly
        float momentum = 0.0f;     // SeqEvent_TdTouch.Momentum
        bool player_touches = true;  // ClassProximityTypes admits a pawn of the player's class
        float los_distance = 0.0f; // SeqEvent_LOS: TriggerDistance, ScreenCenterDistance
        float los_screen = 0.0f;
        bool los_obstructions = true;

        std::string label;  // EventName, InputLabel, OutputLabel, FindVarName, VariableLabel, VarName
        std::string text;   // TutorialMessage, SupersMessage, a subtitle's text, NextLevelName
        std::string text2;  // NextCheckpointName, CustomButtonCallOut
        float f = 0.0f;     // FloatValue, Duration, FadeTime, PlayRate, DamageAmount
        float f2 = 0.0f;    // ValueB
        int i = 0;          // IntValue, Value, IncrementAmount, LinkCount, HintNumber, AutoCloseCount
        bool b = false;     // bValue, bOpen, FadeOut, bLooping, teleportPawnToCheckpoint, bDisablePlayerMoveInput
        bool b2 = false;    // skipSaveToDisk, bDisablePlayerLookInput, bRewindOnPlay, bRequireAccept
        bool b3 = false;    // bSetCinematicMode, bRewindIfAlreadyPlaying, bTriggerSlomo, bShouldBeDisabled
        bool b4 = false;    // bDisableSkipCutscenes, bPauseGame
        Vec3 color{0.0f, 0.0f, 0.0f};  // FadeColor
        std::string cue;    // SeqAct_TdPlaySound / SeqAct_PlaySound: "Group.Name"
        std::string cue_bank;
        bool cue_voice = false;
        int matinee = -1;   // SeqAct_Interp: index into matinees
        int actor = -1;     // SeqVar_Object: the actor it names, when it is one of `actors`
        std::vector<std::string> level_names;  // SeqAct_MultiLevelStreaming / SeqAct_LevelStreaming
        std::vector<int> stat_links;           // not used yet
    };

    std::vector<std::string> packages;  // stems, lower case, in load order
    std::vector<Node> nodes;
    std::vector<ScriptActor> actors;
    std::vector<ScriptMatinee> matinees;
    std::unordered_map<std::string, std::vector<int>> remote_events;  // lower-case EventName -> SeqEvent_RemoteEvent nodes

    // Reads the Main_Sequence of every package. `cutscene_of` maps "package:export" of a
    // SeqAct_Interp to its baked LevelScene::cutscenes index.
    bool load(const std::vector<std::shared_ptr<UPKPackage>>& packages,
              const std::unordered_map<std::string, int>& cutscene_of, std::vector<std::string>& warnings);
    [[nodiscard]] bool valid() const { return !nodes.empty(); }
    [[nodiscard]] int find_node(const std::string& package, int32_t export_index) const;
    [[nodiscard]] int find_checkpoint_actor(const std::string& checkpoint_name) const;
    [[nodiscard]] int find_default_checkpoint_actor() const;
};

// What the player is doing this frame, for the events that watch the player.
struct ScriptPlayerState {
    Vec3 position{0.0f, 0.0f, 0.0f};  // feet
    Vec3 eye{0.0f, 0.0f, 0.0f};
    Vec3 view_dir{1.0f, 0.0f, 0.0f};
    float fov_deg = 90.0f;
    float radius = 30.0f;             // collision cylinder (TdPawn: Radius 30, CollisionHeight 90)
    float half_height = 90.0f;
    float speed = 0.0f;
    bool use_pressed = false;         // the Use key went down this frame
    bool dead = false;
    bool in_cutscene = false;
};

// What the runtime asks of the game. Every callback is optional.
struct ScriptHost {
    // SeqAct_TdCheckpoint: the checkpoint became the active one (and the respawn point).
    std::function<void(const ScriptActor& checkpoint, bool teleport, bool save_to_disk)> set_checkpoint;
    // SeqAct_TdLevelCompleted
    std::function<void(const std::string& next_level, const std::string& next_checkpoint)> level_completed;
    // SeqAct_TdTutorialMessage: the message by its key in [TdTutorialMessages]; `hide` when it ends.
    std::function<void(const std::string& key, float duration, bool replace)> show_tutorial;
    std::function<void()> hide_tutorial;
    // SeqAct_TdSupersMessage: the chapter's place and time of day, as "<Strings:TdGameUI.TdSupersMessage.SP01A>"
    std::function<void(const std::string& text, float duration)> show_supers;
    // SeqAct_TdTriggerSubtitle
    std::function<void(const std::string& text, float duration)> show_subtitle;
    // SeqAct_TdTriggerSplashHint
    std::function<void(int hint_number)> splash_hint;
    // SeqAct_TdFadeEffect
    std::function<void(bool fade_out, float time, const Vec3& color)> fade;
    // SeqAct_TdDisablePlayerInput / SeqAct_TdEnablePlayerInput
    std::function<void(bool move, bool look, bool cinematic, bool no_skip)> disable_input;
    std::function<void()> enable_input;
    // SeqAct_Interp with the local pawn in a group: play LevelScene::cutscenes[index] at `play_rate`;
    // stop it (a skip or a Stop input).
    std::function<void(int cutscene, float play_rate)> play_cutscene;
    std::function<void()> stop_cutscene;
    // SeqAct_TdIntoCutscene: the pawn moves onto the destination before the Matinee starts.
    std::function<void(const Vec3& location, float yaw_deg)> into_cutscene;
    // SeqAct_TdPlaySound / SeqAct_PlaySound and a Matinee's sound keys: returns the cue's length in
    // seconds (0 when it could not play). `at` is the actor the sound plays from, null for the player.
    std::function<float(const std::string& cue, const std::string& bank, bool voice, float volume, const Vec3* at)> play_sound;
    std::function<void(const std::string& cue)> stop_sound;
    // SeqAct_Teleport with the player as target
    std::function<void(const Vec3& location, float yaw_deg)> teleport_player;
    // SeqAct_TdPlayerFail, SeqAct_CauseDamage on the player
    std::function<void()> player_fail;
    std::function<void(float amount)> damage_player;
    // SeqAct_MultiLevelStreaming / SeqAct_LevelStreaming: packages to add or take out of the loaded set
    std::function<void(const std::vector<std::string>& levels, bool load)> stream_levels;
    // SeqAct_TdSlomo / SeqAct_TdTriggerSlomo
    std::function<void(bool on)> slomo;
    // SeqAct_ShowLoading / SeqAct_HideLoading
    std::function<void(bool shown)> loading_indicator;
    // An emitter or a lens flare switched by SeqAct_Toggle or a Matinee's toggle track: 0 on,
    // 1 off, 2 the other way. And one hidden or shown (SeqAct_ToggleHidden, SeqAct_Destroy).
    std::function<void(const ScriptActor& actor, int action)> toggle_effect;
    std::function<void(const ScriptActor& actor, bool hidden)> hide_effect;
    // SeqAct_ActorFactory with an ActorFactoryEmitter: the factory (its package's stem in lower
    // case and its export) makes its particle system at a spawn point.
    std::function<void(const std::string& package, int32_t factory_export, const ScriptActor& at)> spawn_effect;
    // SeqAct_ChangeCollision / SeqAct_Toggle on collision/volume actors
    std::function<void(const ScriptActor& actor, bool collide_actors, bool block_actors)> change_collision;
    // Line checks for SeqEvent_LOS (bCheckForObstructions): true when the way is clear
    std::function<bool(const Vec3& from, const Vec3& to)> line_clear;
    // Logging
    std::function<void(const std::string& line)> log;
};

class LevelScript {
public:
    void init(std::shared_ptr<const ScriptGraph> graph, ScriptHost host);
    [[nodiscard]] bool valid() const { return graph_ && graph_->valid(); }
    [[nodiscard]] const ScriptGraph* graph() const { return graph_.get(); }

    // Which packages' sequences run: the persistent map and the sublevels streamed in. A package
    // joining the set fires its SeqEvent_LevelLoaded; one leaving it stops. Empty = all of them.
    void set_loaded_packages(const std::vector<std::string>& stems);
    [[nodiscard]] const std::vector<std::string>& loaded_packages() const { return loaded_; }

    // The level has loaded at a checkpoint: SeqEvent_LevelLoaded of every loaded package, then the
    // SeqEvt_TdCheckpointLoaded events of that checkpoint (TdCheckpoint.CheckpointName; empty = the
    // DefaultCheckpoint). Returns the checkpoint actor, or null when there is none of that name.
    const ScriptActor* begin_play(const std::string& checkpoint_name);
    // The player respawned at the active checkpoint: the sequences start over as a checkpoint load
    // does in retail, which reloads the level.
    void reload_checkpoint();

    void update(float dt, const ScriptPlayerState& player);

    // The player asked to skip the cutscene playing (SkipCutscene): the Matinee stops and fires Aborted.
    void skip_cutscene();
    // The cutscene the host plays ended by itself (reached its length).
    void cutscene_finished();
    // The tutorial message was accepted / the splash hint dismissed.
    void accept_message();
    // The player died (SeqEvt_TdPlayerDeath)
    void player_died();
    // The host's SeqAct_TdIntoCutscene move finished.
    void into_cutscene_finished();
    // TdUIScene.ActivateLevelEvent and SeqAct_ActivateRemoteEvent: every SeqEvent_RemoteEvent of that name.
    void fire_remote_event(const std::string& name);

    [[nodiscard]] bool input_move_disabled() const { return input_move_disabled_; }
    [[nodiscard]] bool input_look_disabled() const { return input_look_disabled_; }
    [[nodiscard]] bool cinematic_mode() const { return cinematic_mode_; }
    [[nodiscard]] bool skip_disabled() const { return skip_disabled_; }
    [[nodiscard]] bool load_from_checkpoint_disabled() const { return load_checkpoint_disabled_; }
    [[nodiscard]] const ScriptActor* active_checkpoint() const { return active_checkpoint_ >= 0 ? &graph_->actors[static_cast<size_t>(active_checkpoint_)] : nullptr; }
    [[nodiscard]] int playing_cutscene() const { return playing_cutscene_; }
    [[nodiscard]] int playing_cutscene_node() const { return playing_cutscene_node_; }
    // For the trace: the latent ops running, as "SeqAct_Interp_8@12.3".
    [[nodiscard]] std::string playing() const;
    [[nodiscard]] double time() const { return time_; }

private:
    struct State {
        bool active = false;
        uint32_t impulses = 0;
        uint32_t out = 0;
        int trigger_count = 0;
        double last_trigger = -1.0e9;
        bool enabled = true;
        bool touching = false;        // SeqEvent_Touch: the player is inside
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
        bool initialised = false;
        bool aborted = false;
        // SeqAct_Gate / SeqAct_Switch / SeqAct_RandomSwitch
        bool open = true;
        int index = 1;
        int auto_close_left = 0;
        // SeqAct_TdPlaySound: time left of the cue
        bool sound_playing = false;
        // SeqAct_TdTutorialMessage / SeqAct_TdIntoCutscene: waiting on the host
        bool waiting = false;
    };

    void reset_state();
    void queue(int node, bool top);
    void run_sequence(int slot, float dt);
    bool step_op(int node, float dt, bool newly_active);
    void deactivated(int node);
    int resolve_var(int var) const;
    int linked_var(int node, const char* desc) const;
    std::vector<int> linked_vars(int node, const char* desc) const;
    float read_float(int node, const char* desc, float fallback) const;
    int read_int(int node, const char* desc, int fallback) const;
    bool read_bool(int node, const char* desc, bool fallback) const;
    void fire_output(int node, const char* desc);
    void fire_output_index(int node, size_t index);
    bool check_activate(int event);
    void fire_checkpoint_events(int checkpoint_actor, bool loaded);
    bool package_loaded(int package) const;
    bool player_touches(const ScriptActor& actor, const ScriptPlayerState& player) const;
    bool player_sees(const ScriptGraph::Node& ev, const ScriptActor& actor, const ScriptPlayerState& player) const;
    bool var_is_player(int var) const;
    void interp_play(int node);
    void interp_stop(int node, bool aborted);
    void interp_step(int node, float dt);
    void fire_matinee_keys(int node, float previous, float position);
    void check_events(const ScriptPlayerState& player);
    void log(const std::string& line) const;

    std::shared_ptr<const ScriptGraph> graph_;
    ScriptHost host_;
    std::vector<State> state_;
    std::vector<std::vector<int>> active_;  // ActiveSequenceOps per sequence slot
    std::vector<int> sequence_nodes_;       // slot -> Sequence node (-1 for a root)
    std::vector<int> sequence_package_;     // slot -> package
    std::unordered_map<int, int> sequence_slot_;
    struct Delayed {
        int op;
        int input;
        float remaining;
    };
    std::vector<Delayed> delayed_;
    std::vector<std::string> loaded_;
    std::vector<bool> package_loaded_;
    std::vector<bool> level_loaded_fired_;
    double time_ = 0.0;
    uint64_t tick_ = 0;
    bool input_move_disabled_ = false;
    bool input_look_disabled_ = false;
    bool cinematic_mode_ = false;
    bool skip_disabled_ = false;
    bool load_checkpoint_disabled_ = false;
    int active_checkpoint_ = -1;
    int start_checkpoint_ = -1;
    int playing_cutscene_ = -1;
    int playing_cutscene_node_ = -1;
    int tutorial_node_ = -1;
    int into_cutscene_node_ = -1;
    int spawn_pending_ = 2;              // 2: the spawn position is still to be read, 1: the pawn has not moved yet
    Vec3 spawn_pos_{0.0f, 0.0f, 0.0f};
    uint32_t random_ = 0x9E3779B9u;
};

// The localized text behind "<Strings:Package.Section.Key>" and the [TdTutorialMessages] keys:
// Localization/INT/*.int, read once. `<StringAliasBindings:GBA_Jump>` becomes the key's name.
class LocalizedStrings {
public:
    void load(const std::string& game_root);
    // "<Strings:TdGameUI.TdSupersMessage.SP01A>" -> "Financial District 1.58pm"; plain text is returned as is.
    [[nodiscard]] std::string resolve(const std::string& text) const;
    [[nodiscard]] std::string lookup(const std::string& file, const std::string& section, const std::string& key) const;
    // A [TdTutorialMessages] key, with its key-binding aliases spelled out.
    [[nodiscard]] std::string tutorial_message(const std::string& key) const;
    [[nodiscard]] std::string splash_hint(int number) const;
    [[nodiscard]] bool loaded() const { return !strings_.empty(); }

private:
    std::unordered_map<std::string, std::string> strings_;  // "file.section.key" lower case -> text
};

}  // namespace me
