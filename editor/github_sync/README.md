# Real-time GitHub sync (built into the editor)

Native editor code (C++): there is no plugin to install. Any project opened with this engine can be
connected to a GitHub repository.

- **Create, save, rename or delete** a file: it is uploaded to GitHub automatically (one commit per batch of changes).
- **A teammate pushes something**: it shows up in your project within a few seconds; open scenes reload by themselves.
- **Several collaborators at once** on the same repository: changes are merged file by file.
- **GitHub** bottom panel: status, team activity, log and invitations.
- Status button in the editor toolbar (`○` not connected, `◐` syncing, `●` up to date, `▲` error).

## Usage

### Sign in (regular GitHub account, no tokens to paste)

**GitHub** panel, then **Sign in with GitHub**:

1. A code (for example `ABCD-1234`) is shown and `github.com/login/device` is opened.
2. Enter the code on GitHub and sign in / authorize with your usual account (2FA included).
3. The editor is connected: your repositories show up in a drop-down; pick one (or create a new one).

This is GitHub's OAuth *device flow*: no token is pasted and there is no intermediate server.
The session is stored on your computer; **Sign out** deletes it.

### When creating a project

**Create New Project** has a **GitHub Repository (optional)** field:

1. Type `user/repository` or paste the URL.
2. Leave **Create it if missing (private)** checked to create the repository (private) when it does not exist.
3. When the project opens, the editor shows the GitHub panel (if you are not signed in yet), connects and uploads the project.

### In an already open project

**GitHub** panel: pick one of your repositories or type `user/repo` (or a URL) with an optional branch, then **Connect**.
If the repository already has content it is downloaded. **Create and connect** creates a new repository in your account.

### Collaborating

Use **Invite** (you must be an admin of the repository) or invite people from GitHub. Your teammate signs in with **their own**
account, connects to the same repository and receives everything.

## Sign-in setup (once per engine build)

Sign in needs the **Client ID** of a GitHub *OAuth App* (it is public, not a secret):

1. GitHub, Settings, Developer settings, OAuth Apps, **New OAuth App** (<https://github.com/settings/developers>).
   Any name and home page. The callback URL is unused but required (use `http://localhost`).
2. Tick **Enable Device Flow** in the app.
3. Put the **Client ID** in `DEFAULT_CLIENT_ID` in `github_sync.cpp` (already set for this repository), or provide it
   with the `GODOT_GITHUB_CLIENT_ID` environment variable, or in the file `<editor config folder>/github_client_id`.

Permission requested from each user: `repo` (read/write their repositories).

## Alternative: personal access token

**GitHub** panel, then *Use a personal access token instead*, or the `GITHUB_TOKEN` environment variable.
A classic token with the `repo` scope, or a fine-grained one with **Contents: Read & write**.

- The session/token is stored in `<editor config folder>/github_token` with `600` permissions.
  It never goes into the project or to GitHub.
- `GODOT_GITHUB_API_URL` / `GODOT_GITHUB_LOGIN_URL` change the hosts (GitHub Enterprise: `https://HOST/api/v3`
  and `https://HOST`).

## How it works

Every pass compares three states per file (git blob hash): **base** (last synchronization),
**local** (disk) and **remote** (the GitHub branch).

| local vs base | remote vs base | action |
|---|---|---|
| same | changed | download / delete locally |
| changed | same | upload / delete on GitHub |
| changed | changed | **conflict** |

- **Upload**: a single atomic commit through the Git Data API (blobs, tree, commit, then move the branch without forcing).
  If somebody pushed first (422) the cycle is retried.
- **Real time**: the branch is polled every 4 s (configurable, 2 to 60 s) with an `ETag`; `304 Not Modified` answers
  do not count against the API rate limit. Local changes are detected on save (1.5 s debounce) and by checking the
  disk on every cycle (with a modification-time cache).
- **Conflicts** (same file changed on both sides): your version wins and the remote one is kept in
  `.godotgit/conflicts/<date>/remote/...` (it also stays in the git history). On the **first connection** the
  repository version wins and yours is kept in `.../local/...`, so an existing repository is never overwritten.
- **Open scenes**: if they change on GitHub they are reloaded; if you have unsaved changes in that scene it is left
  alone and the log warns you.
- **Retries**: without network, or on errors, checks back off exponentially (up to 60 s).
- **Ignored**: `.godot/`, `.git/`, `.godotgit/`, temporary files and whatever your `.gitignore` says
  (simple patterns: `*.ext`, `folder/`, `/path`; no `!` negations). A default `.gitignore` is created if missing.
- **Repository link**: saved in the project itself, in `.godot_github.cfg` (owner, repository, branch; no secrets).
  It is synchronized with the repository, so anyone who opens the project (another computer, a fresh checkout, a teammate)
  is linked automatically and only has to sign in. Projects linked by older versions are migrated on open.
- **Local state**: `res://.godotgit/` (`config.cfg` with your sync preferences, `state.json`, `conflicts/`), with a
  `.gdignore` and added to `.gitignore`. Use **Disconnect** to stop syncing and remove the link.
- **Expired session**: if GitHub rejects the saved session (expired or revoked) syncing stops instead of retrying forever,
  the GitHub panel opens, and the link and the sync state are kept. Sign in again and it resumes where it stopped.

## Known limits

- Only what is **saved to disk** is synchronized (Ctrl+S).
- There is no line-level merge: two people editing the same `.tscn`/`.gd` at once is resolved as a file conflict.
  For scenes, it is better to agree on who touches what.
- Files larger than 50 MB are not synchronized (use Git LFS or exclude them).
- Executable permissions and symlinks are not preserved.
- The UI is in English (`TTR` strings, ready to be translated in the `.po` files).

## Files

| file | contents |
|---|---|
| `github_client.*` | minimal asynchronous client for the GitHub REST API and the OAuth device flow |
| `github_sync.*` | sync engine (three-way, push/pull, ETag polling, ignore rules, persistence) |
| `github_sync_dock.*` | the "GitHub" editor panel |
| `github_sync_plugin.*` | built-in `EditorPlugin`: bottom panel and status button |
| `../project_manager/project_dialog.*` | repository field when creating a project |
