/**************************************************************************/
/*  github_sync_plugin.cpp                                                */
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

#include "github_sync_plugin.h"

#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/github_sync/github_sync.h"
#include "editor/github_sync/github_sync_dock.h"
#include "editor/gui/editor_bottom_panel.h"
#include "scene/gui/button.h"

GitHubSyncEditorPlugin::GitHubSyncEditorPlugin() {
	sync = memnew(GitHubSync);
	add_child(sync);

	dock = memnew(GitHubSyncDock);
	dock->set_sync(sync);
	dock->build();

	toolbar_button = memnew(Button);
	toolbar_button->set_flat(true);
	toolbar_button->set_text(TTR("GitHub") + String::utf8(" ○"));
	toolbar_button->set_tooltip_text(TTR("GitHub sync status (click to open the panel)"));
	toolbar_button->connect(SceneStringName(pressed), callable_mp(this, &GitHubSyncEditorPlugin::_toolbar_pressed));

	sync->connect("status_changed", callable_mp(this, &GitHubSyncEditorPlugin::_on_status_changed));
	connect("scene_saved", callable_mp(this, &GitHubSyncEditorPlugin::_on_scene_saved));
	connect("resource_saved", callable_mp(this, &GitHubSyncEditorPlugin::_on_resource_saved));
}

void GitHubSyncEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			EditorNode::get_bottom_panel()->add_item(TTR("GitHub"), dock);
			add_control_to_container(CONTAINER_TOOLBAR, toolbar_button);
			// Let the editor finish starting up before touching the network and the filesystem.
			callable_mp(sync, &GitHubSync::start).call_deferred();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			sync->shutdown();
			EditorNode::get_bottom_panel()->remove_item(dock);
			remove_control_from_container(CONTAINER_TOOLBAR, toolbar_button);
		} break;
	}
}

void GitHubSyncEditorPlugin::_toolbar_pressed() {
	EditorNode::get_bottom_panel()->make_item_visible(dock);
}

void GitHubSyncEditorPlugin::_on_scene_saved(const String &p_path) {
	sync->notify_local_change();
}

void GitHubSyncEditorPlugin::_on_resource_saved(const Ref<Resource> &p_resource) {
	sync->notify_local_change();
}

void GitHubSyncEditorPlugin::_on_status_changed(const String &p_text, int p_status) {
	String icon = String::utf8("○");
	switch (p_status) {
		case GitHubSync::STATUS_IDLE: {
			icon = String::utf8("●");
		} break;
		case GitHubSync::STATUS_SYNCING: {
			icon = String::utf8("◐");
		} break;
		case GitHubSync::STATUS_ERROR: {
			icon = String::utf8("▲");
		} break;
		default: {
		} break;
	}
	toolbar_button->set_text(TTR("GitHub") + " " + icon);
	toolbar_button->set_tooltip_text(p_text);
}
