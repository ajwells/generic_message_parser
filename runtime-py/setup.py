"""Build of the optional accelerator gmsg._native (the metadata is in pyproject.toml).

The extension is compiled from native/_native.c and the header-only C runtime (runtime-c/include, found next to this
directory in the GMP repository, or at GMSG_C_INCLUDE). It uses the CPython limited API, so the wheel is abi3 (one
binary for CPython 3.12 and later). When it cannot be built (no C compiler, GMSG_PURE_INSTALL=1) the pure-Python
package is installed: the API and the results are the same, only slower.
"""

import os

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

HERE = os.path.dirname(os.path.abspath(__file__))
C_INCLUDE = os.environ.get("GMSG_C_INCLUDE") or os.path.join(HERE, "..", "runtime-c", "include")

ext_modules = []
if not os.environ.get("GMSG_PURE_INSTALL") and os.path.isfile(os.path.join(C_INCLUDE, "gmp", "c", "runtime.h")):
    ext_modules.append(Extension(
        "gmsg._native",
        sources=["native/_native.c"],
        include_dirs=[C_INCLUDE],
        define_macros=[("Py_LIMITED_API", "0x030C0000")],
        py_limited_api=True,
        optional=True,
    ))


class BuildExt(build_ext):
    """C11 for the C runtime (MSVC needs /std:c11 to define __STDC_VERSION__)."""

    def build_extensions(self):
        flag = "/std:c11" if self.compiler.compiler_type == "msvc" else "-std=c11"
        for ext in self.extensions:
            ext.extra_compile_args = [*(ext.extra_compile_args or []), flag]
        super().build_extensions()


setup(ext_modules=ext_modules, cmdclass={"build_ext": BuildExt},
      options={"bdist_wheel": {"py_limited_api": "cp312"}} if ext_modules else {})
