"""TotK Testbank: eine Oberfläche, die totk-pc-recomp auf einem Windows-PC einrichtet, baut und für den Koop-Test
vorbereitet. Sie führt die Schritte aus docs/building.md des Repos der Reihe nach aus und zeigt ihre Ausgabe.

Start: TotK-Testbank.cmd (installiert bei Bedarf uv und startet dieses Skript mit Python 3.14).
Nur Standardbibliothek. Gespeichert werden ausschließlich Pfade und Einstellungen (testbank.json neben diesem
Skript); den Inhalt der Schlüsseldatei liest die Testbank nie, das tun die Werkzeuge des Repos selbst.

    python testbank.py              Fenster
    python testbank.py --selftest   Prüft die Logik ohne Fenster (Konfiguration, totk.ini, Befehle, Ablauf)
    python testbank.py --install    Einfache Installation: vier Felder, ein Knopf, alles läuft der Reihe nach
                                    (mit --dry nur anzeigen). So startet TotK-Koop-Setup.cmd die Testbank.
"""
import datetime
import hashlib
import json
import os
import queue
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

APP = 'TotK Testbank'
HERE = Path(__file__).resolve().parent
STATE_FILE = HERE / 'testbank.json'
LOG_DIR = HERE / 'logs'
WIN = os.name == 'nt'
EXE = '.exe' if WIN else ''

MSYS = Path(r'C:\msys64')
UCRT = MSYS / 'ucrt64' / 'bin'
MSYS_BASH = MSYS / 'usr' / 'bin' / 'bash.exe'
PF86 = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))
PF = Path(os.environ.get('ProgramFiles', r'C:\Program Files'))
VS_INSTALLER = PF86 / 'Microsoft Visual Studio' / 'Installer'
MSYS_PACKAGES = ('mingw-w64-ucrt-x86_64-{clang,lld,ninja,gcc,make,pkgconf,sdl3,opus,libvpx,zstd,vulkan-devel,'
                 'vulkan-memory-allocator,vulkan-validation-layers,shaderc,spirv-tools} base-devel git unzip')

VERSIONS = [('v143', '1.4.3 (empfohlen)'), ('v121', '1.2.1')]
DEFAULTS = {
    'repo_dir': str(Path.home() / 'totk-pc-recomp'),
    'repo_url': 'https://github.com/valentderah/totk-pc-recomp',
    'patch': str(HERE / 'totk-coop-stage2.patch'),
    'base': '', 'update': '', 'keys': '', 'firmware': '',
    'version': 'v143',
    'shaders': True,
    'coop': True, 'peer_mode': 'loop', 'peer_ip': '', 'port': '27460', 'marker': '2',
}


# ---------------------------------------------------------------- reine Logik (ohne Fenster, im Selbsttest geprüft)

def toml_str(s):
    """Ein TOML-String für einen Pfad: wörtlich in einfachen Anführungszeichen, sonst als Basic String."""
    if "'" not in s and '\n' not in s:
        return f"'{s}'"
    return json.dumps(s, ensure_ascii=False)   # JSON-Escapes sind gültige TOML-Basic-Strings


def config_toml(s):
    """config.local.toml des Repos aus den Eingaben. Nur Pfade; der Arbeitsordner bleibt build (build_game.py legt
    totk.exe immer unter build\\<version> ab)."""
    lines = ['# Geschrieben von der TotK Testbank. Nur Pfade: die Werkzeuge lesen die Schlüsseldatei selbst.', '',
             '[input]', f"base = {toml_str(s['base'])}",
             f"updates = [{toml_str(s['update'])}]" if s['update'] else 'updates = []',
             f"keys = {toml_str(s['keys'])}"]
    lines.append(f"firmware = {toml_str(s['firmware'])}" if s['firmware'] else "# firmware = 'C:\\Switch\\firmware'")
    lines += ['', '[work]', "dir = 'build'", '']
    return '\n'.join(lines)


IPV4 = re.compile(r'^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$')


def valid_ip(s):
    m = IPV4.match(s.strip())
    return bool(m) and all(int(p) <= 255 and (p == '0' or not p.startswith('0')) for p in m.groups()) and s.strip() != '0.0.0.0'


def valid_port(s):
    return s.strip().isdigit() and 1024 <= int(s) <= 65535


def coop_values(s):
    """Die totk.ini-Schlüssel des Koop-Tests (runtime/cfg.c), oder (None, Fehlertext)."""
    if not valid_port(s['port']):
        return None, 'Der Port muss eine Zahl von 1024 bis 65535 sein.'
    if s['peer_mode'] == 'ip' and not valid_ip(s['peer_ip']):
        return None, 'Die IP des Partners muss die Form 192.168.1.20 haben.'
    peer = 'loop' if s['peer_mode'] == 'loop' else s['peer_ip'].strip()
    # coop_puppet: der Partner als zweiter Link in der Welt; 2 ist der Allein-Test (die Figur steht 2 m daneben)
    puppet = '0' if not s['coop'] else '2' if peer == 'loop' else '1'
    return {'coop': '1' if s['coop'] else '0', 'coop_port': s['port'].strip(), 'coop_peer': peer,
            'coop_marker': s['marker'], 'coop_puppet': puppet}, None


KEY_LINE = re.compile(r'^(\s*)([A-Za-z0-9_]+)(\s*=\s*)(.*)$')


def ini_set(text, values):
    """totk.ini mit den Schlüsseln aus values: vorhandene (nicht auskommentierte) Zeilen werden ersetzt, alle anderen
    bleiben, fehlende kommen unter [coop] ans Ende. runtime/cfg.c liest Schlüssel ohne Rücksicht auf Abschnitte und
    Groß-/Kleinschreibung; bei doppelten gilt der letzte, darum wird jedes Vorkommen ersetzt."""
    want = {k.lower(): v for k, v in values.items()}
    found = set()
    out = []
    for line in text.splitlines():
        m = KEY_LINE.match(line)
        if m and m.group(2).lower() in want:
            key = m.group(2).lower()
            out.append(f'{m.group(1)}{m.group(2)}{m.group(3)}{want[key]}')
            found.add(key)
        else:
            out.append(line)
    rest = [k for k in want if k not in found]
    if rest:
        if out and out[-1].strip():
            out.append('')
        if not any(l.strip().lower() == '[coop]' for l in out):
            out.append('[coop]')
        out += [f'{k} = {want[k]}' for k in rest]
    return '\n'.join(out) + '\n'


def local_ips():
    """IPv4-Adressen dieses PCs im Netz (ohne 127.x), die erste ist die der Standardroute. Es geht kein Paket raus:
    ein UDP-connect legt nur die Route fest."""
    ips = []
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(('10.255.255.255', 1))
            ips.append(s.getsockname()[0])
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip not in ips:
                ips.append(ip)
    except OSError:
        pass
    return [ip for ip in ips if not ip.startswith('127.') and ip != '0.0.0.0']


def file_info(p, folder=False):
    """(ok, Text) für ein Eingabefeld."""
    if not p:
        return None, ''
    path = Path(p)
    if folder:
        return (True, 'Ordner gefunden') if path.is_dir() else (False, 'Ordner fehlt')
    if not path.is_file():
        return False, 'Datei fehlt'
    size = path.stat().st_size
    return True, f'{size / 2**30:.1f} GB' if size >= 2**30 else f'{size / 2**20:.1f} MB' if size >= 2**20 else f'{size / 1024:.0f} KB'


def free_gb(p):
    q = Path(p)
    while not q.exists() and q.parent != q:
        q = q.parent
    try:
        return shutil.disk_usage(q).free / 2**30
    except OSError:
        return None


def which(name, *fallbacks):
    found = shutil.which(name)
    if found:
        return Path(found)
    for f in fallbacks:
        if f and Path(f).is_file():
            return Path(f)
    return None


def uv_path():
    home = Path.home()
    return which('uv', home / '.local' / 'bin' / ('uv' + EXE),
                 Path(os.environ.get('LOCALAPPDATA', home)) / 'Microsoft' / 'WinGet' / 'Links' / ('uv' + EXE))


def git_path():
    return which('git', PF / 'Git' / 'cmd' / 'git.exe')


