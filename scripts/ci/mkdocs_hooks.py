"""MkDocs hooks that publish docs/ without copying or editing the Markdown.

The repository documentation is written to be read on GitHub and in a
checkout, so it links freely outside docs/ (source files, AGENTS.md, the
root README). MkDocs only sees docs/, so these hooks:

* render the root README.md as the site home page and CONTRIBUTING.md and
  SECURITY.md as ordinary pages, rewriting their links for the site;
* move docs/README.md (the documentation map) to map.md so it does not
  collide with the home page;
* rewrite any other link that leaves docs/ to the file on GitHub.

Links that stay inside docs/ are left alone, so `mkdocs build --strict`
still reports a genuinely broken documentation link.
"""

from __future__ import annotations

import posixpath
import re
from pathlib import Path
from urllib.parse import urlsplit

from mkdocs.config.defaults import MkDocsConfig
from mkdocs.structure.files import File, Files
from mkdocs.structure.pages import Page

REPOSITORY_URL = "https://github.com/jcsduncan/oscillate"
BRANCH = "main"
DOCS_DIR = "docs"

# Root files rendered as site pages: site path -> repository path.
ROOT_PAGES = {
    "index.md": "README.md",
    "contributing.md": "CONTRIBUTING.md",
    "security.md": "SECURITY.md",
    "code-of-conduct.md": "CODE_OF_CONDUCT.md",
}
DOCS_MAP_SOURCE = "README.md"
DOCS_MAP_PAGE = "map.md"

# Inline Markdown links and images: [text](target "title") / ![alt](target).
LINK = re.compile(r"(!?\[[^\]]*\]\()(<[^>]+>|[^)\s]+)([^)]*\))")


def _repository_root(config: MkDocsConfig) -> Path:
    return Path(config.docs_dir).parent


def _site_target(repo_path: str) -> str | None:
    """Return the docs-relative site path for a repository path, if published."""
    for page, source in ROOT_PAGES.items():
        if repo_path == source:
            return page
    if repo_path == f"{DOCS_DIR}/{DOCS_MAP_SOURCE}":
        return DOCS_MAP_PAGE
    if repo_path.startswith(f"{DOCS_DIR}/"):
        return repo_path[len(DOCS_DIR) + 1 :]
    return None


def _github_url(repo_path: str, root: Path) -> str:
    kind = "tree" if (root / repo_path).is_dir() else "blob"
    return f"{REPOSITORY_URL}/{kind}/{BRANCH}/{repo_path}"


def _rewrite(markdown: str, repo_dir: str, site_dir: str, root: Path) -> str:
    """Rewrite relative links in a page whose source lives in repo_dir.

    repo_dir is the page's directory relative to the repository root;
    site_dir is its directory relative to docs/ on the built site.
    """

    def replace(match: re.Match[str]) -> str:
        prefix, raw, suffix = match.groups()
        target = raw[1:-1] if raw.startswith("<") else raw
        parts = urlsplit(target)
        if parts.scheme or parts.netloc or not parts.path or target.startswith("/"):
            return match.group(0)
        repo_path = posixpath.normpath(posixpath.join(repo_dir, parts.path))
        if repo_path.startswith("../"):
            return match.group(0)
        fragment = f"#{parts.fragment}" if parts.fragment else ""
        site_path = _site_target(repo_path)
        if site_path is not None and (root / repo_path).is_dir():
            # A docs/ folder is a page only through its README.md.
            index = posixpath.join(repo_path, DOCS_MAP_SOURCE)
            site_path = _site_target(index) if (root / index).is_file() else None
        if site_path is None:
            return f"{prefix}{_github_url(repo_path, root)}{fragment}{suffix}"
        relative = posixpath.relpath(site_path, site_dir or ".")
        if parts.path.endswith("/") and not relative.endswith("/"):
            relative += "/"
        return f"{prefix}{relative}{fragment}{suffix}"

    return LINK.sub(replace, markdown)


def on_files(files: Files, config: MkDocsConfig) -> Files:
    root = _repository_root(config)
    docs_map = files.get_file_from_path(DOCS_MAP_SOURCE)
    if docs_map is not None:
        files.remove(docs_map)
        files.append(File.generated(config, DOCS_MAP_PAGE, abs_src_path=docs_map.abs_src_path))
    for page, source in ROOT_PAGES.items():
        files.append(File.generated(config, page, abs_src_path=str(root / source)))
    return files


def on_page_markdown(markdown: str, page: Page, config: MkDocsConfig, files: Files) -> str:
    del files
    root = _repository_root(config)
    src_uri = page.file.src_uri
    site_dir = posixpath.dirname(src_uri)
    source = ROOT_PAGES.get(src_uri)
    if source is not None:
        repo_dir = posixpath.dirname(source)
    elif src_uri == DOCS_MAP_PAGE:
        repo_dir = DOCS_DIR
    else:
        repo_dir = posixpath.join(DOCS_DIR, site_dir) if site_dir else DOCS_DIR
    return _rewrite(markdown, repo_dir, site_dir, root)
