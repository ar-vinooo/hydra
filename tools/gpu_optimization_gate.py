"""Source identity shared with cmake/GpuOptimizationGate.cmake."""
import hashlib


def source_fingerprint(root):
    files = set()
    for directory, extensions in (
            ("src/core/hw/tegra_x1/gpu", ("cpp", "hpp")),
            ("src/core/hw/tegra_x1/cpu", ("cpp", "hpp")),
            ("src/core/hot", ("cpp", "hpp")),
            ("tests", ("cpp", "hpp", "py")), ("tools", ("py",))):
        for extension in extensions:
            files.update((root / directory).rglob(f"*.{extension}"))
    files.update(root / path for path in (
        "CMakeLists.txt", "src/core/CMakeLists.txt", "cmake/GpuOptimizationGate.cmake"))
    entries = []
    for path in sorted(files, key=lambda file: file.relative_to(root).as_posix()):
        content = path.read_bytes().replace(b"\r\n", b"\n")
        entries.append(path.relative_to(root).as_posix() + ":" + hashlib.sha256(content).hexdigest() + "\n")
    return hashlib.sha256("".join(entries).encode("utf-8")).hexdigest()


def check_build(root, build):
    lines = (build / "gpu-optimization-fingerprint.txt").read_text(encoding="utf-8").splitlines()
    fingerprint = source_fingerprint(root)
    if lines != [fingerprint, "1"]:
        raise RuntimeError("Optimization gate requires a current build with HYDRA_HOT_RELOAD=OFF")
    return fingerprint


def write_receipt(path, fingerprint):
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_text("HYDRA_GPU_P0_V1\n" + fingerprint + "\n", encoding="utf-8", newline="\n")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