def child_env():
    """Umgebung der Kindprozesse: MSYS2 UCRT64 vorn im PATH (die Skripte des Repos erwarten seine Werkzeuge), das
    Visual-Studio-Installer-Verzeichnis für vswhere (NativeAOT, docs/building.md), uv und Git, UTF-8-Ausgabe."""
    env = dict(os.environ)
    extra = []
    if WIN:
        extra += [str(UCRT), str(VS_INSTALLER)]
    for tool in (uv_path(), git_path()):
        if tool:
            extra.append(str(tool.parent))
    env['PATH'] = os.pathsep.join(extra + [env.get('PATH', '')])
    env['CHERE_INVOKING'] = '1'   # MSYS2-Login-Bash bleibt im Arbeitsordner (dem Repo) statt nach ~ zu wechseln
    env['PYTHONUTF8'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    return env


# ---------------------------------------------------------------- Schritte

@dataclass
class Ctx:
    repo: Path
    s: dict

    @property
    def ver(self):
        return self.s['version']

    def venv_py(self, name):
        return self.repo / name / ('Scripts/python.exe' if WIN else 'bin/python')

    @property
    def py312(self):
        return str(self.venv_py('.venv'))

    @property
    def py314(self):
        return str(self.venv_py('.venv-py314'))

    @property
    def exe(self):
        return self.repo / 'build' / self.ver / ('totk' + EXE)

    @property
    def extracted(self):
        return self.repo / 'build' / 'extracted' / self.ver


def bash_cmd(script):
    """script in der MSYS2-Bash (Windows) bzw. der System-Bash, im Repo-Ordner."""
    if WIN:
        return [str(MSYS_BASH), '-lc', script]
    return ['bash', '-c', script]


def req_stamp(ctx):
    h = hashlib.sha256()
    for f in ('tools/requirements.txt', 'tools/requirements-py314.txt'):
        p = ctx.repo / f
        h.update(p.read_bytes() if p.is_file() else b'-')
    return h.hexdigest()


def venv_cmds(ctx):
    uv = str(uv_path() or 'uv')
    cmds = []
    for name, py, req in (('.venv', '3.12', 'tools/requirements.txt'), ('.venv-py314', '3.14', 'tools/requirements-py314.txt')):
        if not ctx.venv_py(name).is_file():
            cmds.append([uv, 'venv', '--python', py, name])
        cmds.append([uv, 'pip', 'install', '--python', str(ctx.venv_py(name)), '-r', req])
    return cmds


def venv_done(ctx):
    stamp = ctx.repo / '.venv' / 'testbank-requirements.sha256'
    return (ctx.venv_py('.venv').is_file() and ctx.venv_py('.venv-py314').is_file() and stamp.is_file()
            and stamp.read_text().strip() == req_stamp(ctx))


def venv_after(ctx):
    (ctx.repo / '.venv' / 'testbank-requirements.sha256').write_text(req_stamp(ctx) + '\n')


@dataclass
class Step:
    key: str
    title: str
    detail: str
    commands: Callable[[Ctx], list]
    done: Optional[Callable[[Ctx], bool]] = None   # True: das Ergebnis liegt schon vor, der Schritt wird übersprungen
    after: Optional[Callable[[Ctx], None]] = None
    optional: bool = False                          # nur mit Häkchen
    keep_going: bool = False                        # alle Befehle laufen, auch nach einem Fehler


STEPS = [
    Step('venv', 'Python-Umgebungen', '.venv (3.12) und .venv-py314 (3.14) mit den Paketen des Repos',
         venv_cmds, venv_done, venv_after),
    Step('hactool', 'hactool bauen', 'Werkzeug zum Entpacken von NCA und Update (tools/build_hactool.py)',
         lambda c: [[c.py312, 'tools/build_hactool.py']],
         lambda c: (c.repo / 'toolchain' / 'hactool' / ('hactool' + EXE)).is_file()),
    Step('refs', 'Referenz-Repos holen', 'refs/ mit der Shader-Liste von MacNativeGames (tools/fetch_refs.sh)',
         lambda c: [bash_cmd('bash tools/fetch_refs.sh')],
         lambda c: (c.repo / 'refs' / 'MacNativeGames' / '.git').exists()),
    Step('extract', 'Spieldateien entpacken', 'ExeFS und RomFS aus Basisspiel + Update, mit Hash-Prüfung (tools/extract.py)',
         lambda c: [[c.py314, 'tools/extract.py']],
         lambda c: (c.extracted / 'exefs' / 'main.npdm').is_file() and (c.extracted / 'romfs').is_dir()),
    Step('xlate', 'Shader-Übersetzer bauen', 'totk_xlate.exe mit .NET NativeAOT (tools/build_xlate.py)',
         lambda c: [[c.py312, 'tools/build_xlate.py']],
         lambda c: (c.repo / 'build' / 'xlate' / ('totk_xlate' + EXE)).is_file()),
    Step('gen', 'Spielcode übersetzen', 'ARM64 nach C (recomp/gen.py), etwa 15 Sekunden',
         lambda c: [[c.py312, 'recomp/gen.py', '--exefs', str(c.extracted / 'exefs'), '--out', f'generated/{c.ver}',
                     '--patches', f'recomp/patches-{c.ver}.txt']]),
    Step('romfs', 'RomFS-Patches', 'Overlay für die Bildraten-Korrektur (tools/make_romfs_patches.py)',
         lambda c: [[c.py314, 'tools/make_romfs_patches.py', '--version', c.ver]]),
    Step('build', 'Spiel bauen', 'Kompilieren und Linken von build\\<version>\\totk.exe, beim ersten Mal einige Minuten',
         lambda c: [[c.py312, 'tools/build_game.py', '--version', c.ver]]),
    Step('shaders', 'Shader vorbereiten', 'Übersetzt die Shader vorab, sonst ruckelt der erste Start (dauert)',
         lambda c: [[c.py314, 'tools/prepare_shaders.py', str(c.repo / 'build' / c.ver), '--version', c.ver]],
         optional=True),
    Step('probes', 'Selbsttests', 'totk.exe --selftest, --probe-coop und --probe-cfg',
         lambda c: [[str(c.exe), '--selftest'], [str(c.exe), '--probe-coop'], [str(c.exe), '--probe-cfg']],
         keep_going=True),
]


# ---------------------------------------------------------------- Werkzeuge (Reiter 1)

def run_quiet(cmd, timeout=20):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        return r.returncode, (r.stdout or '') + (r.stderr or '')
    except (OSError, subprocess.SubprocessError) as e:
        return -1, str(e)


def check_git():
    g = git_path()
    return (True, run_quiet([str(g), '--version'])[1].strip()) if g else (False, '')


def check_uv():
    u = uv_path()
    return (True, run_quiet([str(u), '--version'])[1].strip()) if u else (False, '')


def check_msys():
    return (True, str(MSYS)) if MSYS_BASH.is_file() else (False, f'{MSYS} fehlt')


def check_msys_pkgs():
    need = ['clang.exe', 'ld.lld.exe', 'ninja.exe', 'gcc.exe', 'SDL3.dll', 'libopus-0.dll']
    miss = [n for n in need if not (UCRT / n).is_file()]
    return (not miss, 'vollständig' if not miss else 'fehlt: ' + ', '.join(miss))


def check_dotnet():
    d = which('dotnet', PF / 'dotnet' / 'dotnet.exe')
    if not d:
        return False, ''
    sdks = [l.split()[0] for l in run_quiet([str(d), '--list-sdks'])[1].splitlines() if l.strip()]
    ok = any(v.startswith('10.') for v in sdks)
    return ok, ('SDK ' + ', '.join(sdks)) if sdks else 'kein SDK'


def check_vs():
    vswhere = VS_INSTALLER / 'vswhere.exe'
    if not vswhere.is_file():
        return False, ''
    out = run_quiet([str(vswhere), '-latest', '-products', '*', '-requires',
                     'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'])[1].strip()
    return (True, out) if out else (False, 'C++-Werkzeuge fehlen')


def check_vulkan():
    dll = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'vulkan-1.dll'
    return (True, 'Treiber vorhanden') if dll.is_file() else (False, 'vulkan-1.dll fehlt: Grafiktreiber aktualisieren')


def check_vulkan_sdk():
    sdk = os.environ.get('VULKAN_SDK')
    return (True, sdk) if sdk else ((True, r'C:\VulkanSDK') if Path(r'C:\VulkanSDK').is_dir() else (False, 'optional'))


def winget(pkg, *extra):
    return ['winget', 'install', '--id', pkg, '-e', '--accept-package-agreements', '--accept-source-agreements', *extra]


@dataclass
class Tool:
    name: str
    why: str
    check: Callable
    install: Optional[Callable[[], list]]
    optional: bool = False


TOOLS = [
    Tool('Git', 'Repo holen, Patch einspielen', check_git, lambda: [winget('Git.Git')]),
    Tool('uv', 'Python-Umgebungen', check_uv, lambda: [winget('astral-sh.uv')]),
    Tool('MSYS2', 'Compiler-Umgebung (C:\\msys64)', check_msys, lambda: [winget('MSYS2.MSYS2')]),
    Tool('MSYS2-Pakete', 'clang, lld, ninja, SDL3, Opus, libvpx, zstd, Vulkan', check_msys_pkgs,
         lambda: [[str(MSYS_BASH), '-lc', 'pacman -Syuu --noconfirm || true'],
                  [str(MSYS_BASH), '-lc', 'pacman -Syuu --noconfirm || true'],
                  [str(MSYS_BASH), '-lc', 'pacman -S --needed --noconfirm ' + MSYS_PACKAGES]]),
    Tool('.NET SDK 10', 'Shader-Übersetzer', check_dotnet, lambda: [winget('Microsoft.DotNet.SDK.10')]),
    Tool('VS Build Tools (C++)', 'Linker für .NET NativeAOT', check_vs,
         lambda: [winget('Microsoft.VisualStudio.BuildTools', '--override',
                         '--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait')]),
    Tool('Vulkan-Treiber', 'kommt mit dem Grafiktreiber', check_vulkan, None),
    Tool('Vulkan SDK', 'optional: Validierungsschichten', check_vulkan_sdk, lambda: [winget('KhronosGroup.VulkanSDK')],
         optional=True),
]


# ---------------------------------------------------------------- Ablauf (läuft in einem Thread, meldet über eine Queue)

class Runner:
    """Führt Befehlsfolgen nacheinander aus und meldet ('log', text, tag), ('step', key, state) und ('end', ok, text)
    über self.q. Abbrechen beendet den laufenden Prozess samt Kindern."""

    def __init__(self):
        self.q = queue.Queue()
        self.proc = None
        self.cancelled = False
        self.thread = None
        self.logfile = None

    @property
    def busy(self):
        return self.thread is not None and self.thread.is_alive()

    def log(self, text, tag=''):
        self.q.put(('log', text, tag))
        if self.logfile:
            try:
                self.logfile.write(text + '\n')
                self.logfile.flush()
            except OSError:
                pass

    def start(self, title, job):
        if self.busy:
            return False
        self.cancelled = False
        LOG_DIR.mkdir(exist_ok=True)
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        self.logfile = open(LOG_DIR / f'{stamp}.log', 'w', encoding='utf-8')
        self.thread = threading.Thread(target=self._main, args=(title, job), daemon=True)
        self.thread.start()
        return True

    def _main(self, title, job):
        self.log(f'==== {title} ({datetime.datetime.now():%H:%M:%S})', 'head')
        try:
            ok, text = job(self)
        except Exception as e:   # ein Fehler der Testbank selbst: anzeigen statt still sterben
            ok, text = False, f'Interner Fehler: {e!r}'
        if self.cancelled:
            ok, text = False, 'Abgebrochen.'
        self.log(f'==== {text}', 'ok' if ok else 'err')
        self.q.put(('end', ok, text))
        if self.logfile:
            self.logfile.close()
            self.logfile = None

    def run(self, cmd, cwd=None, dry=False):
        """Ein Befehl, Ausgabe zeilenweise ins Log. Rückgabe: Exit-Code (0 im Trockenlauf)."""
        shown = ' '.join(f'"{a}"' if ' ' in str(a) else str(a) for a in cmd)
        self.log(f'> {shown}', 'cmd')
        if dry:
            return 0
        if self.cancelled:
            return -1
        kw = {}
        if WIN:
            kw['creationflags'] = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
        else:
            kw['start_new_session'] = True
        try:
            self.proc = subprocess.Popen([str(a) for a in cmd], cwd=cwd, env=child_env(), stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **kw)
        except OSError as e:
            self.log(f'Kann nicht starten: {e}', 'err')
            return -1
        for raw in iter(self.proc.stdout.readline, b''):
            line = raw.decode('utf-8', errors='replace').rstrip('\r\n')
            low = line.lower()
            tag = 'err' if ('error' in low or 'fail' in low or 'traceback' in low or 'fehler' in low) else ''
            self.log(line, tag)
        code = self.proc.wait()
        self.proc = None
        if code:
            self.log(f'(Exit-Code {code})', 'err')
        return code

    def cancel(self):
        self.cancelled = True
        p = self.proc
        if not p:
            return
        try:
            if WIN:
                subprocess.run(['taskkill', '/T', '/F', '/PID', str(p.pid)], capture_output=True,
                               creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                os.killpg(p.pid, signal.SIGTERM)
        except OSError:
            pass


def build_job(ctx, selected, on_step, dry=False, force=()):
    """Der Ablauf für den Reiter „Bauen“: die gewählten Schritte der Reihe nach; ein fertiger Schritt wird übersprungen
    (außer er steht in force), ein Fehler hält an."""
    def job(r):
        for st in STEPS:
            if st.key not in selected:
                continue
            if r.cancelled:
                return False, 'Abgebrochen.'
            if st.done and st.key not in force and st.done(ctx):
                on_step(st.key, 'skip')
                r.log(f'-- {st.title}: schon erledigt, übersprungen', 'dim')
                continue
            on_step(st.key, 'run')
            r.log(f'-- {st.title}', 'head')
            t0 = time.monotonic()
            failed = 0
            for cmd in st.commands(ctx):
                if r.run(cmd, cwd=str(ctx.repo), dry=dry):
                    failed += 1
                    if not st.keep_going:
                        break
            if failed:
                on_step(st.key, 'fail')
                return False, f'„{st.title}“ ist fehlgeschlagen. Die Ausgabe oben zeigt, woran.'
            if st.after and not dry:
                st.after(ctx)
            on_step(st.key, 'ok')
            r.log(f'-- {st.title}: fertig in {time.monotonic() - t0:.0f} s', 'ok')
        return True, 'Trockenlauf beendet (nichts ausgeführt).' if dry else 'Alle gewählten Schritte sind fertig.'
    return job


# ---------------------------------------------------------------- Zustand

def load_state():
    s = dict(DEFAULTS)
    try:
        s.update({k: v for k, v in json.loads(STATE_FILE.read_text(encoding='utf-8')).items() if k in DEFAULTS})
    except (OSError, ValueError):
        pass
    return s


def save_state(s):
    try:
        STATE_FILE.write_text(json.dumps(s, indent=1, ensure_ascii=False), encoding='utf-8')
    except OSError:
        pass


# ---------------------------------------------------------------- Fenster

def gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    from tkinter.scrolledtext import ScrolledText

    if WIN:
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)   # scharfe Schrift auf skalierten Bildschirmen
        except (AttributeError, OSError):
            pass

    s = load_state()
    runner = Runner()
    root = tk.Tk()
    root.title(APP)
    root.geometry('1060x780')
    root.minsize(900, 640)
    style = ttk.Style(root)
    if 'vista' in style.theme_names():
        style.theme_use('vista')
    elif 'clam' in style.theme_names():
        style.theme_use('clam')
    style.configure('Head.TLabel', font=('Segoe UI', 15, 'bold') if WIN else ('DejaVu Sans', 15, 'bold'))
    style.configure('Sub.TLabel', foreground='#555')
    style.configure('Ok.TLabel', foreground='#1b7f3a')
    style.configure('Bad.TLabel', foreground='#b3261e')
    style.configure('Warn.TLabel', foreground='#9a6700')

    vars_ = {}

    def var(key, kind=tk.StringVar):
        v = kind(value=s[key])
        vars_[key] = v

        def changed(*_):
            try:
                s[key] = v.get()
            except tk.TclError:
                return
            save_state(s)
        v.trace_add('write', changed)
        return v

    head = ttk.Frame(root, padding=(14, 10, 14, 4))
    head.pack(fill='x')
    ttk.Label(head, text=APP, style='Head.TLabel').pack(side='left')
    status = ttk.Label(head, text='', style='Sub.TLabel')
    status.pack(side='left', padx=16)
    if not WIN:
        ttk.Label(head, text='Vorschau: die Schritte laufen nur unter Windows', style='Warn.TLabel').pack(side='right')

    paned = ttk.PanedWindow(root, orient='vertical')
    paned.pack(fill='both', expand=True, padx=10, pady=(0, 10))
    nb = ttk.Notebook(paned)
    logbox = ttk.Frame(paned)
    paned.add(nb, weight=3)
    paned.add(logbox, weight=2)

    log = ScrolledText(logbox, height=10, wrap='none', font=('Consolas', 9) if WIN else ('DejaVu Sans Mono', 9),
                       background='#1e1e1e', foreground='#d4d4d4', insertbackground='#d4d4d4')
    log.pack(fill='both', expand=True)
    for tag, color in (('err', '#ff7b72'), ('ok', '#7ee787'), ('cmd', '#79c0ff'), ('head', '#e3b341'), ('dim', '#8b949e')):
        log.tag_configure(tag, foreground=color)
    logbar = ttk.Frame(logbox)
    logbar.pack(fill='x', pady=(4, 0))
    busy_label = ttk.Label(logbar, text='Bereit.', style='Sub.TLabel')
    busy_label.pack(side='left')
    cancel_btn = ttk.Button(logbar, text='Abbrechen', state='disabled', command=lambda: runner.cancel())
    cancel_btn.pack(side='right')
    ttk.Button(logbar, text='Log-Ordner öffnen', command=lambda: open_path(LOG_DIR)).pack(side='right', padx=6)
    ttk.Button(logbar, text='Log leeren', command=lambda: log.delete('1.0', 'end')).pack(side='right')

    def open_path(p):
        p = Path(p)
        if not p.exists():
            messagebox.showinfo(APP, f'{p} gibt es noch nicht.')
            return
        if WIN:
            os.startfile(str(p))   # noqa: S606 (Explorer bzw. Standardprogramm)
        else:
            subprocess.Popen(['xdg-open', str(p)])

    def ctx():
        return Ctx(Path(s['repo_dir']), s)

    end_hooks = []

    def pump():
        try:
            while True:
                msg = runner.q.get_nowait()
                if msg[0] == 'log':
                    log.insert('end', msg[1] + '\n', msg[2])
                    log.see('end')
                elif msg[0] == 'step':
                    set_step(msg[1], msg[2])
                elif msg[0] == 'end':
                    busy_label.config(text=msg[2], style='Ok.TLabel' if msg[1] else 'Bad.TLabel')
                    cancel_btn.config(state='disabled')
                    for h in end_hooks:
                        h()
        except queue.Empty:
            pass
        root.after(60, pump)

    def start(title, job):
        if runner.busy:
            messagebox.showinfo(APP, 'Es läuft schon etwas. Erst abwarten oder abbrechen.')
            return False
        busy_label.config(text=f'{title} läuft …', style='Sub.TLabel')
        cancel_btn.config(state='normal')
        return runner.start(title, job)

    def section(parent, title, text=None):
        f = ttk.LabelFrame(parent, text=title, padding=10)
        f.pack(fill='x', padx=12, pady=(10, 0))
        if text:
            ttk.Label(f, text=text, style='Sub.TLabel', wraplength=900, justify='left').grid(
                row=0, column=0, columnspan=4, sticky='w', pady=(0, 6))
        return f

    # ---- Reiter 1: Werkzeuge
    t1 = ttk.Frame(nb)
    nb.add(t1, text='1 · Werkzeuge')
    f = section(t1, 'Was der Build braucht',
                'Fehlendes installiert „Installieren“ über winget (Windows-Paketmanager). Manche Installer fragen nach '
                'Administratorrechten. Danach „Neu prüfen“.')
    tool_rows = []
    for i, tool in enumerate(TOOLS, start=1):
        ttk.Label(f, text=tool.name + (' (optional)' if tool.optional else ''), width=24).grid(row=i, column=0, sticky='w', pady=2)
        st = ttk.Label(f, text='…', width=46)
        st.grid(row=i, column=1, sticky='w')
        ttk.Label(f, text=tool.why, style='Sub.TLabel').grid(row=i, column=2, sticky='w', padx=8)
        btn = ttk.Button(f, text='Installieren', state='disabled')
        btn.grid(row=i, column=3, sticky='e', padx=(8, 0))
        tool_rows.append((tool, st, btn))

    def install(tools):
        def job(r):
            for tool in tools:
                r.log(f'-- {tool.name}', 'head')
                for cmd in tool.install():
                    if r.run(cmd):
                        return False, f'{tool.name}: Installation fehlgeschlagen (Ausgabe oben).'
            return True, 'Installiert. „Neu prüfen“ zeigt den neuen Stand (manches erst nach einem Neustart der Testbank).'
        start('Installation', job)

    def recheck():
        missing = []
        for tool, st, btn in tool_rows:
            if not WIN and tool.name not in ('Git', 'uv'):
                st.config(text='– (nur unter Windows geprüft)', style='Sub.TLabel')
                btn.config(state='disabled')
                continue
            ok, info = tool.check()
            word = 'da' if ok else 'fehlt (optional)' if tool.optional else 'fehlt'
            st.config(text=word + (': ' + info[:56] if info else ''),
                      style='Ok.TLabel' if ok else ('Sub.TLabel' if tool.optional else 'Bad.TLabel'))
            can = (not ok) and tool.install is not None and WIN
            btn.config(state='normal' if can else 'disabled', command=lambda t=tool: install([t]))
            if not ok and not tool.optional and tool.install:
                missing.append(tool)
        all_btn.config(state='normal' if missing and WIN else 'disabled', command=lambda: install(missing))
        status.config(text='Werkzeuge vollständig' if not missing else f'{len(missing)} Werkzeug(e) fehlen')

    bar = ttk.Frame(t1, padding=(12, 10))
    bar.pack(fill='x')
    all_btn = ttk.Button(bar, text='Alles Fehlende installieren', state='disabled')
    all_btn.pack(side='left')
    ttk.Button(bar, text='Neu prüfen', command=recheck).pack(side='left', padx=8)
    end_hooks.append(recheck)

    # ---- Reiter 2: Repo
    t2 = ttk.Frame(nb)
    nb.add(t2, text='2 · Repo')
    f = section(t2, 'Repository', 'Der Ordner braucht rund 60 GB freien Platz (entpacktes Spiel, generierter Code, Build).')
    v_repo, v_url, v_patch = var('repo_dir'), var('repo_url'), var('patch')

    def path_row(parent, row, label, v, kind, types=None):
        ttk.Label(parent, text=label, width=18).grid(row=row, column=0, sticky='w', pady=3)
        e = ttk.Entry(parent, textvariable=v, width=80)
        e.grid(row=row, column=1, sticky='we')
        info = ttk.Label(parent, text='', width=22)
        info.grid(row=row, column=3, sticky='w', padx=6)

        def browse():
            cur = v.get()
            start_dir = str(Path(cur).parent) if cur else str(Path.home())
            if kind == 'dir':
                p = filedialog.askdirectory(initialdir=cur or start_dir, title=label)
            else:
                p = filedialog.askopenfilename(initialdir=start_dir, title=label, filetypes=types or [('Alle Dateien', '*.*')])
            if p:
                v.set(str(Path(p)))
        if kind:
            ttk.Button(parent, text='Wählen …', command=browse).grid(row=row, column=2, padx=6)
        parent.columnconfigure(1, weight=1)
        return info

    i_repo = path_row(f, 1, 'Ordner', v_repo, 'dir')
    path_row(f, 2, 'Adresse', v_url, None)
    i_patch = path_row(f, 3, 'Koop-Patch', v_patch, 'file', [('Patch', '*.patch'), ('Alle Dateien', '*.*')])
    repo_state = ttk.Label(f, text='', wraplength=900, justify='left')
    repo_state.grid(row=4, column=0, columnspan=4, sticky='w', pady=(8, 0))

    def refresh_repo():
        repo = Path(s['repo_dir'])
        g = git_path()
        gfree = free_gb(repo)
        i_repo.config(text=f'{gfree:.0f} GB frei' if gfree is not None else '',
                      style='Ok.TLabel' if gfree and gfree >= 60 else 'Warn.TLabel')
        ok, txt = file_info(s['patch'])
        i_patch.config(text=txt, style='Ok.TLabel' if ok else 'Bad.TLabel')
        if not (repo / '.git').exists():
            repo_state.config(text='Noch nicht geklont.', style='Sub.TLabel')
            return
        head_ = run_quiet([str(g), '-C', str(repo), 'log', '-1', '--format=%h %s'])[1].strip() if g else '?'
        coop = (repo / 'runtime' / 'coop.cpp').is_file()
        repo_state.config(text=f'Geklont. Stand: {head_}\nKoop-Patch: {"angewendet" if coop else "noch nicht angewendet"}',
                          style='Ok.TLabel' if coop else 'Warn.TLabel')

    def clone():
        repo = Path(s['repo_dir'])
        g = str(git_path() or 'git')
        if (repo / '.git').exists():
            messagebox.showinfo(APP, 'Das Repo ist schon da.')
            return
        if repo.exists() and any(repo.iterdir()):
            messagebox.showerror(APP, f'{repo} ist nicht leer. Bitte einen leeren oder neuen Ordner wählen.')
            return

        def job(r):
            return (r.run([g, 'clone', s['repo_url'], str(repo)]) == 0,
                    'Geklont.' if (repo / '.git').exists() else 'Klonen fehlgeschlagen.')
        start('Repo klonen', job)

    def apply_patch():
        repo = Path(s['repo_dir'])
        g = str(git_path() or 'git')
        if not (repo / '.git').exists():
            messagebox.showerror(APP, 'Erst das Repo klonen.')
            return
        if (repo / 'runtime' / 'coop.cpp').is_file():
            messagebox.showinfo(APP, 'Der Koop-Patch ist schon angewendet.')
            return
        if not Path(s['patch']).is_file():
            messagebox.showerror(APP, 'Die Patch-Datei fehlt.')
            return

        def job(r):
            ident = ['-c', 'user.name=TotK Testbank', '-c', 'user.email=testbank@localhost']
            if r.run([g, '-C', str(repo), *ident, 'am', '-3', s['patch']]) == 0:
                return True, 'Patch angewendet.'
            r.run([g, '-C', str(repo), 'am', '--abort'])
            return False, 'Der Patch ließ sich nicht anwenden; das Repo ist unverändert. Die Ausgabe oben zeigt die Datei.'
        start('Patch anwenden', job)

    bar = ttk.Frame(t2, padding=(12, 10))
    bar.pack(fill='x')
    ttk.Button(bar, text='Klonen', command=clone).pack(side='left')
    ttk.Button(bar, text='Koop-Patch anwenden', command=apply_patch).pack(side='left', padx=8)
    ttk.Button(bar, text='Ordner öffnen', command=lambda: open_path(s['repo_dir'])).pack(side='left')
    end_hooks.append(refresh_repo)
    for v in (v_repo, v_patch):
        v.trace_add('write', lambda *_: refresh_repo())

    # ---- Reiter 3: Spieldateien
    t3 = ttk.Frame(nb)
    nb.add(t3, text='3 · Spieldateien')
    f = section(t3, 'Deine eigenen Dateien',
                'Basisspiel 1.0.0 und das Update als NSP oder XCI, dazu prod.keys von deiner Konsole. Die Testbank '
                'speichert nur die Pfade (in config.local.toml des Repos, von git ignoriert) und liest den Inhalt der '
                'Schlüsseldatei nie; das tun die Werkzeuge des Repos selbst.')
    game_types = [('Switch-Spiel', '*.nsp *.xci'), ('Alle Dateien', '*.*')]
    infos = {
        'base': path_row(f, 1, 'Basisspiel', var('base'), 'file', game_types),
        'update': path_row(f, 2, 'Update', var('update'), 'file', [('Update', '*.nsp'), ('Alle Dateien', '*.*')]),
        'keys': path_row(f, 3, 'prod.keys', var('keys'), 'file', [('Schlüssel', '*.keys'), ('Alle Dateien', '*.*')]),
        'firmware': path_row(f, 4, 'Firmware (optional)', var('firmware'), 'dir'),
    }
    ttk.Label(f, text='Firmware nur für chinesische oder koreanische Texte nötig.', style='Sub.TLabel').grid(
        row=5, column=1, sticky='w')
    vf = ttk.Frame(f)
    vf.grid(row=6, column=0, columnspan=4, sticky='w', pady=(10, 0))
    ttk.Label(vf, text='Spielversion', width=18).pack(side='left')
    v_ver = var('version')
    for code, label in VERSIONS:
        ttk.Radiobutton(vf, text=label, value=code, variable=v_ver).pack(side='left', padx=(0, 14))
    ttk.Label(vf, text='muss zum Update passen', style='Sub.TLabel').pack(side='left')
    cfg_state = ttk.Label(t3, text='', padding=(12, 8))
    cfg_state.pack(fill='x')

    def refresh_files(*_):
        for key, lab in infos.items():
            ok, txt = file_info(s[key], folder=key == 'firmware')
            lab.config(text=txt, style='Ok.TLabel' if ok else 'Bad.TLabel' if ok is False else 'Sub.TLabel')
        cfg = Path(s['repo_dir']) / 'config.local.toml'
        cfg_state.config(text=f'config.local.toml: {"vorhanden" if cfg.is_file() else "noch nicht gespeichert"}',
                         style='Ok.TLabel' if cfg.is_file() else 'Sub.TLabel')

    def save_config():
        miss = [n for k, n in (('base', 'Basisspiel'), ('update', 'Update'), ('keys', 'prod.keys')) if not file_info(s[k])[0]]
        if miss:
            messagebox.showerror(APP, 'Diese Dateien fehlen oder stimmen nicht: ' + ', '.join(miss))
            return False
        repo = Path(s['repo_dir'])
        if not (repo / '.git').exists():
            messagebox.showerror(APP, 'Erst das Repo klonen (Reiter 2).')
            return False
        cfg = repo / 'config.local.toml'
        text = config_toml(s)
        if cfg.is_file() and cfg.read_text(encoding='utf-8') != text:
            shutil.copyfile(cfg, cfg.with_suffix('.toml.bak'))
        cfg.write_text(text, encoding='utf-8')
        refresh_files()
        return True

    bar = ttk.Frame(t3, padding=(12, 2))
    bar.pack(fill='x')
    ttk.Button(bar, text='In config.local.toml speichern', command=save_config).pack(side='left')
    for k in infos:
        vars_[k].trace_add('write', refresh_files)

    # ---- Reiter 4: Bauen
    t4 = ttk.Frame(nb)
    nb.add(t4, text='4 · Bauen')
    f = section(t4, 'Schritte', 'Fertige Schritte werden übersprungen. Beim ersten Mal dauert alles zusammen je nach PC '
                               'etwa 20 bis 60 Minuten; danach nur noch die geänderten Teile.')
    step_vars, step_marks = {}, {}
    marks = {'wait': ('', 'Sub.TLabel'), 'run': ('läuft …', 'Warn.TLabel'), 'ok': ('fertig', 'Ok.TLabel'),
             'skip': ('war schon da', 'Sub.TLabel'), 'fail': ('Fehler', 'Bad.TLabel')}
    for i, st in enumerate(STEPS, start=1):
        on = tk.BooleanVar(value=(s['shaders'] if st.key == 'shaders' else True))
        step_vars[st.key] = on
        ttk.Checkbutton(f, variable=on).grid(row=i, column=0, sticky='w')
        m = ttk.Label(f, text='', width=12, style='Sub.TLabel')
        m.grid(row=i, column=1)
        step_marks[st.key] = m
        ttk.Label(f, text=f'{i}. {st.title}', width=26).grid(row=i, column=2, sticky='w', pady=1)
        ttk.Label(f, text=st.detail, style='Sub.TLabel').grid(row=i, column=3, sticky='w')

    def shaders_changed(*_):
        s['shaders'] = step_vars['shaders'].get()
        save_state(s)
    step_vars['shaders'].trace_add('write', shaders_changed)

    def set_step(key, state):
        text, st = marks[state]
        step_marks[key].config(text=text, style=st)

    dry = tk.BooleanVar(value=not WIN)
    force_var = tk.BooleanVar(value=False)

    def run_build(only=None):
        repo = Path(s['repo_dir'])
        if not (repo / '.git').exists():
            messagebox.showerror(APP, 'Erst das Repo klonen (Reiter 2).')
            return
        if not dry.get() and not (repo / 'config.local.toml').is_file():
            if not save_config():
                return
        selected = {only} if only else {k for k, v in step_vars.items() if v.get()}
        for k in step_marks:
            set_step(k, 'wait')
        force = selected if force_var.get() else ()

        def on_step(k, state):
            runner.q.put(('step', k, state))
        start('Bauen' if not dry.get() else 'Trockenlauf', build_job(ctx(), selected, on_step, dry=dry.get(), force=force))

    bar = ttk.Frame(t4, padding=(12, 10))
    bar.pack(fill='x')
    ttk.Button(bar, text='Alles bauen', command=run_build).pack(side='left')
    ttk.Button(bar, text='Nur Selbsttests', command=lambda: run_build('probes')).pack(side='left', padx=8)
    ttk.Checkbutton(bar, text='Fertige Schritte trotzdem wiederholen', variable=force_var).pack(side='left', padx=8)
    ttk.Checkbutton(bar, text='Trockenlauf (Befehle nur anzeigen)', variable=dry).pack(side='left', padx=8)
    ttk.Button(bar, text='Build-Logs öffnen', command=lambda: open_path(Path(s['repo_dir']) / 'build' / 'logs')).pack(
        side='right')

    # ---- Reiter 5: Koop & Spielen
    t5 = ttk.Frame(nb)
    nb.add(t5, text='5 · Koop & Spielen')
    f = section(t5, 'Koop-Test', 'Schreibt die Koop-Einstellungen in die totk.ini neben totk.exe. Andere Einstellungen dort '
                                 'bleiben, wie sie sind.')
    v_coop = var('coop', tk.BooleanVar)
    ttk.Checkbutton(f, text='Koop einschalten (coop = 1)', variable=v_coop).grid(row=1, column=0, columnspan=3, sticky='w')
    v_mode = var('peer_mode')
    ttk.Radiobutton(f, text='Allein testen: der eigene Marker kommt zurück (coop_peer = loop)', value='loop',
                    variable=v_mode).grid(row=2, column=0, columnspan=3, sticky='w', pady=(6, 0))
    ttk.Radiobutton(f, text='Mit Partner im selben Netz, dessen IP:', value='ip', variable=v_mode).grid(
        row=3, column=0, sticky='w')
    ip_entry = ttk.Entry(f, textvariable=var('peer_ip'), width=18)
    ip_entry.grid(row=3, column=1, sticky='w')
    ips = local_ips()
    ttk.Label(f, text='Deine IP für den Partner: ' + (', '.join(ips) if ips else 'nicht ermittelbar'),
              style='Sub.TLabel').grid(row=3, column=2, sticky='w', padx=10)
    pf = ttk.Frame(f)
    pf.grid(row=4, column=0, columnspan=3, sticky='w', pady=(6, 0))
    ttk.Label(pf, text='Port (beide gleich):').pack(side='left')
    ttk.Entry(pf, textvariable=var('port'), width=8).pack(side='left', padx=6)
    mf = ttk.Frame(f)
    mf.grid(row=5, column=0, columnspan=3, sticky='w', pady=(6, 0))
    ttk.Label(mf, text='Anzeige:').pack(side='left')
    v_marker = var('marker')
    for val, txt in (('2', 'Testmodus (Kreuz, Status, Log)'), ('1', 'nur Partner-Marker'), ('0', 'nichts')):
        ttk.Radiobutton(mf, text=txt, value=val, variable=v_marker).pack(side='left', padx=(6, 8))
    game_state = ttk.Label(t5, text='', padding=(12, 8))
    game_state.pack(fill='x')

    def refresh_game(*_):
        exe = ctx().exe
        if exe.is_file():
            game_state.config(text=f'Spiel gebaut: {exe}', style='Ok.TLabel')
        else:
            game_state.config(text='Noch kein totk.exe: erst bauen (Reiter 4).', style='Sub.TLabel')
        ip_entry.config(state='normal' if s['peer_mode'] == 'ip' else 'disabled')

    def save_ini():
        exe = ctx().exe
        if not exe.parent.is_dir():
            messagebox.showerror(APP, 'Den Build-Ordner gibt es noch nicht: erst bauen (Reiter 4).')
            return False
        values, err = coop_values(s)
        if err:
            messagebox.showerror(APP, err)
            return False
        # Koop spielt in einem eigenen Profil (build/profile-koop). Beim ersten Koop-Start wird es aus dem Ordner
        # koop-start neben der Testbank gefüllt (account.uid + save/<uid>/slot_XX: der feste Startpunkt); danach bleibt
        # es, wie es ist, und das Spiel macht beim letzten Speichern weiter. Ohne Koop gilt wieder das normale Profil.
        build = exe.parent.parent
        if s['coop']:
            profile = build / 'profile-koop'
            seed = HERE / 'koop-start'
            if not (profile / 'save').is_dir() and (seed / 'account.uid').is_file():
                shutil.copytree(seed, profile, dirs_exist_ok=True)
                log.insert('end', f'Koop-Startstand eingerichtet: {profile}\n', 'ok')
        else:
            profile = build / ('profile' if s['version'] == 'v121' else f'profile-{s["version"]}')
        values['profile_dir'] = str(profile).replace('\\', '/')
        ini = exe.parent / 'totk.ini'
        old = ini.read_text(encoding='utf-8') if ini.is_file() else ''
        ini.write_text(ini_set(old, values), encoding='utf-8')
        shown = ', '.join(f'{k} = {v}' for k, v in values.items())
        log.insert('end', f'totk.ini gespeichert ({ini}): {shown}\n', 'ok')
        return True

    def firewall():
        if not valid_port(s['port']):
            messagebox.showerror(APP, 'Erst einen gültigen Port eintragen.')
            return
        rule = (f'advfirewall firewall add rule name="TotK Koop UDP {s["port"]}" dir=in action=allow protocol=UDP '
                f'localport={s["port"]}')
        if not WIN:
            messagebox.showinfo(APP, 'Nur unter Windows: netsh ' + rule)
            return
        ps = f"Start-Process netsh -Verb RunAs -ArgumentList '{rule}'"
        subprocess.Popen(['powershell', '-NoProfile', '-Command', ps], creationflags=subprocess.CREATE_NO_WINDOW)

    def play():
        exe = ctx().exe
        if not exe.is_file():
            messagebox.showerror(APP, 'totk.exe fehlt: erst bauen (Reiter 4).')
            return
        if not save_ini():
            return
        subprocess.Popen([str(exe)], cwd=str(exe.parent))
        log.insert('end', f'Gestartet: {exe}\n', 'ok')

    bar = ttk.Frame(t5, padding=(12, 2))
    bar.pack(fill='x')
    ttk.Button(bar, text='Spiel starten', command=play).pack(side='left')
    ttk.Button(bar, text='Nur totk.ini speichern', command=save_ini).pack(side='left', padx=8)
    ttk.Button(bar, text='Firewall freigeben (Admin)', command=firewall).pack(side='left')
    ttk.Button(bar, text='Spiel-Log öffnen', command=lambda: open_path(ctx().exe.parent / 'logs' / 'totk.log')).pack(
        side='left', padx=8)
    ttk.Button(bar, text='Build-Ordner öffnen', command=lambda: open_path(ctx().exe.parent)).pack(side='left')

    f = section(t5, 'Worauf du beim Test achtest')
    ttk.Label(f, justify='left', wraplength=950, text=(
        '1. Reiter 4: „Selbsttests“ müssen grün sein, besonders „coop: probe passed“.\n'
        '2. Allein (loop) mit Testmodus: oben links steht „link: peer heard“, Pakete gesendet und empfangen steigen.\n'
        '3. Ein gelbes Kreuz liegt in der Bildmitte, der orange Marker „P2 (test) 10 m“ genau darauf.\n'
        '4. Im Spiel-Log steht etwa alle 10 Sekunden „coop: test … ndc (x, y), expected (0, 0)“ mit Werten nahe 0.\n'
        '5. Liegt das Kreuz woanders oder steht „camera: none“, bitte die Zeilen „coop: test“ und '
        '„coop: scene camera exported“ aus dem Spiel-Log schicken.\n'
        '6. Mit Partner: beide tragen die IP des anderen ein, gleicher Port, Firewall freigeben. Jeder sieht den Punkt '
        'vor der Kamera des anderen.')).grid(row=1, column=0, sticky='w')
    for k in ('peer_mode', 'repo_dir', 'version'):
        vars_[k].trace_add('write', refresh_game)
    end_hooks.append(refresh_game)

    recheck()
    refresh_repo()
    refresh_files()
    refresh_game()
    pump()
    if len(sys.argv) > 2 and sys.argv[1] == '--tab':   # für Bildschirmfotos
        nb.select(int(sys.argv[2]))
    root.mainloop()


# ---------------------------------------------------------------- Selbsttest

def selftest():
    import tempfile
    import tomllib
    fails = []

    def check(c, what):
        if not c:
            fails.append(what)
        print(('ok    ' if c else 'FEHLER') + ' ' + what)

    s = dict(DEFAULTS, base=r'C:\Spiele\TotK\base.xci', update=r"C:\Spiele\O'Neil\update 1.4.3.nsp",
             keys=r'C:\Switch\prod.keys', firmware='')
    data = tomllib.loads(config_toml(s))
    check(data['input']['base'] == s['base'] and data['input']['updates'] == [s['update']] and
          data['input']['keys'] == s['keys'] and 'firmware' not in data['input'] and data['work']['dir'] == 'build',
          'config.local.toml: Pfade mit Leerzeichen und Apostroph bleiben erhalten')
    data = tomllib.loads(config_toml(dict(s, firmware=r'D:\fw\registered')))
    check(data['input']['firmware'] == r'D:\fw\registered', 'config.local.toml: Firmware wird eingetragen')
    check(tomllib.loads(config_toml(dict(s, update='')))['input']['updates'] == [], 'config.local.toml: ohne Update leere Liste')

    for ip, ok in (('192.168.1.20', True), ('10.0.0.2', True), ('256.1.1.1', False), ('1.2.3', False),
                   ('0.0.0.0', False), ('01.2.3.4', False), ('', False), ('a.b.c.d', False)):
        check(valid_ip(ip) == ok, f'IP-Prüfung {ip!r} -> {ok}')
    for p, ok in (('27460', True), ('1024', True), ('65535', True), ('80', False), ('70000', False), ('x', False)):
        check(valid_port(p) == ok, f'Port-Prüfung {p!r} -> {ok}')
    v, e = coop_values(dict(DEFAULTS))
    check(v == {'coop': '1', 'coop_port': '27460', 'coop_peer': 'loop', 'coop_marker': '2', 'coop_puppet': '2'} and not e,
          'Koop-Werte: loop')
    v, e = coop_values(dict(DEFAULTS, peer_mode='ip', peer_ip='192.168.0.7', marker='1', coop=False))
    check(v == {'coop': '0', 'coop_port': '27460', 'coop_peer': '192.168.0.7', 'coop_marker': '1', 'coop_puppet': '0'},
          'Koop-Werte: Partner-IP, Koop aus')
    v, e = coop_values(dict(DEFAULTS, peer_mode='ip', peer_ip='192.168.0.7'))
    check(v['coop_puppet'] == '1', 'Koop-Werte: Partner-IP, zweiter Link an')
    check(coop_values(dict(DEFAULTS, peer_mode='ip', peer_ip='x'))[1] is not None, 'Koop-Werte: falsche IP wird abgelehnt')

    vals = {'coop': '1', 'coop_port': '27460', 'coop_peer': 'loop', 'coop_marker': '2'}
    out = ini_set('', vals)
    check(out == '[coop]\ncoop = 1\ncoop_port = 27460\ncoop_peer = loop\ncoop_marker = 2\n', 'totk.ini: neu angelegt')
    old = '[display]\nfullscreen = 1\n# coop = 0\nCOOP_PORT=1111\n[coop]\ncoop_marker = 0\n'
    out = ini_set(old, vals)
    check('fullscreen = 1' in out and '# coop = 0' in out, 'totk.ini: andere Zeilen und Kommentare bleiben')
    check('COOP_PORT=27460' in out and 'coop_marker = 2' in out and out.count('[coop]') == 1, 'totk.ini: vorhandene Schlüssel ersetzt')
    check(re.search(r'^coop = 1$', out, re.M) and re.search(r'^coop_peer = loop$', out, re.M), 'totk.ini: fehlende ergänzt')
    check(ini_set(out, vals) == out, 'totk.ini: zweites Speichern ändert nichts')
    dup = ini_set('coop_marker = 1\ncoop_marker = 0\n', {'coop_marker': '2'})
    check(dup.count('coop_marker = 2') == 2, 'totk.ini: doppelte Schlüssel alle ersetzt (cfg.c nimmt den letzten)')

    with tempfile.TemporaryDirectory() as tmp:
        repo = Path(tmp) / 'repo mit Leerzeichen'
        (repo / 'tools').mkdir(parents=True)
        (repo / 'tools' / 'requirements.txt').write_text('numpy\n')
        (repo / 'tools' / 'requirements-py314.txt').write_text('lz4\n')
        c = Ctx(repo, dict(DEFAULTS, version='v143'))
        cmds = {st.key: st.commands(c) for st in STEPS}
        check(cmds['gen'][0][1:] == ['recomp/gen.py', '--exefs', str(repo / 'build' / 'extracted' / 'v143' / 'exefs'),
                                     '--out', 'generated/v143', '--patches', 'recomp/patches-v143.txt'],
              'Befehl gen.py wie in docs/building.md')
        check(cmds['build'][0][1:] == ['tools/build_game.py', '--version', 'v143'], 'Befehl build_game.py')
        check(cmds['shaders'][0][1:] == ['tools/prepare_shaders.py', str(repo / 'build' / 'v143'), '--version', 'v143'],
              'Befehl prepare_shaders.py')
        check([x[1] for x in cmds['probes']] == ['--selftest', '--probe-coop', '--probe-cfg'], 'Selbsttests')
        check(len(cmds['venv']) == 4 and cmds['venv'][0][1:] == ['venv', '--python', '3.12', '.venv'], 'venv: anlegen und füllen')
        check(not venv_done(c), 'venv: fehlt -> nicht fertig')
        for name in ('.venv', '.venv-py314'):
            c.venv_py(name).parent.mkdir(parents=True)
            c.venv_py(name).write_text('')
        check(len(venv_cmds(c)) == 2 and not venv_done(c), 'venv: vorhanden, Pakete noch nicht bestätigt')
        venv_after(c)
        check(venv_done(c), 'venv: nach dem Lauf fertig')
        (repo / 'tools' / 'requirements.txt').write_text('numpy\nlz4\n')
        check(not venv_done(c), 'venv: geänderte requirements -> wieder fällig')

        # der Ablauf mit echten Prozessen: Ausgabe, Überspringen, Abbruch bei Fehler, keep_going
        py = sys.executable
        fake = [Step('a', 'A', '', lambda c: [[py, '-c', 'print("hallo a")']]),
                Step('b', 'B', '', lambda c: [[py, '-c', 'x']], done=lambda c: True),
                Step('c', 'C', '', lambda c: [[py, '-c', 'import sys; print("Fehler c"); sys.exit(3)'],
                                              [py, '-c', 'print("c2")']], keep_going=True),
                Step('d', 'D', '', lambda c: [[py, '-c', 'print("nie")']])]
        saved = STEPS[:]
        STEPS[:] = fake
        try:
            states = []
            r = Runner()
            r.start('Test', build_job(c, {'a', 'b', 'c', 'd'}, lambda k, st: states.append((k, st))))
            r.thread.join(30)
            msgs = []
            while not r.q.empty():
                msgs.append(r.q.get())
            text = '\n'.join(m[1] for m in msgs if m[0] == 'log')
            end = [m for m in msgs if m[0] == 'end']
            check('hallo a' in text and 'c2' in text and 'nie' not in text, 'Ablauf: Ausgabe kommt an, nach Fehler Halt')
            check(states == [('a', 'run'), ('a', 'ok'), ('b', 'skip'), ('c', 'run'), ('c', 'fail')], f'Ablauf: Zustände {states}')
            check(end and end[0][1] is False and '„C“' in end[0][2], 'Ablauf: Fehler wird gemeldet')
            # Abbruch eines langen Prozesses
            STEPS[:] = [Step('z', 'Z', '', lambda c: [[py, '-c', 'import time; print("start", flush=True); time.sleep(60)']])]
            r = Runner()
            t0 = time.monotonic()
            r.start('Abbruch', build_job(c, {'z'}, lambda k, st: None))
            while r.proc is None and time.monotonic() - t0 < 10:
                time.sleep(0.05)
            time.sleep(0.3)
            r.cancel()
            r.thread.join(15)
            check(not r.busy and time.monotonic() - t0 < 15, 'Ablauf: Abbrechen beendet den Prozess')
            msgs = []
            while not r.q.empty():
                msgs.append(r.q.get())
            check(any(m[0] == 'end' and m[2] == 'Abgebrochen.' for m in msgs), 'Ablauf: Abbruch wird gemeldet')
        finally:
            STEPS[:] = saved
    print(f'\nSelbsttest: {len(fails)} Fehler' if fails else '\nSelbsttest bestanden')
    return 1 if fails else 0


# ---------------------------------------------------------------- Einfache Installation (--install)

BASE_COMMIT = '7a37fc3'   # der Stand, auf dem totk-coop-stage2.patch aufsetzt
UPDATE_FILE = HERE / 'koop-update.json'   # {"url": "https://github.com/…", "branch": "koop"}; legt make_installer.py bei
KOOP_BRANCH = 'koop'
BUILT_STAMP = 'koop-built.txt'            # neben totk.exe: der Commit, aus dem die exe gebaut wurde
UPDATE_STEPS = [st.key for st in STEPS if st.key not in ('shaders', 'probes')]   # erledigte überspringt build_job


def update_source():
    """(url, branch) der Koop-Updates oder None, wenn diese Installation keine Update-Quelle kennt."""
    try:
        d = json.loads(UPDATE_FILE.read_text(encoding='utf-8'))
        return (d['url'], d.get('branch') or KOOP_BRANCH) if d.get('url') else None
    except (OSError, ValueError, KeyError, TypeError):
        return None


def git_out(repo, *args, timeout=60):
    rc, out = run_quiet([str(git_path() or 'git'), '-C', str(repo), *args], timeout=timeout)
    return out.strip() if rc == 0 else None


def built_commit(exe):
    p = Path(exe).parent / BUILT_STAMP
    return p.read_text(encoding='utf-8').strip() if p.is_file() else None


def mark_built(exe, repo):
    head = git_out(repo, 'rev-parse', 'HEAD')
    if head:
        (Path(exe).parent / BUILT_STAMP).write_text(head + '\n', encoding='utf-8')


def exe_in_use(exe):
    """Windows sperrt eine laufende exe gegen Umbenennen; dann darf nicht gebaut werden (das Linken schlüge fehl)."""
    exe = Path(exe)
    if not exe.is_file():
        return False
    probe = exe.with_suffix('.exe.koop-probe')
    try:
        exe.rename(probe)
        probe.rename(exe)
        return False
    except OSError:
        return True


def self_update(repo):
    """Die Testbank selbst liegt im Koop-Zweig unter koop/; neuere Fassungen ersetzen die eigene Kopie. Python hat
    das laufende Skript schon gelesen, das Ersetzen wirkt ab dem nächsten Start."""
    for name in ('testbank.py', 'LIESMICH.md'):
        src = Path(repo) / 'koop' / name
        if src.is_file() and (not (HERE / name).is_file() or src.read_bytes() != (HERE / name).read_bytes()):
            shutil.copyfile(src, HERE / name)


def shortcut_ps(exe, updates):
    """PowerShell, das „TotK Koop“ auf den Desktop legt. Mit Update-Quelle startet die Verknüpfung testbank.py --play
    über uvw (uv ohne Konsolenfenster): erst Updates holen und bauen, dann das Spiel. Sonst direkt totk.exe."""
    def q(p):
        return "'" + str(p).replace("'", "''") + "'"
    uvw = (uv_path() or Path('uv.exe')).parent / ('uvw' + EXE)
    if updates and uvw.is_file():
        target, args, wd = uvw, f'run --no-project --python 3.14 "{HERE / "testbank.py"}" --play', HERE
    else:
        target, args, wd = exe, '', Path(exe).parent
    return ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Desktop')"
            f"+'\\TotK Koop.lnk');$s.TargetPath={q(target)};$s.Arguments={q(args)};$s.WorkingDirectory={q(wd)};"
            f"$s.IconLocation={q(str(exe) + ',0')};$s.Save()")


