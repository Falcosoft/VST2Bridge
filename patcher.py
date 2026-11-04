# VST2Bridge Patcher: finds VST2 plugins and writes a bridge next to each, so that the plugin works in hosts of the
# other bitness (32-bit plugin -> <name>_x64.dll for 64-bit hosts, 64-bit plugin -> <name>_x86.dll for 32-bit hosts).
# Releases freeze this file with PyInstaller into one Windows exe (.github/workflows/build.yml) that carries the bridge
# binaries (bridge/, built with mingw-w64) and docs/versions.md.
# * Plugins are searched in the patcher's folder (subfolders included) or in a folder the user chooses. The bridge and
#   its helper exe are written into the folder of each plugin (utility/bridges.py).
# * patcher --cli [folder] [--dry-run]   the same without a window
# * patcher --selftest [--selftest-out <file>]   checks that the bundled files are there (used by the build and the update)
# * Update: a release patcher looks for a newer release a moment after it has started and offers it with the Update
#   button (utility/selfupdate.py); --no-update-check (or VB_NO_UPDATE_CHECK=1) leaves out the check at the start.
#   Test options: --update-dir <folder with release files>, --update-test-version r<N>; --after-update <old> is used by
#   the update itself.
import sys, os, threading, queue, locale
from pathlib import Path

FROZEN = getattr(sys, 'frozen', False)
HERE = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent
CODE = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent))
BIN = CODE / 'bin' if FROZEN else CODE / 'bridge'          # the bridge binaries (dev: built by `make -C bridge`)
CHANGELOG = CODE / 'docs' / 'versions.md'
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
    rest = [a for a in args if not a.startswith('--')]
    folder = Path(rest[0]).resolve() if rest else HERE
    missing = bridges.binaries_ok(BIN)
    if missing : print('missing bridge files in', BIN, ':', ', '.join(missing)); return 2
    plugins = bridges.scan(folder, BIN, log = lambda m : print('  skipped:', m))
    if not plugins : print('No VST2 plugins found in', folder); return 0
    fails = 0
    for pl in plugins :
        line = f'{pl.path.relative_to(folder)} ({pl.bits}-bit) -> {pl.bridge.name} [{pl.state}]'
        if not dry and pl.state != 'current' :
            ok = bridges.create(pl, BIN)
            fails += not ok
            line += ' written' if ok else f' FAILED: {pl.error}'
        print(line)
    return 1 if fails else 0

