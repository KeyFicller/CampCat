"""Debug hook for the sidecar, injected through PYTHONPATH.

The host spawns it as `<venv>/python3 campcat/sources/py/main.py` with a fixed
argv (`resolve_sidecar_paths` in llm_host.cpp), so `-m debugpy --listen` cannot
be added from outside. Python imports `sitecustomize` from anywhere on
`sys.path` at startup, and `execvp` passes this process's environment down to
the child, so pointing PYTHONPATH here is the only way in without editing the
sidecar:

    PYTHONPATH=$PWD/misc/debug CAMPCAT_DEBUGPY=1 ./build/campcat config.json

Then attach an IDE to 127.0.0.1:5678 ("attach: sidecar" in .vscode/launch.json).

Listening is deferred because a synchronous `debugpy.listen()` from inside
`sitecustomize` runs before `site` finishes initialising and never opens the
port. The cost is that code running in the first half second cannot be stopped;
everything from `handle_request` onwards can, and the host waits 24 h for a
turn, so sitting on a breakpoint is safe. The `debugpy` check keeps the
adapter process debugpy forks from starting a second one.
"""

import os
import sys

if os.environ.get("CAMPCAT_DEBUGPY") and "debugpy" not in sys.argv[0]:
    import threading

    def _listen() -> None:
        import debugpy

        debugpy.listen(
            ("127.0.0.1", int(os.environ.get("CAMPCAT_DEBUGPY_PORT", "5678")))
        )

    threading.Timer(0.5, _listen).start()