def play_plan(ctx):
    """Vor dem Spielstart: (neuer Commit oder None, exe veraltet?). Ohne Internet bleibt es beim Vorhandenen."""
    repo = ctx.repo
    src = update_source()
    new = None
    if src and (repo / '.git').exists() and git_out(repo, 'fetch', '--quiet', src[0], src[1], timeout=40) is not None:
        fetched = git_out(repo, 'rev-parse', 'FETCH_HEAD')
        if fetched and fetched != git_out(repo, 'rev-parse', 'HEAD'):
            new = fetched
    stale = built_commit(ctx.exe) != git_out(repo, 'rev-parse', 'HEAD')
    return new, stale


def launch_game(exe):
    flags = getattr(subprocess, 'DETACHED_PROCESS', 0) | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
    subprocess.Popen([str(exe)], cwd=str(Path(exe).parent), creationflags=flags, close_fds=True)


def update_job(ctx, new, say):
    """Neuen Koop-Stand auschecken und nur das Nötige neu bauen (Spielcode übersetzen, Kompilieren)."""
    def job(r):
        repo = ctx.repo
        if new:
            say('Neuen Stand holen …')
            if r.run([str(git_path() or 'git'), '-C', str(repo), 'checkout', '-q', '-f', '-B', KOOP_BRANCH, new]):
                return False, 'Der neue Stand ließ sich nicht auschecken.'
            self_update(repo)
        ok, text = build_job(ctx, set(UPDATE_STEPS),
                             lambda k, st: st == 'run' and say(f'Update wird gebaut: {k} …'))(r)
        if ok:
            mark_built(ctx.exe, repo)
        return ok, text
    return job


