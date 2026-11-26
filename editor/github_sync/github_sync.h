/**************************************************************************/
/*  github_sync.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"
#include "editor/github_sync/github_client.h"
#include "scene/main/node.h"

#include <functional>
#include <memory>

class Timer;

// Two-way synchronization of the open project (res://) with a branch of a GitHub repository.
//
// Model (file level three-way merge, by git blob SHA):
//   base   = state of every file at the last synchronization
//   local  = current state on disk
//   remote = current state of the GitHub branch
// For every path:
//   local == remote                   nothing to do
//   local == base  (remote changed)   download / delete locally
//   remote == base (local changed)    upload / delete on GitHub
//   both changed                      conflict: the local version wins and the remote one is kept in
//                                     .godotgit/conflicts/ (on the very first connection the remote
//                                     wins instead, so an existing repository is never overwritten).
// Local changes go up in one atomic commit (Git Data API) and the branch is moved fast-forward only; if
// somebody pushed in the meantime the whole cycle is retried. "Real time" is achieved by polling the
// branch head with an ETag (304 answers do not count against the API rate limit).
class GitHubSync : public Node {
	GDCLASS(GitHubSync, Node);

public:
	enum Status {
		STATUS_DISCONNECTED,
		STATUS_IDLE,
		STATUS_SYNCING,
		STATUS_ERROR,
	};

	enum LogLevel {
		LOG_INFO,
		LOG_WARN,
		LOG_ERROR,
	};

	using ErrorCallback = std::function<void(const String &p_error)>; // Empty string = success.

	static const char *DATA_DIR_NAME; // ".godotgit" (local state, never synchronized)
	static const char *LINK_FILE_NAME; // ".godot_github.cfg" (repository link, travels with the project)

private:
	enum RunResult {
		RUN_OK,
		RUN_RETRY,
		RUN_ERROR,
	};

	struct Download {
		String path;
		String sha;
		bool apply = true;
		bool save_local = false;
		bool save_remote = false;
	};

	struct CacheEntry {
		uint64_t mtime = 0;
		int64_t size = 0;
		String sha;
	};

	struct IgnoreRule {
		String pattern;
		bool dir_only = false;
		bool anchored = false;
	};

	struct SyncRun {
		bool first_sync = false;
		bool failed = false;
		String head_sha;
		String tree_sha;
		HashMap<String, String> remote;
		HashMap<String, String> local;
		Vector<Download> downloads;
		Vector<String> local_deletes;
		Vector<String> in_sync;
		Vector<String> conflicts;
		HashMap<String, String> pushes; // path -> local sha ("" = delete on GitHub).
		HashMap<String, Vector<uint8_t>> fetched;
		HashMap<String, String> blob_shas;
		std::function<void(RunResult)> done;
	};

	static GitHubSync *singleton;

	GitHubClient *client = nullptr;
	Timer *poll_timer = nullptr;
	Timer *debounce_timer = nullptr;
	Timer *login_timer = nullptr;

	// Account sign-in (OAuth device flow).
	bool login_active = false;
	String login_client_id;
	String login_device_code;
	double login_interval = 5.0;
	double login_expires_at = 0.0;
	ErrorCallback login_cb;
	String account_login;

	String repo_owner;
	String repo;
	String branch;
	String user_login;
	bool create_if_missing = false;
	bool create_private = true;

	bool connected = false;
	bool busy = false;
	bool again = false;
	bool branch_ready = false;
	bool activity_dirty = true;
	bool auto_sync = true;
	double poll_seconds = 4.0;
	int error_streak = 0;
	String last_sync_text;

	String base_commit;
	String ref_etag;
	HashMap<String, String> base;
	HashMap<String, CacheEntry> cache;
	Vector<String> pending_refresh;
	Vector<IgnoreRule> ignore_rules;
	HashSet<String> warned_big;
	std::function<void()> delayed_action;

	// Connection.
	void _verify_and_resume(const String &p_token);
	void _prepare_branch(const ErrorCallback &p_cb);
	void _repo_ready(const Dictionary &p_repo_info, const ErrorCallback &p_cb);
	void _ensure_branch(const String &p_owner, const String &p_repo, const String &p_wanted, const String &p_default_branch, const ErrorCallback &p_cb);
	void _create_branch_from_default(const String &p_owner, const String &p_repo, const String &p_wanted, const String &p_default_branch, const ErrorCallback &p_cb);
	void _after_delay(double p_seconds, const std::function<void()> &p_action);
	void _run_delayed();

	void _session_expired();

	// Sign-in.
	void _login_poll();
	void _login_finish(const String &p_error);

	// Triggers.
	void _on_poll();
	void _on_debounce();
	void _check_remote(const std::function<void(int)> &p_cb);

	// Synchronization.
	void _sync_attempt(int p_attempt);
	void _sync_finished(bool p_ok);
	void _sync_once(const std::function<void(RunResult)> &p_done);
	void _run_fetch_tree(const std::shared_ptr<SyncRun> &p_run);
	void _run_plan(const std::shared_ptr<SyncRun> &p_run);
	void _run_download(const std::shared_ptr<SyncRun> &p_run);
	void _run_apply(const std::shared_ptr<SyncRun> &p_run);
	void _run_push(const std::shared_ptr<SyncRun> &p_run);
	void _run_push_commit(const std::shared_ptr<SyncRun> &p_run, const String &p_message);
	void _run_parallel(int p_count, const std::function<void(int, std::function<void()>)> &p_worker, const std::function<void()> &p_on_done);
	String _make_commit_message(const std::shared_ptr<SyncRun> &p_run) const;

	// Local files.
	bool _local_dirty();
	HashMap<String, String> _scan_local();
	void _scan_dir(const String &p_dir, HashMap<String, String> &r_result);
	bool _write_file(const String &p_rel, const Vector<uint8_t> &p_bytes);
	void _save_conflict_copy(const String &p_rel, const Vector<uint8_t> &p_bytes, const String &p_stamp, const String &p_tag);
	void _remove_empty_parents(const String &p_rel);
	void _warn_big(const String &p_rel);
	void _flush_editor_refresh();

	// .gitignore.
	void _load_ignore_rules();
	bool _path_ignored(const String &p_rel, bool p_is_dir) const;
	bool _remote_path_ignored(const String &p_rel) const;

	// Persistence.
	void _save_state();
	void _load_state();
	bool _load_config();
	void _clear_state();

	void _emit_status(const String &p_text, Status p_status);
	void _log(const String &p_text, LogLevel p_level);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static GitHubSync *get_singleton() { return singleton; }

	static String git_blob_sha(const uint8_t *p_data, size_t p_len);
	static String get_token_path();
	static String get_client_id();
	static String load_token();
	static void save_token(const String &p_token);
	static Vector<String> parse_repo(const String &p_text); // [owner, repo] or empty.
	static String sanitize_repo_name(const String &p_name);

	// Writes <project>/.godotgit/config.cfg so the editor connects to the repository when the project
	// is opened. `p_project_dir` is a filesystem path (or res://).
	static Error write_project_config(const String &p_project_dir, const String &p_owner, const String &p_repo, const String &p_branch, bool p_create_if_missing, bool p_create_private);

	// Called once the editor is ready: resumes the connection stored in the project, if any.
	void start();
	void shutdown();

	// Sign in with a GitHub account: shows a code to authorize at github.com/login/device
	// (OAuth device flow). The callback receives "" on success or an error message.
	void begin_device_login(const ErrorCallback &p_cb);
	void cancel_device_login();
	void sign_out();
	void refresh_account();
	void list_my_repos(const std::function<void(const Vector<String> &, const String &)> &p_cb);
	bool is_login_in_progress() const { return login_active; }
	String get_account_login() const { return account_login; }

	void connect_repo(const String &p_owner, const String &p_repo, const String &p_branch, const String &p_token, const ErrorCallback &p_cb);
	void create_and_connect(const String &p_repo_name, bool p_private, const String &p_token, const ErrorCallback &p_cb);
	void disconnect_repo();
	void invite(const String &p_username);
	void refresh_activity();

	void sync_now();
	void notify_local_change();

	void set_poll_seconds(double p_seconds);
	void set_auto_sync(bool p_enabled);

	bool is_repo_connected() const { return connected; }
	bool is_busy() const { return busy; }
	bool is_auto_sync() const { return auto_sync; }
	double get_poll_seconds() const { return poll_seconds; }
	String get_repo_owner() const { return repo_owner; }
	String get_repo_name() const { return repo; }
	String get_branch() const { return branch; }
	String get_user_login() const { return user_login; }
	bool has_saved_config() const { return !repo_owner.is_empty() && !repo.is_empty(); }

	GitHubSync();
	~GitHubSync();
};
