/**************************************************************************/
/*  github_sync.cpp                                                       */
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

#include "github_sync.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/editor_interface.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/file_system/editor_paths.h"
#include "editor/script/script_editor_plugin.h"
#include "scene/main/scene_tree.h"
#include "scene/main/timer.h"

GitHubSync *GitHubSync::singleton = nullptr;
const char *GitHubSync::DATA_DIR_NAME = ".godotgit";
const char *GitHubSync::LINK_FILE_NAME = ".godot_github.cfg";

static const int64_t MAX_FILE_BYTES = 50 * 1024 * 1024;
static const int PARALLEL_REQUESTS = 6;
static const int MAX_ATTEMPTS = 4;
static const double DEBOUNCE_SECONDS = 1.5;

// Public identifier of the GitHub OAuth App used for "Sign in with GitHub" (device flow; it is not a secret).
// Register one at https://github.com/settings/developers (enable "Device Flow") and put its Client ID here,
// or provide it at runtime with the GODOT_GITHUB_CLIENT_ID environment variable.
static const char *DEFAULT_CLIENT_ID = "Ov23li6SzlWZociZ8Bvk";
static const char *DEFAULT_GITIGNORE = "# Godot 4+\n.godot/\n/android/\n\n# Local GitHub sync state\n.godotgit/\n\n# Temporary files\n*.tmp\n";

static const char *ALWAYS_IGNORED_DIRS[] = { ".godot", ".git", ".godotgit", ".import", ".mono", ".vs", ".idea", nullptr };
static const char *ALWAYS_IGNORED_FILES[] = { ".DS_Store", "Thumbs.db", "desktop.ini", nullptr };

// ------------------------------------------------------------------ Helpers

static bool _res_ok(const Dictionary &p_res) {
	return p_res["ok"];
}

static int _res_code(const Dictionary &p_res) {
	return p_res["code"];
}

static String _res_error(const Dictionary &p_res) {
	return p_res["error"];
}

static Dictionary _res_data(const Dictionary &p_res) {
	const Variant data = p_res["data"];
	return data.get_type() == Variant::DICTIONARY ? Dictionary(data) : Dictionary();
}

static Dictionary _res_headers(const Dictionary &p_res) {
	return p_res["headers"];
}

static Vector<uint8_t> _string_to_bytes(const String &p_string) {
	const CharString utf8 = p_string.utf8();
	Vector<uint8_t> bytes;
	bytes.resize(utf8.length());
	if (utf8.length() > 0) {
		memcpy(bytes.ptrw(), utf8.get_data(), utf8.length());
	}
	return bytes;
}

static String _lookup(const HashMap<String, String> &p_map, const String &p_key) {
	const String *value = p_map.getptr(p_key);
	return value ? *value : String();
}

static Vector<uint8_t> _decode_blob(const Dictionary &p_blob) {
	String content = p_blob.get("content", "");
	const String encoding = p_blob.get("encoding", "");
	if (encoding != "base64") {
		return _string_to_bytes(content);
	}
	content = content.replace("\n", "").replace("\r", "");
	const CharString cs = content.ascii();
	Vector<uint8_t> bytes;
	bytes.resize(cs.length() / 4 * 3 + 4);
	size_t written = 0;
	if (cs.length() > 0 && CryptoCore::b64_decode(bytes.ptrw(), bytes.size(), &written, (const uint8_t *)cs.get_data(), cs.length()) != OK) {
		return Vector<uint8_t>();
	}
	bytes.resize(written);
	return bytes;
}

String GitHubSync::git_blob_sha(const uint8_t *p_data, size_t p_len) {
	CryptoCore::SHA1Context ctx;
	ctx.start();
	const CharString header = (String("blob ") + String::num_int64((int64_t)p_len)).utf8();
	// Include the terminating NUL of the header, as git does.
	ctx.update((const uint8_t *)header.get_data(), header.length() + 1);
	if (p_len > 0) {
		ctx.update(p_data, p_len);
	}
	unsigned char hash[20];
	ctx.finish(hash);
	return String::hex_encode_buffer(hash, 20);
}

// -------------------------------------------------------------- Token store

String GitHubSync::get_token_path() {
	return EditorPaths::get_singleton()->get_config_dir().path_join("github_token");
}

String GitHubSync::get_client_id() {
	String id = OS::get_singleton()->get_environment("GODOT_GITHUB_CLIENT_ID").strip_edges();
	if (!id.is_empty()) {
		return id;
	}
	const String path = EditorPaths::get_singleton()->get_config_dir().path_join("github_client_id");
	if (FileAccess::exists(path)) {
		id = FileAccess::get_file_as_string(path).strip_edges();
		if (!id.is_empty()) {
			return id;
		}
	}
	return String(DEFAULT_CLIENT_ID);
}

String GitHubSync::load_token() {
	const String path = get_token_path();
	if (FileAccess::exists(path)) {
		const String saved = FileAccess::get_file_as_string(path).strip_edges();
		if (!saved.is_empty()) {
			return saved;
		}
	}
	return OS::get_singleton()->get_environment("GITHUB_TOKEN").strip_edges();
}

bool GitHubSync::save_token(const String &p_token) {
	const String path = get_token_path();
	const String wanted = p_token.strip_edges();
	DirAccess::make_dir_recursive_absolute(path.get_base_dir());
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_null()) {
		ERR_PRINT("GitHub sync: couldn't save the token to " + path);
		return false;
	}
	f->store_string(wanted);
	f->flush();
	f.unref();
	// Readable by the owner only, like git credential files.
	FileAccess::set_unix_permissions(path, BitField<FileAccess::UnixPermissionFlags>((int)FileAccess::UNIX_READ_OWNER | (int)FileAccess::UNIX_WRITE_OWNER));
	// Read it back: on some devices a silent write failure would otherwise look like "the session closed".
	return FileAccess::get_file_as_string(path).strip_edges() == wanted;
}

static String _account_cache_path() {
	return EditorPaths::get_singleton()->get_config_dir().path_join("github_account");
}

static String _pending_login_path() {
	return EditorPaths::get_singleton()->get_config_dir().path_join("github_login_pending.json");
}

// Last known account name: lets the UI show "signed in as ..." right away, even without network.
static String _load_cached_login() {
	return FileAccess::exists(_account_cache_path()) ? FileAccess::get_file_as_string(_account_cache_path()).strip_edges() : String();
}

static void _save_cached_login(const String &p_login) {
	if (p_login.is_empty()) {
		DirAccess::remove_absolute(_account_cache_path());
		return;
	}
	Ref<FileAccess> f = FileAccess::open(_account_cache_path(), FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(p_login);
	}
}

Vector<String> GitHubSync::parse_repo(const String &p_text) {
	String t = p_text.strip_edges();
	while (t.ends_with("/")) {
		t = t.trim_suffix("/");
	}
	t = t.trim_suffix(".git");
	const char *prefixes[] = { "https://github.com/", "http://github.com/", "git@github.com:", "github.com/", nullptr };
	for (int i = 0; prefixes[i]; i++) {
		if (t.begins_with(prefixes[i])) {
			t = t.trim_prefix(prefixes[i]);
			break;
		}
	}
	const PackedStringArray parts = t.split("/");
	Vector<String> result;
	if (parts.size() < 2 || parts[0].is_empty() || parts[1].is_empty()) {
		return result;
	}
	result.push_back(parts[0]);
	result.push_back(parts[1].trim_suffix(".git"));
	return result;
}

String GitHubSync::sanitize_repo_name(const String &p_name) {
	const String lower = p_name.to_lower().strip_edges().replace(" ", "-");
	String clean;
	for (int i = 0; i < lower.length(); i++) {
		const char32_t c = lower[i];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
			clean += c;
		}
	}
	return clean.is_empty() ? String("godot-project") : clean;
}

// ----------------------------------------------------- Project config on disk