def play_main():
    """Start über die Desktop-Verknüpfung: Updates holen, bei Bedarf bauen, dann das Spiel starten."""
    import tkinter as tk
    from tkinter import messagebox, ttk
    from tkinter.scrolledtext import ScrolledText

    s = load_state()
    ctx = Ctx(Path(s['repo_dir']), s)
    if not ctx.exe.is_file() and not (ctx.repo / '.git').exists():
        tk.Tk().withdraw()
        messagebox.showerror('TotK Koop', 'Das Spiel ist noch nicht installiert. Bitte zuerst das Setup ausführen.')
        return 1
    if exe_in_use(ctx.exe):   # läuft schon: nicht bauen, einfach noch eine Instanz starten
        launch_game(ctx.exe)
        return 0
    new, stale = play_plan(ctx)
    if not new and not stale and ctx.exe.is_file():
        launch_game(ctx.exe)
        return 0

    root = tk.Tk()
    root.title('TotK Koop – Update')
    root.geometry('760x420')
    ttk.Label(root, text='Update wird installiert', font=('Segoe UI', 14, 'bold')).pack(anchor='w', padx=16, pady=(14, 2))
    ttk.Label(root, foreground='#555', wraplength=720, justify='left', text=(
        'Es gibt Verbesserungen für das Koop-Spiel. Sie werden jetzt eingebaut (meist 1 bis 5 Minuten), danach startet '
        'das Spiel von selbst. Bitte das Fenster offen lassen.')).pack(anchor='w', padx=16)
    status = ttk.Label(root, text='Startet …')
    status.pack(anchor='w', padx=16, pady=8)
    bar = ttk.Progressbar(root, mode='indeterminate')
    bar.pack(fill='x', padx=16)
    bar.start(12)
    log = ScrolledText(root, height=10, wrap='none', font=('Consolas', 8), background='#1e1e1e', foreground='#d4d4d4')
    log.pack(fill='both', expand=True, padx=16, pady=12)
    runner = Runner()
    say_q = queue.Queue()
    runner.start('Update', update_job(ctx, new, say_q.put))

    def pump():
        try:
            while True:
                status.config(text=say_q.get_nowait())
        except queue.Empty:
            pass
        try:
            while True:
                m = runner.q.get_nowait()
                if m[0] == 'log':
                    log.insert('end', m[1] + '\n')
                    log.see('end')
                elif m[0] == 'end':
                    bar.stop()
                    if not m[1]:
                        messagebox.showwarning('TotK Koop', 'Das Update ist fehlgeschlagen:\n' + m[2] +
                                               '\n\nDie bisherige Version startet. Beim nächsten Start wird es neu '
                                               f'versucht. Protokoll: {LOG_DIR}')
                    if ctx.exe.is_file():
                        launch_game(ctx.exe)
                    root.destroy()
                    return
        except queue.Empty:
            pass
        root.after(100, pump)
    pump()
    root.mainloop()
    return 0


