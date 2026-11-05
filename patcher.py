# VST2Bridge Patcher: finds VST2 plugins and writes bridges next to each.
# * Windows: the plugin works in hosts of the other bitness (32-bit plugin -> <name>_x64.dll + <name>_x64.clap for
#   64-bit hosts, 64-bit plugin -> <name>_x86.dll + <name>_x86.clap for 32-bit hosts; the .clap is a CLAP plugin around
#   the VST2 bridge of the same name).
# * Linux and macOS: CLAP only, <name>.clap (macOS: a bundle) + the helper of the plugin's bitness; the CLAP runs the
#   Windows plugin under Wine.
# Releases freeze this file with PyInstaller for each system (.github/workflows/build.yml); each patcher carries the
# bridge files of its system (bridge/: mingw-w64 for the Windows files and helpers, gcc / clang for the Linux / macOS
# CLAP) and docs/versions.md.
# * Plugins are searched directly in the patcher's own folder (top level only) and in the folders the user adds
#   (subfolders included); each plugin has a checkbox (all selected at the start). The original plugins are never moved or
#   changed: the bridges and the helper exe are written into the folder each plugin lies in (utility/bridges.py).
#   Windows only: the output format (VST2 + CLAP, VST2 only, CLAP only).
# * patcher --cli [folder] [--dry-run] [--format both|vst2|clap]   the same without a window
#   (the patcher's folder + folder if given; --format counts for the Windows patcher only)
# * patcher --selftest [--selftest-out <file>]   checks that the bundled files are there (used by the build and the update)
# * Update: a release patcher looks for a newer release a moment after it has started and offers it with the Update
#   button (utility/selfupdate.py); --no-update-check (or VB_NO_UPDATE_CHECK=1) leaves out the check at the start.
#   Test options: --update-dir <folder with release files>, --update-test-version r<N>; --after-update <old> is used by
#   the update itself.
import sys, os, threading, queue, locale, subprocess
from pathlib import Path

FROZEN = getattr(sys, 'frozen', False)
HERE = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent
if FROZEN and sys.platform == 'darwin' and HERE.parent.name == 'Contents' :
    HERE = HERE.parents[2]                      # VST2Bridge-Patcher.app/Contents/MacOS -> folder holding the .app
CODE = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent))
BIN = CODE / 'bin' if FROZEN else CODE / 'bridge'          # the bridge binaries (dev: built by `make -C bridge`)
CHANGELOG = CODE / 'docs' / 'versions.md'
ICONS = [CODE / ('icons' if FROZEN else 'utility/icons') / n for n in ('patcher.png', 'patcher_32.png', 'patcher_16.png')]
                                                # window icons in three sizes (the exe / app icon is set by the build)
sys.path.insert(0, str(CODE / 'utility'))
sys.path.insert(0, str(CODE))
try : from patcher_version import VERSION
except ImportError : VERSION = 'dev'

import bridges

def system_language(known) -> str :
    names = []
    if sys.platform == 'win32' :
        try :
            import ctypes
            names.append(locale.windows_locale.get(ctypes.windll.kernel32.GetUserDefaultUILanguage(), ''))
        except Exception : pass
    elif sys.platform == 'darwin' :                 # apps started from the Finder get no LANG
        try :
            out = subprocess.run(['defaults', 'read', '-g', 'AppleLanguages'], capture_output = True, text = True, timeout = 3).stdout
            names += [x.strip(' "(),\n') for x in out.split('\n') if x.strip(' "(),\n')]
        except Exception : pass
    for var in ('LC_ALL', 'LC_MESSAGES', 'LANG', 'LANGUAGE') :
        if os.environ.get(var) : names.append(os.environ[var])
    alias = {'iw' : 'he', 'in' : 'id', 'pes' : 'fa'}         # old codes some systems report
    for n in names :
        code = n.replace('-', '_').split('_')[0].split('.')[0].lower()
        code = alias.get(code, code)
        if code in known : return code
    return 'en'

def changelog_text() -> str :
    try : return CHANGELOG.read_text(encoding = 'utf-8')
    except OSError : return 'https://github.com/NightFright2k19/VST2Bridge/blob/main/docs/versions.md'

# ---- command line ---------------------------------------------------------------------------------------------------

def attach_console() :
    """The frozen patcher has no console of its own: print into the one it was started from."""
    if not (FROZEN and sys.platform == 'win32') : return
    try :
        import ctypes
        if ctypes.windll.kernel32.AttachConsole(-1) :              # ATTACH_PARENT_PROCESS
            sys.stdout = sys.stderr = open('CONOUT$', 'w', encoding = 'utf-8', errors = 'replace')
    except Exception : pass