static Error _write_config_file(const String &p_project_dir, const String &p_owner, const String &p_repo, const String &p_branch, bool p_create_if_missing, bool p_private, bool p_auto, double p_poll_seconds) {
	const String data_dir = p_project_dir.path_join(GitHubSync::DATA_DIR_NAME);
	Error err = DirAccess::make_dir_recursive_absolute(data_dir);
	if (err != OK) {
		return err;
	}

	// Keep the editor from scanning this folder as project content.
	const String marker = data_dir.path_join(".gdignore");
	if (!FileAccess::exists(marker)) {
		Ref<FileAccess> f = FileAccess::open(marker, FileAccess::WRITE);
	}

	// Keep plain git users from seeing the local state as untracked files.
	const String gitignore = p_project_dir.path_join(".gitignore");
	if (FileAccess::exists(gitignore)) {
		String text = FileAccess::get_file_as_string(gitignore);
		if (!text.contains(GitHubSync::DATA_DIR_NAME)) {
			Ref<FileAccess> f = FileAccess::open(gitignore, FileAccess::WRITE);
			if (f.is_valid()) {
				if (!text.is_empty() && !text.ends_with("\n")) {
					text += "\n";
				}
				f->store_string(text + String(GitHubSync::DATA_DIR_NAME) + "/\n");
			}
		}
	}

	// The repository link lives in the project root, so it travels with the project (it is synchronized
	// to the repository itself) and survives losing the local editor state or signing in again.
	// It holds no secrets.
	Ref<ConfigFile> link;
	link.instantiate();
	link->set_value("github", "owner", p_owner);
	link->set_value("github", "repo", p_repo);
	link->set_value("github", "branch", p_branch);
	link->set_value("github", "create_if_missing", p_create_if_missing);
	link->set_value("github", "private", p_private);
	err = link->save(p_project_dir.path_join(GitHubSync::LINK_FILE_NAME));
	if (err != OK) {
		return err;
	}

	// Per-user preferences stay local.
	Ref<ConfigFile> cfg;
	cfg.instantiate();
	cfg->set_value("sync", "auto", p_auto);
	cfg->set_value("sync", "poll_seconds", p_poll_seconds);
	return cfg->save(data_dir.path_join("config.cfg"));
}

Error GitHubSync::write_project_config(const String &p_project_dir, const String &p_owner, const String &p_repo, const String &p_branch, bool p_create_if_missing, bool p_create_private) {
	return _write_config_file(p_project_dir, p_owner, p_repo, p_branch, p_create_if_missing, p_create_private, true, 4.0);
}

bool GitHubSync::_load_config() {
	Ref<ConfigFile> link;
	link.instantiate();
	const bool have_link = link->load(String("res://") + LINK_FILE_NAME) == OK;

	Ref<ConfigFile> cfg;
	cfg.instantiate();
	const bool have_local = cfg->load(String("res://") + DATA_DIR_NAME + "/config.cfg") == OK;

	if (!have_link && !have_local) {
		return false;
	}
	// Projects connected by older versions only have the local file: it is used and migrated below.
	const Ref<ConfigFile> source = have_link ? link : cfg;
	repo_owner = source->get_value("github", "owner", "");
	repo = source->get_value("github", "repo", "");
	branch = source->get_value("github", "branch", "");
	create_if_missing = source->get_value("github", "create_if_missing", false);
	create_private = source->get_value("github", "private", true);
	if (have_local) {
		auto_sync = cfg->get_value("sync", "auto", true);
		poll_seconds = CLAMP((double)cfg->get_value("sync", "poll_seconds", poll_seconds), 2.0, 60.0);
	}

	const bool valid = !repo_owner.is_empty() && !repo.is_empty();
	if (valid && !have_link) {
		_write_config_file("res://", repo_owner, repo, branch, create_if_missing, create_private, auto_sync, poll_seconds);
	}
	return valid;
}

static String _state_path() {
	return String("res://") + GitHubSync::DATA_DIR_NAME + "/state.json";
}

void GitHubSync::_save_state() {
	if (!connected) {
		return;
	}
	DirAccess::make_dir_recursive_absolute(String("res://") + DATA_DIR_NAME);
	Dictionary base_dict;
	for (const KeyValue<String, String> &kv : base) {
		base_dict[kv.key] = kv.value;
	}
	Dictionary cache_dict;
	for (const KeyValue<String, CacheEntry> &kv : cache) {
		Array entry;
		entry.push_back((int64_t)kv.value.mtime);
		entry.push_back(kv.value.size);
		entry.push_back(kv.value.sha);
		cache_dict[kv.key] = entry;
	}
	Dictionary state;
	state["base_commit"] = base_commit;
	state["base"] = base_dict;
	state["cache"] = cache_dict;
	Ref<FileAccess> f = FileAccess::open(_state_path(), FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(JSON::stringify(state));
	}
}

void GitHubSync::_clear_state() {
	base_commit = "";
	ref_etag = "";
	base.clear();
	cache.clear();
}

void GitHubSync::_load_state() {
	_clear_state();
	if (!FileAccess::exists(_state_path())) {
		return;
	}
	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(_state_path()));
	if (parsed.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary state = parsed;
	base_commit = state.get("base_commit", "");
	if (state.get("base", Variant()).get_type() == Variant::DICTIONARY) {
		const Dictionary base_dict = state["base"];
		for (const Variant &key : base_dict.keys()) {
			base[key] = base_dict[key];
		}
	}
	if (state.get("cache", Variant()).get_type() == Variant::DICTIONARY) {
		const Dictionary cache_dict = state["cache"];
		for (const Variant &key : cache_dict.keys()) {
			const Variant value = cache_dict[key];
			if (value.get_type() != Variant::ARRAY) {
				continue;
			}
			const Array entry = value;
			if (entry.size() != 3) {
				continue;
			}
			CacheEntry c;
			c.mtime = (int64_t)entry[0];
			c.size = entry[1];
			c.sha = entry[2];
			cache[key] = c;
		}
	}
}

// ============================================================ Life cycle

GitHubSync::GitHubSync() {
	singleton = this;
#ifdef ANDROID_ENABLED
	poll_seconds = 10.0; // Gentler on battery and data.
#endif

	client = memnew(GitHubClient);
	client->connect("unauthorized", callable_mp(this, &GitHubSync::_on_unauthorized));
	add_child(client);

	poll_timer = memnew(Timer);
	poll_timer->set_one_shot(false);
	poll_timer->set_wait_time(poll_seconds);
	poll_timer->connect("timeout", callable_mp(this, &GitHubSync::_on_poll));
	add_child(poll_timer);

	debounce_timer = memnew(Timer);
	debounce_timer->set_one_shot(true);
	debounce_timer->set_wait_time(DEBOUNCE_SECONDS);
	debounce_timer->connect("timeout", callable_mp(this, &GitHubSync::_on_debounce));
	add_child(debounce_timer);

	login_timer = memnew(Timer);
	login_timer->set_one_shot(true);
	login_timer->connect("timeout", callable_mp(this, &GitHubSync::_login_poll));
	add_child(login_timer);
}

GitHubSync::~GitHubSync() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void GitHubSync::_bind_methods() {
	ADD_SIGNAL(MethodInfo("status_changed", PropertyInfo(Variant::STRING, "text"), PropertyInfo(Variant::INT, "status")));
	ADD_SIGNAL(MethodInfo("log_message", PropertyInfo(Variant::STRING, "text"), PropertyInfo(Variant::INT, "level")));
	ADD_SIGNAL(MethodInfo("activity_updated", PropertyInfo(Variant::ARRAY, "entries")));
	ADD_SIGNAL(MethodInfo("connection_changed", PropertyInfo(Variant::BOOL, "connected")));
	ADD_SIGNAL(MethodInfo("device_code", PropertyInfo(Variant::STRING, "user_code"), PropertyInfo(Variant::STRING, "verification_uri")));
	ADD_SIGNAL(MethodInfo("account_changed", PropertyInfo(Variant::STRING, "login")));
	ADD_SIGNAL(MethodInfo("login_required"));
	ADD_SIGNAL(MethodInfo("login_finished", PropertyInfo(Variant::STRING, "error")));
}

void GitHubSync::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_EXIT_TREE: {
			shutdown();
		} break;

		case NOTIFICATION_APPLICATION_RESUMED: {
			// Mobile: back from the background (for example after authorizing in the browser).
			if (login_active) {
				login_timer->stop();
				_login_poll();
			} else if (connected && !busy && auto_sync) {
				_on_poll();
			}
		} break;
	}
}

void GitHubSync::start() {
	if (load_token().is_empty()) {
		_resume_pending_login();
	}
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (efs && !efs->is_connected("filesystem_changed", callable_mp(this, &GitHubSync::notify_local_change))) {
		efs->connect("filesystem_changed", callable_mp(this, &GitHubSync::notify_local_change));
	}
	if (_load_config()) {
		_verify_and_resume(load_token());
	} else {
		_emit_status(TTR("Not connected"), STATUS_DISCONNECTED);
		emit_signal("connection_changed", false);
		refresh_account();
	}
}

