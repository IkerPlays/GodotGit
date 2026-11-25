/**************************************************************************/
/*  github_sync_dock.h                                                    */
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

#include "scene/gui/box_container.h"

class Button;
class CheckBox;
class GitHubSync;
class HBoxContainer;
class Label;
class LineEdit;
class LinkButton;
class OptionButton;
class RichTextLabel;
class SpinBox;

// Bottom panel "GitHub": connect a repository, see the sync status, the team activity and the log.
class GitHubSyncDock : public VBoxContainer {
	GDCLASS(GitHubSyncDock, VBoxContainer);

	GitHubSync *sync = nullptr;

	// "Not connected" view.
	VBoxContainer *connect_box = nullptr;
	Label *account_label = nullptr;
	Button *signin_button = nullptr;
	Button *signout_button = nullptr;
	HBoxContainer *code_row = nullptr;
	Label *code_label = nullptr;
	String current_code;
	String current_uri;
	OptionButton *repo_picker = nullptr;
	HBoxContainer *token_row = nullptr;
	Button *advanced_button = nullptr;
	LineEdit *token_edit = nullptr;
	LineEdit *repo_edit = nullptr;
	LineEdit *branch_edit = nullptr;
	LineEdit *new_name_edit = nullptr;
	CheckBox *private_check = nullptr;
	Button *connect_button = nullptr;
	Button *create_button = nullptr;
	Label *connect_message = nullptr;

	// "Connected" view.
	HBoxContainer *connected_box = nullptr;
	Label *repo_label = nullptr;
	Label *status_label = nullptr;
	Button *sync_button = nullptr;
	CheckBox *auto_check = nullptr;
	SpinBox *poll_spin = nullptr;
	LineEdit *invite_edit = nullptr;
	RichTextLabel *activity = nullptr;
	RichTextLabel *log = nullptr;

	void _build_connect_box();
	void _build_connected_box();
	void _set_connecting(bool p_active);
	void _show_error(const String &p_error);

	void _signin_pressed();
	void _signout_pressed();
	void _cancel_login_pressed();
	void _copy_code_pressed();
	void _open_code_page_pressed();
	void _toggle_advanced();
	void _refresh_repos();
	void _repo_picked(int p_index);
	void _update_account_ui();

	void _on_device_code(const String &p_code, const String &p_uri);
	void _on_account_changed(const String &p_login);

	void _connect_pressed();
	void _create_pressed();
	void _open_on_github();
	void _invite_pressed();
	void _auto_toggled(bool p_pressed);
	void _poll_changed(double p_value);

	void _on_connection_changed(bool p_connected);
	void _on_status_changed(const String &p_text, int p_status);
	void _on_log(const String &p_text, int p_level);
	void _on_activity(const Array &p_entries);

protected:
	void _notification(int p_what);

public:
	void set_sync(GitHubSync *p_sync) { sync = p_sync; }
	void build();
	void select_connect_tab() {}

	GitHubSyncDock();
};