def selftest(out : str | None) -> int :
    attach_console()
    import pe
    missing = bridges.binaries_ok(BIN)
    problems = list(missing)
    for bits, (stub, helper, _) in bridges.BRIDGE.items() :
        if stub in missing or helper in missing : continue
        s, h = pe.read(BIN / stub), pe.read(BIN / helper)
        if not (s.is_bridge and s.is_vst2 and s.bits == 96 - bits) : problems.append(stub)
        if h.bits != bits : problems.append(helper)
    if not CHANGELOG.is_file() : problems.append('docs/versions.md')
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
    st = dict(lang = system_language(LANGUAGES), folder = Path(opts.get('folder') or HERE), plugins = [], busy = False,
              status = ('scanning', {}), upd = dict(state = 'off'))
    q = queue.Queue()
    def T(key, **kw) : return TEXT[st['lang']].get(key, TEXT['en'][key]).format(**kw)

    frame = ttk.Frame(root, padding = 12); frame.grid(sticky = 'nsew')
    root.columnconfigure(0, weight = 1); root.rowconfigure(0, weight = 1)
    frame.columnconfigure(0, weight = 1); frame.rowconfigure(3, weight = 1)
    head = ttk.Frame(frame); head.grid(row = 0, column = 0, sticky = 'ew')
    ttk.Label(head, text = 'VST2Bridge Patcher', font = ('TkDefaultFont', 13, 'bold')).pack(side = 'left')
    lang_var = tk.StringVar(value = LANGUAGES[st['lang']])
    lang_box = ttk.Combobox(head, textvariable = lang_var, values = list(LANGUAGES.values()), state = 'readonly', width = 16,
                            height = len(LANGUAGES))
    lang_box.pack(side = 'right')
    intro = ttk.Label(frame, wraplength = 640, justify = 'left'); intro.grid(row = 1, column = 0, sticky = 'w', pady = (2, 8))
    fbox = ttk.Frame(frame); fbox.grid(row = 2, column = 0, sticky = 'ew')
    fbox.columnconfigure(1, weight = 1)
    folder_lbl = ttk.Label(fbox); folder_lbl.grid(row = 0, column = 0, sticky = 'w')
    folder_val = ttk.Label(fbox, foreground = '#444'); folder_val.grid(row = 0, column = 1, sticky = 'w', padx = 6)
    browse = ttk.Button(fbox); browse.grid(row = 0, column = 2, padx = (0, 4))
    rescan = ttk.Button(fbox); rescan.grid(row = 0, column = 3)
    cols = ('type', 'bridge', 'state')
    tree = ttk.Treeview(frame, columns = cols, height = 10)
    tree.column('#0', width = 240); tree.column('type', width = 110, anchor = 'center')
    tree.column('bridge', width = 250); tree.column('state', width = 120)
    tree.grid(row = 3, column = 0, sticky = 'nsew', pady = (8, 0))
    sb = ttk.Scrollbar(frame, orient = 'vertical', command = tree.yview); sb.grid(row = 3, column = 1, sticky = 'ns', pady = (8, 0))
    tree.configure(yscrollcommand = sb.set)
    progress = ttk.Progressbar(frame, mode = 'determinate'); progress.grid(row = 4, column = 0, columnspan = 2, sticky = 'ew', pady = (8, 0))
    bottom = ttk.Frame(frame); bottom.grid(row = 5, column = 0, columnspan = 2, sticky = 'ew', pady = (8, 0))
    bottom.columnconfigure(0, weight = 1)
    status = ttk.Label(bottom, wraplength = 380); status.grid(row = 0, column = 0, sticky = 'w')
    log_btn = ttk.Button(bottom); log_btn.grid(row = 0, column = 1, padx = 4)
    upd_btn = ttk.Button(bottom)
    create_btn = ttk.Button(bottom, width = 16); create_btn.grid(row = 0, column = 3)

    def state_text(pl) :
        if pl.error : return T('st_error', err = pl.error) if pl.error != 'blocked' else T('st_blocked')
        return T('st_' + pl.state)

    def refresh() :
        root.title(f'VST2Bridge Patcher {VERSION}')
        intro.configure(text = T('intro')); folder_lbl.configure(text = T('folder')); folder_val.configure(text = str(st['folder']))
        browse.configure(text = T('browse')); rescan.configure(text = T('rescan')); create_btn.configure(text = T('create'))
        log_btn.configure(text = T('changelog')); upd_btn.configure(text = T('upd_btn'))
        for c, k in (('#0', 'col_plugin'), ('type', 'col_type'), ('bridge', 'col_bridge'), ('state', 'col_state')) : tree.heading(c, text = T(k))
        tree.delete(*tree.get_children())
        for i, pl in enumerate(st['plugins']) :
            try : name = str(pl.path.relative_to(st['folder']))
            except ValueError : name = str(pl.path)
            tree.insert('', 'end', iid = str(i), text = name,
                        values = (T('bits', bits = pl.bits), T('for_bits', name = pl.bridge.name, bits = pl.target_bits), state_text(pl)))
        key, kw = st['status']
        status.configure(text = T(key, **kw))
        todo = [p for p in st['plugins'] if p.state in ('new', 'old')]
        busy = st['busy']
        create_btn.configure(state = 'disabled' if busy or not todo else 'normal')
        for b in (browse, rescan) : b.configure(state = 'disabled' if busy else 'normal')
        u = st['upd']
        if u['state'] == 'new' : upd_btn.grid(row = 0, column = 2, padx = 4)
        else : upd_btn.grid_remove()
        upd_btn.configure(state = 'disabled' if busy else 'normal')

    def set_lang(_e = None) :
        st['lang'] = next(k for k, v in LANGUAGES.items() if v == lang_var.get()); refresh()
    lang_box.bind('<<ComboboxSelected>>', set_lang)

    missing = bridges.binaries_ok(BIN)
    def start_scan() :
        if missing : st['status'] = ('missing_bin', dict(names = ', '.join(missing))); st['busy'] = True; refresh(); return
        st.update(busy = True, plugins = [], status = ('scanning', {})); progress.configure(mode = 'indeterminate'); progress.start(12); refresh()
        folder = st['folder']
        threading.Thread(target = lambda : q.put(('scanned', bridges.scan(folder, BIN))), daemon = True).start()

    def choose() :
        d = filedialog.askdirectory(initialdir = str(st['folder']), parent = root)
        if d : st['folder'] = Path(d); start_scan()

    def create() :
        todo = [p for p in st['plugins'] if p.state in ('new', 'old')]
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
                    todo = sum(p.state in ('new', 'old') for p in msg[1])
                    st['status'] = ('found', dict(n = len(msg[1]), todo = todo)) if msg[1] else ('none', {})
                    if any(p.state == 'blocked' for p in msg[1]) : st['status'] = ('blocked_hint', {})
                elif kind == 'progress' :
                    progress.configure(value = msg[1]); st['status'] = ('creating', dict(i = msg[1], n = msg[2]))
                elif kind == 'created' :
                    fail = sum(1 for p in msg[1] if p.error); st['busy'] = False
                    st['status'] = ('done_fail', dict(ok = len(msg[1]) - fail, fail = fail)) if fail else ('done', dict(ok = len(msg[1])))
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
