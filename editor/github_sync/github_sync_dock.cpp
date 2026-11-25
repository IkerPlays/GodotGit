/**************************************************************************/
/*  github_sync_dock.cpp                                                  */
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

#include "github_sync_dock.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/editor_string_names.h"
#include "editor/github_sync/github_sync.h"
#include "editor/themes/editor_scale.h"
#include "editor/themes/editor_theme_manager.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/link_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/spin_box.h"
#include "servers/display/display_server.h"

static Label *_make_label(const String &p_text) {
	Label *label = memnew(Label);
	label->set_text(p_text);
	return label;
}

static RichTextLabel *_make_rich_text() {
	RichTextLabel *rich = memnew(RichTextLabel);
	rich->set_use_bbcode(true);
	rich->set_scroll_follow(true);
	rich->set_selection_enabled(true);
	rich->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	rich->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	return rich;
}

// Square brackets must not be interpreted as BBCode tags.
static String _escape_bbcode(const String &p_text) {
	return p_text.replace("[", "[lb]");
}

GitHubSyncDock::GitHubSyncDock() {
	set_name(TTRC("GitHub"));
	set_custom_minimum_size(Size2(0, 230 * EDSCALE));
	set_v_size_flags(Control::SIZE_EXPAND_FILL);
}

void GitHubSyncDock::build() {
	_build_connect_box();
	_build_connected_box();

	sync->connect("connection_changed", callable_mp(this, &GitHubSyncDock::_on_connection_changed));
	sync->connect("status_changed", callable_mp(this, &GitHubSyncDock::_on_status_changed));
	sync->connect("log_message", callable_mp(this, &GitHubSyncDock::_on_log));
	sync->connect("activity_updated", callable_mp(this, &GitHubSyncDock::_on_activity));
	sync->connect("device_code", callable_mp(this, &GitHubSyncDock::_on_device_code));
	sync->connect("account_changed", callable_mp(this, &GitHubSyncDock::_on_account_changed));
	_on_connection_changed(sync->is_repo_connected());
	_update_account_ui();
}

void GitHubSyncDock::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED && connect_message) {
		connect_message->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
	}
}

// ================================================================ Construction

