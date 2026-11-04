# Finds VST2 plugins in a folder and writes the bridges next to them (the work behind patcher.py).
# Windows patcher:
#   32-bit plugin  name.dll  ->  name_x64.dll (vst2bridge-x64.dll) + name_x64.clap (vst2bridge-x64.clap)
#                                + vst2bridge-host32.exe                                     for 64-bit hosts
#   64-bit plugin  name.dll  ->  name_x86.dll (vst2bridge-x86.dll) + name_x86.clap (vst2bridge-x86.clap)
#                                + vst2bridge-host64.exe                                     for 32-bit hosts
#   The CLAP bridge loads the VST2 bridge of its own name, so both always go together.
# Linux and macOS patchers (CLAP only; the helper runs the plugin under Wine):
#   32-bit or 64-bit plugin  name.dll  ->  name.clap + vst2bridge-host32.exe / vst2bridge-host64.exe (plugin's bitness)
#   Linux: name.clap is vst2bridge-linux.clap. macOS: name.clap is a bundle name.clap/Contents/MacOS/name made from the
#   universal vst2bridge-macos (bundled gzipped in the frozen patcher, see build.yml) and signed ad hoc with codesign.
import gzip, hashlib, os, plistlib, shutil, filecmp, stat, subprocess, sys
from dataclasses import dataclass
from pathlib import Path
import pe

SYSTEM = os.environ.get('VB_TEST_SYSTEM') or ('windows' if sys.platform == 'win32' else 'macos' if sys.platform == 'darwin' else 'linux')   # VB_TEST_SYSTEM: CI only
BRIDGE = {32 : ('vst2bridge-x64.dll', 'vst2bridge-host32.exe', '_x64', 'vst2bridge-x64.clap'),
          64 : ('vst2bridge-x86.dll', 'vst2bridge-host64.exe', '_x86', 'vst2bridge-x86.clap')}
HELPER = {32 : 'vst2bridge-host32.exe', 64 : 'vst2bridge-host64.exe'}
POSIX_CLAP = {'linux' : 'vst2bridge-linux.clap', 'macos' : 'vst2bridge-macos'}
MARKER = b'VST2BridgeClap'                  # exported by every CLAP bridge (Windows: found by pe.py, else in the bytes)
PLIST_KEY = 'VST2BridgeSource'              # macOS bundle: SHA-256 of the binary it was made from (signing changes the file)
SKIP_DIRS = {'.git', '__pycache__'}

@dataclass
class Plugin :
    path : Path
    bits : int
    state : str = ''        # 'new', 'current' (bridges are the same as the bundled ones), 'old' (older or missing bridge files), 'blocked' (a file of another kind has a bridge name)
    error : str = ''

    @property
    def bridge(self) -> Path | None :
        return self.path.with_name(self.path.stem + BRIDGE[self.bits][2] + '.dll') if SYSTEM == 'windows' else None
    @property
    def clap(self) -> Path :
        if SYSTEM == 'windows' : return self.path.with_name(self.path.stem + BRIDGE[self.bits][2] + '.clap')
        return self.path.with_name(self.path.stem + '.clap')
    @property
    def helper(self) -> Path : return self.path.with_name(HELPER[self.bits])
    @property
    def target_bits(self) -> int : return 96 - self.bits if SYSTEM == 'windows' else 64    # Linux / macOS hosts are 64-bit
    @property
    def label(self) -> str :
        """What the patcher writes for this plugin, as shown in the list."""
        return f'{self.bridge.name} + .clap' if self.bridge else self.clap.name

def needed(bin_dir : Path) -> list[str] :
    if SYSTEM == 'windows' : return [n for b in BRIDGE.values() for n in (b[0], b[1], b[3])]
    return [POSIX_CLAP[SYSTEM]] + list(HELPER.values())

def binaries_ok(bin_dir : Path) -> list[str] :
    """Names of bridge files missing in bin_dir (empty: all there)."""
    return [n for n in needed(bin_dir) if not (bin_dir / n).is_file() and not (bin_dir / (n + '.gz')).is_file()]

def clap_bytes(bin_dir : Path) -> bytes :
    """The Linux / macOS CLAP; the frozen macOS patcher carries it gzipped (PyInstaller would thin a Mach-O data file
    to the architecture of the build machine)."""
    f = bin_dir / POSIX_CLAP[SYSTEM]
    return f.read_bytes() if f.is_file() else gzip.decompress((bin_dir / (f.name + '.gz')).read_bytes())

def scan(folder : Path, bin_dir : Path, log = lambda m : None) -> list[Plugin] :
    found = []
    for p in sorted(folder.rglob('*.dll'), key = lambda q : str(q).lower()) :
        if any(part in SKIP_DIRS for part in p.relative_to(folder).parts) or not p.is_file() : continue
        try : info = pe.read(p)
        except (pe.PEError, OSError) as e : log(f'{p}: {e}'); continue
        if info.is_bridge or info.is_jbridge or not info.is_vst2 or info.bits is None : continue
        found.append(state(Plugin(p, info.bits), bin_dir))
    return found

def _ours(path : Path, test) -> bool :
    try : return test(pe.read(path))
    except (pe.PEError, OSError) : return False

