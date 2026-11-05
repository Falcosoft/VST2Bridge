# Finds VST2 plugins and writes the bridges into the output folder (the work behind patcher.py). Everything for one plugin
# goes into the same folder: a copy of the plugin DLL (the bridge looks for <name>.dll next to itself), the bridge files and
# the helper exe.
# Windows patcher (FORMATS: 'both' = VST2 + CLAP, 'vst2', 'clap'):
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
FORMATS = ('both', 'vst2', 'clap')           # Windows patcher: which bridges to write (VST2 + CLAP is the default)

@dataclass
class Plugin :
    path : Path             # the plugin as found
    bits : int
    out : Path              # output folder: the plugin is copied there and the bridges are written next to the copy
    fmt : str = 'both'      # Windows: 'both', 'vst2' or 'clap'; Linux / macOS always write the CLAP
    state : str = ''        # 'new', 'current' (bridges are the same as the bundled ones), 'old' (older or missing bridge files), 'blocked' (a file of another kind has a bridge name), 'dup' (another plugin of the same name came first)
    error : str = ''
    selected : bool = True  # the window's checkbox

    @property
    def copy(self) -> Path : return self.out / (self.path.stem + '.dll')
    @property
    def bridge(self) -> Path | None :
        wanted = SYSTEM == 'windows' and self.fmt in ('both', 'vst2')
        return self.out / (self.path.stem + BRIDGE[self.bits][2] + '.dll') if wanted else None
    @property
    def clap(self) -> Path | None :
        if SYSTEM != 'windows' : return self.out / (self.path.stem + '.clap')
        return self.out / (self.path.stem + BRIDGE[self.bits][2] + '.clap') if self.fmt in ('both', 'clap') else None
    @property
    def helper(self) -> Path : return self.out / HELPER[self.bits]
    @property
    def target_bits(self) -> int : return 96 - self.bits if SYSTEM == 'windows' else 64    # Linux / macOS hosts are 64-bit
    @property
    def label(self) -> str :
        """What the patcher writes for this plugin, as shown in the list."""
        return ' + '.join(f.name for f in (self.bridge, self.clap) if f)

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

def scan(folders : list[Path], bin_dir : Path, out_dir : Path, fmt : str = 'both', log = lambda m : None) -> list[Plugin] :
    """VST2 plugins in the folders (subfolders included); files inside out_dir are left out."""
    found, seen, names = [], set(), set()
    try : out_real = out_dir.resolve()
    except OSError : out_real = out_dir
    for folder in folders :
        if not folder.is_dir() : continue
        for p in sorted(folder.rglob('*.dll'), key = lambda q : str(q).lower()) :
            if any(part in SKIP_DIRS for part in p.relative_to(folder).parts) or not p.is_file() : continue
            try : real = p.resolve()
            except OSError : real = p
            if real in seen or out_real in real.parents : continue
            seen.add(real)
            try : info = pe.read(p)
            except (pe.PEError, OSError) as e : log(f'{p}: {e}'); continue
            if info.is_bridge or info.is_jbridge or not info.is_vst2 or info.bits is None : continue
            pl = Plugin(p, info.bits, out_dir, fmt)
            if pl.copy.name.lower() in names : pl.state = 'dup'
            else : names.add(pl.copy.name.lower()); state(pl, bin_dir)
            found.append(pl)
    found.sort(key = lambda q : str(q.path).lower())
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
    own = _same(pl.copy, pl.path)
    if SYSTEM != 'windows' :
        c = pl.clap
        if c.exists() and not _posix_ours(c) : pl.state = 'blocked'
        elif not c.exists() : pl.state = 'new'
        elif _posix_current(c, bin_dir) and _same(pl.helper, bin_dir / HELPER[pl.bits]) and own : pl.state = 'current'
        else : pl.state = 'old'
        return pl
    stub, helper, _, clap = BRIDGE[pl.bits]
    b, c = pl.bridge, pl.clap
    files = [(b, stub, lambda i : i.is_bridge)] * (b is not None) + [(c, clap, lambda i : i.is_clap_bridge)] * (c is not None)
    if any(f.exists() and not _ours(f, test) for f, _, test in files) : pl.state = 'blocked'
    elif not any(f.exists() for f, _, _ in files) : pl.state = 'new'
    elif all(_same(f, bin_dir / n) for f, n, _ in files) and _same(pl.helper, bin_dir / helper) and own : pl.state = 'current'
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
    """Write the plugin copy, the bridges and their helper into the output folder. Never replaces a bridge file that is not one of ours."""
    if pl.state in ('blocked', 'dup') :
        pl.error = 'blocked'; return False
    try :
        pl.out.mkdir(parents = True, exist_ok = True)
        if not _same(pl.copy, pl.path) : shutil.copyfile(pl.path, pl.copy)
        if SYSTEM == 'windows' :
            stub, helper, _, clap = BRIDGE[pl.bits]
            if pl.bridge : shutil.copyfile(bin_dir / stub, pl.bridge)
            if pl.clap : shutil.copyfile(bin_dir / clap, pl.clap)
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