void GitHubSync::shutdown() {
	poll_timer->stop();
	debounce_timer->stop();
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (efs && efs->is_connected("filesystem_changed", callable_mp(this, &GitHubSync::notify_local_change))) {
		efs->disconnect("filesystem_changed", callable_mp(this, &GitHubSync::notify_local_change));
	}
	connected = false;
}

void GitHubSync::_verify_and_resume(const String &p_token) {
	client->set_token(p_token);
	if (p_token.is_empty()) {
		_emit_status(TTR("Sign in to GitHub to sync this project"), STATUS_ERROR);
		emit_signal("connection_changed", false);
		emit_signal("login_required");
		return;
	}
	client->get_user([this](const Dictionary &p_me) {
		const bool ok = _res_ok(p_me);
		if (!ok && _res_code(p_me) == 401) {
			// Only a 401 means the session is no good. Anything else (5xx, rate limits, captive portals,
			// network changes) is temporary and must never ask the user to sign in again.
			_log(TTR("GitHub rejected the saved session: ") + _res_error(p_me), LOG_WARN);
			_emit_status(TTR("GitHub session expired: sign in again"), STATUS_ERROR);
			_save_cached_login(String());
			emit_signal("connection_changed", false);
			emit_signal("login_required");
			return;
		}
		if (ok) {
			user_login = _res_data(p_me).get("login", "");
			account_login = user_login;
			_save_cached_login(account_login);
			emit_signal("account_changed", account_login);
		} else {
			user_login = _load_cached_login();
			account_login = user_login;
			if (!account_login.is_empty()) {
				emit_signal("account_changed", account_login);
			}
			_log(vformat(TTR("GitHub is not reachable right now (%s). Your session is kept; retrying."), _res_error(p_me)), LOG_WARN);
		}
		_load_state();
		connected = true;
		branch_ready = false;
		emit_signal("connection_changed", true);
		poll_timer->start(poll_seconds);
		if (!ok) {
			_emit_status(TTR("GitHub not reachable, retrying..."), STATUS_ERROR);
			return;
		}
		sync_now();
	});
}

// ============================================================ Connection

void GitHubSync::connect_repo(const String &p_owner, const String &p_repo, const String &p_branch, const String &p_token, const ErrorCallback &p_cb) {
	if (busy) {
		p_cb(TTR("A synchronization is in progress."));
		return;
	}
	const String token = p_token.strip_edges().is_empty() ? load_token() : p_token.strip_edges();
	if (token.is_empty()) {
		p_cb(TTR("Sign in with GitHub first."));
		return;
	}
	client->set_token(token);

	client->get_user([=](const Dictionary &p_me) {
		if (!_res_ok(p_me)) {
			p_cb(TTR("GitHub rejected the credentials: ") + _res_error(p_me));
			return;
		}
		user_login = _res_data(p_me).get("login", "");

		client->get_repo(p_owner, p_repo, [=](const Dictionary &p_info) {
			if (!_res_ok(p_info)) {
				if (_res_code(p_info) == 404) {
					p_cb(vformat(TTR("Repository '%s/%s' not found (or your token has no access to it)."), p_owner, p_repo));
				} else {
					p_cb(TTR("Couldn't read the repository: ") + _res_error(p_info));
				}
				return;
			}
			const Dictionary info = _res_data(p_info);
			const Dictionary perms = info.get("permissions", Dictionary());
			if (perms.has("push") && !(bool)perms["push"]) {
				p_cb(TTR("Your user has no write permission on that repository. Ask to be invited as a collaborator."));
				return;
			}
			const String default_branch = info.get("default_branch", "main");
			String wanted = p_branch.strip_edges();
			if (wanted.is_empty()) {
				wanted = default_branch;
			}

			_ensure_branch(p_owner, p_repo, wanted, default_branch, [=](const String &p_error) {
				if (!p_error.is_empty()) {
					p_cb(p_error);
					return;
				}
				if (!p_token.strip_edges().is_empty() && !save_token(token)) {
					_log(vformat(TTR("Warning: this device would not store your token (%s)."), get_token_path()), LOG_ERROR);
				}
				repo_owner = p_owner;
				repo = p_repo;
				branch = wanted;
				create_if_missing = false;
				_clear_state();
				activity_dirty = true;
				branch_ready = true;
				_write_config_file("res://", repo_owner, repo, branch, false, create_private, auto_sync, poll_seconds);
				connected = true;
				_save_state();
				emit_signal("connection_changed", true);
				_log(vformat(TTR("Connected to %s/%s (branch %s) as %s"), repo_owner, repo, branch, user_login), LOG_INFO);
				poll_timer->start(poll_seconds);
				p_cb(String());
				sync_now();
			});
		});
	});
}

void GitHubSync::create_and_connect(const String &p_repo_name, bool p_private, const String &p_token, const ErrorCallback &p_cb) {
	const String token = p_token.strip_edges().is_empty() ? load_token() : p_token.strip_edges();
	if (token.is_empty()) {
		p_cb(TTR("Sign in with GitHub first."));
		return;
	}
	client->set_token(token);
	const String name = p_repo_name.strip_edges();
	create_private = p_private;

	client->get_user([=](const Dictionary &p_me) {
		if (!_res_ok(p_me)) {
			p_cb(TTR("GitHub rejected the credentials: ") + _res_error(p_me));
			return;
		}
		user_login = _res_data(p_me).get("login", "");
		client->create_repo(name, p_private, "Godot project", [=](const Dictionary &p_created) {
			if (!_res_ok(p_created)) {
				if (_res_code(p_created) == 422) {
					p_cb(vformat(TTR("Couldn't create '%s': a repository with that name already exists."), name));
				} else {
					p_cb(TTR("Couldn't create the repository: ") + _res_error(p_created));
				}
				return;
			}
			const Dictionary owner_info = _res_data(p_created).get("owner", Dictionary());
			const String new_owner = owner_info.get("login", user_login);
			// GitHub needs a moment to have the initial branch ready.
			_after_delay(1.5, [=]() {
				connect_repo(new_owner, name, "", token, p_cb);
			});
		});
	});
}

// ======================================================== Account sign-in

void GitHubSync::begin_device_login(const ErrorCallback &p_cb) {
	if (login_active) {
		p_cb(TTR("A sign-in is already in progress."));
		return;
	}
	const String client_id = get_client_id();
	if (client_id.is_empty()) {
		p_cb(TTR("This editor has no GitHub OAuth Client ID configured. Set GODOT_GITHUB_CLIENT_ID (see editor/github_sync/README.md) or use a personal access token."));
		return;
	}
	login_active = true;
	login_cb = p_cb;
	login_client_id = client_id;

	client->request_device_code(client_id, [this](const Dictionary &p_res) {
		if (!login_active) {
			return; // Cancelled meanwhile.
		}
		const Dictionary answer = _res_data(p_res);
		if (!_res_ok(p_res) || answer.has("error")) {
			String error = answer.get("error_description", _res_error(p_res));
			if (_res_code(p_res) == 404) {
				error = TTR("GitHub doesn't recognize this OAuth Client ID (is \"Device Flow\" enabled on the OAuth App?).");
			}
			_login_finish(error);
			return;
		}
		login_device_code = answer.get("device_code", "");
		login_interval = MAX(1.0, (double)answer.get("interval", 5));
		login_expires_at = OS::get_singleton()->get_unix_time() + (double)answer.get("expires_in", 900);
		const String user_code = answer.get("user_code", "");
		const String uri = answer.get("verification_uri", "https://github.com/login/device");
		_save_pending_login(user_code, uri);
		emit_signal("device_code", user_code, uri);
		_log(vformat(TTR("Open %s and enter the code %s to sign in."), uri, user_code), LOG_INFO);
		OS::get_singleton()->shell_open(uri);
		login_timer->start(login_interval);
	});
}

void GitHubSync::cancel_device_login() {
	if (!login_active) {
		return;
	}
	_login_finish(TTR("Sign-in cancelled."));
}

// If the OS kills the app while the user authorizes in the browser (common on phones), the sign-in resumes
// when the editor starts again instead of starting over.
void GitHubSync::_save_pending_login(const String &p_user_code, const String &p_uri) {
	Dictionary d;
	d["client_id"] = login_client_id;
	d["device_code"] = login_device_code;
	d["user_code"] = p_user_code;
	d["uri"] = p_uri;
	d["interval"] = login_interval;
	d["expires_at"] = login_expires_at;
	Ref<FileAccess> f = FileAccess::open(_pending_login_path(), FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(JSON::stringify(d));
		f.unref();
		FileAccess::set_unix_permissions(_pending_login_path(), BitField<FileAccess::UnixPermissionFlags>((int)FileAccess::UNIX_READ_OWNER | (int)FileAccess::UNIX_WRITE_OWNER));
	}
}

