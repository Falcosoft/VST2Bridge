# Reads what the patcher needs from a Windows DLL (PE file): the machine type and the names of the exported functions.
# Standard library only.
import struct
from pathlib import Path

MACHINES = {0x014C : 32, 0x8664 : 64}           # IMAGE_FILE_MACHINE_I386, IMAGE_FILE_MACHINE_AMD64

class PEError(Exception) : pass

class PEInfo :
    def __init__(self, machine : int, exports : set[str], is_dll : bool) :
        self.machine, self.exports, self.is_dll = machine, exports, is_dll

    @property
    def bits(self) -> int | None : return MACHINES.get(self.machine)
    @property
    def is_vst2(self) -> bool : return 'VSTPluginMain' in self.exports or 'main' in self.exports
    @property
    def is_bridge(self) -> bool : return 'VST2BridgeStub' in self.exports
    @property
    def is_clap_bridge(self) -> bool : return 'VST2BridgeClap' in self.exports     # our CLAP bridge (<name>_x64.clap)
    @property
    def is_jbridge(self) -> bool : return 'JBridgeBootstrap' in self.exports       # JBridge's own bridge files

def read(path : Path) -> PEInfo :
    data = Path(path).read_bytes()
    def u16(o) : return struct.unpack_from('<H', data, o)[0]
    def u32(o) : return struct.unpack_from('<I', data, o)[0]
    try :
        if data[:2] != b'MZ' : raise PEError('not a Windows program file')
        pe = u32(0x3C)
        if data[pe : pe + 4] != b'PE\0\0' : raise PEError('not a PE file')
        machine, nsec = u16(pe + 4), u16(pe + 6)
        opt_size, chars = u16(pe + 20), u16(pe + 22)
        opt = pe + 24
        magic = u16(opt)
        if magic == 0x10B : dirs = opt + 96          # PE32
        elif magic == 0x20B : dirs = opt + 112       # PE32+
        else : raise PEError('unknown optional header')
        sections = []
        for i in range(nsec) :
            s = opt + opt_size + i * 40
            vsize, vaddr, rawsize, rawptr = u32(s + 8), u32(s + 12), u32(s + 16), u32(s + 20)
            sections.append((vaddr, max(vsize, rawsize), rawptr))
        def off(rva) :
            for vaddr, size, rawptr in sections :
                if vaddr <= rva < vaddr + size : return rva - vaddr + rawptr
            raise PEError('address outside the sections')
        def cstr(o) :
            end = data.index(b'\0', o)
            return data[o : end].decode('ascii', 'replace')
        exports = set()
        exp_rva, exp_size = u32(dirs), u32(dirs + 4)
        if exp_rva and exp_size :
            e = off(exp_rva)
            n_names, names = u32(e + 24), u32(e + 32)
            for i in range(min(n_names, 100000)) :
                exports.add(cstr(off(u32(off(names) + 4 * i))))
        return PEInfo(machine, exports, bool(chars & 0x2000))
    except (struct.error, IndexError, ValueError) as e :
        raise PEError(f'damaged PE file ({e})')
