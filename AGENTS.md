# Agent Workflow & Git Worktree Guidelines (`AGENTS.md`)

All agents working in this repository **must** use isolated **Git Worktrees** for their tasks so that concurrent agents never interfere with each other's edits, builds, or verification runs.

---

## 1. Mandatory Rule: Use Git Worktrees for All Work

- **Never edit files or run builds directly in the shared root workspace (`/Users/tomnom/git/mierrorsedgere`) while working on a task.** The one exception is rebuilding `main`'s binary there right after you merge (Step 4).
- Before making any code, documentation, or build changes, create a dedicated Git worktree on a task-specific branch.
- When invoking subagents (`invoke_subagent`), either pass `Workspace: "share"` / `Workspace: "branch"` or instruct the subagent to create its own `git worktree`.

---

## 2. Step-by-Step Worktree Lifecycle

### Step 1: Ensure `main` Has an Initial Commit & Create Your Worktree
Pick a unique, descriptive branch and worktree name (for example, `agent/<task-name>` inside `.worktrees/<task-name>` or `/tmp/me-worktree-<task-name>`):

```bash
# From /Users/tomnom/git/mierrorsedgere
git worktree add -b agent/<task-name> /Users/tomnom/git/mierrorsedgere/.worktrees/<task-name> main
```

### Step 2: Develop, Build, and Verify Inside Your Worktree
Perform **all** file reads/edits, CMake builds, and verification runs inside your worktree directory (`/Users/tomnom/git/mierrorsedgere/.worktrees/<task-name>`):

```bash
cmake -B /Users/tomnom/git/mierrorsedgere/.worktrees/<task-name>/build \
      -S /Users/tomnom/git/mierrorsedgere/.worktrees/<task-name> \
      -DCMAKE_BUILD_TYPE=Release
cmake --build /Users/tomnom/git/mierrorsedgere/.worktrees/<task-name>/build -j
/Users/tomnom/git/mierrorsedgere/.worktrees/<task-name>/build/mirrorsedge_macos --verify-all
```

### Step 3: Update `TODO.md`, Document in `MODLOG.md`, and Commit in Your Worktree
Once your changes compile cleanly and pass verification:
- Check `TODO.md` at the root of your worktree. **Remove any issues from `TODO.md` that your change fixed and verified** (keep only open issues in `TODO.md`—do not leave checked-off `[x]` history). If you uncovered new out-of-scope bugs or retail parity gaps, add concise bullets for them in `TODO.md`.
- Record technical details and verification results in `MODLOG.md`.
- Commit your changes in the worktree:

```bash
git -C /Users/tomnom/git/mierrorsedgere/.worktrees/<task-name> add -A
git -C /Users/tomnom/git/mierrorsedgere/.worktrees/<task-name> commit -m "<clear description of changes>"
```

### Step 4: Merge Back to `main`, Rebuild `main`, Push, and Clean Up
When your task is complete, merge your worktree branch back into `main`, rebuild `main`'s binary in the primary repository, push to the remote repository, and remove the temporary worktree:

```bash
# 1. Switch/update main in the primary repository and merge your branch
git -C /Users/tomnom/git/mierrorsedgere checkout main
git -C /Users/tomnom/git/mierrorsedgere pull --rebase origin main || true
git -C /Users/tomnom/git/mierrorsedgere merge --no-ff agent/<task-name> -m "Merge agent/<task-name> into main"

# 2. Rebuild the binary on main in the primary repository
cmake -B /Users/tomnom/git/mierrorsedgere/build \
      -S /Users/tomnom/git/mierrorsedgere \
      -DCMAKE_BUILD_TYPE=Release
cmake --build /Users/tomnom/git/mierrorsedgere/build -j

# 3. Push main to remote
git -C /Users/tomnom/git/mierrorsedgere push origin main

# 4. Remove the worktree and delete the temporary branch
git -C /Users/tomnom/git/mierrorsedgere worktree remove /Users/tomnom/git/mierrorsedgere/.worktrees/<task-name>
git -C /Users/tomnom/git/mierrorsedgere branch -d agent/<task-name>
```

- **Always do step 2 after every merge.** `/Users/tomnom/git/mierrorsedgere/build/mirrorsedge_macos` (what `./play_macos.sh` launches) must match `main`, so the user plays what was just merged and not an old binary.
- **If the rebuild fails, do not push.** Fix the build in your worktree, commit, merge again, and rebuild before pushing.
- Only build there (no `--verify-all`: it rewrites the tracked `screenshots/` in the primary repository). Verification runs in your worktree (Step 2).

---

## 3. Issue Tracker (`TODO.md`) Rules

- **`TODO.md` is the shared backlog of open bugs and retail parity gaps** (checked into Git at the repository root).
- **Check `TODO.md` when starting a task** — especially `## User-Added Issues` at the top of the file.
- **Delete resolved items from `TODO.md` once fixed and verified**, and log the implementation & oracle proof in `MODLOG.md`. Never delete unverified items from `TODO.md`.

---

## 4. Conflict & Safety Rules

1. **No Proprietary Game Assets in Git**: Never commit `.me1`, `.upk`, `.u`, `.bik`, `.exe`, or `.dll` files from `/Users/tomnom/mirrorsedge`.
2. **Resolve Merge Conflicts Before Pushing**: If `main` has advanced while you were working in your worktree, rebase your `agent/<task-name>` branch onto `main` inside your worktree, re-run `./build/mirrorsedge_macos --verify-all` to confirm nothing broke, and then merge into `main` and push.
3. **Always Leave `main` Buildable**: Every merge to `main` must compile cleanly on macOS `arm64` (`clang++ -std=c++20 -fobjc-arc`) and pass `--verify-all`. The post-merge rebuild of `main` (Step 4, step 2) checks the merged tree compiles before it is pushed, and leaves the primary repository's binary up to date.

