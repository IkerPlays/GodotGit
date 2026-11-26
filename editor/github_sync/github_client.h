/**************************************************************************/
/*  github_client.h                                                       */
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
#include "core/variant/dictionary.h"
#include "scene/main/http_request.h"
#include "scene/main/node.h"

#include <functional>

// Minimal client for the GitHub REST API (only what GitHubSync needs).
//
// Every request is asynchronous and always completes from the main loop (never
// synchronously from inside `request()`), through a callback that receives:
//   {
//     "ok": bool,            // HTTP status is 2xx
//     "code": int,           // HTTP status (0 when the network failed)
//     "data": Variant,       // parsed JSON body (Dictionary / Array / null)
//     "headers": Dictionary, // response headers, keys in lowercase
//     "error": String,       // human readable error ("" when ok)
//   }
class GitHubClient : public Node {
	GDCLASS(GitHubClient, Node);

public:
	using Callback = std::function<void(const Dictionary &)>;

private:
	struct Pending {
		HTTPRequest *http = nullptr;
		Callback callback;
		bool authenticated = false;
	};

	String token;
	int next_id = 1;
	HashMap<int, Pending> pending;

	void _request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body, int p_id);
	void _fail_deferred(int p_id, const String &p_message);

	void _send(HTTPClient::Method p_method, const String &p_url, const Variant &p_body, const Callback &p_callback, const Vector<String> &p_extra_headers, bool p_authenticated);
	static Dictionary _make_result(bool p_ok, int p_code, const Variant &p_data, const Dictionary &p_headers, const String &p_error);

protected:
	static void _bind_methods();

public:
	static String repo_path(const String &p_owner, const String &p_repo);
	static String encode_path(const String &p_path);

	void set_token(const String &p_token) { token = p_token; }
	String get_token() const { return token; }

	void request(HTTPClient::Method p_method, const String &p_path, const Variant &p_body, const Callback &p_callback, const Vector<String> &p_extra_headers = Vector<String>());

	// User / repository.
	void get_user(const Callback &p_cb);
	void get_repo(const String &p_owner, const String &p_repo, const Callback &p_cb);
	void create_repo(const String &p_name, bool p_private, const String &p_description, const Callback &p_cb);
	void list_branches(const String &p_owner, const String &p_repo, const Callback &p_cb);
	void add_collaborator(const String &p_owner, const String &p_repo, const String &p_username, const Callback &p_cb);
	void list_commits(const String &p_owner, const String &p_repo, const String &p_branch, int p_per_page, const Callback &p_cb);

	// OAuth device flow: lets the user sign in with their GitHub account, no token to paste.
	void request_device_code(const String &p_client_id, const Callback &p_cb);
	void poll_device_token(const String &p_client_id, const String &p_device_code, const Callback &p_cb);
	void list_user_repos(const Callback &p_cb);

	// Git Data API.
	void get_ref(const String &p_owner, const String &p_repo, const String &p_branch, const String &p_etag, const Callback &p_cb);
	void create_ref(const String &p_owner, const String &p_repo, const String &p_branch, const String &p_sha, const Callback &p_cb);
	void update_ref(const String &p_owner, const String &p_repo, const String &p_branch, const String &p_sha, const Callback &p_cb);
	void get_commit(const String &p_owner, const String &p_repo, const String &p_sha, const Callback &p_cb);
	void get_tree(const String &p_owner, const String &p_repo, const String &p_sha, const Callback &p_cb);
	void get_blob(const String &p_owner, const String &p_repo, const String &p_sha, const Callback &p_cb);
	void create_blob(const String &p_owner, const String &p_repo, const Vector<uint8_t> &p_content, const Callback &p_cb);
	void create_tree(const String &p_owner, const String &p_repo, const String &p_base_tree, const Array &p_entries, const Callback &p_cb);
	void create_commit(const String &p_owner, const String &p_repo, const String &p_message, const String &p_tree, const Array &p_parents, const Callback &p_cb);

	// Contents API: the only way to create the first commit of a completely empty repository.
	void put_file(const String &p_owner, const String &p_repo, const String &p_path, const Vector<uint8_t> &p_content, const String &p_message, const Callback &p_cb);
};