void GitHubSync::_resume_pending_login() {
	if (login_active || !FileAccess::exists(_pending_login_path())) {
		return;
	}
	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(_pending_login_path()));
	if (parsed.get_type() != Variant::DICTIONARY) {
		DirAccess::remove_absolute(_pending_login_path());
		return;
	}
	const Dictionary d = parsed;
	const double expires_at = d.get("expires_at", 0.0);
	if (expires_at < OS::get_singleton()->get_unix_time() + 5.0 || String(d.get("device_code", "")).is_empty()) {
		DirAccess::remove_absolute(_pending_login_path());
		return;
	}
	login_active = true;
	login_cb = nullptr;
	login_client_id = d.get("client_id", "");
	login_device_code = d.get("device_code", "");
	login_interval = MAX(1.0, (double)d.get("interval", 5));
	login_expires_at = expires_at;
	_log(TTR("Resuming the GitHub sign-in that was in progress."), LOG_INFO);
	emit_signal("device_code", String(d.get("user_code", "")), String(d.get("uri", "")));
	login_timer->start(1.0);
}

void GitHubSync::_login_finish(const String &p_error) {
	login_active = false;
	login_timer->stop();
	DirAccess::remove_absolute(_pending_login_path());
	emit_signal("login_finished", p_error);
	const ErrorCallback cb = login_cb;
	login_cb = nullptr;
	if (cb) {
		cb(p_error);
	}
}

void GitHubSync::_login_poll() {
	if (!login_active) {
		return;
	}
	if (OS::get_singleton()->get_unix_time() > login_expires_at) {
		_login_finish(TTR("The sign-in code expired. Try again."));
		return;
	}
	client->poll_device_token(login_client_id, login_device_code, [this](const Dictionary &p_res) {
		if (!login_active) {
			return;
		}
		if (!_res_ok(p_res) && _res_code(p_res) == 0) {
			login_timer->start(login_interval); // Network hiccup: keep waiting.
			return;
		}
		const Dictionary answer = _res_data(p_res);
		const String error = answer.get("error", "");
		if (error == "authorization_pending") {
			login_timer->start(login_interval);
			return;
		}
		if (error == "slow_down") {
			login_interval = MAX(login_interval + 5.0, (double)answer.get("interval", 0));
			login_timer->start(login_interval);
			return;
		}
		if (error == "expired_token") {
			_login_finish(TTR("The sign-in code expired. Try again."));
			return;
		}
		if (error == "access_denied") {
			_login_finish(TTR("Sign-in was denied on GitHub."));
			return;
		}
		if (!error.is_empty()) {
			_login_finish(String(answer.get("error_description", error)));
			return;
		}
		const String token = answer.get("access_token", "");
		if (token.is_empty()) {
			_login_finish(TTR("GitHub returned an unexpected answer."));
			return;
		}

		const bool stored = save_token(token);
		client->set_token(token);
		_login_finish(String());
		_log(TTR("Signed in with GitHub."), LOG_INFO);
		if (!stored) {
			_log(vformat(TTR("Warning: this device would not store your session (%s). You will have to sign in again next time."), get_token_path()), LOG_ERROR);
		}
		if (has_saved_config() && !connected) {
			_verify_and_resume(token);
		} else {
			refresh_account();
		}
	});
}

// GitHub answered 401 to an authenticated request. One stray answer must not log the user out, so the
// session is checked once more before giving up.
void GitHubSync::_on_unauthorized() {
	if (!connected || verifying_session) {
		return;
	}
	verifying_session = true;
	client->get_user([this](const Dictionary &p_me) {
		verifying_session = false;
		if (!_res_ok(p_me) && _res_code(p_me) == 401) {
			_session_expired();
		}
	});
}

// The saved session was confirmed invalid (expired or revoked). The repository link and the sync state are
// kept, so signing in again resumes where it stopped.
void GitHubSync::_session_expired() {
	if (!connected) {
		return;
	}
	_save_state();
	poll_timer->stop();
	debounce_timer->stop();
	connected = false;
	busy = false;
	again = false;
	branch_ready = false;
	account_login = String();
	_save_cached_login(String());
	_log(TTR("GitHub rejected the saved session (it expired or was revoked). The repository link is kept: sign in again to continue."), LOG_WARN);
	_emit_status(TTR("GitHub session expired: sign in again"), STATUS_ERROR);
	emit_signal("account_changed", String());
	emit_signal("connection_changed", false);
	emit_signal("login_required");
}

void GitHubSync::sign_out() {
	cancel_device_login();
	poll_timer->stop();
	debounce_timer->stop();
	connected = false;
	busy = false;
	branch_ready = false;
	DirAccess::remove_absolute(get_token_path());
	_save_cached_login(String());
	client->set_token(String());
	account_login = String();
	_log(TTR("Signed out of GitHub."), LOG_INFO);
	_emit_status(TTR("Not connected"), STATUS_DISCONNECTED);
	emit_signal("account_changed", String());
	emit_signal("connection_changed", false);
}

void GitHubSync::refresh_account() {
	const String token = load_token();
	if (token.is_empty()) {
		account_login = String();
		emit_signal("account_changed", String());
		return;
	}
	client->set_token(token);
	if (account_login.is_empty()) {
		account_login = _load_cached_login();
		if (!account_login.is_empty()) {
			emit_signal("account_changed", account_login);
		}
	}
	client->get_user([this](const Dictionary &p_me) {
		if (_res_ok(p_me)) {
			account_login = _res_data(p_me).get("login", "");
			_save_cached_login(account_login);
		} else if (_res_code(p_me) == 401) {
			account_login = String();
			_save_cached_login(String());
		} else {
			return; // Temporary problem: keep showing the last known account.
		}
		emit_signal("account_changed", account_login);
	});
}

void GitHubSync::list_my_repos(const std::function<void(const Vector<String> &, const String &)> &p_cb) {
	if (load_token().is_empty()) {
		p_cb(Vector<String>(), TTR("Sign in with GitHub first."));
		return;
	}
	client->set_token(load_token());
	client->list_user_repos([p_cb](const Dictionary &p_res) {
		Vector<String> names;
		const Variant answer = p_res["data"];
		if (!_res_ok(p_res) || answer.get_type() != Variant::ARRAY) {
			p_cb(names, _res_error(p_res));
			return;
		}
		for (const Variant &item : Array(answer)) {
			const Dictionary repo_info = item;
			names.push_back(repo_info.get("full_name", ""));
		}
		p_cb(names, String());
	});
}

void GitHubSync::disconnect_repo() {
	poll_timer->stop();
	debounce_timer->stop();
	connected = false;
	busy = false;
	branch_ready = false;
	_clear_state();
	DirAccess::remove_absolute(String("res://") + DATA_DIR_NAME + "/config.cfg");
	DirAccess::remove_absolute(String("res://") + LINK_FILE_NAME);
	DirAccess::remove_absolute(_state_path());
	_log(TTR("Disconnected. Your local files and the repository stay as they are."), LOG_INFO);
	_emit_status(TTR("Not connected"), STATUS_DISCONNECTED);
	emit_signal("connection_changed", false);
}

void GitHubSync::_prepare_branch(const ErrorCallback &p_cb) {
	client->get_repo(repo_owner, repo, [=](const Dictionary &p_info) {
		if (_res_ok(p_info)) {
			_repo_ready(_res_data(p_info), p_cb);
			return;
		}
		if (_res_code(p_info) == 404 && create_if_missing && !user_login.is_empty() && repo_owner.nocasecmp_to(user_login) == 0) {
			_log(vformat(TTR("Creating repository %s/%s..."), repo_owner, repo), LOG_INFO);
			client->create_repo(repo, create_private, "Godot project", [=](const Dictionary &p_created) {
				if (!_res_ok(p_created)) {
					p_cb(TTR("Couldn't create the repository: ") + _res_error(p_created));
					return;
				}
				create_if_missing = false;
				_write_config_file("res://", repo_owner, repo, branch, false, create_private, auto_sync, poll_seconds);
				_after_delay(1.5, [=]() {
					_prepare_branch(p_cb);
				});
			});
			return;
		}
		p_cb(vformat(TTR("Couldn't read %s/%s: %s"), repo_owner, repo, _res_error(p_info)));
	});
}