void GitHubSyncDock::_build_connect_box() {
	connect_box = memnew(VBoxContainer);
	connect_box->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	add_child(connect_box);

	Label *intro = memnew(Label);
	intro->set_text(TTR("Connect this project to GitHub: everything you create, save or delete is uploaded automatically, and what your collaborators do shows up here within seconds."));
	intro->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	connect_box->add_child(intro);

	// Step 1: GitHub account.
	HBoxContainer *account_row = memnew(HBoxContainer);
	connect_box->add_child(account_row);
	account_label = memnew(Label);
	account_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	account_row->add_child(account_label);
	signin_button = memnew(Button);
	signin_button->set_text(TTR("Sign in with GitHub"));
	signin_button->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncDock::_signin_pressed));
	account_row->add_child(signin_button);
	signout_button = memnew(Button);
	signout_button->set_text(TTR("Sign out"));
	signout_button->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncDock::_signout_pressed));
	account_row->add_child(signout_button);

	code_row = memnew(HBoxContainer);
	code_row->hide();
	connect_box->add_child(code_row);
	code_row->add_child(_make_label(TTR("Enter this code on github.com/login/device:")));
	code_label = memnew(Label);
	code_label->add_theme_font_size_override(SceneStringName(font_size), 22 * EDSCALE);
	code_row->add_child(code_label);
	Button *copy_code = memnew(Button);
	copy_code->set_text(TTR("Copy code"));
	copy_code->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncDock::_copy_code_pressed));
	code_row->add_child(copy_code);
	Button *open_page = memnew(Button);
	open_page->set_text(TTR("Open GitHub"));
	open_page->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncDock::_open_code_page_pressed));
	code_row->add_child(open_page);
	Button *cancel_login = memnew(Button);
	cancel_login->set_text(TTR("Cancel"));
	cancel_login->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncDock::_cancel_login_pressed));
	code_row->add_child(cancel_login);

	// Step 2: repository.
	GridContainer *grid = memnew(GridContainer);
	grid->set_columns(2);
	grid->add_theme_constant_override("h_separation", 12 * EDSCALE);
	connect_box->add_child(grid);

	grid->add_child(_make_label(TTR("Your repositories")));
	HBoxContainer *picker_row = memnew(HBoxContainer);
	picker_row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	grid->add_child(picker_row);
	repo_picker = memnew(OptionButton);
	repo_picker->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	repo_picker->set_fit_to_longest_item(false);
	repo_picker->add_item(TTR("Choose one of your repositories..."));
	repo_picker->connect(SceneStringName(item_selected), callable_mp(this, &GitHubSyncDock::_repo_picked));
	picker_row->add_child(repo_picker);
	Button *refresh_repos = memnew(Button);
	refresh_repos->set_text(TTR("Refresh"));
	refresh_repos->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncDock::_refresh_repos));
	picker_row->add_child(refresh_repos);

	grid->add_child(_make_label(TTR("Repository")));
	HBoxContainer *repo_row = memnew(HBoxContainer);
	repo_row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	grid->add_child(repo_row);
	repo_edit = memnew(LineEdit);
	repo_edit->set_placeholder(TTR("user/repository  or  https://github.com/user/repository"));
	repo_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	repo_row->add_child(repo_edit);
	branch_edit = memnew(LineEdit);
	branch_edit->set_placeholder(TTR("branch (optional)"));
	branch_edit->set_custom_minimum_size(Size2(130, 0) * EDSCALE);
	repo_row->add_child(branch_edit);
	connect_button = memnew(Button);
	connect_button->set_text(TTR("Connect"));
	connect_button->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncDock::_connect_pressed));
	repo_row->add_child(connect_button);

	grid->add_child(_make_label(TTR("or create a new one")));
	HBoxContainer *new_row = memnew(HBoxContainer);
	new_row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	grid->add_child(new_row);
	new_name_edit = memnew(LineEdit);
	new_name_edit->set_text(GitHubSync::sanitize_repo_name(GLOBAL_GET("application/config/name")));
	new_name_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	new_row->add_child(new_name_edit);
	private_check = memnew(CheckBox);
	private_check->set_text(TTR("Private"));
	private_check->set_pressed(true);
	new_row->add_child(private_check);
	create_button = memnew(Button);
	create_button->set_text(TTR("Create and connect"));
	create_button->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncDock::_create_pressed));
	new_row->add_child(create_button);

	connect_message = memnew(Label);
	connect_message->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	connect_box->add_child(connect_message);

	// Advanced: personal access token instead of the account sign-in.
	advanced_button = memnew(Button);
	advanced_button->set_flat(true);
	advanced_button->set_text(TTR("Use a personal access token instead"));
	advanced_button->set_h_size_flags(Control::SIZE_SHRINK_BEGIN);
	advanced_button->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncDock::_toggle_advanced));
	connect_box->add_child(advanced_button);
	token_row = memnew(HBoxContainer);
	token_row->hide();
	connect_box->add_child(token_row);
	token_edit = memnew(LineEdit);
	token_edit->set_secret(true);
	token_edit->set_placeholder(TTR("ghp_... or github_pat_... (only needed if you don't sign in)"));
	token_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	token_row->add_child(token_edit);
	LinkButton *token_link = memnew(LinkButton);
	token_link->set_text(TTR("Create token"));
	token_link->set_uri("https://github.com/settings/tokens/new?scopes=repo&description=Godot");
	token_row->add_child(token_link);
}

void GitHubSyncDock::_build_connected_box() {
	connected_box = memnew(HBoxContainer);
	connected_box->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	connected_box->add_theme_constant_override("separation", 14 * EDSCALE);
	add_child(connected_box);

	// Left column: controls.
	VBoxContainer *left = memnew(VBoxContainer);
	left->set_custom_minimum_size(Size2(320, 0) * EDSCALE);
	connected_box->add_child(left);

	repo_label = memnew(Label);
	left->add_child(repo_label);

	status_label = memnew(Label);
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	left->add_child(status_label);

	HBoxContainer *buttons = memnew(HBoxContainer);
	left->add_child(buttons);
	sync_button = memnew(Button);
	sync_button->set_text(TTR("Sync now"));
	sync_button->connect(SceneStringName(pressed), callable_mp(sync, &GitHubSync::sync_now));
	buttons->add_child(sync_button);
	Button *open_button = memnew(Button);
	open_button->set_text(TTR("Open on GitHub"));
	open_button->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncDock::_open_on_github));
	buttons->add_child(open_button);
	Button *disconnect_button = memnew(Button);
	disconnect_button->set_text(TTR("Disconnect"));
	disconnect_button->connect(SceneStringName(pressed), callable_mp(sync, &GitHubSync::disconnect_repo));
	buttons->add_child(disconnect_button);

	HBoxContainer *options = memnew(HBoxContainer);
	left->add_child(options);
	auto_check = memnew(CheckBox);
	auto_check->set_text(TTR("Sync automatically"));
	auto_check->set_pressed_no_signal(sync->is_auto_sync());
	auto_check->connect(SceneStringName(toggled), callable_mp(this, &GitHubSyncDock::_auto_toggled));
	options->add_child(auto_check);
	options->add_child(_make_label(TTR("every")));
	poll_spin = memnew(SpinBox);
	poll_spin->set_min(2);
	poll_spin->set_max(60);
	poll_spin->set_step(1);
	poll_spin->set_suffix("s");
	poll_spin->set_value_no_signal(sync->get_poll_seconds());
	poll_spin->connect(SceneStringName(value_changed), callable_mp(this, &GitHubSyncDock::_poll_changed));
	options->add_child(poll_spin);

	HBoxContainer *invite_row = memnew(HBoxContainer);
	left->add_child(invite_row);
	invite_edit = memnew(LineEdit);
	invite_edit->set_placeholder(TTR("GitHub user to invite"));
	invite_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	invite_edit->connect(SceneStringName(text_submitted), callable_mp(this, &GitHubSyncDock::_invite_pressed).unbind(1));
	invite_row->add_child(invite_edit);
	Button *invite_button = memnew(Button);
	invite_button->set_text(TTR("Invite"));
	invite_button->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncDock::_invite_pressed));
	invite_row->add_child(invite_button);

	// Middle: team activity.
	VBoxContainer *middle = memnew(VBoxContainer);
	middle->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	connected_box->add_child(middle);
	middle->add_child(_make_label(TTR("Team activity (latest commits)")));
	activity = _make_rich_text();
	middle->add_child(activity);

	// Right: log.
	VBoxContainer *right = memnew(VBoxContainer);
	right->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	connected_box->add_child(right);
	right->add_child(_make_label(TTR("Log")));
	log = _make_rich_text();
	right->add_child(log);
}

