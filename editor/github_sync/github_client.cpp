/**************************************************************************/
/*  github_client.cpp                                                     */
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

#include "github_client.h"

#include "core/crypto/crypto_core.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"

static const char *GITHUB_API_URL = "https://api.github.com";

// GODOT_GITHUB_API_URL overrides the API root (GitHub Enterprise: https://HOST/api/v3).
static String _api_url() {
	const String custom = OS::get_singleton()->get_environment("GODOT_GITHUB_API_URL").strip_edges();
	return custom.is_empty() ? String(GITHUB_API_URL) : custom.trim_suffix("/");
}
static const char *GITHUB_USER_AGENT = "GodotEngine-GitHubSync";

static String _base64_encode(const Vector<uint8_t> &p_data) {
	if (p_data.is_empty()) {
		return String();
	}
	return CryptoCore::b64_encode_str(p_data.ptr(), p_data.size());
}

Dictionary GitHubClient::_make_result(bool p_ok, int p_code, const Variant &p_data, const Dictionary &p_headers, const String &p_error) {
	Dictionary d;
	d["ok"] = p_ok;
	d["code"] = p_code;
	d["data"] = p_data;
	d["headers"] = p_headers;
	d["error"] = p_error;
	return d;
}

String GitHubClient::encode_path(const String &p_path) {
	PackedStringArray parts = p_path.split("/");
	PackedStringArray out;
	for (const String &part : parts) {
		out.push_back(part.uri_encode());
	}
	return String("/").join(out);
}

String GitHubClient::repo_path(const String &p_owner, const String &p_repo) {
	return vformat("/repos/%s/%s", p_owner.uri_encode(), p_repo.uri_encode());
}

void GitHubClient::request(HTTPClient::Method p_method, const String &p_path, const Variant &p_body, const Callback &p_callback, const Vector<String> &p_extra_headers) {
	HTTPRequest *http = memnew(HTTPRequest);
	http->set_accept_gzip(true);
	http->set_timeout(40.0);
	add_child(http);

	const int id = next_id++;
	Pending entry;
	entry.http = http;
	entry.callback = p_callback;
	pending[id] = entry;

	Vector<String> headers;
	headers.push_back("Accept: application/vnd.github+json");
	headers.push_back("X-GitHub-Api-Version: 2022-11-28");
	headers.push_back(String("User-Agent: ") + GITHUB_USER_AGENT);
	if (!token.is_empty()) {
		headers.push_back("Authorization: Bearer " + token);
	}
	headers.append_array(p_extra_headers);

	String payload;
	if (p_body.get_type() != Variant::NIL) {
		payload = JSON::stringify(p_body);
		headers.push_back("Content-Type: application/json");
	}

	http->connect("request_completed", callable_mp(this, &GitHubClient::_request_completed).bind(id));
	const Error err = http->request(_api_url() + p_path, headers, p_method, payload);
	if (err != OK) {
		callable_mp(this, &GitHubClient::_fail_deferred).bind(id, vformat(TTR("Could not start the request (error %d)."), (int)err)).call_deferred();
	}
}

void GitHubClient::_fail_deferred(int p_id, const String &p_message) {
	if (!pending.has(p_id)) {
		return;
	}
	Pending entry = pending[p_id];
	pending.erase(p_id);
	if (entry.http) {
		entry.http->queue_free();
	}
	if (entry.callback) {
		entry.callback(_make_result(false, 0, Variant(), Dictionary(), p_message));
	}
}

void GitHubClient::_request_completed(int p_result, int p_code, const PackedStringArray &p_headers, const PackedByteArray &p_body, int p_id) {
	if (!pending.has(p_id)) {
		return;
	}
	Pending entry = pending[p_id];
	pending.erase(p_id);
	if (entry.http) {
		entry.http->queue_free();
	}

	Dictionary result;
	if (p_result != HTTPRequest::RESULT_SUCCESS) {
		result = _make_result(false, 0, Variant(), Dictionary(), vformat(TTR("Network error (code %d). Is there an internet connection?"), p_result));
	} else {
		Dictionary headers;
		for (const String &line : p_headers) {
			const int idx = line.find_char(':');
			if (idx > 0) {
				headers[line.substr(0, idx).strip_edges().to_lower()] = line.substr(idx + 1).strip_edges();
			}
		}

		Variant parsed;
		if (p_body.size() > 0) {
			parsed = JSON::parse_string(String::utf8((const char *)p_body.ptr(), p_body.size()));
		}

		const bool ok = p_code >= 200 && p_code < 300;
		String message;
		if (!ok) {
			if (parsed.get_type() == Variant::DICTIONARY && Dictionary(parsed).has("message")) {
				message = String(Dictionary(parsed)["message"]);
			} else {
				message = vformat("HTTP %d", p_code);
			}
			if (p_code == 401) {
				message = TTR("Invalid or expired token (401).");
			} else if (p_code == 403 && String(headers.get("x-ratelimit-remaining", "1")) == "0") {
				message = TTR("GitHub API rate limit reached. Wait a few minutes.");
			}
		}
		result = _make_result(ok, p_code, parsed, headers, message);
	}

	if (entry.callback) {
		entry.callback(result);
	}
}

// ---------------------------------------------------------------- User / repo

void GitHubClient::get_user(const Callback &p_cb) {
	request(HTTPClient::METHOD_GET, "/user", Variant(), p_cb);
}

