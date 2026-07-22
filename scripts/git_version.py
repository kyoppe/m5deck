Import("env")
import subprocess

revision = "dev"
try:
    revision = (
        subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=env["PROJECT_DIR"],
            stderr=subprocess.DEVNULL,
        )
        .decode()
        .strip()
    )
except Exception:
    pass

env.Append(BUILD_FLAGS=['-DM5DECK_VERSION=\\"%s\\"' % revision])