def run_cli(args : list[str]) -> int :
    attach_console()
    dry = '--dry-run' in args
    fmt, rest = 'both', []
    it = iter(args)
    for a in it :
        if a == '--format' : fmt = next(it, '')
        elif not a.startswith('--') : rest.append(a)
    if fmt not in bridges.FORMATS : print('--format must be one of:', ', '.join(bridges.FORMATS)); return 2
    if bridges.SYSTEM != 'windows' : fmt = 'both'
    folders = [Path(r).resolve() for r in rest]
    missing = bridges.binaries_ok(BIN)
    if missing : print('missing bridge files in', BIN, ':', ', '.join(missing)); return 2
    plugins = bridges.scan(folders, BIN, fmt, flat = [HERE], log = lambda m : print('  skipped:', m))
    if not plugins : print('No VST2 plugins found in', ', '.join(str(f) for f in folders + [HERE])); return 0
    fails = 0
    if bridges.SYSTEM != 'windows' and not bridges.wine_found() : print('note: Wine was not found; the CLAP bridges need it')
    for pl in plugins :
        line = f'{pl.path} ({pl.bits}-bit) -> {pl.out / pl.label} [{pl.state}]'
        if not dry and pl.state in ('new', 'old') :
            ok = bridges.create(pl, BIN)
            fails += not ok
            line += ' written' if ok else f' FAILED: {pl.error}'
        print(line)
    return 1 if fails else 0

def macho_archs(data : bytes) -> set[int] :
    """CPU types in a (universal) Mach-O file: 0x0100000C arm64, 0x01000007 x86_64."""
    import struct
    if data[:4] == b'\xca\xfe\xba\xbe' :
        n = struct.unpack_from('>I', data, 4)[0]
        return {struct.unpack_from('>I', data, 8 + 20 * i)[0] for i in range(min(n, 16))}
    return {struct.unpack_from('<I', data, 4)[0]} if data[:4] == b'\xcf\xfa\xed\xfe' else set()

def selftest(out : str | None) -> int :
    attach_console()
    import pe
    missing = bridges.binaries_ok(BIN)
    problems = list(missing)
    for bits, helper in bridges.HELPER.items() :
        if helper not in missing and pe.read(BIN / helper).bits != bits : problems.append(helper)
    if bridges.SYSTEM == 'windows' :
        for bits, (stub, helper, _, clap) in bridges.BRIDGE.items() :
            if stub in missing or clap in missing : continue
            s, c = pe.read(BIN / stub), pe.read(BIN / clap)
            if not (s.is_bridge and s.is_vst2 and s.bits == 96 - bits) : problems.append(stub)
            if not (c.is_clap_bridge and 'clap_entry' in c.exports and c.bits == 96 - bits) : problems.append(clap)
    elif bridges.POSIX_CLAP[bridges.SYSTEM] not in missing :
        name, data = bridges.POSIX_CLAP[bridges.SYSTEM], bridges.clap_bytes(BIN)
        if bridges.SYSTEM == 'linux' : kind_ok = data[:4] == b'\x7fELF' and data[18:20] == b'\x3e\x00'           # x86-64
        else : kind_ok = macho_archs(data) == {0x0100000C, 0x01000007}                                          # arm64 + x86_64
        if not (kind_ok and bridges.MARKER in data and b'clap_entry' in data) : problems.append(name)
    if not CHANGELOG.is_file() : problems.append('docs/versions.md')
    if not all(f.is_file() for f in ICONS) : problems.append('icons')
    text = f'VST2Bridge-Patcher {VERSION}'
    if out : Path(out).write_text(text + '\n')
    print(text, 'OK' if not problems else 'PROBLEMS: ' + ', '.join(problems))
    return 1 if problems else 0

# ---- window ---------------------------------------------------------------------------------------------------------

