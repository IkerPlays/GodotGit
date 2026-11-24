@tool
extends Node
## Cliente mínimo de la REST API de GitHub (solo lo que necesita GodotGit).
##
## Todas las funciones son corrutinas (usar `await`) y devuelven un Dictionary:
##   {
##     "ok": bool,          # true si el código HTTP es 2xx
##     "code": int,         # código HTTP (0 si falló la red)
##     "data": Variant,     # JSON ya parseado (Dictionary / Array / null)
##     "headers": Dictionary, # cabeceras en minúsculas
##     "error": String,     # mensaje de error legible (vacío si ok)
##   }

const API_URL := "https://api.github.com"
const USER_AGENT := "GodotGit-Sync"

var token: String = ""


# ---------------------------------------------------------------- Núcleo HTTP

func request(method: int, path: String, body: Variant = null, extra_headers: PackedStringArray = PackedStringArray()) -> Dictionary:
	var http := HTTPRequest.new()
	http.accept_gzip = true
	http.timeout = 40.0
	add_child(http)

	var headers := PackedStringArray([
		"Accept: application/vnd.github+json",
		"X-GitHub-Api-Version: 2022-11-28",
		"User-Agent: " + USER_AGENT,
	])
	if token != "":
		headers.append("Authorization: Bearer " + token)
	headers.append_array(extra_headers)

	var payload := ""
	if body != null:
		payload = JSON.stringify(body)
		headers.append("Content-Type: application/json")

	var err := http.request(API_URL + path, headers, method, payload)
	if err != OK:
		http.queue_free()
		return _failure(0, "No se pudo lanzar la petición (error %d)" % err)

	var res: Array = await http.request_completed
	http.queue_free()

	var result_code: int = res[0]
	var code: int = res[1]
	var raw_headers: PackedStringArray = res[2]
	var raw_body: PackedByteArray = res[3]

	if result_code != HTTPRequest.RESULT_SUCCESS:
		return _failure(0, "Error de red (código %d). ¿Hay conexión a internet?" % result_code)

	var parsed_headers := {}
	for line in raw_headers:
		var idx := line.find(":")
		if idx > 0:
			parsed_headers[line.substr(0, idx).strip_edges().to_lower()] = line.substr(idx + 1).strip_edges()

	var data: Variant = null
	if raw_body.size() > 0:
		data = JSON.parse_string(raw_body.get_string_from_utf8())

	var ok := code >= 200 and code < 300
	var message := ""
	if not ok:
		if data is Dictionary and data.has("message"):
			message = str(data["message"])
		else:
			message = "HTTP %d" % code
		if code == 401:
			message = "Token inválido o caducado (401)."
		elif code == 403 and parsed_headers.get("x-ratelimit-remaining", "1") == "0":
			message = "Límite de peticiones de GitHub alcanzado. Espera unos minutos."
	return {"ok": ok, "code": code, "data": data, "headers": parsed_headers, "error": message}


func _failure(code: int, message: String) -> Dictionary:
	return {"ok": false, "code": code, "data": null, "headers": {}, "error": message}


static func _encode_path(path: String) -> String:
	var parts := path.split("/")
	var out := PackedStringArray()
	for part in parts:
		out.append(part.uri_encode())
	return "/".join(out)


static func _repo_path(owner: String, repo: String) -> String:
	return "/repos/%s/%s" % [owner.uri_encode(), repo.uri_encode()]


# ------------------------------------------------------------------- Usuario

func get_user() -> Dictionary:
	return await request(HTTPClient.METHOD_GET, "/user")


func get_repo(owner: String, repo: String) -> Dictionary:
	return await request(HTTPClient.METHOD_GET, _repo_path(owner, repo))


func create_repo(repo_name: String, is_private: bool, description: String = "") -> Dictionary:
	return await request(HTTPClient.METHOD_POST, "/user/repos", {
		"name": repo_name,
		"description": description,
		"private": is_private,
		"auto_init": true,
	})


