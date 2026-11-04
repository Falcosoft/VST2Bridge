# Finds VST2 plugins in a folder and writes the bridges next to them (the work behind patcher.py).
#   32-bit plugin  name.dll  ->  name_x64.dll (vst2bridge-x64.dll) + vst2bridge-host32.exe   for 64-bit hosts
#   64-bit plugin  name.dll  ->  name_x86.dll (vst2bridge-x86.dll) + vst2bridge-host64.exe   for 32-bit hosts
import shutil, filecmp
from dataclasses import dataclass
from pathlib import Path
import pe

BRIDGE = {32 : ('vst2bridge-x64.dll', 'vst2bridge-host32.exe', '_x64'),
          64 : ('vst2bridge-x86.dll', 'vst2bridge-host64.exe', '_x86')}
SKIP_DIRS = {'.git', '__pycache__'}

@dataclass
class Plugin :
    path : Path
    bits : int
    state : str = ''        # 'new', 'current' (bridge is the same as the bundled one), 'old' (an older bridge), 'blocked' (a non-bridge file has the name)
    error : str = ''

    @property
    def bridge(self) -> Path : return self.path.with_name(self.path.stem + BRIDGE[self.bits][2] + '.dll')
    @property
    def helper(self) -> Path : return self.path.with_name(BRIDGE[self.bits][1])
    @property
    def target_bits(self) -> int : return 96 - self.bits      # 32 -> 64, 64 -> 32

def binaries_ok(bin_dir : Path) -> list[str] :
    """Names of bridge files missing in bin_dir (empty: all there)."""
    return [n for b in BRIDGE.values() for n in b[:2] if not (bin_dir / n).is_file()]

def scan(folder : Path, bin_dir : Path, log = lambda m : None) -> list[Plugin] :
    found = []
    for p in sorted(folder.rglob('*.dll'), key = lambda q : str(q).lower()) :
        if any(part in SKIP_DIRS for part in p.relative_to(folder).parts) or not p.is_file() : continue
        try : info = pe.read(p)
        except (pe.PEError, OSError) as e : log(f'{p}: {e}'); continue
        if info.is_bridge or info.is_jbridge or not info.is_vst2 or info.bits is None : continue
        found.append(state(Plugin(p, info.bits), bin_dir))
    return found

def state(pl : Plugin, bin_dir : Path) -> Plugin :
    b = pl.bridge
    if not b.exists() : pl.state = 'new'
    else :
        try : ours = pe.read(b).is_bridge
        except (pe.PEError, OSError) : ours = False
        if not ours : pl.state = 'blocked'
        elif filecmp.cmp(b, bin_dir / BRIDGE[pl.bits][0], shallow = False) and pl.helper.exists() \
             and filecmp.cmp(pl.helper, bin_dir / BRIDGE[pl.bits][1], shallow = False) : pl.state = 'current'
        else : pl.state = 'old'
    return pl

def create(pl : Plugin, bin_dir : Path) -> bool :
    """Write the bridge and its helper next to the plugin. Never replaces a file that is not one of ours."""
    stub, helper, _ = BRIDGE[pl.bits]
    if pl.state == 'blocked' :
        pl.error = 'blocked'; return False
    try :
        shutil.copyfile(bin_dir / stub, pl.bridge)
        if not pl.helper.exists() or not filecmp.cmp(pl.helper, bin_dir / helper, shallow = False) :
            shutil.copyfile(bin_dir / helper, pl.helper)
    except OSError as e :
        pl.error = str(e.strerror or e); return False
    pl.state, pl.error = 'current', ''
    return True
