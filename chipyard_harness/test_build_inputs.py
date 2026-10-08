"""Cold native builds must bind FESVR's transitive selected headers."""

import tempfile
import unittest
from pathlib import Path

from build import selected_include_inputs, sha


class SelectedIncludeClosure(unittest.TestCase):
    def test_nested_and_sibling_inputs_are_bound_and_links_refuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary)
            include = prefix / "include"
            (include / "fesvr").mkdir(parents=True)
            (include / "riscv/nested").mkdir(parents=True)
            (include / "fesvr/memif.h").write_text('#include "../riscv/cfg.h"\n')
            (include / "riscv/cfg.h").write_text('#include "nested/options.inc"\n')
            options = include / "riscv/nested/options.inc"
            options.write_text("#define SELECTED 1\n")
            rows = selected_include_inputs(prefix)
            self.assertEqual([role for role, _ in rows], [
                "fesvr:memif.h", "selected-include:riscv/cfg.h",
                "selected-include:riscv/nested/options.inc",
            ])
            before = {str(path): sha(path) for _, path in rows}
            options.write_text("#define SELECTED 2\n")
            self.assertNotEqual(sha(options), before[str(options)])
            (include / "alias.h").symlink_to("riscv/cfg.h")
            with self.assertRaises(RuntimeError):
                selected_include_inputs(prefix)


if __name__ == "__main__":
    unittest.main()