void GitHubSync::_repo_ready(const Dictionary &p_repo_info, const ErrorCallback &p_cb) {
	const String default_branch = p_repo_info.get("default_branch", "main");
	if (branch.is_empty()) {
		branch = default_branch;
		_write_config_file("res://", repo_owner, repo, branch, create_if_missing, create_private, auto_sync, poll_seconds);
	}
	_ensure_branch(repo_owner, repo, branch, default_branch, p_cb);
}

void GitHubSync::_ensure_branch(const String &p_owner, const String &p_repo, const String &p_wanted, const String &p_default_branch, const ErrorCallback &p_cb) {
	client->get_ref(p_owner, p_repo, p_wanted, "", [=](const Dictionary &p_ref) {
		if (_res_ok(p_ref)) {
			p_cb(String());
			return;
		}
		const int ref_code = _res_code(p_ref);
		const String ref_error = _res_error(p_ref);
		client->list_branches(p_owner, p_repo, [=](const Dictionary &p_branches) {
			const Variant branches_data = p_branches["data"];
			const bool no_branches = _res_ok(p_branches) && branches_data.get_type() == Variant::ARRAY && Array(branches_data).is_empty();
			if (no_branches || ref_code == 409) {
				// Completely empty repository: the Contents API can create the first commit.
				_log(TTR("The repository is empty: creating the first commit."), LOG_INFO);
				client->put_file(p_owner, p_repo, ".gitignore", _string_to_bytes(DEFAULT_GITIGNORE), "Initial commit (Godot)", [=](const Dictionary &p_first) {
					if (!_res_ok(p_first)) {
						p_cb(TTR("Couldn't initialize the empty repository: ") + _res_error(p_first));
						return;
					}
					if (p_wanted == p_default_branch) {
						p_cb(String());
					} else {
						_create_branch_from_default(p_owner, p_repo, p_wanted, p_default_branch, p_cb);
					}
				});
			} else if (p_wanted == p_default_branch) {
				p_cb(vformat(TTR("Couldn't read branch '%s': %s"), p_wanted, ref_error));
			} else {
				_create_branch_from_default(p_owner, p_repo, p_wanted, p_default_branch, p_cb);
			}
		});
	});
}

void GitHubSync::_create_branch_from_default(const String &p_owner, const String &p_repo, const String &p_wanted, const String &p_default_branch, const ErrorCallback &p_cb) {
	client->get_ref(p_owner, p_repo, p_default_branch, "", [=](const Dictionary &p_ref) {
		if (!_res_ok(p_ref)) {
			p_cb(vformat(TTR("Couldn't read branch '%s': %s"), p_default_branch, _res_error(p_ref)));
			return;
		}
		const Dictionary object = _res_data(p_ref).get("object", Dictionary());
		const String sha = object.get("sha", "");
		client->create_ref(p_owner, p_repo, p_wanted, sha, [=](const Dictionary &p_made) {
			if (!_res_ok(p_made)) {
				p_cb(vformat(TTR("Couldn't create branch '%s': %s"), p_wanted, _res_error(p_made)));
				return;
			}
			_log(vformat(TTR("Branch '%s' created from '%s'."), p_wanted, p_default_branch), LOG_INFO);
			p_cb(String());
		});
	});
}

void GitHubSync::_after_delay(double p_seconds, const std::function<void()> &p_action) {
	delayed_action = p_action;
	get_tree()->create_timer(p_seconds)->connect("timeout", callable_mp(this, &GitHubSync::_run_delayed));
}

void GitHubSync::_run_delayed() {
	std::function<void()> action = delayed_action;
	delayed_action = nullptr;
	if (action) {
		action();
	}
}

void GitHubSync::invite(const String &p_username) {
	String who = p_username.strip_edges();
	who = who.trim_prefix("@");
	if (who.is_empty() || !connected) {
		return;
	}
	client->add_collaborator(repo_owner, repo, who, [=](const Dictionary &p_res) {
		if (_res_ok(p_res)) {
			if (_res_code(p_res) == 204) {
				_log(vformat(TTR("%s is already a collaborator of %s/%s."), who, repo_owner, repo), LOG_INFO);
			} else {
				_log(vformat(TTR("Invitation sent to %s. They must accept it (they get an email and it shows in github.com/notifications)."), who), LOG_INFO);
			}
		} else {
			_log(vformat(TTR("Couldn't invite %s: %s (you must be an admin of the repository)"), who, _res_error(p_res)), LOG_ERROR);
		}
	});
}

void GitHubSync::refresh_activity() {
	if (!connected) {
		return;
	}
	client->list_commits(repo_owner, repo, branch, 25, [this](const Dictionary &p_res) {
		const Variant commits_data = p_res["data"];
		if (!_res_ok(p_res) || commits_data.get_type() != Variant::ARRAY) {
			return;
		}
		Array entries;
		for (const Variant &item_var : Array(commits_data)) {
			const Dictionary item = item_var;
			const Dictionary commit = item.get("commit", Dictionary());
			const Dictionary author_info = commit.get("author", Dictionary());
			String who;
			if (item.get("author", Variant()).get_type() == Variant::DICTIONARY) {
				who = Dictionary(item["author"]).get("login", "");
			}
			if (who.is_empty()) {
				who = author_info.get("name", "?");
			}
			Dictionary entry;
			entry["sha"] = String(item.get("sha", "")).substr(0, 7);
			entry["message"] = String(commit.get("message", "")).get_slicec('\n', 0);
			entry["author"] = who;
			entry["date"] = author_info.get("date", "");
			entries.push_back(entry);
		}
		emit_signal("activity_updated", entries);
	});
}

// ============================================================ Triggers

void GitHubSync::notify_local_change() {
	if (!connected || !auto_sync) {
		return;
	}
	if (busy) {
		again = true;
		return;
	}
	debounce_timer->start(DEBOUNCE_SECONDS);
}

void GitHubSync::set_poll_seconds(double p_seconds) {
	poll_seconds = CLAMP(p_seconds, 2.0, 60.0);
	if (connected) {
		poll_timer->start(poll_seconds);
		_write_config_file("res://", repo_owner, repo, branch, create_if_missing, create_private, auto_sync, poll_seconds);
	}
}

void GitHubSync::set_auto_sync(bool p_enabled) {
	auto_sync = p_enabled;
	if (connected) {
		_write_config_file("res://", repo_owner, repo, branch, create_if_missing, create_private, auto_sync, poll_seconds);
	}
}

void GitHubSync::_on_debounce() {
	if (!connected || !auto_sync) {
		return;
	}
	if (busy) {
		again = true;
		return;
	}
	if (_local_dirty()) {
		sync_now();
	}
}

void GitHubSync::_on_poll() {
	if (!connected || busy || !auto_sync) {
		return;
	}
	if (!branch_ready) {
		sync_now();
		return;
	}
	_check_remote([this](int p_state) {
		if (!connected) {
			return;
		}
		if (p_state == -1) {
			error_streak++;
			poll_timer->start(MIN(poll_seconds * Math::pow(2.0, (double)error_streak), 60.0));
			_emit_status(TTR("No connection to GitHub, retrying..."), STATUS_ERROR);
			return;
		}
		if (error_streak > 0) {
			error_streak = 0;
			poll_timer->start(poll_seconds);
		}
		if (p_state == 1 || _local_dirty()) {
			sync_now();
		}
	});
}

// 1 = the branch moved, 0 = no changes, -1 = network/API error.
void GitHubSync::_check_remote(const std::function<void(int)> &p_cb) {
	client->get_ref(repo_owner, repo, branch, ref_etag, [this, p_cb](const Dictionary &p_res) {
		if (_res_code(p_res) == 304) {
			p_cb(0);
			return;
		}
		if (!_res_ok(p_res)) {
			p_cb(-1);
			return;
		}
		ref_etag = _res_headers(p_res).get("etag", "");
		const Dictionary object = _res_data(p_res).get("object", Dictionary());
		const String sha = object.get("sha", "");
		p_cb(sha != base_commit ? 1 : 0);
	});
}

// ============================================================ Synchronize

void GitHubSync::sync_now() {
	if (!connected) {
		return;
	}
	if (busy) {
		again = true;
		return;
	}
	busy = true;
	_emit_status(TTR("Syncing..."), STATUS_SYNCING);

	if (!branch_ready) {
		_prepare_branch([this](const String &p_error) {
			if (!p_error.is_empty()) {
				_log(p_error, LOG_ERROR);
				_sync_finished(false);
				return;
			}
			branch_ready = true;
			_sync_attempt(0);
		});
	} else {
		_sync_attempt(0);
	}
}

