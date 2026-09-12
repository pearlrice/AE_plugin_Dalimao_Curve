"""
Build and deploy DalimaoCurves AEGP plugin to the configured After Effects.

Usage:
    py deploy.py --check-env      # inspect paths without changing AE
    py deploy.py --build-only     # compile without installing
    py deploy.py --build          # compile + deploy (close AE first)
    py deploy.py                  # deploy only (DLL must exist)
    py deploy.py --verify         # verify installed file
"""
import argparse
import csv
import hashlib
import shutil
import subprocess
import os
import sys
from datetime import datetime
from pathlib import Path

PROJECT_ROOT   = os.path.dirname(os.path.abspath(__file__))
MSBUILD_PATH   = os.environ.get('MSBUILD_PATH', os.path.join(
    os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
    r'Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'))
VCXPROJ_PATH   = os.path.join(PROJECT_ROOT, 'DalimaoCurves', 'DalimaoCurves.vcxproj')
DLL_PATH       = os.path.join(PROJECT_ROOT, 'DalimaoCurves', 'x64', 'Release', 'DalimaoCurves.dll')

SDK_ROOT       = os.environ.get('AE_SDK_ROOT', os.path.join(
    PROJECT_ROOT, 'AfterEffectsSDK_25.6_61_win', 'ae25.6_61.64bit.AfterEffectsSDK'))
AE_SUPPORT_DIR = os.environ.get('AE_SUPPORT_DIR', os.path.join(
    os.environ.get('ProgramFiles', r'C:\Program Files'),
    r'Adobe\Adobe After Effects 2025\Support Files'))
AE_PLUGIN_DIR  = os.path.join(AE_SUPPORT_DIR, 'Plug-ins', 'DalimaoCurves')
AEX_FILENAME   = 'DalimaoCurves.aex'
AE_VERSION     = os.environ.get('AE_VERSION', '25.4')
DEPLOY_FILES   = []


def check_environment():
    expected = {
        'MSBuild': MSBUILD_PATH,
        'Project': VCXPROJ_PATH,
        'AE executable': os.path.join(AE_SUPPORT_DIR, 'AfterFX.exe'),
        'SDK headers': os.path.join(SDK_ROOT, 'Examples', 'Headers', 'AE_GeneralPlug.h'),
        'SDK SP': os.path.join(SDK_ROOT, 'Examples', 'Headers', 'SP', 'SPBasic.h'),
        'SDK utilities': os.path.join(SDK_ROOT, 'Examples', 'Util', 'AEGP_SuiteHandler.h'),
        'PiPL tool': os.path.join(SDK_ROOT, 'Examples', 'Resources', 'PiPLtool.exe'),
    }
    for label, path in expected.items():
        print('{0}: {1}: {2}'.format('OK' if os.path.isfile(path) else 'MISSING', label, path))
    print('Install target: {0}'.format(AE_PLUGIN_DIR))
    return all(os.path.isfile(path) for path in expected.values())


def build():
    if not os.path.exists(MSBUILD_PATH):
        print("ERROR: MSBuild not found at {0}".format(MSBUILD_PATH))
        return False
    if not os.path.exists(VCXPROJ_PATH):
        print("ERROR: Project file not found at {0}".format(VCXPROJ_PATH))
        return False
    # Rebuild the environment block to remove duplicate Path/PATH entries
    # inherited from some Windows launchers; MSBuild rejects those entries.
    build_env = {key.upper(): value for key, value in os.environ.items()}
    build_env['VSLANG'] = '1033'
    result = subprocess.run(
        [MSBUILD_PATH, VCXPROJ_PATH,
         '/p:Configuration=Release', '/p:Platform=x64',
         '/p:AESDKRoot=' + SDK_ROOT, '/t:Rebuild', '/nologo'],
        cwd=PROJECT_ROOT, capture_output=True, text=True,
        encoding='utf-8', errors='replace', env=build_env)
    log_path = Path(PROJECT_ROOT) / 'x64' / 'build.log'
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(result.stdout + result.stderr, encoding='utf-8')
    print('Build log: {0}'.format(log_path))
    if result.returncode != 0:
        lines = (result.stdout + result.stderr).strip().split('\n')
        print("Build FAILED. Last output:")
        for line in lines[-30:]:
            print("  {0}".format(line.rstrip()))
        return False
    if os.path.exists(DLL_PATH):
        print("Build succeeded: {0} ({1} bytes)".format(
            DLL_PATH, os.path.getsize(DLL_PATH)))
        return True
    print("Build reported success but DLL not found at {0}".format(DLL_PATH))
    return False


