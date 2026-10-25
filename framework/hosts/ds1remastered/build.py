"""Build the Remastered/x64 host proxy. No installation or game access."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ZIG = ROOT / "work/tools/zig-windows-x86_64-0.13.0/zig.exe"

def run(args):
    import subprocess
    subprocess.run([str(a) for a in args], check=True)

def compile_cpp(source, dest, includes=(), extra=(), target="x86_64-windows-gnu"):
    dest.parent.mkdir(parents=True, exist_ok=True)
    args = [
        ZIG,
        "c++",
        "-target",
        target,
        "-std=c++17",
        "-Werror=unused-variable",
        "-Werror=unused-but-set-variable",
        "-O2",
        "-static-libstdc++",
        "-I",
        ROOT / "framework/sdk",
    ]
    for path in includes:
        args.extend(["-I", path])
    args.extend([source, *extra, "-o", dest])
    import subprocess
    log = dest.with_suffix(dest.suffix + ".build.log")
    with log.open("w") as f:
        result = subprocess.run([str(a) for a in args], stdout=f, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text(encoding="utf-8")[-14000:])
        raise RuntimeError("Build failed: " + str(source))

def build_remastered_host(output):
    """Build the Remastered x64 host proxy as dinput8.dll"""
    target = "x86_64-windows-gnu"
    
    # Compile minhook for x64
    objects = []
    for name in ("buffer", "hook", "trampoline", "hde/hde64"):
        source = ROOT / "work/minhook/src" / (name + ".c")
        obj = output.parent / (source.stem + ".o")
        run([ZIG, "cc", "-target", target, "-O2", "-c", source, "-o", obj])
        objects.append(obj)
    
    host = ROOT / "framework/hosts/ds1remastered"
    
    # camera_capture.h is header-only (inline functions), no separate compilation needed
    # Compile camera_capture.cpp
    cam_obj = output.parent / "camera_capture.o"
    compile_cpp(
        host / "camera_capture.cpp",
        cam_obj,
        [output.parent],
        extra=["-c"],
        target=target
    )
    objects.append(cam_obj)
    
    # Compile logging.cpp
    log_obj = output.parent / "logging.o"
    compile_cpp(
        host / "logging.cpp",
        log_obj,
        [output.parent],
        extra=["-c"],
        target=target
    )
    objects.append(log_obj)

    # Compile geometry_pass.cpp (geometry-pass test draw)
    gp_obj = output.parent / "geometry_pass.o"
    compile_cpp(
        host / "geometry_pass.cpp",
        gp_obj,
        [output.parent],
        extra=["-c"],
        target=target
    )
    objects.append(gp_obj)
    
    # Build the main host DLL
    compile_cpp(
        host / "runtime.cpp",
        output,
        [output.parent],
        extra=[
            "-shared",
            host / "proxy.def",
            *objects,
            "-ld3d11",
            "-ldxgi",
            "-ldinput8",
            "-luser32",
            "-lwinmm",
            "-lgdi32",
            "-lshell32",
            "-lole32",
        ],
        target=target
    )

if __name__ == "__main__":
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "work/framework-build/dinput8_x64.dll"
    build_remastered_host(output)
    print("Built Remastered host: " + str(output))