void GitHubSync::_sync_attempt(int p_attempt) {
	_sync_once([this, p_attempt](RunResult p_result) {
		if (p_result == RUN_RETRY && p_attempt + 1 < MAX_ATTEMPTS) {
			_sync_attempt(p_attempt + 1);
			return;
		}
		_sync_finished(p_result == RUN_OK);
	});
}

void GitHubSync::_sync_finished(bool p_ok) {
	busy = false;
	if (!connected) {
		return;
	}
	_flush_editor_refresh();

	if (p_ok) {
		error_streak = 0;
		poll_timer->start(poll_seconds);
		last_sync_text = Time::get_singleton()->get_time_string_from_system();
		_emit_status(vformat(TTR("Synced - %s"), last_sync_text), STATUS_IDLE);
		if (activity_dirty) {
			activity_dirty = false;
			refresh_activity();
		}
	} else {
		error_streak++;
		poll_timer->start(MIN(poll_seconds * Math::pow(2.0, (double)error_streak), 60.0));
		_emit_status(TTR("Sync error (see the log)"), STATUS_ERROR);
	}
	_save_state();

	const bool rerun = again && p_ok;
	again = false;
	if (rerun) {
		callable_mp(this, &GitHubSync::sync_now).call_deferred();
	}
}

void GitHubSync::_sync_once(const std::function<void(RunResult)> &p_done) {
	std::shared_ptr<SyncRun> run = std::make_shared<SyncRun>();
	run->first_sync = base_commit.is_empty();
	run->done = p_done;
	_load_ignore_rules();

	client->get_ref(repo_owner, repo, branch, "", [this, run](const Dictionary &p_ref) {
		if (!_res_ok(p_ref)) {
			_log(vformat(TTR("Couldn't read branch '%s': %s"), branch, _res_error(p_ref)), LOG_ERROR);
			run->done(RUN_ERROR);
			return;
		}
		const Dictionary object = _res_data(p_ref).get("object", Dictionary());
		run->head_sha = object.get("sha", "");
		ref_etag = _res_headers(p_ref).get("etag", "");

		if (run->head_sha == base_commit) {
			// Nothing moved remotely: the remote tree is what we synchronized last time.
			run->remote = base;
			_run_plan(run);
		} else {
			_run_fetch_tree(run);
		}
	});
}

void GitHubSync::_run_fetch_tree(const std::shared_ptr<SyncRun> &p_run) {
	client->get_commit(repo_owner, repo, p_run->head_sha, [this, p_run](const Dictionary &p_commit) {
		if (!_res_ok(p_commit)) {
			_log(TTR("Couldn't read the remote commit: ") + _res_error(p_commit), LOG_ERROR);
			p_run->done(RUN_ERROR);
			return;
		}
		const Dictionary tree_ref = _res_data(p_commit).get("tree", Dictionary());
		p_run->tree_sha = tree_ref.get("sha", "");

		client->get_tree(repo_owner, repo, p_run->tree_sha, [this, p_run](const Dictionary &p_tree) {
			if (!_res_ok(p_tree)) {
				_log(TTR("Couldn't read the remote tree: ") + _res_error(p_tree), LOG_ERROR);
				p_run->done(RUN_ERROR);
				return;
			}
			const Dictionary tree = _res_data(p_tree);
			if ((bool)tree.get("truncated", false)) {
				_log(TTR("The repository is huge and GitHub returned an incomplete tree; some files won't be synchronized."), LOG_WARN);
			}
			const Array items = tree.get("tree", Array());
			for (const Variant &item_var : items) {
				const Dictionary item = item_var;
				if (String(item.get("type", "")) != "blob" || String(item.get("mode", "")) == "120000") {
					continue;
				}
				const String rel = item.get("path", "");
				if (rel.is_empty() || _remote_path_ignored(rel)) {
					continue;
				}
				if ((int64_t)item.get("size", 0) > MAX_FILE_BYTES) {
					_warn_big(rel);
					continue;
				}
				p_run->remote[rel] = item.get("sha", "");
			}
			_run_plan(p_run);
		});
	});
}

void GitHubSync::_run_plan(const std::shared_ptr<SyncRun> &p_run) {
	HashMap<String, String> local = _scan_local();
	if (p_run->first_sync && !p_run->remote.has(".gitignore") && !local.has(".gitignore")) {
		_write_file(".gitignore", _string_to_bytes(DEFAULT_GITIGNORE));
		_load_ignore_rules();
		local = _scan_local();
	}
	p_run->local = local;

	HashSet<String> all_paths;
	for (const KeyValue<String, String> &kv : base) {
		all_paths.insert(kv.key);
	}
	for (const KeyValue<String, String> &kv : p_run->local) {
		all_paths.insert(kv.key);
	}
	for (const KeyValue<String, String> &kv : p_run->remote) {
		all_paths.insert(kv.key);
	}

	for (const String &p : all_paths) {
		const String b = _lookup(base, p);
		const String l = _lookup(p_run->local, p);
		const String r = _lookup(p_run->remote, p);
		if (l == r) {
			p_run->in_sync.push_back(p);
		} else if (l == b) {
			// Changed remotely.
			if (r.is_empty()) {
				p_run->local_deletes.push_back(p);
			} else {
				Download d;
				d.path = p;
				d.sha = r;
				p_run->downloads.push_back(d);
			}
		} else if (r == b) {
			// Changed locally.
			p_run->pushes[p] = l;
		} else {
			// Both sides changed.
			p_run->conflicts.push_back(p);
			if (p_run->first_sync) {
				// First connection: the repository wins, the local copy is kept aside.
				Download d;
				d.path = p;
				d.sha = r;
				d.save_local = true;
				p_run->downloads.push_back(d);
			} else {
				p_run->pushes[p] = l;
				if (!r.is_empty()) {
					Download d;
					d.path = p;
					d.sha = r;
					d.apply = false;
					d.save_remote = true;
					p_run->downloads.push_back(d);
				}
			}
		}
	}

	_run_download(p_run);
}

void GitHubSync::_run_parallel(int p_count, const std::function<void(int, std::function<void()>)> &p_worker, const std::function<void()> &p_on_done) {
	if (p_count <= 0) {
		p_on_done();
		return;
	}

	struct PoolState {
		int total = 0;
		int next = 0;
		int running = 0;
		int completed = 0;
		std::function<void(int, std::function<void()>)> worker;
		std::function<void()> on_done;
		std::function<void()> launch;
	};

	std::shared_ptr<PoolState> state = std::make_shared<PoolState>();
	state->total = p_count;
	state->worker = p_worker;
	state->on_done = p_on_done;

	// `launch` only holds a weak reference to the state (in-flight jobs hold the strong ones),
	// so there is no reference cycle.
	std::weak_ptr<PoolState> weak = state;
	state->launch = [weak]() {
		std::shared_ptr<PoolState> s = weak.lock();
		if (!s) {
			return;
		}
		while (s->running < PARALLEL_REQUESTS && s->next < s->total) {
			const int index = s->next++;
			s->running++;
			s->worker(index, [s]() {
				s->running--;
				s->completed++;
				if (s->completed == s->total) {
					std::function<void()> finished = s->on_done;
					finished();
				} else {
					s->launch();
				}
			});
		}
	};
	state->launch();
}

void GitHubSync::_run_download(const std::shared_ptr<SyncRun> &p_run) {
	if (!p_run->downloads.is_empty()) {
		_emit_status(vformat(TTR("Downloading %d file(s)..."), p_run->downloads.size()), STATUS_SYNCING);
	}

	_run_parallel(
			p_run->downloads.size(),
			[this, p_run](int p_index, std::function<void()> p_done) {
				if (p_run->failed) {
					p_done();
					return;
				}
				const String path = p_run->downloads[p_index].path;
				const String sha = p_run->downloads[p_index].sha;
				client->get_blob(repo_owner, repo, sha, [this, p_run, path, sha, p_done](const Dictionary &p_res) {
					if (!_res_ok(p_res)) {
						p_run->failed = true;
						_log(vformat(TTR("Couldn't download %s: %s"), path, _res_error(p_res)), LOG_ERROR);
						p_done();
						return;
					}
					const Vector<uint8_t> bytes = _decode_blob(_res_data(p_res));
					if (git_blob_sha(bytes.ptr(), bytes.size()) != sha) {
						p_run->failed = true;
						_log(vformat(TTR("The download of %s arrived corrupted."), path), LOG_ERROR);
						p_done();
						return;
					}
					p_run->fetched[path] = bytes;
					p_done();
				});
			},
			[this, p_run]() {
				if (p_run->failed) {
					p_run->done(RUN_ERROR);
					return;
				}
				_run_apply(p_run);
			});
}