// ================================================================== Actions

void GitHubSyncDock::_set_connecting(bool p_active) {
	connect_button->set_disabled(p_active);
	create_button->set_disabled(p_active);
	if (p_active) {
		connect_message->set_text(TTR("Connecting..."));
	}
}

void GitHubSyncDock::_show_error(const String &p_error) {
	_set_connecting(false);
	connect_message->set_text(p_error);
}

void GitHubSyncDock::_signin_pressed() {
	connect_message->set_text(String());
	ObjectID id = get_instance_id();
	sync->begin_device_login([id, this](const String &p_error) {
		if (!ObjectDB::get_instance(id)) {
			return;
		}
		code_row->hide();
		connect_message->set_text(p_error);
		_update_account_ui();
	});
	_update_account_ui();
}

void GitHubSyncDock::_signout_pressed() {
	sync->sign_out();
}

void GitHubSyncDock::_cancel_login_pressed() {
	sync->cancel_device_login();
}

void GitHubSyncDock::_copy_code_pressed() {
	DisplayServer::get_singleton()->clipboard_set(current_code);
}

void GitHubSyncDock::_open_code_page_pressed() {
	OS::get_singleton()->shell_open(current_uri);
}

void GitHubSyncDock::_toggle_advanced() {
	token_row->set_visible(!token_row->is_visible());
}

void GitHubSyncDock::_refresh_repos() {
	ObjectID id = get_instance_id();
	sync->list_my_repos([id, this](const Vector<String> &p_names, const String &p_error) {
		if (!ObjectDB::get_instance(id)) {
			return;
		}
		repo_picker->clear();
		repo_picker->add_item(TTR("Choose one of your repositories..."));
		for (const String &name : p_names) {
			repo_picker->add_item(name);
		}
		repo_picker->select(0);
		if (!p_error.is_empty()) {
			connect_message->set_text(p_error);
		}
	});
}

void GitHubSyncDock::_repo_picked(int p_index) {
	if (p_index > 0) {
		repo_edit->set_text(repo_picker->get_item_text(p_index));
	}
}

void GitHubSyncDock::_update_account_ui() {
	const bool signed_in = !sync->get_account_login().is_empty();
	const bool in_progress = sync->is_login_in_progress();
	account_label->set_text(signed_in ? vformat(TTR("Signed in as %s"), sync->get_account_login()) : TTR("Not signed in to GitHub"));
	signin_button->set_visible(!signed_in);
	signin_button->set_disabled(in_progress);
	signout_button->set_visible(signed_in);
	code_row->set_visible(in_progress && !current_code.is_empty());
	repo_picker->set_disabled(!signed_in);
}

void GitHubSyncDock::_on_device_code(const String &p_code, const String &p_uri) {
	current_code = p_code;
	current_uri = p_uri;
	code_label->set_text(p_code);
	_update_account_ui();
}

void GitHubSyncDock::_on_account_changed(const String &p_login) {
	current_code = String();
	_update_account_ui();
	if (!p_login.is_empty()) {
		_refresh_repos();
	}
}