func list_branches(owner: String, repo: String) -> Dictionary:
	return await request(HTTPClient.METHOD_GET, _repo_path(owner, repo) + "/branches?per_page=1")


func add_collaborator(owner: String, repo: String, username: String, permission: String = "push") -> Dictionary:
	return await request(
		HTTPClient.METHOD_PUT,
		"%s/collaborators/%s" % [_repo_path(owner, repo), username.uri_encode()],
		{"permission": permission})


# --------------------------------------------------------------- Git Data API

## `etag` opcional: si se pasa, GitHub responde 304 (sin gastar cuota) cuando nada cambió.
func get_ref(owner: String, repo: String, branch: String, etag: String = "") -> Dictionary:
	var extra := PackedStringArray()
	if etag != "":
		extra.append("If-None-Match: " + etag)
	return await request(
		HTTPClient.METHOD_GET,
		"%s/git/ref/heads/%s" % [_repo_path(owner, repo), _encode_path(branch)],
		null, extra)


func create_ref(owner: String, repo: String, branch: String, sha: String) -> Dictionary:
	return await request(HTTPClient.METHOD_POST, _repo_path(owner, repo) + "/git/refs", {
		"ref": "refs/heads/" + branch,
		"sha": sha,
	})


## Avance rápido (force = false): devuelve 422 si alguien más empujó antes.
func update_ref(owner: String, repo: String, branch: String, sha: String) -> Dictionary:
	return await request(
		HTTPClient.METHOD_PATCH,
		"%s/git/refs/heads/%s" % [_repo_path(owner, repo), _encode_path(branch)],
		{"sha": sha, "force": false})


func get_commit(owner: String, repo: String, sha: String) -> Dictionary:
	return await request(HTTPClient.METHOD_GET, "%s/git/commits/%s" % [_repo_path(owner, repo), sha])


func get_tree(owner: String, repo: String, sha: String) -> Dictionary:
	return await request(HTTPClient.METHOD_GET, "%s/git/trees/%s?recursive=1" % [_repo_path(owner, repo), sha])


func get_blob(owner: String, repo: String, sha: String) -> Dictionary:
	return await request(HTTPClient.METHOD_GET, "%s/git/blobs/%s" % [_repo_path(owner, repo), sha])


func create_blob(owner: String, repo: String, content: PackedByteArray) -> Dictionary:
	return await request(HTTPClient.METHOD_POST, _repo_path(owner, repo) + "/git/blobs", {
		"content": Marshalls.raw_to_base64(content),
		"encoding": "base64",
	})


func create_tree(owner: String, repo: String, base_tree: String, entries: Array) -> Dictionary:
	return await request(HTTPClient.METHOD_POST, _repo_path(owner, repo) + "/git/trees", {
		"base_tree": base_tree,
		"tree": entries,
	})


func create_commit(owner: String, repo: String, message: String, tree: String, parents: Array) -> Dictionary:
	return await request(HTTPClient.METHOD_POST, _repo_path(owner, repo) + "/git/commits", {
		"message": message,
		"tree": tree,
		"parents": parents,
	})


func list_commits(owner: String, repo: String, branch: String, per_page: int = 20) -> Dictionary:
	return await request(
		HTTPClient.METHOD_GET,
		"%s/commits?sha=%s&per_page=%d" % [_repo_path(owner, repo), branch.uri_encode(), per_page])


## Contents API: es la única forma de crear el primer commit en un repo totalmente vacío.
func put_file(owner: String, repo: String, path: String, content: PackedByteArray, message: String, branch: String = "") -> Dictionary:
	var body := {
		"message": message,
		"content": Marshalls.raw_to_base64(content),
	}
	if branch != "":
		body["branch"] = branch
	return await request(
		HTTPClient.METHOD_PUT,
		"%s/contents/%s" % [_repo_path(owner, repo), _encode_path(path)],
		body)
