"""Apply compiler overrides after the native platform loads its GCC tool defaults."""

import os

Import("env", "projenv")  # noqa: F821 -- SCons/PlatformIO

# The native builder replaces CC/CXX after pre-scripts and clones environments
# for project sources and libraries. Override every environment used by targets.
compilers = {name: os.environ[name] for name in ("CC", "CXX") if os.environ.get(name)}
if compilers:
    for target in [env, projenv] + [library.env for library in env.GetLibBuilders()]:
        target.Replace(**compilers)
