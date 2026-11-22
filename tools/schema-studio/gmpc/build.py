"""Build gmpc as a WebAssembly module for Schema Studio's exact decode engine.

    py tools/schema-studio/gmpc/build.py [--emsdk DIR] [--build-dir DIR]   build, copy gmpc.js here, write gmpc.stamp
    py tools/schema-studio/gmpc/build.py --check                           exit 1 when gmpc.js is older than the sources

gmpc.js is a single file (the WebAssembly is embedded), so Studio loads it from file:// with a <script> tag. It is
committed, so Studio's exact engine works without a build; the stamp is a hash of the generator sources, and the
--check run (ctest: gmp_studio_gmpc_fresh) fails when they changed without a rebuild.

Needs emsdk (https://emscripten.org): EMSDK, or --emsdk, or ~/emsdk. CMake and Ninja must be on PATH.
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
OUT_JS = HERE / "gmpc.js"
STAMP = HERE / "gmpc.stamp"
INFO = HERE / "gmpc-info.js"


def source_files():
    files = [REPO / "generator" / "CMakeLists.txt"]
    for ext in ("cpp", "hpp"):
        files += [Path(p) for p in glob.glob(str(REPO / "generator" / "src" / "**" / f"*.{ext}"), recursive=True)]
    return sorted(files, key=lambda p: p.relative_to(REPO).as_posix())


def stamp():
    h = hashlib.sha256()
    for f in source_files():
        h.update(f.relative_to(REPO).as_posix().encode())
        h.update(b"\0")
        h.update(f.read_bytes().replace(b"\r\n", b"\n"))
        h.update(b"\0")
    return h.hexdigest()[:16]


def check():
    want = stamp()
    have = STAMP.read_text().strip() if STAMP.exists() else ""
    if not OUT_JS.exists() or have != want:
        print(f"gmpc.js is out of date (built from {have or 'nothing'}, sources are {want}): "
              f"run py tools/schema-studio/gmpc/build.py")
        return 1
    print(f"gmpc.js is up to date ({want})")
    return 0


def find_emsdk(arg):
    for d in [arg, os.environ.get("EMSDK"), str(Path.home() / "emsdk")]:
        if d and (Path(d) / "upstream" / "emscripten").is_dir():
            return Path(d)
    sys.exit("emsdk not found: install it (https://emscripten.org/docs/getting_started) or pass --emsdk")


def build(args):
    emsdk = find_emsdk(args.emsdk)
    env = dict(os.environ)
    pythons = sorted(glob.glob(str(emsdk / "python" / "*" / "python.exe"))) or sorted(glob.glob(str(emsdk / "python" / "*" / "bin" / "python3")))
    nodes = sorted(glob.glob(str(emsdk / "node" / "*" / "node.exe"))) or sorted(glob.glob(str(emsdk / "node" / "*" / "bin" / "node")))
    python = pythons[-1] if pythons else sys.executable
    env["EMSDK"] = str(emsdk)
    env["EM_CONFIG"] = str(emsdk / ".emscripten")
    env["EMSDK_PYTHON"] = python
    if nodes:
        env["EMSDK_NODE"] = nodes[-1]
    env["PATH"] = os.pathsep.join([str(emsdk / "upstream" / "emscripten")] + ([str(Path(nodes[-1]).parent)] if nodes else []) +
                                  [env.get("PATH", "")])
    build_dir = Path(args.build_dir or REPO / "build-wasm")
    s = stamp()
    emcmake = emsdk / "upstream" / "emscripten" / "emcmake.py"
    subprocess.run([python, str(emcmake), "cmake", "-S", str(REPO), "-B", str(build_dir), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=MinSizeRel", "-DGMP_BUILD_TESTS=OFF", "-DGMP_BUILD_EXAMPLES=OFF",
                    f"-DGMPC_SOURCE_STAMP={s}"], check=True, env=env)
    subprocess.run(["cmake", "--build", str(build_dir), "--target", "gmpc_wasm"], check=True, env=env)
    shutil.copyfile(build_dir / "generator" / "gmpc_wasm.js", OUT_JS)
    STAMP.write_text(s + "\n")
    INFO.write_text("// Written by build.py: Schema Studio loads this small file to know that gmpc.js (the exact engine) exists.\n"
                    f"window.GMP_GMPC = {{ stamp: '{s}', size: {OUT_JS.stat().st_size} }};\n")
    print(f"wrote {OUT_JS} ({OUT_JS.stat().st_size // 1024} KB), stamp {s}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="only check that gmpc.js matches the generator sources")
    ap.add_argument("--emsdk", help="emsdk directory (default: EMSDK or ~/emsdk)")
    ap.add_argument("--build-dir", help="build directory (default: build-wasm in the repository)")
    args = ap.parse_args()
    return check() if args.check else build(args)


if __name__ == "__main__":
    sys.exit(main())
