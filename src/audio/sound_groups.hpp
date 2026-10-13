#pragma once

// -----------------------------------------------------------------------------
// The sound groups and mix groups of the retail game's ini ([Engine.AudioDevice] in
// TdGame/Config/DefaultEngine.ini, lines 134 to 175), as far as the port uses them.
//
// A cue names one sound group (USoundCue::SoundGroup). On every audio update
// UAudioComponent::UpdateWaveInstances (MirrorsEdge.exe 0x00B6F570) looks the group up and takes
// from it the volume, the flags of every wave the cue then plays, and the distance beyond which the
// cue is not parsed at all. docs/AUDIO_SYSTEM_RE.md sections 2 and 3.3.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <string_view>

namespace me {

// One +SoundGroups line: the members of its Properties the port looks at.
struct SoundGroupProperties {
    const char* name;
    float volume;               // Volume
    // MaxUpdateDistance (uu), DICE's addition; 0: none. A sound of the group whose nearest listener is
    // farther than this is not parsed that update (0x00B6FBB5): it submits no wave, so it holds no source.
    float max_update_distance;
    // bAlwaysPlay: 1.0 on top of the volume a wave is ranked by for a source (USoundNodeWave::ParseNodes, 0x00B77A8D).
    bool always_play;
    // bIsUISound: its sources are not paused with the game and go on being started and updated
    // while it is paused (UAudioDevice::HandlePause, 0x00B652B0; StartSources, 0x00B6EFE4).
    bool ui_sound;
};

inline constexpr SoundGroupProperties kSoundGroups[] = {
    {"Master", 1.0f, 0.0f, false, false},
    {"Music", 1.0f, 0.0f, false, false},
    {"SFX", 1.0f, 0.0f, false, false},
    {"VO", 1.0f, 0.0f, false, false},
    {"VOMain", 0.9f, 0.0f, false, false},
    {"AmbientPropSounds", 1.0f, 0.0f, false, false},
    {"DialogueRadio", 1.0f, 0.0f, true, false},
    {"DialogueFaith", 1.0f, 0.0f, true, false},
    {"DialogueOther", 1.0f, 0.0f, true, false},
    {"DialogueAI", 1.4f, 0.0f, false, false},
    {"DialogueHUD", 1.0f, 0.0f, true, true},
    {"IngameSFX", 1.0f, 0.0f, false, false},
    {"Weapons1p", 1.0f, 0.0f, false, false},
    {"Weapons3p", 0.75f, 0.0f, false, false},
    {"FaithVocal", 1.0f, 0.0f, false, false},
    {"FaithBreath", 1.0f, 0.0f, false, false},
    {"Dead", 1.0f, 0.0f, false, false},
    {"StereoAmbient", 0.9f, 0.0f, false, false},
    {"BigLooped", 1.0f, 0.0f, true, false},
    {"IndoorAmbientProps", 1.2f, 2000.0f, false, false},
    {"IndoorAmbient", 1.0f, 0.0f, false, false},
    {"OutdoorAmbientProps", 1.3f, 3000.0f, false, false},
    {"OutdoorAmbient", 1.0f, 0.0f, false, false},
    {"Helicopter", 1.0f, 0.0f, true, false},
    {"Convoy", 1.0f, 0.0f, true, false},
    {"HudMusic", 0.8f, 0.0f, false, true},
    {"CutSceneMusic", 1.0f, 0.0f, true, false},
    {"InGameMusic", 1.2f, 0.0f, true, true},
    {"TimeTrialMusic", 1.0f, 0.0f, true, true},
    {"HudSFX", 1.0f, 0.0f, false, true},
};

// Names are FNames in the game, which compare without regard to case (the ini has IngameSFX, the
// cues InGameSFX).
inline bool sound_name_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (x != y) return false;
    }
    return true;
}

// The group a cue names, null for a name the ini does not list.
inline const SoundGroupProperties* find_sound_group(std::string_view name) {
    for (const SoundGroupProperties& g : kSoundGroups) {
        if (sound_name_equal(name, g.name)) return &g;
    }
    return nullptr;
}

// The level's music is of the group InGameMusic, which the ini gives a Volume of 1.2.
// TdPlayerController.SetPause (TdGame.u) calls SetAudioGroupVolume('InGameMusic', 0.25) when it has
// paused the game and SetAudioGroupVolume('InGameMusic', 1.0) when it has let it go, and
// UAudioDevice::SetGroupVolume (0x00B6DD00) puts that number in the place of the group's own Volume:
// the ini's 1.2 is gone with the first call. TdPlayerController.ClientRestart calls SetPause(false)
// every time the controller is given its pawn (Engine.u: GameInfo.RestartPlayer -> Possess -> Restart
// -> ClientRestart), so in play the group is at 1.0 and a pause is a quarter of that.
inline constexpr float kInGameMusicIniVolume = 1.2f;
inline constexpr float kInGameMusicPausedVolume = 0.25f;
inline constexpr float kInGameMusicPlayVolume = 1.0f;

// One +MixGroups line. DICE's TdSoundNodeMixGroup (UTdSoundNodeMixGroup::ParseNodes, 0x012292A0)
// multiplies the volume and the pitch of what is below it by those of each mix group it names.
// (What the device's mix group object adds to these while the game runs, the ducking of
// TdSoundNodeDuckTrigger and slow motion, was not read: these are the ini's numbers.)
struct MixGroupProperties {
    const char* name;
    float volume;  // Volume
    float pitch;   // Pitch
};

inline constexpr MixGroupProperties kMixGroups[] = {
    {"Default", 1.0f, 1.0f},        {"WeaponSounds", 1.0f, 1.0f},     {"CharacterSounds", 1.0f, 1.0f},
    {"AmbientSounds", 1.0f, 1.0f},  {"Reverb", 1.0f, 1.0f},           {"AIVO", 1.0f, 1.0f},
    {"AmbientPropSounds", 0.9f, 1.0f}, {"SlowMotionSounds", 1.0f, 1.0f}, {"Outdoor", 1.0f, 1.0f},
    {"Indoor", 1.0f, 1.0f},         {"Helicopter", 1.0f, 1.0f},       {"Breathing", 1.0f, 1.0f},
};

inline const MixGroupProperties* find_mix_group(std::string_view name) {
    for (const MixGroupProperties& g : kMixGroups) {
        if (sound_name_equal(name, g.name)) return &g;
    }
    return nullptr;
}

}  // namespace me