void GitHubClient::get_repo(const String &p_owner, const String &p_repo, const Callback &p_cb) {
	request(HTTPClient::METHOD_GET, repo_path(p_owner, p_repo), Variant(), p_cb);
}

void GitHubClient::create_repo(const String &p_name, bool p_private, const String &p_description, const Callback &p_cb) {
	Dictionary body;
	body["name"] = p_name;
	body["description"] = p_description;
	body["private"] = p_private;
	body["auto_init"] = true;
	request(HTTPClient::METHOD_POST, "/user/repos", body, p_cb);
}

void GitHubClient::list_branches(const String &p_owner, const String &p_repo, const Callback &p_cb) {
	request(HTTPClient::METHOD_GET, repo_path(p_owner, p_repo) + "/branches?per_page=1", Variant(), p_cb);
}

void GitHubClient::add_collaborator(const String &p_owner, const String &p_repo, const String &p_username, const Callback &p_cb) {
	Dictionary body;
	body["permission"] = "push";
	request(HTTPClient::METHOD_PUT, vformat("%s/collaborators/%s", repo_path(p_owner, p_repo), p_username.uri_encode()), body, p_cb);
}

void GitHubClient::list_commits(const String &p_owner, const String &p_repo, const String &p_branch, int p_per_page, const Callback &p_cb) {
	request(HTTPClient::METHOD_GET, vformat("%s/commits?sha=%s&per_page=%d", repo_path(p_owner, p_repo), p_branch.uri_encode(), p_per_page), Variant(), p_cb);
}

// ------------------------------------------------------------- Git Data API

void GitHubClient::get_ref(const String &p_owner, const String &p_repo, const String &p_branch, const String &p_etag, const Callback &p_cb) {
	Vector<String> extra;
	if (!p_etag.is_empty()) {
		extra.push_back("If-None-Match: " + p_etag);
	}
	request(HTTPClient::METHOD_GET, vformat("%s/git/ref/heads/%s", repo_path(p_owner, p_repo), encode_path(p_branch)), Variant(), p_cb, extra);
}

void GitHubClient::create_ref(const String &p_owner, const String &p_repo, const String &p_branch, const String &p_sha, const Callback &p_cb) {
	Dictionary body;
	body["ref"] = "refs/heads/" + p_branch;
	body["sha"] = p_sha;
	request(HTTPClient::METHOD_POST, repo_path(p_owner, p_repo) + "/git/refs", body, p_cb);
}

void GitHubClient::update_ref(const String &p_owner, const String &p_repo, const String &p_branch, const String &p_sha, const Callback &p_cb) {
	// Fast-forward only: GitHub answers 422 when somebody else pushed first.
	Dictionary body;
	body["sha"] = p_sha;
	body["force"] = false;
	request(HTTPClient::METHOD_PATCH, vformat("%s/git/refs/heads/%s", repo_path(p_owner, p_repo), encode_path(p_branch)), body, p_cb);
}

void GitHubClient::get_commit(const String &p_owner, const String &p_repo, const String &p_sha, const Callback &p_cb) {
	request(HTTPClient::METHOD_GET, vformat("%s/git/commits/%s", repo_path(p_owner, p_repo), p_sha), Variant(), p_cb);
}

void GitHubClient::get_tree(const String &p_owner, const String &p_repo, const String &p_sha, const Callback &p_cb) {
	request(HTTPClient::METHOD_GET, vformat("%s/git/trees/%s?recursive=1", repo_path(p_owner, p_repo), p_sha), Variant(), p_cb);
}

void GitHubClient::get_blob(const String &p_owner, const String &p_repo, const String &p_sha, const Callback &p_cb) {
	request(HTTPClient::METHOD_GET, vformat("%s/git/blobs/%s", repo_path(p_owner, p_repo), p_sha), Variant(), p_cb);
}

void GitHubClient::create_blob(const String &p_owner, const String &p_repo, const Vector<uint8_t> &p_content, const Callback &p_cb) {
	Dictionary body;
	body["content"] = _base64_encode(p_content);
	body["encoding"] = "base64";
	request(HTTPClient::METHOD_POST, repo_path(p_owner, p_repo) + "/git/blobs", body, p_cb);
}

void GitHubClient::create_tree(const String &p_owner, const String &p_repo, const String &p_base_tree, const Array &p_entries, const Callback &p_cb) {
	Dictionary body;
	body["base_tree"] = p_base_tree;
	body["tree"] = p_entries;
	request(HTTPClient::METHOD_POST, repo_path(p_owner, p_repo) + "/git/trees", body, p_cb);
}

void GitHubClient::create_commit(const String &p_owner, const String &p_repo, const String &p_message, const String &p_tree, const Array &p_parents, const Callback &p_cb) {
	Dictionary body;
	body["message"] = p_message;
	body["tree"] = p_tree;
	body["parents"] = p_parents;
	request(HTTPClient::METHOD_POST, repo_path(p_owner, p_repo) + "/git/commits", body, p_cb);
}

void GitHubClient::put_file(const String &p_owner, const String &p_repo, const String &p_path, const Vector<uint8_t> &p_content, const String &p_message, const Callback &p_cb) {
	Dictionary body;
	body["message"] = p_message;
	body["content"] = _base64_encode(p_content);
	request(HTTPClient::METHOD_PUT, vformat("%s/contents/%s", repo_path(p_owner, p_repo), encode_path(p_path)), body, p_cb);
}
