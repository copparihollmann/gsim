"""Build an unchanged Chipyard FIRRTL model and retain its actual native transcript.

No elaboration or generated-source repair is performed. An optional Merlin v3
receipt explicitly records that the selected FIRRTL was adopted, not elaborated.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import threading
import time


def sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firrtl", type=Path, required=True)
    parser.add_argument("--emitter", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--compiler-arg", action="append", default=[])
    parser.add_argument("--chipyard", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--comb-extmod", action="append", default=[])
    parser.add_argument("--merlin-receipt", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 4:
        parser.error("native build parallelism must be between one and four")
    root = Path(__file__).resolve().parent
    firrtl, emitter, compiler = (p.resolve(strict=True) for p in (args.firrtl, args.emitter, args.compiler))
    chipyard = args.chipyard.resolve(strict=True)
    out = args.out.resolve()
    model, native = out / "model", out / "native"
    model.mkdir(parents=True, exist_ok=True)
    native.mkdir(parents=True, exist_ok=True)
    if any(model.iterdir()) or any(native.iterdir()):
        parser.error("choose a fresh output directory; retained builds are not overwritten")
    prefix = chipyard / ".conda-env/riscv-tools"
    support = chipyard / "generators/testchipip/src/main/resources/testchipip/csrc"
    vendor_sources = [support / f"{name}.cc" for name in ("mm", "testchip_htif", "testchip_tsi", "uart")]
    inputs = [("harness:" + path.name, path) for path in
              (root / "main.cpp", root / "blackboxes.cpp", root / "terminal_dump.h", Path(__file__).resolve())]
    inputs += [("vendor:" + path.name, path) for path in sorted(support.glob("*.h")) + vendor_sources]
    inputs += [("fesvr:" + path.name, path) for path in sorted((prefix / "include/fesvr").glob("*.h"))]
    inputs += [("fesvr-library", prefix / "lib/libfesvr.a")]
    # Preserve the emitter's source ownership, including uncommitted generic fixes.
    emitter_repo = root.parent
    for directory, patterns in (("src", ("*.cpp",)), ("include", ("*.h",)),
                                ("parser", ("*.cpp", "*.y", "*.l")), ("parser/include", ("*.h",))):
        for pattern in patterns:
            for path in sorted((emitter_repo / directory).glob(pattern)):
                inputs.append(("emitter-source:" + str(path.relative_to(emitter_repo)), path))
    inputs = [(role, path.resolve(strict=True)) for role, path in inputs]
    before = {str(path): sha(path) for _, path in inputs}
    before.update({str(path): sha(path) for path in (firrtl, emitter, compiler)})
    commands: list[dict] = []
    lock = threading.Lock()

    def run(stage: str, argv: list[str], log: Path) -> None:
        row = {"stage": stage, "cwd": str(root), "argv": argv, "log": str(log), "started_ns": time.time_ns()}
        with lock:
            commands.append(row)
        with log.open("w") as stream:
            completed = subprocess.run(argv, cwd=root, stdout=stream, stderr=subprocess.STDOUT, check=False)
        row.update(returncode=completed.returncode, finished_ns=time.time_ns())
        if completed.returncode:
            raise RuntimeError(f"{stage} failed ({completed.returncode}); see {log}")

    try:
        emit = [str(emitter), "--dynamic-clocks", "--log-level=1", "--supernode-max-size=65",
                "--cpp-max-size-KB=8192", "--dir=" + str(model)]
        emit += ["--comb-extmod=" + name for name in args.comb_extmod]
        run("emit", emit + [str(firrtl)], out / "emit.log")
        if not (model / "TestHarness.h").is_file():
            raise RuntimeError("Chipyard harness requires FIRRTL circuit TestHarness")
        sources = sorted(model.glob("TestHarness*.cpp")) + vendor_sources + [root / "main.cpp", root / "blackboxes.cpp"]
        objects = [native / f"{index:03d}_{source.stem}.o" for index, source in enumerate(sources)]
        cxx = [str(compiler), "--driver-mode=g++", *args.compiler_arg]
        flags = ["-O1", "-std=c++17", "-Wno-format", "-I" + str(model), "-I" + str(support), "-I" + str(prefix / "include")]
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(run, "compile", cxx + flags + ["-c", str(source), "-o", str(obj)],
                                   native / f"{index:03d}_{source.stem}.log")
                       for index, (source, obj) in enumerate(zip(sources, objects))]
            for future in futures:
                future.result()
        binary = native / "emulator"
        run("link", cxx + [str(obj) for obj in objects] + [str(prefix / "lib/libfesvr.a"), "-lgmp", "-lpthread", "-o", str(binary)], out / "link.log")
        # A source/tool change during the build must never yield a complete receipt.
        if any(sha(Path(path)) != value for path, value in before.items()):
            raise RuntimeError("a selected build input changed while building")
        if args.merlin_receipt:
            from merlin_experiments.phase2.gsim_certificate import write_model_manifest, write_build_receipt

            manifest = write_model_manifest(model, sorted(path.name for path in model.iterdir() if path.suffix in {".cpp", ".h"}), out / "model_manifest.json")
            write_build_receipt(output=native / "build_receipt.json", firrtl=firrtl,
                                model_manifest=manifest, binary=binary, emitter=emitter,
                                cxx_wrapper=compiler, cxx_compiler=compiler, inputs=inputs,
                                commands=commands, firrtl_boundary="adopted_preexisting")
        status = "complete"
    except BaseException:
        status = "failed"
        raise
    finally:
        (out / "command_transcript.json").write_text(json.dumps({"status": status, "commands": commands}, indent=2) + "\n")
    print(json.dumps({"status": status, "binary": str(binary), "firrtl_sha256": sha(firrtl), "binary_sha256": sha(binary)}))


if __name__ == "__main__":
    main()
