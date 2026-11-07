"""Check the modules' oscillate:: links against the allowed dependency table.

Usage: check_module_deps.py [TABLE]
TABLE defaults to cmake/module_dependencies.json, the machine-readable form of
the module table in docs/HLD.md section 3. Every CMakeLists.txt under a
module's path is read. A target belongs to the module that defines it
(oscillate_add_library or an oscillate:: ALIAS). A module may link its own
targets, the modules in every_module, and the modules or oscillate::<target>
entries in its allowed list. Anything else fails, unless an exception names
that module and target with its issue. An exception that no link needs fails
too, so the list shrinks as the violations are fixed.
"""

import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
TABLE = REPO / "cmake" / "module_dependencies.json"
SOURCE_ROOT = "src"
TARGET_PREFIX = "oscillate::"

BRACKET_COMMENT = re.compile(r"#\[(=*)\[.*?\]\1\]", re.DOTALL)
LINE_COMMENT = re.compile(r"#[^\n]*")
ALIAS = re.compile(r"add_library\(\s*oscillate::(\w+)\s+ALIAS\b")
ADD_LIBRARY = re.compile(r"\boscillate_add_library\(\s*(\w+)")
LINK = re.compile(r"\boscillate::(\w+)")


def strip_comments(text):
    """Blank CMake comments, keeping line numbers."""
    text = BRACKET_COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return LINE_COMMENT.sub("", text)


def parse(text):
    """Return (targets defined, [(linked target, line)]) for one CMakeLists.txt."""
    text = strip_comments(text)
    defined = set(ALIAS.findall(text)) | set(ADD_LIBRARY.findall(text))
    links = []
    for number, line in enumerate(ALIAS.sub("", text).splitlines(), start=1):
        links.extend((target, number) for target in LINK.findall(line))
    return defined, links


def owning_module(relative, modules):
    """The module whose path is the longest prefix of a CMakeLists path."""
    candidates = []
    for name, spec in modules.items():
        path = pathlib.PurePosixPath(spec["path"])
        if relative == path or path in relative.parents:
            candidates.append((len(path.parts), name))
    return max(candidates)[1] if candidates else None


def scan(root, modules):
    """Read every CMakeLists.txt under src/.

    Returns (owner of each target, [(module, target, file, line)], [problem]).
    """
    owners = {}
    links = []
    found = []
    for listfile in sorted((root / SOURCE_ROOT).rglob("CMakeLists.txt")):
        relative = pathlib.PurePosixPath(listfile.relative_to(root).as_posix())
        module = owning_module(relative.parent, modules)
        if module is None:
            found.append(f"{relative}: no module in the table owns this directory")
            continue
        defined, linked = parse(listfile.read_text(encoding="utf-8"))
        for target in defined:
            owners[target] = module
        links.extend((module, target, str(relative), line) for target, line in linked)
    return owners, links, found


def table_problems(table, owners):
    """Entries in the table that name no module or target."""
    modules = table["modules"]
    found = [
        f"every_module: unknown module `{name}`"
        for name in table["every_module"]
        if name not in modules
    ]
    for name, spec in modules.items():
        for entry in spec["allowed"]:
            if entry.startswith(TARGET_PREFIX):
                if entry.removeprefix(TARGET_PREFIX) not in owners:
                    found.append(f"{name}: allowed target `{entry}` is not defined")
            elif entry not in modules:
                found.append(f"{name}: allowed module `{entry}` is not in the table")
    for exception in table["exceptions"]:
        if exception["module"] not in modules:
            found.append(f"exception for unknown module `{exception['module']}`")
        if not isinstance(exception.get("issue"), int):
            found.append(
                f"exception {exception['module']} -> {exception['target']} must name its issue"
            )
    return found


def is_allowed(table, owners, module, target):
    owner = owners[target]
    allowed = table["modules"][module]["allowed"]
    return (
        owner == module
        or owner in table["every_module"]
        or owner in allowed
        or TARGET_PREFIX + target in allowed
    )


def link_problems(table, owners, links):
    """Links outside the table, and exceptions that no link needs."""
    exceptions = {(e["module"], e["target"]): e.get("issue") for e in table["exceptions"]}
    used = set()
    found = []
    for module, target, path, line in links:
        if target not in owners:
            found.append(
                f"{path}:{line}: {module} links `{TARGET_PREFIX}{target}`, which no module defines"
            )
            continue
        if is_allowed(table, owners, module, target):
            continue
        key = (module, TARGET_PREFIX + target)
        if key in exceptions:
            used.add(key)
            continue
        found.append(
            f"{path}:{line}: {module} may not link `{TARGET_PREFIX}{target}` "
            f"(module {owners[target]}); see docs/HLD.md section 3"
        )
    for key, issue in exceptions.items():
        if key not in used:
            found.append(
                f"exception {key[0]} -> {key[1]} (#{issue}) is no longer needed; "
                "remove it from the table and HLD section 3.1"
            )
    return found


def check(root, table):
    owners, links, found = scan(root, table["modules"])
    return found + table_problems(table, owners) + link_problems(table, owners, links)


def main():
    path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else TABLE
    table = json.loads(path.read_text(encoding="utf-8"))
    found = check(REPO, table)
    for problem in found:
        print(f"module dependencies: {problem}")
    if not found:
        print(
            f"module dependencies: {len(table['modules'])} modules within the table, "
            f"{len(table['exceptions'])} known exceptions"
        )
    sys.exit(1 if found else 0)


if __name__ == "__main__":
    main()
