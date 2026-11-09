#!/usr/bin/env python3
"""Build optional C++ service dependencies without mixing Protobuf ABIs.

Windows example (after the normal CMake configure):
  python tools/bootstrap_services.py
  cmake --preset windows-x64 -DHELIX_BUILD_SERVICES=ON -DCMAKE_PREFIX_PATH=build/services-deps/install

Downloads pinned upstream archives into build/, verifies SHA256, and installs
gRPC/Protobuf/Abseil/librdkafka together (Kafka PLAINTEXT only). No engine source or global tools change.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ARCHIVES = {
    "grpc": ("https://github.com/grpc/grpc/archive/refs/tags/v1.85.0.tar.gz",
             "78d933f81e66d88c9f7a5ac419aa929691ef62b61d32a92f69c16aabf21ffabe", "grpc-1.85.0"),
    "librdkafka": ("https://github.com/confluentinc/librdkafka/archive/refs/tags/v2.16.0.tar.gz",
                   "e6b61de61d3282879a88e4ee3d9f634a8b05bf78a9198dfc25c4820c0c9ed231", "librdkafka-2.16.0"),
}


def command(*args):
    print("+", *map(str, args), flush=True)
    subprocess.run(list(map(str, args)), cwd=ROOT, check=True, stdout=sys.stdout, stderr=sys.stderr,
                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)


def unpack(archive, destination, prefix):
    """Extract only regular source files, including on Windows without symlinks."""
    destination.mkdir(parents=True, exist_ok=True)
    with (zipfile.ZipFile(archive) if zipfile.is_zipfile(archive) else tarfile.open(archive)) as source:
        members = source.infolist() if isinstance(source, zipfile.ZipFile) else source.getmembers()
        for member in members:
            name = member.filename if isinstance(source, zipfile.ZipFile) else member.name
            parts = Path(name).parts
            if not parts or parts[0] != prefix:
                raise ValueError(f"Unexpected archive prefix: {name}")
            relative = Path(*parts[1:])
            target = (destination / relative).resolve()
            if not target.is_relative_to(destination.resolve()):
                raise ValueError(f"Unsafe archive path: {name}")
            if isinstance(source, tarfile.TarFile) and not member.isfile():
                continue  # Build inputs are regular files; examples contain symlinks.
            if isinstance(source, zipfile.ZipFile) and member.is_dir():
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with (source.open(member) if isinstance(source, zipfile.ZipFile) else source.extractfile(member)) as reader:
                with target.open("wb") as writer:
                    shutil.copyfileobj(reader, writer)


def acquire(cache, url, checksum, destination, prefix):
    archive = cache / (checksum + ".archive")
    if not archive.exists():
        # Reuse archives already downloaded into the workspace.
        aliases = {"grpc-1.85.0": "grpc-1.85.0.tar.gz", "librdkafka-2.16.0": "librdkafka-2.16.0.tar.gz"}
        previous = ROOT / "build" / aliases.get(prefix, "__not_present__")
        if previous.is_file():
            shutil.copyfile(previous, archive)
        else:
            command("curl.exe" if shutil.which("curl.exe") else "curl", "-fL", "--retry", "2", "--output", archive, url)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != checksum:
        raise ValueError(f"SHA256 mismatch: {archive}")
    marker = destination / ".helix-archive-sha256"
    if not marker.exists() or marker.read_text() != checksum:
        unpack(archive, destination, prefix)
        marker.write_text(checksum)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine-build", type=Path, default=ROOT / "build/windows-x64")
    parser.add_argument("--directory", type=Path, default=ROOT / "build/services-deps")
    parser.add_argument("--system-prefix", type=Path, help="Additional optional CMake package prefix")
    parser.add_argument("--parallel", type=int, default=4)
    parser.add_argument("--msvc-mp", type=int, default=4, help="Compiler processes per MSBuild project")
    args = parser.parse_args()
    if args.parallel < 1 or args.msvc_mp < 1:
        parser.error("--parallel and --msvc-mp must be positive")
    work = args.directory.resolve()
    if not work.is_relative_to(ROOT / "build"):
        parser.error("--directory must be inside the repository build directory")
    cache = work / "archives"
    cache.mkdir(parents=True, exist_ok=True)
    for name, (url, checksum, prefix) in ARCHIVES.items():
        acquire(cache, url, checksum, work / name, prefix)
    grpc = work / "grpc"
    bazel = (grpc / "bazel/grpc_deps.bzl").read_text(encoding="utf-8")
    for name, target in [("com_google_absl", "abseil-cpp"), ("com_github_cares_cares", "cares/cares"),
                         ("com_googlesource_code_re2", "re2"), ("cel_c", "cel-c"),
                         ("boringssl", "boringssl-with-bazel"), ("zlib", "zlib")]:
        start = bazel.index('name = "' + name + '"')
        end = bazel.find("native.existing_rules()", start)
        block = bazel[start:end if end >= 0 else None]
        checksum = re.search(r'sha256 = "([0-9a-f]+)"', block)[1]
        prefix = re.search(r'strip_prefix = "([^"]+)"', block)[1]
        url = re.search(r'"(https://github.com/[^"\s]+)"', block)[1]
        acquire(cache, url, checksum, grpc / "third_party" / target, prefix)
    cmake_text = (grpc / "CMakeLists.txt").read_text(encoding="utf-8")
    pattern = r'download_archive\(\s+\$\{CMAKE_CURRENT_SOURCE_DIR\}/(\S+)\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+(\S+)\s+\)'
    for target, _, url, checksum, prefix in re.findall(pattern, cmake_text):
        acquire(cache, url, checksum, grpc / target, prefix)

    protobuf = args.engine_build.resolve() / "_deps/helix_protobuf-src"
    if not (protobuf / "CMakeLists.txt").is_file():
        raise ValueError("Configure the normal engine build first; its pinned Protobuf 36.2 source is required")
    install = work / "install"
    common = ["-DCMAKE_INSTALL_PREFIX=" + str(install), "-DCMAKE_CXX_STANDARD=20", "-DCMAKE_BUILD_TYPE=Release"]
    if args.system_prefix:
        system = args.system_prefix.resolve()
        common += ["-DCMAKE_PREFIX_PATH=" + str(system), "-DOPENSSL_ROOT_DIR=" + str(system)]
    if shutil.which("cl.exe") or os.name == "nt":
        common += ["-G", "Visual Studio 17 2022", "-A", "x64", "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /W3 /GR /EHsc /utf-8 /bigobj"]
    grpc_build = work / "grpc-build"
    flags = ["-DgRPC_INSTALL=ON", f"-DgRPC_BUILD_MSVC_MP_COUNT={args.msvc_mp}", "-DgRPC_BUILD_TESTS=OFF", "-DgRPC_DOWNLOAD_ARCHIVES=OFF",
             "-DgRPC_SSL_PROVIDER=module", "-DgRPC_ZLIB_PROVIDER=module", "-DPROTOBUF_ROOT_DIR=" + str(protobuf),
             "-Dprotobuf_BUILD_TESTS=OFF", "-Dprotobuf_BUILD_SHARED_LIBS=OFF", "-Dprotobuf_MSVC_STATIC_RUNTIME=OFF",
             "-Dprotobuf_WITH_ZLIB=OFF", "-Dprotobuf_BUILD_LIBUPB=OFF", "-Dprotobuf_INSTALL=ON",
             "-DABSL_ENABLE_INSTALL=ON", "-DRE2_BUILD_TESTING=OFF"]
    flags += [f"-DgRPC_BUILD_GRPC_{language}_PLUGIN=OFF" for language in ["CSHARP", "NODE", "OBJECTIVE_C", "PHP", "PYTHON", "RUBY"]]
    command("cmake", "-S", grpc, "-B", grpc_build, *common, *flags)
    command("cmake", "--build", grpc_build, "--config", "Release", "--parallel", args.parallel)
    command("cmake", "--install", grpc_build, "--config", "Release")
    kafka_build = work / "kafka-build"
    kafka_flags = ["-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /W3 /utf-8"] if os.name == "nt" else []
    command("cmake", "-S", work / "librdkafka", "-B", kafka_build, *common,
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON",
            "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON",
            "-DCMAKE_DISABLE_FIND_PACKAGE_ZSTD=ON", "-DRDKAFKA_BUILD_STATIC=ON", "-DRDKAFKA_BUILD_EXAMPLES=OFF",
            "-DRDKAFKA_BUILD_TESTS=OFF", "-DWITH_SSL=OFF", "-DWITH_SASL=OFF", "-DWITH_ZLIB=OFF", "-DWITH_ZSTD=OFF", "-DWITH_CURL=OFF", "-DENABLE_LZ4_EXT=OFF", *kafka_flags)
    command("cmake", "--build", kafka_build, "--config", "Release", "--parallel", args.parallel)
    command("cmake", "--install", kafka_build, "--config", "Release")
    print("Service dependency prefix:", install)


if __name__ == "__main__":
    main()