def _same(path : Path, ref : Path) -> bool : return path.is_file() and filecmp.cmp(path, ref, shallow = False)

def _bundle_exe(bundle : Path) -> Path : return bundle / 'Contents' / 'MacOS' / bundle.stem

def _posix_ours(clap : Path) -> bool :
    f = _bundle_exe(clap) if SYSTEM == 'macos' else clap
    if SYSTEM == 'macos' and not clap.is_dir() : return False
    try : return f.is_file() and MARKER in f.read_bytes()
    except OSError : return False

def _posix_current(clap : Path, bin_dir : Path) -> bool :
    data = clap_bytes(bin_dir)
    if SYSTEM == 'linux' : return clap.is_file() and clap.read_bytes() == data
    try : info = plistlib.loads((clap / 'Contents' / 'Info.plist').read_bytes())
    except (OSError, plistlib.InvalidFileException, ValueError) : return False
    return info.get(PLIST_KEY) == hashlib.sha256(data).hexdigest() and _bundle_exe(clap).is_file()

def state(pl : Plugin, bin_dir : Path) -> Plugin :
    if SYSTEM != 'windows' :
        c = pl.clap
        if c.exists() and not _posix_ours(c) : pl.state = 'blocked'
        elif not c.exists() : pl.state = 'new'
        elif _posix_current(c, bin_dir) and _same(pl.helper, bin_dir / HELPER[pl.bits]) : pl.state = 'current'
        else : pl.state = 'old'
        return pl
    stub, helper, _, clap = BRIDGE[pl.bits]
    b, c = pl.bridge, pl.clap
    if (b.exists() and not _ours(b, lambda i : i.is_bridge)) or (c.exists() and not _ours(c, lambda i : i.is_clap_bridge)) :
        pl.state = 'blocked'
    elif not b.exists() and not c.exists() : pl.state = 'new'
    elif _same(b, bin_dir / stub) and _same(c, bin_dir / clap) and _same(pl.helper, bin_dir / helper) : pl.state = 'current'
    else : pl.state = 'old'
    return pl

def make_bundle(binary : bytes, bundle : Path) -> None :
    """macOS: <stem>.clap/Contents/{Info.plist, MacOS/<stem>}; hosts load the bundle, Apple Silicon wants a signature
    (ad hoc is enough)."""
    stem = bundle.stem
    if bundle.exists() : shutil.rmtree(bundle)
    exe = _bundle_exe(bundle)
    exe.parent.mkdir(parents = True)
    exe.write_bytes(binary)
    exe.chmod(exe.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    ident = ''.join(ch if ch.isalnum() or ch in '-.' else '-' for ch in stem)
    info = dict(CFBundleName = stem, CFBundleExecutable = stem, CFBundleIdentifier = 'io.github.nightfright2k19.vst2bridge.' + ident,
                CFBundlePackageType = 'BNDL', CFBundleSignature = '????', CFBundleVersion = '1.0', CFBundleShortVersionString = '1.0',
                LSMinimumSystemVersion = '11.0', **{PLIST_KEY : hashlib.sha256(binary).hexdigest()})
    (bundle / 'Contents' / 'Info.plist').write_bytes(plistlib.dumps(info))
    r = subprocess.run(['codesign', '--force', '--sign', '-', str(bundle)], capture_output = True, text = True)
    if r.returncode != 0 : raise OSError(f'codesign failed for {bundle.name}: {r.stderr.strip()}')

def create(pl : Plugin, bin_dir : Path) -> bool :
    """Write the bridges and their helper next to the plugin. Never replaces a file that is not one of ours."""
    if pl.state == 'blocked' :
        pl.error = 'blocked'; return False
    try :
        if SYSTEM == 'windows' :
            stub, helper, _, clap = BRIDGE[pl.bits]
            shutil.copyfile(bin_dir / stub, pl.bridge)
            shutil.copyfile(bin_dir / clap, pl.clap)
        else :
            helper = HELPER[pl.bits]
            if SYSTEM == 'macos' : make_bundle(clap_bytes(bin_dir), pl.clap)
            else : pl.clap.write_bytes(clap_bytes(bin_dir))
        if not _same(pl.helper, bin_dir / helper) :
            shutil.copyfile(bin_dir / helper, pl.helper)
    except OSError as e :
        pl.error = str(e.strerror or e); return False
    pl.state, pl.error = 'current', ''
    return True

def wine_found() -> bool :
    """Linux / macOS: is there a Wine the CLAP will find (same search as bridge/stub.c, roughly)?"""

    if os.environ.get('VB_WINE') : return True
    if shutil.which('wine') : return True
    if SYSTEM == 'macos' :
        cands = ['/opt/homebrew/bin/wine', '/usr/local/bin/wine', '/opt/homebrew/bin/wine64', '/usr/local/bin/wine64',
                 '/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine', '/Applications/Wine Staging.app/Contents/Resources/wine/bin/wine',
                 '/Applications/Wine Devel.app/Contents/Resources/wine/bin/wine',
                 '/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine']
        return any(os.access(c, os.X_OK) for c in cands)
    return False