def install_values(profile):
    """totk.ini der einfachen Installation: Koop wird im Startmenü des Spiels gewählt (Mehrspieler: Gastgeber oder
    Beitreten), darum coop = 0; Deutsch; das Koop-Profil mit dem gemeinsamen Startstand."""
    return {'coop': '0', 'coop_port': '27460', 'coop_marker': '0', 'coop_puppet': '0', 'lang': 'de', 'menu_lang': 'de',
            'profile_dir': str(profile).replace('\\', '/')}


def install_job(s, dry, say):
    """Der ganze Ablauf der einfachen Installation, für Runner.start. say(text) setzt die Statuszeile."""
    def job(r):
        repo = Path(s['repo_dir'])

        def g():
            return str(git_path() or 'git')
        # 1. Werkzeuge
        say('1/6 Werkzeuge installieren …')
        for tool in TOOLS:
            if tool.optional or not tool.install:
                continue
            ok, txt = tool.check()
            if ok:
                r.log(f'-- {tool.name}: vorhanden ({txt})', 'dim')
                continue
            r.log(f'-- {tool.name} wird installiert', 'head')
            for cmd in tool.install():
                r.run(cmd, dry=dry)
            if not dry and not tool.check()[0]:
                return False, f'{tool.name} konnte nicht installiert werden. Setup neu starten (der PATH ist dann frisch).'
        ok, txt = check_vulkan()
        if not ok and not dry:
            return False, 'Vulkan fehlt: bitte den Grafiktreiber aktualisieren und das Setup neu starten.'
        # 2. Quellcode holen und den Koop-Patch einspielen
        say('2/6 Quellcode holen …')
        src = update_source()
        if src and not (repo / '.git').exists():
            # Koop-Zweig mit allen Fixes direkt holen; spätere Updates holt der Start über die Verknüpfung (--play)
            if repo.exists() and any(repo.iterdir()) and not dry:
                return False, f'{repo} ist nicht leer. Bitte einen anderen Ordner wählen.'
            if r.run([g(), 'clone', '--branch', src[1], src[0], str(repo)], dry=dry):
                return False, 'Klonen fehlgeschlagen (Internet?).'
        elif src and not dry:
            # schon installiert (auch mit einem älteren Setup): auf den neuesten Koop-Stand bringen
            if r.run([g(), '-C', str(repo), 'fetch', src[0], src[1]], dry=dry):
                return False, 'Update holen fehlgeschlagen (Internet?).'
            if r.run([g(), '-C', str(repo), 'checkout', '-q', '-f', '-B', KOOP_BRANCH, 'FETCH_HEAD'], dry=dry):
                return False, 'Der neue Stand ließ sich nicht auschecken.'
        elif not src and not (repo / '.git').exists():
            # ohne Update-Quelle: Original-Repo und der beigelegte Koop-Patch
            if repo.exists() and any(repo.iterdir()) and not dry:
                return False, f'{repo} ist nicht leer. Bitte einen anderen Ordner wählen.'
            if r.run([g(), 'clone', s['repo_url'], str(repo)], dry=dry):
                return False, 'Klonen fehlgeschlagen (Internet?).'
            if r.run([g(), '-C', str(repo), 'checkout', '-q', '-B', 'koop', BASE_COMMIT], dry=dry):
                return False, f'Der Stand {BASE_COMMIT} fehlt im Repo.'
        if not (repo / 'runtime' / 'coop.cpp').is_file():
            ident = ['-c', 'user.name=TotK Koop Setup', '-c', 'user.email=setup@localhost']
            if r.run([g(), '-C', str(repo), *ident, 'am', '-3', s['patch']], dry=dry):
                r.run([g(), '-C', str(repo), 'am', '--abort'], dry=dry)
                return False, 'Der Koop-Patch ließ sich nicht anwenden.'
        else:
            r.log('-- Koop-Patch ist schon drin', 'dim')
        # 3. Pfade der Spieldateien
        say('3/6 Spieldateien eintragen …')
        if not dry:
            (repo / 'config.local.toml').write_text(config_toml(s), encoding='utf-8')
        r.log('-- config.local.toml geschrieben (nur Pfade)', 'ok')
        # 4. Bauen
        say('4/6 Spiel bauen (20 bis 60 Minuten) …')
        ctx = Ctx(repo, s)
        ok, text = build_job(ctx, {st.key for st in STEPS}, lambda k, st: say(f'4/6 Bauen: {k} ({st})'), dry=dry)(r)
        if not ok:
            return False, text
        # 5. totk.ini und Koop-Startstand
        say('5/6 Einstellungen …')
        build = ctx.exe.parent.parent
        profile = build / 'profile-koop'
        seed = HERE / 'koop-start'
        if not dry:
            mark_built(ctx.exe, repo)
            if src:
                self_update(repo)
            if not (profile / 'save').is_dir() and (seed / 'account.uid').is_file():
                shutil.copytree(seed, profile, dirs_exist_ok=True)
            ini = ctx.exe.parent / 'totk.ini'
            old = ini.read_text(encoding='utf-8') if ini.is_file() else ''
            ini.write_text(ini_set(old, install_values(profile)), encoding='utf-8')
        r.log(f'-- totk.ini und Koop-Startstand ({profile})', 'ok')
        # 6. Firewall und Verknüpfung auf dem Desktop
        say('6/6 Firewall und Desktop-Verknüpfung …')
        if os.environ.get('TOTK_SETUP_SKIP_SYSTEM'):   # Probelauf: keine Admin-Abfrage, keine Verknüpfung
            r.log('-- Firewall und Verknüpfung übersprungen (TOTK_SETUP_SKIP_SYSTEM)', 'dim')
        elif WIN:
            rule = ('advfirewall firewall add rule name="TotK Koop UDP 27460" dir=in action=allow protocol=UDP '
                    'localport=27460')
            r.run(['powershell', '-NoProfile', '-Command',
                   f"Start-Process netsh -Verb RunAs -Wait -ArgumentList '{rule}'"], dry=dry)
            r.run(['powershell', '-NoProfile', '-Command', shortcut_ps(ctx.exe, bool(src))], dry=dry)
        return True, 'Fertig! Starte das Spiel über „TotK Koop“ auf dem Desktop.'
    return job