void GitHubSyncDock::_connect_pressed() {
	const Vector<String> parsed = GitHubSync::parse_repo(repo_edit->get_text());
	if (parsed.is_empty()) {
		connect_message->set_text(TTR("Write the repository as \"user/repository\" or paste its URL."));
		return;
	}
	_set_connecting(true);
	ObjectID id = get_instance_id();
	sync->connect_repo(parsed[0], parsed[1], branch_edit->get_text(), token_edit->get_text(), [id, this](const String &p_error) {
		if (ObjectDB::get_instance(id)) {
			_show_error(p_error);
		}
	});
}

void GitHubSyncDock::_create_pressed() {
	const String repo_name = new_name_edit->get_text().strip_edges();
	if (repo_name.is_empty()) {
		connect_message->set_text(TTR("Give the new repository a name."));
		return;
	}
	_set_connecting(true);
	ObjectID id = get_instance_id();
	sync->create_and_connect(repo_name, private_check->is_pressed(), token_edit->get_text(), [id, this](const String &p_error) {
		if (ObjectDB::get_instance(id)) {
			_show_error(p_error);
		}
	});
}

void GitHubSyncDock::_open_on_github() {
	OS::get_singleton()->shell_open(vformat("https://github.com/%s/%s/tree/%s", sync->get_repo_owner(), sync->get_repo_name(), sync->get_branch()));
}

void GitHubSyncDock::_invite_pressed() {
	const String who = invite_edit->get_text();
	invite_edit->clear();
	sync->invite(who);
}

void GitHubSyncDock::_auto_toggled(bool p_pressed) {
	sync->set_auto_sync(p_pressed);
}

void GitHubSyncDock::_poll_changed(double p_value) {
	sync->set_poll_seconds(p_value);
}

// ================================================================== Signals

void GitHubSyncDock::_on_connection_changed(bool p_connected) {
	connect_box->set_visible(!p_connected);
	connected_box->set_visible(p_connected);
	if (p_connected) {
		repo_label->set_text(vformat("%s/%s  -  %s  -  @%s", sync->get_repo_owner(), sync->get_repo_name(), sync->get_branch(), sync->get_user_login()));
		auto_check->set_pressed_no_signal(sync->is_auto_sync());
		poll_spin->set_value_no_signal(sync->get_poll_seconds());
		activity->clear();
		log->clear();
		connect_message->set_text(String());
	} else {
		_update_account_ui();
		if (sync->has_saved_config()) {
			repo_edit->set_text(sync->get_repo_owner() + "/" + sync->get_repo_name());
			branch_edit->set_text(sync->get_branch());
		}
		_set_connecting(false);
	}
}

void GitHubSyncDock::_on_status_changed(const String &p_text, int p_status) {
	status_label->set_text(p_text);
	Color color = get_theme_color(SNAME("success_color"), EditorStringName(Editor));
	switch (p_status) {
		case GitHubSync::STATUS_SYNCING: {
			color = get_theme_color(SNAME("warning_color"), EditorStringName(Editor));
		} break;
		case GitHubSync::STATUS_ERROR: {
			color = get_theme_color(SNAME("error_color"), EditorStringName(Editor));
		} break;
		case GitHubSync::STATUS_DISCONNECTED: {
			color = get_theme_color(SNAME("disabled_font_color"), EditorStringName(Editor));
		} break;
		default: {
		} break;
	}
	status_label->add_theme_color_override(SceneStringName(font_color), color);
	sync_button->set_disabled(p_status == GitHubSync::STATUS_SYNCING);
}

void GitHubSyncDock::_on_log(const String &p_text, int p_level) {
	Color color = get_theme_color(SceneStringName(font_color), SNAME("Label"));
	if (p_level == GitHubSync::LOG_WARN) {
		color = get_theme_color(SNAME("warning_color"), EditorStringName(Editor));
	} else if (p_level == GitHubSync::LOG_ERROR) {
		color = get_theme_color(SNAME("error_color"), EditorStringName(Editor));
	}
	const String stamp = Time::get_singleton()->get_time_string_from_system();
	log->append_text(vformat("[color=#808c99]%s[/color] [color=#%s]%s[/color]\n", stamp, color.to_html(false), _escape_bbcode(p_text)));
}

void GitHubSyncDock::_on_activity(const Array &p_entries) {
	activity->clear();
	for (const Variant &entry_var : p_entries) {
		const Dictionary entry = entry_var;
		const String date = String(entry.get("date", "")).replace("T", " ").replace("Z", "").left(16);
		activity->append_text(vformat("[color=#808c99]%s[/color]  [b]%s[/b]  %s  [color=#808c99]%s[/color]\n",
				date,
				_escape_bbcode(entry.get("author", "?")),
				_escape_bbcode(entry.get("message", "")),
				String(entry.get("sha", ""))));
	}
}
