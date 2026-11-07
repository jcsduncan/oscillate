"""Unit tests for check_module_deps.py."""

import json
import pathlib
import tempfile
import unittest

import check_module_deps

FOUNDATION = "oscillate_add_library(foundation SOURCES id.cpp)\n"
ENGINE = "oscillate_add_library(engine SOURCES graph.cpp PUBLIC oscillate::foundation)\n"
ENGINE_LINKS_DSP = ENGINE + "target_link_libraries(oscillate_engine oscillate::native_dsp)\n"
SECOND_LINE = 2
THIRD_LINE = 3
FOURTH_LINE = 4
LINK_ISSUE = 129


def table(exceptions=()):
    return {
        "every_module": ["foundation"],
        "modules": {
            "foundation": {"path": "src/foundation", "allowed": []},
            "engine": {"path": "src/engine", "allowed": []},
            "native_dsp": {"path": "src/native_dsp", "allowed": ["engine"]},
            "plugin_ui": {"path": "src/app/plugin_ui", "allowed": ["oscillate::engine"]},
        },
        "exceptions": list(exceptions),
    }


class TreeTest(unittest.TestCase):
    """Builds a throwaway src/ tree for each test."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        self.write("foundation", FOUNDATION)
        self.write("engine", ENGINE)

    def tearDown(self):
        self.directory.cleanup()

    def write(self, module_path, text):
        directory = self.root / "src" / module_path
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "CMakeLists.txt").write_text(text, encoding="utf-8")

    def check(self, exceptions=()):
        return check_module_deps.check(self.root, table(exceptions))


class ParseTests(unittest.TestCase):
    def test_definitions_are_not_links(self):
        defined, links = check_module_deps.parse(
            "add_library(oscillate_x a.cpp)\n"
            "add_library(oscillate::x ALIAS oscillate_x)\n"
            "target_link_libraries(oscillate_x PUBLIC oscillate::engine)\n"
        )
        self.assertEqual(defined, {"x"})
        self.assertEqual(links, [("engine", THIRD_LINE)])

    def test_comments_are_ignored_and_lines_kept(self):
        _, links = check_module_deps.parse(
            "#[[ oscillate::project\n oscillate::timeline ]]\n"
            "# oscillate::media\n"
            "oscillate_add_library(x SOURCES a.cpp PUBLIC oscillate::engine) # oscillate::codec\n"
        )
        self.assertEqual(links, [("engine", FOURTH_LINE)])


class CheckTests(TreeTest):
    def test_allowed_links_pass(self):
        self.write(
            "native_dsp",
            "oscillate_add_library(native_dsp SOURCES eq.cpp\n"
            "  PUBLIC oscillate::engine oscillate::foundation)\n",
        )
        self.assertEqual(self.check(), [])

    def test_disallowed_link_fails_with_its_place(self):
        self.write("engine", ENGINE_LINKS_DSP)
        self.write("native_dsp", "oscillate_add_library(native_dsp SOURCES eq.cpp)\n")
        found = self.check()
        self.assertEqual(len(found), 1)
        self.assertIn(f"src/engine/CMakeLists.txt:{SECOND_LINE}", found[0])
        self.assertIn("oscillate::native_dsp", found[0])

    def test_conditional_links_are_checked(self):
        self.write(
            "engine",
            ENGINE + "if(X)\n  target_link_libraries(oscillate_engine "
            "oscillate::native_dsp)\nendif()\n",
        )
        self.write("native_dsp", "oscillate_add_library(native_dsp SOURCES eq.cpp)\n")
        self.assertEqual(len(self.check()), 1)

    def test_target_entry_allows_only_that_target(self):
        self.write("engine", ENGINE + "oscillate_add_library(engine_extra SOURCES e.cpp)\n")
        self.write(
            "app/plugin_ui",
            "add_library(oscillate_plugin_ui ui.cpp)\n"
            "target_link_libraries(oscillate_plugin_ui oscillate::engine\n"
            "  oscillate::engine_extra)\n",
        )
        found = self.check()
        self.assertEqual(len(found), 1)
        self.assertIn("oscillate::engine_extra", found[0])

    def test_nested_module_path_wins(self):
        nested = table()
        nested["modules"]["app"] = {"path": "src/app", "allowed": []}
        self.write("app/plugin_ui", "target_link_libraries(x oscillate::engine)\n")
        self.assertEqual(check_module_deps.check(self.root, nested), [])

    def test_unowned_directory_fails(self):
        self.write("newmodule", "oscillate_add_library(newmodule SOURCES n.cpp)\n")
        self.assertIn("no module", " ".join(self.check()))

    def test_unknown_target_fails(self):
        self.write("native_dsp", "target_link_libraries(x oscillate::nowhere)\n")
        self.assertIn("no module defines", " ".join(self.check()))

    def test_own_targets_pass(self):
        self.write(
            "native_dsp",
            "oscillate_add_library(native_synth SOURCES s.cpp)\n"
            "oscillate_add_library(native_dsp SOURCES e.cpp\n"
            "  PUBLIC oscillate::native_synth)\n",
        )
        self.assertEqual(self.check(), [])


class ExceptionTests(TreeTest):
    def setUp(self):
        super().setUp()
        self.write("native_dsp", "oscillate_add_library(native_dsp SOURCES eq.cpp)\n")

    def test_needed_exception_passes(self):
        self.write("engine", ENGINE_LINKS_DSP)
        exception = {"module": "engine", "target": "oscillate::native_dsp", "issue": LINK_ISSUE}
        self.assertEqual(self.check([exception]), [])

    def test_unneeded_exception_fails(self):
        exception = {"module": "engine", "target": "oscillate::native_dsp", "issue": LINK_ISSUE}
        found = self.check([exception])
        self.assertEqual(len(found), 1)
        self.assertIn("no longer needed", found[0])
        self.assertIn(f"#{LINK_ISSUE}", found[0])

    def test_exception_for_an_allowed_link_fails(self):
        self.write(
            "native_dsp",
            "oscillate_add_library(native_dsp SOURCES eq.cpp\n  PUBLIC oscillate::engine)\n",
        )
        exception = {"module": "native_dsp", "target": "oscillate::engine", "issue": LINK_ISSUE}
        self.assertIn("no longer needed", " ".join(self.check([exception])))

    def test_exception_without_issue_fails(self):
        self.write("engine", ENGINE_LINKS_DSP)
        exception = {"module": "engine", "target": "oscillate::native_dsp"}
        self.assertIn("must name its issue", " ".join(self.check([exception])))


class TableTests(TreeTest):
    def test_unknown_allowed_module_fails(self):
        broken = table()
        broken["modules"]["engine"]["allowed"] = ["nothing"]
        self.assertIn("not in the table", " ".join(check_module_deps.check(self.root, broken)))


class RepositoryTests(unittest.TestCase):
    def test_repository_table_holds(self):
        repository_table = json.loads(check_module_deps.TABLE.read_text(encoding="utf-8"))
        self.assertEqual(check_module_deps.check(check_module_deps.REPO, repository_table), [])


if __name__ == "__main__":
    unittest.main()