def main_window(opts : dict) :
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    from patcher_i18n import TEXT, LANGUAGES
    import selfupdate

    root = tk.Tk()
    root.title(f'VST2Bridge Patcher {VERSION}')
    root.minsize(640, 360)
    try : root.iconphoto(True, *[tk.PhotoImage(file = str(f)) for f in ICONS])
    except Exception : pass                                     # a missing icon must never stop the patcher
    st = dict(lang = system_language(LANGUAGES), folders = [Path(opts['folder'])] if opts.get('folder') else [],
              fmt = 'both', unsel = set(), plugins = [], busy = False,
              status = ('scanning', {}), upd = dict(state = 'off'))
    q = queue.Queue()
    def T(key, **kw) : return TEXT[st['lang']].get(key, TEXT['en'][key]).format(**kw)

    frame = ttk.Frame(root, padding = 12); frame.grid(sticky = 'nsew')
    root.columnconfigure(0, weight = 1); root.rowconfigure(0, weight = 1)
    frame.columnconfigure(0, weight = 1); frame.rowconfigure(4, weight = 1)
    head = ttk.Frame(frame); head.grid(row = 0, column = 0, sticky = 'ew')
    ttk.Label(head, text = 'VST2Bridge Patcher', font = ('TkDefaultFont', 13, 'bold')).pack(side = 'left')
    lang_var = tk.StringVar(value = LANGUAGES[st['lang']])
    lang_box = ttk.Combobox(head, textvariable = lang_var, values = list(LANGUAGES.values()), state = 'readonly', width = 16,
                            height = len(LANGUAGES))
    lang_box.pack(side = 'right')
    intro = ttk.Label(frame, wraplength = 640, justify = 'left'); intro.grid(row = 1, column = 0, sticky = 'w', pady = (2, 8))
    fbox = ttk.Frame(frame); fbox.grid(row = 2, column = 0, sticky = 'ew')
    fbox.columnconfigure(1, weight = 1)
    folder_lbl = ttk.Label(fbox); folder_lbl.grid(row = 0, column = 0, sticky = 'nw')
    folder_val = ttk.Label(fbox, foreground = '#444', wraplength = 400, justify = 'left'); folder_val.grid(row = 0, column = 1, sticky = 'nw', padx = 6, pady = (1, 0))  # top-aligned like the label, 1 px lower
    browse = ttk.Button(fbox); browse.grid(row = 0, column = 2, padx = (0, 4), sticky = 'n')
    rescan = ttk.Button(fbox); rescan.grid(row = 0, column = 3, sticky = 'n')
    hint = ttk.Label(fbox, foreground = '#444', wraplength = 600, justify = 'left'); hint.grid(row = 1, column = 0, columnspan = 4, sticky = 'w', pady = (6, 0))
    fmt_box = ttk.Frame(frame)
    fmt_lbl = ttk.Label(fmt_box); fmt_lbl.pack(side = 'left')
    fmt_var = tk.StringVar()
    fmt_combo = ttk.Combobox(fmt_box, textvariable = fmt_var, state = 'readonly', width = 18)
    fmt_combo.pack(side = 'left', padx = 6)
    if bridges.SYSTEM == 'windows' : fmt_box.grid(row = 3, column = 0, sticky = 'w', pady = (6, 0))
    cols = ('type', 'bridge', 'state')
    tree = ttk.Treeview(frame, columns = cols, height = 10, selectmode = 'browse')
    tree.column('#0', width = 240); tree.column('type', width = 110, anchor = 'center')
    tree.column('bridge', width = 250); tree.column('state', width = 120)
    tree.grid(row = 4, column = 0, sticky = 'nsew', pady = (8, 0))
    sb = ttk.Scrollbar(frame, orient = 'vertical', command = tree.yview); sb.grid(row = 4, column = 1, sticky = 'ns', pady = (8, 0))
    tree.configure(yscrollcommand = sb.set)
    progress = ttk.Progressbar(frame, mode = 'determinate'); progress.grid(row = 5, column = 0, columnspan = 2, sticky = 'ew', pady = (8, 0))
    bottom = ttk.Frame(frame); bottom.grid(row = 6, column = 0, columnspan = 2, sticky = 'ew', pady = (8, 0))
    bottom.columnconfigure(0, weight = 1)
    status = ttk.Label(bottom, wraplength = 380); status.grid(row = 0, column = 0, sticky = 'w')
    log_btn = ttk.Button(bottom); log_btn.grid(row = 0, column = 1, padx = 4)
    upd_btn = ttk.Button(bottom)
    create_btn = ttk.Button(bottom, width = 16); create_btn.grid(row = 0, column = 3)

    def state_text(pl) :
        if pl.error : return T('st_error', err = pl.error) if pl.error != 'blocked' else T('st_blocked')
        return T('st_' + pl.state)

    def todo_list() : return [p for p in st['plugins'] if p.selected and p.state in ('new', 'old')]
    def row_text(pl) :
        try : name = str(pl.path.relative_to(next(f for f in [HERE] + st['folders'] if f in pl.path.parents)))
        except (ValueError, StopIteration) : name = str(pl.path)
        return ('\u2611 ' if pl.selected else '\u2610 ') + name
    def fmt_names() : return [T('fmt_both'), T('fmt_vst2'), T('fmt_clap')]

    def refresh() :
        root.title(f'VST2Bridge Patcher {VERSION}')
        intro.configure(text = T('intro') if bridges.SYSTEM == 'windows' else T('intro_posix')); folder_lbl.configure(text = T('folders'))
        folder_val.configure(text = '\n'.join(str(f) for f in [HERE] + st['folders'])); hint.configure(text = T('hint'))
        fmt_lbl.configure(text = T('out_format')); fmt_combo.configure(values = fmt_names(), state = 'disabled' if st['busy'] else 'readonly')
        fmt_var.set(fmt_names()[bridges.FORMATS.index(st['fmt'])])
        browse.configure(text = T('add_folder')); rescan.configure(text = T('rescan')); create_btn.configure(text = T('create'))
        log_btn.configure(text = T('changelog')); upd_btn.configure(text = T('upd_btn'))
        for c, k in (('#0', 'col_plugin'), ('type', 'col_type'), ('bridge', 'col_bridge'), ('state', 'col_state')) : tree.heading(c, text = T(k))
        tree.delete(*tree.get_children())
        for i, pl in enumerate(st['plugins']) :
            tree.insert('', 'end', iid = str(i), text = row_text(pl),
                        values = (T('bits', bits = pl.bits), T('for_bits', name = pl.label, bits = pl.target_bits), state_text(pl)))
        key, kw = st['status']
        status.configure(text = T(key, **kw))
        busy = st['busy']
        create_btn.configure(state = 'disabled' if busy or not todo_list() else 'normal')
        for b in (browse, rescan) : b.configure(state = 'disabled' if busy else 'normal')
        u = st['upd']
        if u['state'] == 'new' : upd_btn.grid(row = 0, column = 2, padx = 4)
        else : upd_btn.grid_remove()
        upd_btn.configure(state = 'disabled' if busy else 'normal')

    def set_lang(_e = None) :
        st['lang'] = next(k for k, v in LANGUAGES.items() if v == lang_var.get()); refresh()
    lang_box.bind('<<ComboboxSelected>>', set_lang)

    def set_fmt(_e = None) :
        st['fmt'] = bridges.FORMATS[fmt_combo.current()]
        for pl in st['plugins'] :
            pl.fmt = st['fmt']
            bridges.state(pl, BIN); pl.error = ''
        n = len(st['plugins']); st['status'] = ('found', dict(n = n, todo = len(todo_list()))) if n else ('none_input', {}); refresh()
    fmt_combo.bind('<<ComboboxSelected>>', set_fmt)

    def toggle(_e) :
        if st['busy'] : return
        iid = tree.identify_row(_e.y) if _e.type == '4' else (tree.focus() if _e.type == '2' else '')
        if not iid : return
        pl = st['plugins'][int(iid)]
        if pl.state == 'blocked' : return
        pl.selected = not pl.selected
        (st['unsel'].discard if pl.selected else st['unsel'].add)(pl.path)
        tree.item(iid, text = row_text(pl))
        st['status'] = ('found', dict(n = len(st['plugins']), todo = len(todo_list())))
        status.configure(text = T(*st['status'][:1], **st['status'][1])); create_btn.configure(state = 'normal' if todo_list() else 'disabled')
    tree.bind('<Button-1>', toggle); tree.bind('<space>', toggle)

    missing = bridges.binaries_ok(BIN)
    wine_ok = bridges.SYSTEM == 'windows' or bridges.wine_found()
    def start_scan() :
        if missing : st['status'] = ('missing_bin', dict(names = ', '.join(missing))); st['busy'] = True; refresh(); return
        st.update(busy = True, plugins = [], status = ('scanning', {})); progress.configure(mode = 'indeterminate'); progress.start(12); refresh()
        folders, fmt = st['folders'], st['fmt']
        threading.Thread(target = lambda : q.put(('scanned', bridges.scan(folders, BIN, fmt, flat = [HERE]))), daemon = True).start()

    def choose() :
        d = filedialog.askdirectory(initialdir = str((st['folders'] or [HERE])[-1]), parent = root)
        if d and Path(d) not in st['folders'] : st['folders'].append(Path(d))
        if d : start_scan()

    def create() :
        todo = todo_list()
        st['busy'] = True; progress.configure(mode = 'determinate', maximum = len(todo), value = 0); refresh()
        def work() :
            for i, pl in enumerate(todo) :
                q.put(('progress', i + 1, len(todo)))
                bridges.create(pl, BIN)
            q.put(('created', todo))
        threading.Thread(target = work, daemon = True).start()

    def show_log() :
        w = tk.Toplevel(root); w.title(T('changelog_title')); w.geometry('760x480')
        txt = tk.Text(w, wrap = 'word', padx = 8, pady = 8); txt.pack(fill = 'both', expand = True)
        txt.insert('1.0', changelog_text()); txt.configure(state = 'disabled')

    # ---- update
    upd_on = selfupdate.enabled(VERSION, FROZEN)
    cur = selfupdate.current(VERSION)
    def check_update() :
        def work() :
            try : q.put(('upd', selfupdate.latest_revision(), None))
            except selfupdate.UpdateError as e : q.put(('upd', None, str(e)))
        threading.Thread(target = work, daemon = True).start()
    def do_update() :
        rev = st['upd']['rev']
        if not messagebox.askokcancel(T('upd_btn'), T('upd_ask', ver = f'r{rev}'), parent = root) : return
        st['busy'] = True; progress.configure(mode = 'determinate', maximum = 100, value = 0); refresh()
        def work() :
            try :
                cmd = selfupdate.perform(rev, HERE, FROZEN, lambda d, t : q.put(('upd_prog', d, t)), lambda k : q.put(('status', 'upd_verify')))
                q.put(('upd_go', cmd, None))
            except (selfupdate.UpdateError, OSError) as e : q.put(('upd_go', None, str(e)))
        threading.Thread(target = work, daemon = True).start()

    def poll() :
        try :
            while True :
                msg = q.get_nowait()
                kind = msg[0]
                if kind == 'scanned' :
                    progress.stop(); progress.configure(mode = 'determinate', value = 0)
                    st['plugins'] = msg[1]; st['busy'] = False
                    for p in msg[1] : p.selected = p.path not in st['unsel']
                    st['status'] = ('found', dict(n = len(msg[1]), todo = len(todo_list()))) if msg[1] else ('none_input', {})
                    if any(p.state == 'blocked' for p in msg[1]) : st['status'] = ('blocked_hint', {})
                    if bridges.SYSTEM != 'windows' and not wine_ok : st['status'] = ('no_wine', {})
                elif kind == 'progress' :
                    progress.configure(value = msg[1]); st['status'] = ('creating', dict(i = msg[1], n = msg[2]))
                elif kind == 'created' :
                    fail = sum(1 for p in msg[1] if p.error); st['busy'] = False
                    st['status'] = ('done_fail', dict(ok = len(msg[1]) - fail, fail = fail)) if fail else ('done_out', dict(ok = len(msg[1])))
                elif kind == 'status' : st['status'] = (msg[1], {})
                elif kind == 'upd' :
                    rev, err = msg[1], msg[2]
                    if err : st['upd'] = dict(state = 'error')
                    elif rev is not None and cur is not None and rev > cur :
                        st['upd'] = dict(state = 'new', rev = rev)
                        if not st['busy'] : st['status'] = ('upd_new', dict(ver = f'r{rev}'))
                elif kind == 'upd_prog' :
                    d, t = msg[1], msg[2]
                    if t : progress.configure(value = 100 * d / t)
                    st['status'] = ('upd_load', dict(pct = f'{100 * d // t} %' if t else f'{d >> 20} MB'))
                elif kind == 'upd_go' :
                    cmd, err = msg[1], msg[2]
                    if err : st['busy'] = False; st['status'] = ('upd_failed', dict(err = err))
                    else : selfupdate.launch(cmd, HERE); root.destroy(); return
                refresh()
        except queue.Empty : pass
        root.after(80, poll)

    browse.configure(command = choose); rescan.configure(command = start_scan); create_btn.configure(command = create)
    log_btn.configure(command = show_log); upd_btn.configure(command = do_update)
    if opts.get('after_update') :
        threading.Thread(target = selfupdate.after_update, args = (opts['after_update'], FROZEN), daemon = True).start()
        st['status'] = ('upd_done', dict(old = Path(opts['after_update']).stem))
    refresh(); start_scan()
    if upd_on and selfupdate.auto_check() and not opts.get('no_update_check') : root.after(1500, check_update)
    root.after(80, poll)
    root.mainloop()

def main() :
    args = sys.argv[1:]
    if '--cli' in args : sys.exit(run_cli([a for a in args if a != '--cli']))
    def opt(name) :
        if name in args :
            i = args.index(name)
            return args[i + 1] if i + 1 < len(args) else ''
        return None
    if '--selftest' in args : sys.exit(selftest(opt('--selftest-out')))
    for name, env in (('--update-dir', 'VB_UPDATE_DIR'), ('--update-test-version', 'VB_UPDATE_TEST_VERSION')) :
        if opt(name) : os.environ[env] = opt(name)
    main_window(dict(after_update = opt('--after-update'), no_update_check = '--no-update-check' in args or None,
                     folder = opt('--folder')))

if __name__ == '__main__' :
    main()
