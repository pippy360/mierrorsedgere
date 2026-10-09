#pragma once

// -----------------------------------------------------------------------------
// The start-of-level intro: the Matinee that moves Faith into place before the
// player gets control (sp01_intro .. sp09_intro, sp01_intro_b).
//
// In the cooked level it is a SeqAct_Interp whose "Faith 1p" group is linked to
// SeqVar_TdLocalPawn and to a placed SkeletalMeshActor(MAT). The group plays one
// full-body first-person AnimSequence on the Custom_Canned slot; event tracks fire
// Kismet (the teleport, voice lines, door sounds, the fade), and the animation's
// own notifies carry the footsteps, clothing and foley. The view is the animated
// camera joint of the first-person skeleton, carried on the pawn's root.
//
// extract_level_intro() reads all of that into LevelScene::level_intro: the camera
// baked per animation frame in world space, every sound with its time, and the
// doors the intro swings. What retail does with the data, and how the result was
// measured against it, is in docs/LEVEL_INTROS.md.
// -----------------------------------------------------------------------------

#include "../math/types.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace me {

class UPKPackage;

void extract_level_intro(const std::string& game_root, const std::vector<std::shared_ptr<UPKPackage>>& packages,
                         LevelIntroSequence& out);

// Every Matinee that drives the local pawn with a placed body, baked the same way: the intro
// (copied from `intro` when it is one of them, so the two agree to the frame), the cutscenes
// the level's triggers start, the outro. A pawn group that plays several animations in turn
// (sp01b_outro_part1 then part2) is baked onto one timeline, its segments listed. `cutscene_of`
// maps "<package stem, lower case>:<SeqAct_Interp export index>" to the index in `out`, for the
// level script to play them by.
void extract_player_cutscenes(const std::string& game_root, const std::vector<std::shared_ptr<UPKPackage>>& packages,
                              const LevelIntroSequence& intro, std::vector<LevelIntroSequence>& out,
                              std::unordered_map<std::string, int>& cutscene_of);

}  // namespace me