def install_gui(dry=False):
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    from tkinter.scrolledtext import ScrolledText

    if WIN:
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    s = load_state()
    s['patch'] = str(HERE / 'totk-coop-stage2.patch')
    s['version'] = 'v143'
    s['shaders'] = True
    runner = Runner()
    root = tk.Tk()
    root.title('TotK Koop – Installation')
    root.geometry('900x640')
    style = ttk.Style(root)
    if 'vista' in style.theme_names():
        style.theme_use('vista')
    style.configure('Head.TLabel', font=('Segoe UI', 16, 'bold'))
    style.configure('Sub.TLabel', foreground='#555')
    style.configure('Ok.TLabel', foreground='#1b7f3a')
    style.configure('Bad.TLabel', foreground='#b3261e')

    top = ttk.Frame(root, padding=16)
    top.pack(fill='x')
    ttk.Label(top, text='TotK Koop installieren', style='Head.TLabel').grid(row=0, column=0, columnspan=3, sticky='w')
    ttk.Label(top, style='Sub.TLabel', wraplength=840, justify='left', text=(
        'Wähle deine eigenen Spieldateien und einen Ordner mit etwa 60 GB freiem Platz. Dann „Installieren“. Alles '
        'Weitere läuft von selbst; beim ersten Mal 30 bis 90 Minuten. Manche Schritte fragen nach Administratorrechten.')
              ).grid(row=1, column=0, columnspan=3, sticky='w', pady=(4, 12))
    fields = [('base', 'Basisspiel (XCI/NSP)', 'file', [('Switch-Spiel', '*.nsp *.xci'), ('Alle Dateien', '*.*')]),
              ('update', 'Update 1.4.3 (NSP)', 'file', [('Update', '*.nsp'), ('Alle Dateien', '*.*')]),
              ('keys', 'prod.keys', 'file', [('Schlüssel', '*.keys'), ('Alle Dateien', '*.*')]),
              ('dest', 'Installieren nach', 'dir', None)]
    # Vorschlag: ein früherer Ort, sonst das Laufwerk mit dem meisten freien Platz (C: ist oft voll)
    default_dest = str(Path(s['repo_dir']).parent) if (Path(s['repo_dir']) / 'build').is_dir() else ''
    if not default_dest:
        drives = [Path(f'{c}:\\') for c in 'CDEFGHIJKLMNOPQRSTUVWXYZ'] if WIN else [Path.home()]
        best = max((d for d in drives if d.exists()), key=lambda d: free_gb(d) or 0, default=Path.home())
        default_dest = str(best / 'TotK-Koop') if WIN else str(best)
    vals = {k: tk.StringVar(value=(s.get(k, '') if k != 'dest' else default_dest)) for k, *_ in fields}
    infos = {}

    def pick(key, kind, types):
        p = filedialog.askdirectory() if kind == 'dir' else filedialog.askopenfilename(filetypes=types)
        if p:
            vals[key].set(str(Path(p)))

    for i, (key, label, kind, types) in enumerate(fields, start=1):
        ttk.Label(top, text=label, width=22).grid(row=i * 2, column=0, sticky='w', pady=(4, 0))
        ttk.Entry(top, textvariable=vals[key], width=78).grid(row=i * 2, column=1, sticky='we', pady=(4, 0))
        ttk.Button(top, text='Wählen …', command=lambda k=key, kd=kind, t=types: pick(k, kd, t)).grid(
            row=i * 2, column=2, padx=6, pady=(4, 0))
        infos[key] = ttk.Label(top, text='', style='Sub.TLabel')
        infos[key].grid(row=i * 2 + 1, column=1, sticky='w')

    def refresh(*_):
        for key, *_ in fields:
            p = vals[key].get()
            if key == 'dest':
                gb = free_gb(Path(p)) if p else None
                infos[key].config(text=f'{gb:.0f} GB frei' if gb is not None else '',
                                  style='Ok.TLabel' if gb and gb >= 60 else 'Bad.TLabel')
            else:
                ok, txt = file_info(p)
                infos[key].config(text=txt, style='Ok.TLabel' if ok else 'Bad.TLabel' if ok is False else 'Sub.TLabel')
    for v in vals.values():
        v.trace_add('write', refresh)
    refresh()

    bar = ttk.Frame(root, padding=(16, 4))
    bar.pack(fill='x')
    go = ttk.Button(bar, text='Installieren')
    go.pack(side='left')
    status = ttk.Label(bar, text='Bereit.', style='Sub.TLabel')
    status.pack(side='left', padx=12)
    log = ScrolledText(root, height=14, wrap='none', font=('Consolas', 9), background='#1e1e1e', foreground='#d4d4d4')
    log.pack(fill='both', expand=True, padx=16, pady=(4, 16))
    for tag, color in (('err', '#ff7b72'), ('ok', '#7ee787'), ('cmd', '#79c0ff'), ('head', '#e3b341'), ('dim', '#8b949e')):
        log.tag_configure(tag, foreground=color)
    say_q = queue.Queue()

    def start():
        miss = [n for k, n, *_ in fields[:3] if not file_info(vals[k].get())[0]]
        if miss and not dry:
            messagebox.showerror('TotK Koop', 'Bitte zuerst wählen: ' + ', '.join(miss))
            return
        dest = Path(vals['dest'].get() or '.')
        gb = free_gb(dest)
        if gb is not None and gb < 60 and not (dest / 'totk-pc-recomp' / '.git').exists() and not dry:
            if not messagebox.askyesno('TotK Koop', f'Dort sind nur {gb:.0f} GB frei, gebraucht werden etwa 60 GB. Trotzdem?'):
                return
        for k in ('base', 'update', 'keys'):
            s[k] = vals[k].get()
        s['repo_dir'] = str(dest / 'totk-pc-recomp')
        save_state(s)
        go.config(state='disabled')
        runner.start('Installation', install_job(s, dry, say_q.put))

    go.config(command=start)

    def pump():
        try:
            while True:
                status.config(text=say_q.get_nowait(), style='Sub.TLabel')
        except queue.Empty:
            pass
        try:
            while True:
                m = runner.q.get_nowait()
                if m[0] == 'log':
                    log.insert('end', m[1] + '\n', m[2])
                    log.see('end')
                elif m[0] == 'end':
                    status.config(text=m[2], style='Ok.TLabel' if m[1] else 'Bad.TLabel')
                    go.config(state='normal', text='Nochmal versuchen')
                    if m[1] and not dry and '--auto' not in sys.argv:
                        go.config(state='disabled', text='Fertig')
                        messagebox.showinfo('TotK Koop', m[2] + '\n\nIm Spiel: „Mehrspieler“. Einer wählt „Spiel erstellen“ '
                                            'und nennt seine Adresse, der andere „Spiel beitreten“ und tippt sie ein.')
        except queue.Empty:
            pass
        root.after(80, pump)
    pump()
    if '--auto' in sys.argv:   # Selbstprüfung: mit den gespeicherten Pfaden gleich loslegen, nach dem Ende schließen
        root.after(300, start)

        def closer():
            if runner.thread and not runner.busy:
                root.after(1500, root.destroy)
            else:
                root.after(500, closer)
        root.after(1000, closer)
    root.mainloop()


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--selftest':
        sys.exit(selftest())
    if len(sys.argv) > 1 and sys.argv[1] == '--install':
        install_gui(dry='--dry' in sys.argv)
        sys.exit(0)
    if len(sys.argv) > 1 and sys.argv[1] == '--play':
        sys.exit(play_main())
    gui()
