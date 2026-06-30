# pre_extra_script.py

Import("env")

print("\n Running extra scripts ...\n")

ENV_LIBDEPS_DIR = env["PROJECT_LIBDEPS_DIR"] + "/" + env["PIOENV"]
SEMVER_INCR_BUILD_SCRIPT = ENV_LIBDEPS_DIR + "/semver-incr-build/semver-incr-build.bat"

# Pre-build action
def pre_build(source, target, env):
    print("\npre_build\n")

    # Make executable first
    #env.Execute(f"chmod +x {SEMVER_INCR_BUILD_SCRIPT}")
    env.Execute(f"{SEMVER_INCR_BUILD_SCRIPT} src/version.h")

env.AddPreAction("$PROGPATH", pre_build)