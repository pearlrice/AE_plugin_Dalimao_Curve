"""
Deploy DalimaoCurves AEGP plugin to After Effects 2024.

Usage:
    python deploy.py --build      # compile + deploy
    python deploy.py              # deploy only (DLL must exist)
    python deploy.py --verify     # verify only
    python deploy.py --clear-cache
"""
import shutil
import subprocess
import os
import sys

PROJECT_ROOT   = r'D:\Adobe\AE_Plugin_dev\DalimaoCurves'
MSBUILD_PATH   = r'G:\vs\vs_ide\MSBuild\Current\Bin\MSBuild.exe'
VCXPROJ_PATH   = os.path.join(PROJECT_ROOT, 'DalimaoCurves', 'DalimaoCurves.vcxproj')
DLL_PATH       = os.path.join(PROJECT_ROOT, 'DalimaoCurves', 'x64', 'Release', 'DalimaoCurves.dll')

AE_PLUGIN_DIR  = r'D:\Adobe\Adobe After Effects 2024\Support Files\Plug-ins\DalimaoCurves'
AEX_FILENAME   = 'DalimaoCurves.aex'
AE_VERSION     = '24.6'
DEPLOY_FILES   = []


def build():
    if not os.path.exists(MSBUILD_PATH):
        print("ERROR: MSBuild not found at {0}".format(MSBUILD_PATH))
        return False
    if not os.path.exists(VCXPROJ_PATH):
        print("ERROR: Project file not found at {0}".format(VCXPROJ_PATH))
        return False
    result = subprocess.run(
        [MSBUILD_PATH, VCXPROJ_PATH,
         '/p:Configuration=Release', '/p:Platform=x64', '/t:Rebuild'],
        cwd=PROJECT_ROOT, capture_output=True, text=True,
        encoding='utf-8', errors='replace')
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
    os.makedirs(AE_PLUGIN_DIR, exist_ok=True)
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
        else:
            print("  MISSING: {0}".format(fname))
            ok = False
    return ok


def main():
    args = sys.argv[1:]
    if '--verify' in args:
        verify()
        return
    if '--build' in args:
        if not build():
            sys.exit(1)
    if not deploy():
        sys.exit(1)
    if '--clear-cache' in args:
        clear_cache()
    verify()
    print("\nDone! Restart AE {0}.".format(AE_VERSION))


if __name__ == '__main__':
    main()