def deploy():
    aex_path = os.path.join(AE_PLUGIN_DIR, AEX_FILENAME)
    if not os.path.exists(DLL_PATH):
        print("ERROR: DLL not found at {0}".format(DLL_PATH))
        return False
    processes = subprocess.run(
        ['tasklist.exe', '/FI', 'IMAGENAME eq AfterFX.exe', '/FO', 'CSV', '/NH'],
        capture_output=True, text=True, errors='replace', check=True)
    if any(row and row[0].lower() == 'afterfx.exe'
           for row in csv.reader(processes.stdout.splitlines())):
        print('ERROR: Close After Effects before installing the plugin.')
        return False
    os.makedirs(AE_PLUGIN_DIR, exist_ok=True)
    if os.path.isfile(aex_path):
        backup = aex_path + '.' + datetime.now().strftime('%Y%m%d-%H%M%S-%f') + '.bak'
        shutil.copy2(aex_path, backup)
        print('Previous plugin backed up: {0}'.format(backup))
    shutil.copy2(DLL_PATH, aex_path)
    print("Deployed: {0} ({1} bytes)".format(aex_path, os.path.getsize(aex_path)))
    return True


def clear_cache():
    cmd = (
        'Remove-Item -Path "HKCU:\\Software\\Adobe\\After Effects'
        '\\{0}\\PluginCache\\en_US\\AELibraryPlugins'
        '\\DalimaoCurves.aex_*" -Force -ErrorAction SilentlyContinue'
    ).format(AE_VERSION)
    subprocess.run(['powershell', '-Command', cmd], capture_output=True)
    print("Cache cleared.")


def verify():
    expected = [AEX_FILENAME] + [f[1] for f in DEPLOY_FILES]
    print("\nVerify {0}:".format(AE_PLUGIN_DIR))
    ok = True
    for fname in expected:
        path = os.path.join(AE_PLUGIN_DIR, fname)
        if os.path.exists(path):
            print("  OK: {0} ({1} bytes)".format(fname, os.path.getsize(path)))
            if fname == AEX_FILENAME and os.path.isfile(DLL_PATH):
                source_hash = hashlib.sha256(Path(DLL_PATH).read_bytes()).hexdigest()
                target_hash = hashlib.sha256(Path(path).read_bytes()).hexdigest()
                matches = source_hash == target_hash
                print('  SHA256 {0}: {1}'.format('MATCH' if matches else 'MISMATCH', target_hash))
                ok = ok and matches
        else:
            print("  MISSING: {0}".format(fname))
            ok = False
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--check-env', action='store_true')
    modes.add_argument('--build-only', action='store_true')
    modes.add_argument('--build', action='store_true')
    modes.add_argument('--verify', action='store_true')
    parser.add_argument('--clear-cache', action='store_true', help='Clear this plugin cache after installation')
    args = parser.parse_args()
    if args.clear_cache and (args.check_env or args.build_only or args.verify):
        parser.error('--clear-cache requires an installation operation')
    if args.check_env:
        return 0 if check_environment() else 1
    if args.verify:
        return 0 if verify() else 1
    if args.build or args.build_only:
        if not build():
            return 1
    if args.build_only:
        return 0
    if not deploy():
        return 1
    if args.clear_cache:
        clear_cache()
    if not verify():
        return 1
    print("\nDone! Restart AE {0}.".format(AE_VERSION))
    return 0


if __name__ == '__main__':
    sys.stdout.reconfigure(errors='replace')
    sys.stderr.reconfigure(errors='replace')
    try:
        sys.exit(main())
    except (OSError, subprocess.SubprocessError) as error:
        print('ERROR: {0}'.format(error), file=sys.stderr)
        sys.exit(1)