void GitHubSync::_run_apply(const std::shared_ptr<SyncRun> &p_run) {
	const String stamp = Time::get_singleton()->get_datetime_string_from_system().replace(":", "-").replace("T", "_");

	for (const Download &job : p_run->downloads) {
		const Vector<uint8_t> *bytes = p_run->fetched.getptr(job.path);
		if (!bytes) {
			continue;
		}
		if (job.save_local) {
			_save_conflict_copy(job.path, FileAccess::get_file_as_bytes(String("res://") + job.path), stamp, "local");
		}
		if (job.save_remote) {
			_save_conflict_copy(job.path, *bytes, stamp, "remote");
		}
		if (job.apply) {
			if (!_write_file(job.path, *bytes)) {
				p_run->done(RUN_ERROR);
				return;
			}
			base[job.path] = job.sha;
			pending_refresh.push_back(job.path);
		}
	}
	for (const String &rel : p_run->local_deletes) {
		DirAccess::remove_absolute(String("res://") + rel);
		_remove_empty_parents(rel);
		base.erase(rel);
		cache.erase(rel);
		pending_refresh.push_back(rel);
	}
	for (const String &rel : p_run->in_sync) {
		const String l = _lookup(p_run->local, rel);
		if (l.is_empty()) {
			base.erase(rel);
		} else {
			base[rel] = l;
		}
	}

	if (!p_run->conflicts.is_empty()) {
		String shown;
		for (int i = 0; i < p_run->conflicts.size() && i < 5; i++) {
			shown += (i > 0 ? ", " : "") + p_run->conflicts[i];
		}
		const String how = p_run->first_sync ? TTR("the repository version is used and yours is kept in .godotgit/conflicts/") : TTR("your version is kept and the remote one is saved in .godotgit/conflicts/");
		_log(vformat(TTR("%d conflict(s): %s (%s)"), p_run->conflicts.size(), how, shown), LOG_WARN);
	}

	int pulled = 0;
	for (const Download &job : p_run->downloads) {
		if (job.apply) {
			pulled++;
		}
	}
	if (pulled > 0 || !p_run->local_deletes.is_empty()) {
		_log(vformat(TTR("Received from GitHub: %d new/modified file(s), %d deleted."), pulled, p_run->local_deletes.size()), LOG_INFO);
		activity_dirty = true;
	}

	_run_push(p_run);
}

void GitHubSync::_run_push(const std::shared_ptr<SyncRun> &p_run) {
	if (p_run->pushes.is_empty()) {
		if (base_commit != p_run->head_sha) {
			activity_dirty = true;
		}
		base_commit = p_run->head_sha;
		p_run->done(RUN_OK);
		return;
	}

	_emit_status(vformat(TTR("Uploading %d file(s)..."), p_run->pushes.size()), STATUS_SYNCING);
	const String message = _make_commit_message(p_run);

	Vector<String> blob_paths;
	for (const KeyValue<String, String> &kv : p_run->pushes) {
		if (!kv.value.is_empty()) {
			blob_paths.push_back(kv.key);
		}
	}

	_run_parallel(
			blob_paths.size(),
			[this, p_run, blob_paths](int p_index, std::function<void()> p_done) {
				if (p_run->failed) {
					p_done();
					return;
				}
				const String rel = blob_paths[p_index];
				const Vector<uint8_t> file_bytes = FileAccess::get_file_as_bytes(String("res://") + rel);
				client->create_blob(repo_owner, repo, file_bytes, [this, p_run, rel, p_done](const Dictionary &p_res) {
					if (!_res_ok(p_res)) {
						p_run->failed = true;
						_log(vformat(TTR("Couldn't upload %s: %s"), rel, _res_error(p_res)), LOG_ERROR);
					} else {
						p_run->blob_shas[rel] = _res_data(p_res).get("sha", "");
					}
					p_done();
				});
			},
			[this, p_run, message]() {
				if (p_run->failed) {
					p_run->done(RUN_ERROR);
					return;
				}
				if (!p_run->tree_sha.is_empty()) {
					_run_push_commit(p_run, message);
					return;
				}
				// The remote tree was not fetched (nothing moved): we still need its SHA as the base tree.
				client->get_commit(repo_owner, repo, p_run->head_sha, [this, p_run, message](const Dictionary &p_commit) {
					if (!_res_ok(p_commit)) {
						_log(TTR("Couldn't read the remote commit: ") + _res_error(p_commit), LOG_ERROR);
						p_run->done(RUN_ERROR);
						return;
					}
					const Dictionary tree_ref = _res_data(p_commit).get("tree", Dictionary());
					p_run->tree_sha = tree_ref.get("sha", "");
					_run_push_commit(p_run, message);
				});
			});
}

void GitHubSync::_run_push_commit(const std::shared_ptr<SyncRun> &p_run, const String &p_message) {
	Array entries;
	for (const KeyValue<String, String> &kv : p_run->pushes) {
		Dictionary entry;
		entry["path"] = kv.key;
		entry["mode"] = "100644";
		entry["type"] = "blob";
		if (kv.value.is_empty()) {
			entry["sha"] = Variant(); // null = delete.
		} else {
			entry["sha"] = _lookup(p_run->blob_shas, kv.key);
		}
		entries.push_back(entry);
	}

	client->create_tree(repo_owner, repo, p_run->tree_sha, entries, [this, p_run, p_message](const Dictionary &p_tree) {
		if (!_res_ok(p_tree)) {
			_log(TTR("Couldn't create the tree: ") + _res_error(p_tree), LOG_ERROR);
			p_run->done(RUN_ERROR);
			return;
		}
		Array parents;
		parents.push_back(p_run->head_sha);
		client->create_commit(repo_owner, repo, p_message, String(_res_data(p_tree).get("sha", "")), parents, [this, p_run, p_message](const Dictionary &p_commit) {
			if (!_res_ok(p_commit)) {
				_log(TTR("Couldn't create the commit: ") + _res_error(p_commit), LOG_ERROR);
				p_run->done(RUN_ERROR);
				return;
			}
			const String new_sha = _res_data(p_commit).get("sha", "");
			client->update_ref(repo_owner, repo, branch, new_sha, [this, p_run, p_message, new_sha](const Dictionary &p_moved) {
				if (!_res_ok(p_moved)) {
					const int code = _res_code(p_moved);
					if (code == 422 || code == 409) {
						_log(TTR("Somebody pushed at the same time; syncing again..."), LOG_INFO);
						p_run->done(RUN_RETRY);
					} else {
						_log(TTR("Couldn't update the branch: ") + _res_error(p_moved), LOG_ERROR);
						p_run->done(RUN_ERROR);
					}
					return;
				}
				for (const KeyValue<String, String> &kv : p_run->pushes) {
					if (kv.value.is_empty()) {
						base.erase(kv.key);
					} else {
						base[kv.key] = kv.value;
					}
				}
				base_commit = new_sha;
				ref_etag = "";
				activity_dirty = true;
				_log(vformat(TTR("Uploaded to GitHub: %s (%s)"), p_message.get_slicec('\n', 0), new_sha.substr(0, 7)), LOG_INFO);
				p_run->done(RUN_OK);
			});
		});
	});
}

String GitHubSync::_make_commit_message(const std::shared_ptr<SyncRun> &p_run) const {
	Vector<String> lines;
	for (const KeyValue<String, String> &kv : p_run->pushes) {
		String verb = "Update";
		if (kv.value.is_empty()) {
			verb = "Delete";
		} else if (!base.has(kv.key)) {
			verb = "Add";
		}
		lines.push_back(verb + " " + kv.key);
	}
	lines.sort();
	if (lines.size() == 1) {
		return "[Godot] " + lines[0];
	}
	String body;
	const int shown = MIN(lines.size(), 60);
	for (int i = 0; i < shown; i++) {
		body += (i > 0 ? "\n" : "") + lines[i];
	}
	if (lines.size() > shown) {
		body += vformat("\n... and %d more", lines.size() - shown);
	}
	return vformat("[Godot] %d changes in the project\n\n%s", lines.size(), body);
}

// ============================================================ Local files

