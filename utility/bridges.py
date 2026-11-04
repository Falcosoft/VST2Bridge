# Finds VST2 plugins in a folder and writes the bridges next to them (the work behind patcher.py).
#   32-bit plugin  name.dll  ->  name_x64.dll (vst2bridge-x64.dll) + name_x64.clap (vst2bridge-x64.clap)
#                                + vst2bridge-host32.exe                                     for 64-bit hosts
#   64-bit plugin  name.dll  ->  name_x86.dll (vst2bridge-x86.dll) + name_x86.clap (vst2bridge-x86.clap)
#                                + vst2bridge-host64.exe                                     for 32-bit hosts
# The CLAP bridge loads the VST2 bridge of its own name, so both always go together.
import shutil, filecmp
from dataclasses import dataclass
from pathlib import Path
import pe

BRIDGE = {32 : ('vst2bridge-x64.dll', 'vst2bridge-host32.exe', '_x64', 'vst2bridge-x64.clap'),
          64 : ('vst2bridge-x86.dll', 'vst2bridge-host64.exe', '_x86', 'vst2bridge-x86.clap')}
SKIP_DIRS = {'.git', '__pycache__'}

@dataclass
class Plugin :
    path : Path
    bits : int
    state : str = ''        # 'new', 'current' (bridges are the same as the bundled ones), 'old' (older or missing bridge files), 'blocked' (a file of another kind has a bridge name)
    error : str = ''

    @property
    def bridge(self) -> Path : return self.path.with_name(self.path.stem + BRIDGE[self.bits][2] + '.dll')
    @property
    def clap(self) -> Path : return self.path.with_name(self.path.stem + BRIDGE[self.bits][2] + '.clap')
    @property
    def helper(self) -> Path : return self.path.with_name(BRIDGE[self.bits][1])
    @property
    def target_bits(self) -> int : return 96 - self.bits      # 32 -> 64, 64 -> 32

def binaries_ok(bin_dir : Path) -> list[str] :
    """Names of bridge files missing in bin_dir (empty: all there)."""
    return [n for b in BRIDGE.values() for n in (b[0], b[1], b[3]) if not (bin_dir / n).is_file()]

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

def _same(path : Path, ref : Path) -> bool : return path.exists() and filecmp.cmp(path, ref, shallow = False)

def state(pl : Plugin, bin_dir : Path) -> Plugin :
    stub, helper, _, clap = BRIDGE[pl.bits]
    b, c = pl.bridge, pl.clap
    if (b.exists() and not _ours(b, lambda i : i.is_bridge)) or (c.exists() and not _ours(c, lambda i : i.is_clap_bridge)) :
        pl.state = 'blocked'
    elif not b.exists() and not c.exists() : pl.state = 'new'
    elif _same(b, bin_dir / stub) and _same(c, bin_dir / clap) and _same(pl.helper, bin_dir / helper) : pl.state = 'current'
    else : pl.state = 'old'
    return pl

def create(pl : Plugin, bin_dir : Path) -> bool :
    """Write the bridges and their helper next to the plugin. Never replaces a file that is not one of ours."""
    stub, helper, _, clap = BRIDGE[pl.bits]
    if pl.state == 'blocked' :
        pl.error = 'blocked'; return False
    try :
        shutil.copyfile(bin_dir / stub, pl.bridge)
        shutil.copyfile(bin_dir / clap, pl.clap)
        if not _same(pl.helper, bin_dir / helper) :
            shutil.copyfile(bin_dir / helper, pl.helper)
    except OSError as e :
        pl.error = str(e.strerror or e); return False
    pl.state, pl.error = 'current', ''
    return True
