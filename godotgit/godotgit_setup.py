#!/usr/bin/env python3
"""Crea un proyecto Godot con GodotGit listo para usar, o instala GodotGit en uno existente.

Ejemplos:
    # Proyecto nuevo, ya apuntando a un repo (te pedirá el token al abrirlo en el editor)
    python3 godotgit_setup.py ~/juegos/mi-juego --repo usuario/mi-juego

    # Proyecto nuevo sin repo todavía (te deja conectarlo o crearlo desde el panel «GitHub»)
    python3 godotgit_setup.py ~/juegos/mi-juego --name "Mi Juego"

    # Añadir GodotGit a un proyecto que ya tienes
    python3 godotgit_setup.py ~/juegos/otro-proyecto

El token de GitHub NO se escribe en el proyecto: se pega una vez en el editor (o se lee de
la variable de entorno GITHUB_TOKEN) y queda en la configuración de tu usuario.
"""
import argparse
import re
import shutil
import sys
from pathlib import Path

PLUGIN_CFG = "res://addons/github_sync/plugin.cfg"
ADDON_SRC = Path(__file__).resolve().parent / "addons" / "github_sync"

MINIMAL_PROJECT = """; Engine configuration file.
; It's best edited using the editor UI and not directly,
; since the parameters that go here are not all obvious.
;
; Format:
;   [section] ; section goes between []
;   param=value ; assign values to parameters

config_version=5

[application]

config/name="{name}"
config/features=PackedStringArray("4.4")

[editor_plugins]

enabled=PackedStringArray("{plugin}")
"""

DEFAULT_GITIGNORE = "# Godot 4+\n.godot/\n/android/\n\n# Estado local de GodotGit\n.godotgit/\n\n# Temporales\n*.tmp\n"


def enable_plugin(project_file: Path, name: str) -> None:
    if not project_file.exists():
        project_file.write_text(MINIMAL_PROJECT.format(name=name, plugin=PLUGIN_CFG), encoding="utf-8")
        return

    text = project_file.read_text(encoding="utf-8")
    if PLUGIN_CFG in text:
        return

    section = re.search(r"^\[editor_plugins\][ \t]*$", text, re.MULTILINE)
    if not section:
        sep = "" if text.endswith("\n") else "\n"
        text += f'{sep}\n[editor_plugins]\n\nenabled=PackedStringArray("{PLUGIN_CFG}")\n'
    else:
        enabled = re.compile(r"^enabled=PackedStringArray\((.*)\)[ \t]*$", re.MULTILINE)
        match = enabled.search(text, section.end())
        next_section = re.compile(r"^\[", re.MULTILINE).search(text, section.end())
        if match and (next_section is None or match.start() < next_section.start()):
            inner = match.group(1).strip()
            merged = f'{inner}, "{PLUGIN_CFG}"' if inner else f'"{PLUGIN_CFG}"'
            text = text[: match.start()] + f"enabled=PackedStringArray({merged})" + text[match.end():]
        else:
            text = text[: section.end()] + f'\n\nenabled=PackedStringArray("{PLUGIN_CFG}")' + text[section.end():]
    project_file.write_text(text, encoding="utf-8")


def write_connection(project: Path, repo: str, branch: str) -> None:
    owner, _, name = repo.partition("/")
    if not owner or not name:
        sys.exit("--repo debe tener el formato usuario/repositorio")
    data_dir = project / ".godotgit"
    data_dir.mkdir(exist_ok=True)
    (data_dir / ".gdignore").write_text("", encoding="utf-8")
    (data_dir / "config.cfg").write_text(
        f'[github]\n\nowner="{owner}"\nrepo="{name}"\nbranch="{branch}"\n\n'
        f"[sync]\n\nauto=true\npoll_seconds=4.0\n",
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("project", help="carpeta del proyecto (se crea si no existe)")
    parser.add_argument("--repo", help="repo de GitHub al que conectar: usuario/repositorio")
    parser.add_argument("--branch", default="", help="rama (por defecto, la principal del repo)")
    parser.add_argument("--name", help="nombre del proyecto (solo si se crea uno nuevo)")
    args = parser.parse_args()

    if not (ADDON_SRC / "plugin.cfg").exists():
        sys.exit(f"No encuentro el addon en {ADDON_SRC}")

    project = Path(args.project).expanduser().resolve()
    project.mkdir(parents=True, exist_ok=True)

    dest = project / "addons" / "github_sync"
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(ADDON_SRC, dest, ignore=shutil.ignore_patterns("*.uid", "__pycache__"))

    enable_plugin(project / "project.godot", args.name or project.name)

    gitignore = project / ".gitignore"
    if not gitignore.exists():
        gitignore.write_text(DEFAULT_GITIGNORE, encoding="utf-8")

    if args.repo:
        write_connection(project, args.repo, args.branch)

    print(f"Listo: {project}")
    print("  1. Abre la carpeta con Godot 4.4 o superior (Importar proyecto → project.godot).")
    if args.repo:
        print(f"  2. Se conectará a {args.repo}. Pega tu token de GitHub en el panel «GitHub» (solo la primera vez).")
    else:
        print("  2. En el panel inferior «GitHub» conecta un repo existente o crea uno nuevo.")


if __name__ == "__main__":
    main()