bool GitHubSync::_local_dirty() {
	_load_ignore_rules();
	const HashMap<String, String> local = _scan_local();
	if (local.size() != base.size()) {
		return true;
	}
	for (const KeyValue<String, String> &kv : local) {
		if (_lookup(base, kv.key) != kv.value) {
			return true;
		}
	}
	return false;
}

HashMap<String, String> GitHubSync::_scan_local() {
	HashMap<String, String> result;
	_scan_dir("res://", result);

	Vector<String> stale;
	for (const KeyValue<String, CacheEntry> &kv : cache) {
		if (!result.has(kv.key)) {
			stale.push_back(kv.key);
		}
	}
	for (const String &key : stale) {
		cache.erase(key);
	}
	return result;
}

void GitHubSync::_scan_dir(const String &p_dir, HashMap<String, String> &r_result) {
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}
	dir->set_include_hidden(true);
	dir->set_include_navigational(false);
	const double now = OS::get_singleton()->get_unix_time();

	Vector<String> subdirs;
	Vector<String> files;
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (entry == "." || entry == "..") {
			// Never follow navigational entries, whatever the DirAccess flags say.
		} else if (dir->current_is_dir()) {
			subdirs.push_back(entry);
		} else {
			files.push_back(entry);
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();

	for (const String &name : files) {
		const String full = p_dir.path_join(name);
		const String rel = full.trim_prefix("res://");
		if (_path_ignored(rel, false)) {
			continue;
		}
		const uint64_t mtime = FileAccess::get_modified_time(full);
		String sha;
		const CacheEntry *cached = cache.getptr(rel);
		// Files modified in the last 2 seconds may still be changing: always re-hash them.
		if (cached && cached->mtime == mtime && now - (double)mtime > 2.0) {
			sha = cached->sha;
		} else {
			Ref<FileAccess> f = FileAccess::open(full, FileAccess::READ);
			if (f.is_null()) {
				continue;
			}
			const int64_t size = f->get_length();
			if (size > MAX_FILE_BYTES) {
				_warn_big(rel);
				continue;
			}
			Vector<uint8_t> bytes;
			bytes.resize(size);
			if (size > 0) {
				f->get_buffer(bytes.ptrw(), size);
			}
			sha = git_blob_sha(bytes.ptr(), size);
			CacheEntry c;
			c.mtime = mtime;
			c.size = size;
			c.sha = sha;
			cache[rel] = c;
		}
		r_result[rel] = sha;
	}

	for (const String &name : subdirs) {
		const String full = p_dir.path_join(name);
		if (!_path_ignored(full.trim_prefix("res://"), true)) {
			_scan_dir(full, r_result);
		}
	}
}

bool GitHubSync::_write_file(const String &p_rel, const Vector<uint8_t> &p_bytes) {
	const String path = String("res://") + p_rel;
	const String dir = path.get_base_dir();
	if (!DirAccess::dir_exists_absolute(dir)) {
		DirAccess::make_dir_recursive_absolute(dir);
	}
	Error err;
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE, &err);
	if (f.is_null()) {
		_log(vformat(TTR("Couldn't write %s (error %d)"), p_rel, (int)err), LOG_ERROR);
		return false;
	}
	if (p_bytes.size() > 0) {
		f->store_buffer(p_bytes.ptr(), p_bytes.size());
	}
	f.unref();

	CacheEntry c;
	c.mtime = FileAccess::get_modified_time(path);
	c.size = p_bytes.size();
	c.sha = git_blob_sha(p_bytes.ptr(), p_bytes.size());
	cache[p_rel] = c;
	return true;
}

void GitHubSync::_save_conflict_copy(const String &p_rel, const Vector<uint8_t> &p_bytes, const String &p_stamp, const String &p_tag) {
	const String path = vformat("res://%s/conflicts/%s/%s/%s", DATA_DIR_NAME, p_stamp, p_tag, p_rel);
	DirAccess::make_dir_recursive_absolute(path.get_base_dir());
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_valid() && p_bytes.size() > 0) {
		f->store_buffer(p_bytes.ptr(), p_bytes.size());
	}
}

void GitHubSync::_remove_empty_parents(const String &p_rel) {
	String dir_rel = p_rel.get_base_dir();
	while (!dir_rel.is_empty()) {
		Ref<DirAccess> dir = DirAccess::open(String("res://") + dir_rel);
		if (dir.is_null()) {
			return;
		}
		dir->set_include_hidden(true);
		if (!dir->get_files().is_empty() || !dir->get_directories().is_empty()) {
			return;
		}
		dir.unref();
		DirAccess::remove_absolute(String("res://") + dir_rel);
		dir_rel = dir_rel.get_base_dir();
	}
}

void GitHubSync::_warn_big(const String &p_rel) {
	if (!warned_big.has(p_rel)) {
		warned_big.insert(p_rel);
		_log(vformat(TTR("'%s' is larger than 50 MB and is not synchronized (use Git LFS or exclude it in .gitignore)."), p_rel), LOG_WARN);
	}
}

void GitHubSync::_flush_editor_refresh() {
	if (pending_refresh.is_empty()) {
		return;
	}
	const Vector<String> paths = pending_refresh;
	pending_refresh.clear();

	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (efs && !efs->is_scanning()) {
		efs->scan();
	}

	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return;
	}
	const PackedStringArray open_scenes = ei->get_open_scenes();
	const PackedStringArray unsaved = ei->get_unsaved_scenes();
	for (const String &rel : paths) {
		const String res_path = String("res://") + rel;
		if (open_scenes.has(res_path) && FileAccess::exists(res_path)) {
			if (unsaved.has(res_path)) {
				_log(vformat(TTR("'%s' changed on GitHub but you have unsaved changes in that scene. Save it or reload it manually."), rel), LOG_WARN);
			} else {
				ei->reload_scene_from_path(res_path);
			}
		}
	}
	ScriptEditor *script_editor = ScriptEditor::get_singleton();
	if (script_editor) {
		script_editor->reload_open_files();
	}
}

// ============================================================ .gitignore

void GitHubSync::_load_ignore_rules() {
	ignore_rules.clear();
	const String path = "res://.gitignore";
	if (!FileAccess::exists(path)) {
		return;
	}
	const PackedStringArray lines = FileAccess::get_file_as_string(path).split("\n");
	for (const String &raw : lines) {
		String line = raw.strip_edges();
		if (line.is_empty() || line.begins_with("#") || line.begins_with("!")) {
			continue;
		}
		IgnoreRule rule;
		rule.dir_only = line.ends_with("/");
		if (rule.dir_only) {
			line = line.trim_suffix("/");
		}
		rule.anchored = line.contains("/");
		line = line.trim_prefix("/");
		if (line.is_empty()) {
			continue;
		}
		rule.pattern = line;
		ignore_rules.push_back(rule);
	}
}

// Only the last path segment is evaluated (the parents were already filtered while walking).
bool GitHubSync::_path_ignored(const String &p_rel, bool p_is_dir) const {
	const String leaf = p_rel.get_file();
	if (p_is_dir) {
		for (int i = 0; ALWAYS_IGNORED_DIRS[i]; i++) {
			if (leaf == ALWAYS_IGNORED_DIRS[i]) {
				return true;
			}
		}
	} else {
		for (int i = 0; ALWAYS_IGNORED_FILES[i]; i++) {
			if (leaf == ALWAYS_IGNORED_FILES[i]) {
				return true;
			}
		}
		if (leaf.ends_with(".tmp") || leaf.ends_with(".TMP")) {
			return true;
		}
	}
	for (const IgnoreRule &rule : ignore_rules) {
		if (rule.dir_only && !p_is_dir) {
			continue;
		}
		if (rule.anchored ? p_rel.match(rule.pattern) : leaf.match(rule.pattern)) {
			return true;
		}
	}
	return false;
}

bool GitHubSync::_remote_path_ignored(const String &p_rel) const {
	const PackedStringArray parts = p_rel.split("/");
	String acc;
	for (int i = 0; i < parts.size(); i++) {
		acc = acc.is_empty() ? parts[i] : acc + "/" + parts[i];
		if (_path_ignored(acc, i < parts.size() - 1)) {
			return true;
		}
	}
	return false;
}

// ============================================================ Utilities

void GitHubSync::_emit_status(const String &p_text, Status p_status) {
	print_verbose("[GitHub sync] status: " + p_text);
	emit_signal("status_changed", p_text, (int)p_status);
}

void GitHubSync::_log(const String &p_text, LogLevel p_level) {
	emit_signal("log_message", p_text, (int)p_level);
	print_line("[GitHub sync] " + p_text);
}
