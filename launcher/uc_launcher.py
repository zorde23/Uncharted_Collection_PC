#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Uncharted: The Nathan Drake Collection launcher for Windows (Tkinter).

One window: choose the folder of your own PS4 dump (and its update), pick a game, press Play.
Each game starts with the option set tested for it (GAMES below); everything else stays at the
port's defaults. Settings live next to the executable in launcher.json; saves in user\\.

Frozen with PyInstaller (packaging/windows/package_uc.sh) the same executable also runs run.py
(`--run`), the preparation scripts (`--script`) and a game without the window
(`--play uc1|uc2|uc3`, for desktop shortcuts), so players need no Python and no terminal.
"""
import hashlib
import json
import os
from pathlib import Path
import queue
import runpy
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
import webbrowser

WINDOWS = sys.platform == 'win32'
if WINDOWS:
    import ctypes
FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / 'scripts'))
DATA_DIR = Path(os.environ.get('UC_DATA_DIR') or os.environ.get('GOW3_DATA_DIR') or PORT_DIR)
SETTINGS_FILE = DATA_DIR / 'launcher.json'
INI_FILE = DATA_DIR / 'ucndc.ini'
OLD_INI_FILE = DATA_DIR / 'gow3.ini'  # name before 0.3; copied once, left in place
USER_DIR = DATA_DIR / 'user'
LOG_FILE = USER_DIR / 'last_run.log'
INPUT_FILE = DATA_DIR / 'input.ini'
APP_NAME = 'Uncharted Collection PC'
EXE_NAME = 'Uncharted Launcher.exe'
VERSION = '0.3.7-beta2'
RELEASES_API = 'https://api.github.com/repos/zorde23/Uncharted_Collection_PC/releases/latest'
RELEASES_PAGE = 'https://github.com/zorde23/Uncharted_Collection_PC/releases/latest'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
TESTED_TITLE = 'CUSA02344'

# Options every game starts with (all of them are off by default in the code).
COMMON = {
    'GOW3_READBACKS': '2', 'GOW3_FRAME_STATS': '1', 'GOW3_READBACK_ASYNC': '1', 'GOW3_LEAF_COPY': '1',
    'GOW3_SCALAR_COPY_HLE': '1', 'GOW3_HLE_INVALIDATE': '1', 'GOW3_EAGER_SUBMIT': '128',
    'GOW3_EAGER_SUBMIT_US': '1000', 'GOW3_ASYNC_SUBMIT': '1',
    'GOW3_RING_COPY_THREAD': '1', 'GOW3_DEFERRED_SUBMIT': '1', 'GOW3_RING_DEDUP': '1',
    'GOW3_READBACK_FAST': '1', 'GOW3_READBACK_FAST_WAIT': '1', 'GOW3_READBACK_FAST_SIDEQ': '1',
    # Bloom mips keep their place when another image shares their memory (red spots, 2026-10-22).
    'GOW3_OVERLAP_KEEP_VIEW': '1',
    # Guest stores of red-zone leaf functions run 128 bytes below rsp, so a fault's frame does not
    # overwrite their locals (crashes on Intel 12th gen and later, UC1 0x64011d / UC3 0x12d83fe).
    'GOW3_RED_ZONE': '1',
}
# key, title, executable, (status pt, status en), extra options
GAMES = [
    ('uc1', "Uncharted: Drake's Fortune", 'eboot.bin', ('Jogável', 'Playable'),
     {'GOW3_COMPUTE_PIPE': '1', 'GOW3_LOW_IMAGE': '1', 'GOW3_AJM_END_RULES': '1', 'GOW3_EOP_TICK_WAIT': '1',
      'GOW3_MOUSE_AIM_U1_ALL': '1', 'GOW3_READBACK_SIDEQ': '1',
      # From UC3 (tested on UC1 2026-10-20: the flashlight works with them). FAULT_WRITES: crash log aid.
      'GOW3_SYNC_SMALL_TARGETS': '1', 'GOW3_JOB_SPIN': '100', 'GOW3_JOB_SLEEP': '1', 'GOW3_MIP_FIX': '1',
      'GOW3_SCOPE_SPLIT_SCALAR': '1', 'GOW3_DEPTH_KEEP_CLEAR': '1', 'GOW3_FAULT_WRITES': '1',
      'GOW3_MOUSE_CAMERA_LOG': '1', 'GOW3_CB_WATCH': '1',
      # The friend's crash (execute at 0, 2026-10-22): a binding with no handler waits, else is skipped.
      'GOW3_UC1_BIND_RETRY': '1',
      # UC1 mouse camera (2026-10-23/24): aim, cover, car turret, jet ski, walking/stairs (hold), mounted gun.
      'GOW3_MOUSE_STEADY': '1', 'GOW3_MOUSE_AIM_U1_POST': '1', 'GOW3_MOUSE_U1_LEASH': '1',
      'GOW3_MOUSE_U1_PER_OBJ': '1', 'GOW3_MOUSE_U1_SWAP_SNAP': '1', 'GOW3_MOUSE_U1_AIM_PHASE': '1',
      'GOW3_MOUSE_U1_AIM_SEED_FOLLOW': '1', 'GOW3_MOUSE_U1_FAST_STICK': '1', 'GOW3_MOUSE_U1_HOLD_INPUT': '1',
      'GOW3_MOUSE_U1_GUN': '1', 'GOW3_MOUSE_U1_CAR': '1', 'GOW3_MOUSE_PUSH': '0',
      # 0.3.7: videos keep updating (frozen picture), texture memory limit from the driver's budget.
      'GOW3_VIDEO_INVALIDATE': '1', 'GOW3_GC_LIVE_BUDGET': '1'}),
    ('uc2', 'Uncharted 2: Among Thieves', 'big2-ps4_Shipping.elf', ('Jogável', 'Playable'),
     # Without GOW3_COMPUTE_PIPE: with it the train carriage in the prologue falls too early. The two
     # in-order options keep the rooftop at 60 FPS; the train jump is still timing-sensitive with them.
     {'GOW3_HLE_INORDER': '1', 'GOW3_ACB_DISPATCH_PIPE': '1', 'GOW3_READBACK_SIDEQ': '1',
      # From UC3 (2026-10-22, Matheus); COMPUTE_PIPE stays off (train).
      'GOW3_SYNC_SMALL_TARGETS': '1', 'GOW3_JOB_SPIN': '100', 'GOW3_JOB_SLEEP': '1', 'GOW3_MIP_FIX': '1',
      'GOW3_SCOPE_SPLIT_SCALAR': '1', 'GOW3_DEPTH_KEEP_CLEAR': '1',
      # 0.3.7: aim focus without whole-screen blur (Umar-Qayyum-dev, Supermedo's uncharted_pc), videos
      # keep updating, texture memory limit from the driver's budget.
      'GOW3_TINY_READBACK': '1', 'GOW3_VIDEO_INVALIDATE': '1', 'GOW3_GC_LIVE_BUDGET': '1'}),
    # uc3 without GOW3_READBACK_SIDEQ: with it Drake's scarf jumps around in the desert (the guest
    # resumes before the graphics work behind a readback runs); no FPS difference measured.
    ('uc3', "Uncharted 3: Drake's Deception", 'big3-ps4_Shipping.elf', ('Jogável · 60 FPS', 'Playable · 60 FPS'),
     {'GOW3_COMPUTE_PIPE': '1', 'GOW3_SYNC_SMALL_TARGETS': '1', 'GOW3_JOB_SPIN': '100', 'GOW3_JOB_SLEEP': '1',
      'GOW3_MIP_FIX': '1', 'GOW3_MOUSE_EDGE_AIM': '1', 'GOW3_SCOPE_SPLIT_SCALAR': '1',
      'GOW3_DEPTH_KEEP_CLEAR': '1',
      # 0.3.7: Sully's shirt on NVIDIA (BilalReloaded, via Supermedo's uncharted_pc), videos keep
      # updating, texture memory limit from the driver's budget.
      'GOW3_LDS_BARRIERS_MULTIWAVE': '1', 'GOW3_VIDEO_INVALIDATE': '1', 'GOW3_GC_LIVE_BUDGET': '1'}),
]
# Off unless chosen in Options > Advanced (launcher.json 'experimental'): (games, value).
EXPERIMENTAL = {'GOW3_PREP_TRUST_A1': (('uc3',), '1'), 'GOW3_PREP_WORKERS': (('uc1', 'uc2', 'uc3'), '2'),
                # 0.3.7: the game mixes 7.1 when told the output has 8 channels (sceAudioOutGetPortState);
                # the port passes them to a 5.1/7.1 Windows output, or folds them to stereo.
                'GOW3_AUDIO_SURROUND': (('uc1', 'uc2', 'uc3'), '1')}
# ucndc.ini values the games were tested with, written when the file does not exist yet.
# Only keys the Uncharted games use (render_resolution, engine_fps, fsr1, rcas and upscaler are GoW3 patches).
INI_DEFAULTS = {'display_mode': 'windowed', 'vsync': '1', 'fps_limit': '0',
                'async_shaders': '1', 'frames_queued': '1',
                'deferred_readback': '1', 'stale_readback': '0', 'show_fps': '1', 'overlay_fps': '1'}
DISPLAY_MODES = [('windowed', ('Janela', 'Window')), ('borderless', ('Janela sem borda', 'Borderless')),
                 ('fullscreen', ('Tela cheia', 'Fullscreen'))]
DEFAULTS = {'game_dir': '', 'update_dir': '', 'language': '', 'last_game': 'uc3', 'disabled': [], 'experimental': [],
            'check_updates': True, 'resolution': '1080'}
# Render and output resolution of the three games (launcher.json 'resolution'; the GOW3_UC3_* names
# predate UC1 and UC2, whose executables share UC3's video settings and memory table). Above 1080p the game's own targets
# grow (GOW3_UC3_RES); its render target heaps and GPU memory grow with them, and the intro movies
# are scaled to the screen. Measured: 1440p and 4K on an RX 6750 XT; the image is scaled to the window.
RESOLUTIONS = [('1080', ('1920x1080 (padrão)', '1920x1080 (default)')),
               ('1440', ('2560x1440', '2560x1440')),
               ('2160', ('3840x2160 (4K)', '3840x2160 (4K)'))]
UC3_RESOLUTION_ENV = {
    '1440': {'GOW3_UC3_RES': '2560x1440', 'GOW3_UC3_RES_ALL': '1', 'GOW3_UC3_MOVIE_FIX': '1',
             'GOW3_UC3_RTHEAP_MB': '512', 'GOW3_DMEM_MB': '6144'},
    '2160': {'GOW3_UC3_RES': '3840x2160', 'GOW3_UC3_RES_ALL': '1', 'GOW3_UC3_MOVIE_FIX': '1',
             'GOW3_UC3_RTHEAP_MB': '1792', 'GOW3_UC3_RT_MB': '448', 'GOW3_DMEM_MB': '8192'},
}
# What each optimization does (Options > Advanced); the games turn them on by default.
TOGGLE_HELP = {
    'GOW3_READBACK_ASYNC': ('Leituras da GPU sem parar a fila', 'GPU readbacks without draining the queue'),
    'GOW3_READBACK_FAST': ('Leitura rápida de buffers', 'Fast buffer readbacks'),
    'GOW3_READBACK_FAST_WAIT': ('Espera só a escrita certa da GPU', 'Wait only for the right GPU write'),
    'GOW3_READBACK_FAST_SIDEQ': ('Leituras na fila de transferência', 'Readbacks on the transfer queue'),
    'GOW3_READBACK_SIDEQ': ('Fila lateral de leituras', 'Side readback queue'),
    'GOW3_SCALAR_COPY_HLE': ('Cópia de constantes em código nativo', 'Constant copies in native code'),
    'GOW3_HLE_INVALIDATE': ('Avisa os caches da cópia nativa (evita texturas trocadas)', 'Tell caches about native copies (no swapped textures)'),
    'GOW3_COMPUTE_PIPE': ('Atalho no processamento de compute (UC1 e UC3)', 'Compute processing shortcut (UC1 and UC3)'),
    'GOW3_LEAF_COPY': ('Cópia das folhas de recursos', 'Resource leaf copies'),
    'GOW3_EAGER_SUBMIT': ('Envia comandos à GPU mais cedo', 'Submit GPU work earlier'),
    'GOW3_EAGER_SUBMIT_US': ('Intervalo do envio antecipado', 'Early submit interval'),
    'GOW3_ASYNC_SUBMIT': ('Envio em outra thread', 'Submit on another thread'),
    'GOW3_DEFERRED_SUBMIT': ('Envio adiado sem esperar a gravação', 'Deferred submit without waiting'),
    'GOW3_RING_COPY_THREAD': ('Cópias do ring buffer em outra thread', 'Ring buffer copies on another thread'),
    'GOW3_RING_DEDUP': ('Não repete cópias do ring buffer', 'Skip repeated ring buffer copies'),
    'GOW3_FRAME_STATS': ('Estatísticas de quadro no log', 'Frame statistics in the log'),
    'GOW3_EOP_TICK_WAIT': ('Evita o erro m_gfxEopTick (UC1)', 'Avoid the m_gfxEopTick assert (UC1)'),
    'GOW3_AJM_END_RULES': ('Regras de áudio do Ajm (UC1)', 'Ajm audio rules (UC1)'),
    'GOW3_MOUSE_AIM_U1_ALL': ('Mira com mouse estável (UC1)', 'Steady mouse aiming (UC1)'),
    'GOW3_HLE_INORDER': ('Cópias nativas em ordem (UC2; desligue se o trem do prólogo falhar)',
                         'Native copies in order (UC2; turn off if the prologue train fails)'),
    'GOW3_ACB_DISPATCH_PIPE': ('Compute assíncrono em ordem (UC2; desligue se o trem do prólogo falhar)',
                               'Async compute in order (UC2; turn off if the prologue train fails)'),
    'GOW3_SYNC_SMALL_TARGETS': ('Compila na hora shaders de alvos pequenos (luz da tocha, UC1, UC2 e UC3)',
                                'Compile small-target shaders right away (torch light, UC1, UC2 and UC3)'),
    'GOW3_JOB_SPIN': ('Threads de jobs giram menos antes de dormir (menos CPU)', 'Job threads spin less before sleeping (less CPU)'),
    'GOW3_MIP_FIX': ('Bloom e distorção corretos (imagem não fica lavada, UC1, UC2 e UC3)', 'Correct bloom and distortion (no washed-out image, UC1, UC2 and UC3)'),
    'GOW3_MOUSE_EDGE_AIM': ('Mouse na mira pendurado e escalando (UC3)', 'Mouse aiming while hanging and climbing (UC3)'),
    'GOW3_SCOPE_SPLIT_SCALAR': ('Névoa dos cones de luz sem cunhas pretas (UC1, UC2 e UC3)', 'Light cone fog without black wedges (UC1, UC2 and UC3)'),
    'GOW3_DEPTH_KEEP_CLEAR': ('Sombras e luzes sem piscar (UC1, UC2 e UC3)', 'Shadows and lights without flicker (UC1, UC2 and UC3)'),
    'GOW3_PREP_TRUST_A1': ('Experimental: preparo de desenhos mais rápido (+FPS, UC3)', 'Experimental: faster draw preparation (+FPS, UC3)'),
    'GOW3_PREP_WORKERS': ('Experimental: 2 threads de preparo (pode ajudar CPUs de 4 e 6 núcleos)', 'Experimental: 2 preparation threads (may help 4 and 6 core CPUs)'),
    'GOW3_JOB_SLEEP': ('Threads de jobs podem dormir no gameplay (menos CPU)', 'Job threads may sleep in gameplay (less CPU)'),
    'GOW3_AUDIO_SURROUND': ('Experimental: som surround 5.1/7.1 (caixas ou fone surround configurados no Windows)',
                            'Experimental: 5.1/7.1 surround sound (speakers or surround headphones set up in Windows)'),
    'GOW3_TINY_READBACK': ('Foco da mira sem borrar a tela (UC2)', 'Aim focus without blurring the screen (UC2)'),
    'GOW3_LDS_BARRIERS_MULTIWAVE': ('Roupa do Sully sem esticar em placas NVIDIA (UC3)', "Sully's shirt without stretching on NVIDIA cards (UC3)"),
    'GOW3_VIDEO_INVALIDATE': ('Vídeos não congelam a imagem (UC1, UC2 e UC3)', 'Videos do not freeze on one picture (UC1, UC2 and UC3)'),
    'GOW3_GC_LIVE_BUDGET': ('Limite de memória de texturas pelo que a placa tem livre (menos descarte em placas de 8 GB)',
                            'Texture memory limit from what the card has free (fewer evictions on 8 GB cards)'),
}
# Options > Advanced shows the toggles in two lists: these fix bugs, the others are for performance.
FIX_TOGGLES = {'GOW3_MOUSE_AIM_U1_ALL', 'GOW3_SYNC_SMALL_TARGETS', 'GOW3_MIP_FIX', 'GOW3_MOUSE_EDGE_AIM',
               'GOW3_SCOPE_SPLIT_SCALAR', 'GOW3_DEPTH_KEEP_CLEAR', 'GOW3_TINY_READBACK',
               'GOW3_LDS_BARRIERS_MULTIWAVE', 'GOW3_VIDEO_INVALIDATE'}
# The Uncharted engines run at 60 at most, so higher limits would do nothing.
FPS_LIMITS = [('0', ('Sem limite (60)', 'No limit (60)')), ('30', ('30', '30')), ('60', ('60', '60'))]
# Fixed per game, never a user switch: UC1 needs LOW_IMAGE, AJM_END_RULES and EOP_TICK_WAIT; READBACKS
# keeps the scenery; FRAME_STATS, FAULT_WRITES, MOUSE_CAMERA_LOG and CB_WATCH are only log aids; HLE_INVALIDATE follows SCALAR_COPY_HLE (texture swaps).
FIXED = {'GOW3_READBACKS', 'GOW3_FRAME_STATS', 'GOW3_LOW_IMAGE', 'GOW3_AJM_END_RULES', 'GOW3_EOP_TICK_WAIT',
         'GOW3_HLE_INVALIDATE', 'GOW3_FAULT_WRITES',
         'GOW3_MOUSE_CAMERA_LOG', 'GOW3_CB_WATCH', 'GOW3_OVERLAP_KEEP_VIEW', 'GOW3_UC1_BIND_RETRY',
         'GOW3_RED_ZONE', 'GOW3_MOUSE_STEADY', 'GOW3_MOUSE_AIM_U1_POST', 'GOW3_MOUSE_U1_LEASH',
         'GOW3_MOUSE_U1_PER_OBJ', 'GOW3_MOUSE_U1_SWAP_SNAP', 'GOW3_MOUSE_U1_AIM_PHASE',
         'GOW3_MOUSE_U1_AIM_SEED_FOLLOW', 'GOW3_MOUSE_U1_FAST_STICK', 'GOW3_MOUSE_U1_HOLD_INPUT',
         'GOW3_MOUSE_U1_GUN', 'GOW3_MOUSE_U1_CAR', 'GOW3_MOUSE_PUSH'}

# Controls window (input.ini, read by the game: src/runtime_pad.c), ported from SuperMedo's launcher
# (uncharted_pc, GPL). Actions with their PS4 names, the default keys ("A|B": two bindings) and the
# default controller buttons (SDL names). Without a saved Controls window the game keeps its own.
BUTTON_ACTIONS = [
    ('cross', ('X', 'Cross ✕'), 'Space', 'a'), ('circle', ('Círculo ○', 'Circle ○'), 'Left Ctrl|Q', 'b'),
    ('square', ('Quadrado □', 'Square □'), 'F', 'x'), ('triangle', ('Triângulo △', 'Triangle △'), 'E', 'y'),
    ('l1', ('L1', 'L1'), 'G', 'leftshoulder'), ('r1', ('R1', 'R1'), 'R', 'rightshoulder'),
    ('l2', ('L2 (mirar)', 'L2 (aim)'), 'Mouse Right', 'lefttrigger'),
    ('r2', ('R2 (atirar)', 'R2 (fire)'), 'Mouse Left', 'righttrigger'),
    ('l3', ('L3', 'L3'), 'V', 'leftstick'), ('r3', ('R3', 'R3'), 'Mouse Middle', 'rightstick'),
    ('options', ('Options', 'Options'), 'Escape|Return', 'start'),
    ('touchpad_left', ('Touchpad, lado esquerdo', 'Touchpad, left side'), 'Tab', 'none'),
    ('touchpad_right', ('Touchpad, lado direito', 'Touchpad, right side'), 'Backspace', 'back'),
    ('up', ('Direcional cima', 'D-pad up'), '4', 'dpup'), ('down', ('Direcional baixo', 'D-pad down'), '3', 'dpdown'),
    ('left', ('Direcional esquerda', 'D-pad left'), '1|Wheel Up', 'dpleft'),
    ('right', ('Direcional direita', 'D-pad right'), '2|Wheel Down', 'dpright'),
]
STICK_ACTIONS = [
    ('ls_up', ('Andar para frente', 'Move forward'), 'W'), ('ls_down', ('Andar para trás', 'Move back'), 'S'),
    ('ls_left', ('Andar para a esquerda', 'Move left'), 'A'), ('ls_right', ('Andar para a direita', 'Move right'), 'D'),
    ('rs_up', ('Olhar para cima', 'Look up'), 'I'), ('rs_down', ('Olhar para baixo', 'Look down'), 'K'),
    ('rs_left', ('Olhar para a esquerda', 'Look left'), 'J'), ('rs_right', ('Olhar para a direita', 'Look right'), 'L'),
]
DEFAULT_KEYS = {a: k for a, _t, k, _p in BUTTON_ACTIONS} | {a: k for a, _t, k in STICK_ACTIONS}
DEFAULT_PAD = {a: p for a, _t, _k, p in BUTTON_ACTIONS}
PAD_BUTTONS = [('a', ('A / X', 'A / Cross')), ('b', ('B / Círculo', 'B / Circle')),
               ('x', ('X / Quadrado', 'X / Square')), ('y', ('Y / Triângulo', 'Y / Triangle')),
               ('leftshoulder', ('LB / L1', 'LB / L1')), ('rightshoulder', ('RB / R1', 'RB / R1')),
               ('lefttrigger', ('LT / L2', 'LT / L2')), ('righttrigger', ('RT / R2', 'RT / R2')),
               ('leftstick', ('Clique do analógico esquerdo / L3', 'Left stick click / L3')),
               ('rightstick', ('Clique do analógico direito / R3', 'Right stick click / R3')),
               ('start', ('Start / Options', 'Start / Options')), ('back', ('Back / Share', 'Back / Share')),
               ('touchpad', ('Clique do touchpad', 'Touchpad click')),
               ('dpup', ('Direcional cima', 'D-pad up')), ('dpdown', ('Direcional baixo', 'D-pad down')),
               ('dpleft', ('Direcional esquerda', 'D-pad left')), ('dpright', ('Direcional direita', 'D-pad right')),
               ('misc1', ('Share / Mic (misc)', 'Share / Mic (misc)')), ('paddle1', ('Paddle 1', 'Paddle 1')),
               ('paddle2', ('Paddle 2', 'Paddle 2')), ('paddle3', ('Paddle 3', 'Paddle 3')),
               ('paddle4', ('Paddle 4', 'Paddle 4')), ('none', ('Nenhum', 'Not assigned'))]
CONTROL_DEFAULTS = {'mouse_look': True, 'mouse_sensitivity': 1.0, 'mouse_aim_sensitivity': 1.0, 'mouse_invert_y': False,
                    'pad_invert_y': False, 'key_icons': False, 'keys': {}, 'pad': {}}
# Keyboard icons (Controls window): the game files redrawn with the player's keys, by game, as paths in
# the game folder, and the fonts tried for the key names (Windows first). data.psarc holds the font the
# texts draw buttons with (scripts/key_icons.py), the pak the HUD's button pictures (scripts/hud_icons.py).
# Both need Pillow.
ICON_FILES = {'uc1': ('u1data/data.psarc', 'u1data/build/main/pak19/ps4/particle.pak'),
              'uc2': ('u2data/data.psarc', 'u2data/build/main/pak22/ps4/hud.pak'),
              'uc3': ('u3data/data.psarc', 'u3data/build/main/pak23/ps4/hud.pak')}
# Keyboard icons only: UC1's texts, where "Click left |A|" (the left stick, WASD here) becomes |L3|.
ICON_TEXT_FILES = {'uc1': ('u1data/build/main/core.psarc',)}
ICON_FONTS = [Path(os.environ.get('WINDIR', r'C:\Windows')) / 'Fonts' / name
              for name in ('arialbd.ttf', 'arial.ttf', 'segoeuib.ttf')] + \
             [Path('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf')]
KEY_ICONS_VERSION = 5  # raise when key_icons.py draws differently, so old copies are made again
# Controls > icons in the game: the game's own PlayStation buttons, Xbox buttons in the game's style or the keys.
ICON_STYLES = ('ps', 'xbox', 'keys')


def icon_style(controls):
    """'ps', 'xbox' or 'keys'; launcher.json from 0.3.5 betas only has key_icons (True = keys)."""
    style = controls.get('icons')
    return style if style in ICON_STYLES else 'keys' if controls.get('key_icons') else 'ps'
# Tk key names -> SDL key names (letters and digits map to themselves, upper case).
TK_KEYS = {'space': 'Space', 'Return': 'Return', 'Escape': 'Escape', 'Tab': 'Tab', 'BackSpace': 'Backspace',
           'Shift_L': 'Left Shift', 'Shift_R': 'Right Shift', 'Control_L': 'Left Ctrl',
           'Control_R': 'Right Ctrl', 'Alt_R': 'Right Alt', 'Caps_Lock': 'CapsLock', 'Up': 'Up',
           'Down': 'Down', 'Left': 'Left', 'Right': 'Right', 'Delete': 'Delete', 'Home': 'Home',
           'End': 'End', 'Prior': 'PageUp', 'Next': 'PageDown', 'minus': '-', 'equal': '=',
           'bracketleft': '[', 'bracketright': ']', 'semicolon': ';', 'apostrophe': "'", 'comma': ',',
           'period': '.', 'slash': '/', 'backslash': '\\', 'grave': '`', 'KP_Enter': 'Keypad Enter',
           'KP_Add': 'Keypad +', 'KP_Subtract': 'Keypad -', 'KP_Multiply': 'Keypad *',
           'KP_Divide': 'Keypad /', 'KP_Decimal': 'Keypad .', 'KP_Delete': 'Keypad .',
           'KP_Insert': 'Keypad 0', 'KP_End': 'Keypad 1', 'KP_Down': 'Keypad 2', 'KP_Next': 'Keypad 3',
           'KP_Left': 'Keypad 4', 'KP_Begin': 'Keypad 5', 'KP_Right': 'Keypad 6', 'KP_Home': 'Keypad 7',
           'KP_Up': 'Keypad 8', 'KP_Prior': 'Keypad 9'}
# Insert and F1 open the port menu; Left Alt frees the mouse cursor.
RESERVED_KEYS = {'Insert': ('Insert abre o menu do port.', 'Insert opens the port menu.'),
                 'F1': ('F1 abre o menu do port.', 'F1 opens the port menu.'),
                 'Alt_L': ('Alt esquerdo solta o cursor do mouse enquanto segurado.',
                           'Left Alt frees the mouse cursor while held.')}

BG, PANEL, CARD, TEXT, MUTED, ACCENT, OK, BAD = ('#15130f', '#1f1b16', '#2a241c', '#efe6d4', '#a89c86',
                                                 '#d9a441', '#7fbf6a', '#d9674f')
LANG = 'pt'


def _(pt, en):
    return pt if LANG == 'pt' else en


# ---------------------------------------------------------------------------------------------
# Settings, game folders, environment

def load_settings():
    try:
        return {**DEFAULTS, **json.loads(SETTINGS_FILE.read_text(encoding='utf-8'))}
    except (OSError, ValueError):
        return dict(DEFAULTS)


def save_settings(settings):
    try:
        SETTINGS_FILE.write_text(json.dumps(settings, indent=2), encoding='utf-8')
    except OSError:
        pass


def system_language():
    if WINDOWS:
        try:
            lang_id = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
            return 'pt' if lang_id == 0x16 else 'en'
        except (AttributeError, OSError):
            pass
    return 'pt' if os.environ.get('LANG', '').lower().startswith('pt') else 'en'


def title_id(folder):
    """The TITLE_ID of a dump folder (sce_sys/param.sfo), or None."""
    try:
        from prepare import sfo
        return sfo((Path(folder) / 'sce_sys' / 'param.sfo').read_bytes()).get('TITLE_ID')
    except (OSError, ValueError, ImportError):
        return None


def is_update(folder):
    return (Path(folder) / 'sce_discmap_patch.plt').is_file()


def app_version(folder):
    """The game version of a dump folder ("01.00" base, "01.02" updated), or None: APP_VER, or VERSION when
    higher (CUSA02826 ships the 01.02 executables in its base package: APP_VER 01.00, VERSION 01.02)."""
    try:
        from prepare import sfo
        params = sfo((Path(folder) / 'sce_sys' / 'param.sfo').read_bytes())
    except (OSError, ValueError, ImportError):
        return None
    def key(v):
        try:
            return tuple(int(n) for n in v.split('.'))
        except (AttributeError, ValueError):
            return ()
    versions = [v for v in (params.get('APP_VER'), params.get('VERSION')) if key(v)]
    return max(versions, key=key) if versions else None


def has_update_merged(folder):
    """The game folder already holds the update (merged dump): its param.sfo says 01.02 or later."""
    version = app_version(folder) if folder else None
    try:
        return version is not None and tuple(int(n) for n in version.split('.')) >= (1, 2)
    except ValueError:
        return False


def has_games(folder):
    return any((Path(folder) / exe).is_file() for _k, _t, exe, _s, _e in GAMES)


def has_base_data(folder):
    """The base game's shared data (an update folder brings only what changed)."""
    return (Path(folder) / 'shareddata').is_dir()


def inner_dump(folder):
    """The dump inside a folder of the same name (CUSA01398\\CUSA01398, as pkg extractors leave it)."""
    nested = Path(folder) / Path(folder).name
    if not has_games(folder) and not is_update(folder) and nested.is_dir() and (has_games(nested) or is_update(nested)):
        return str(nested).replace('\\', '/')
    return folder


def guess_update(game_dir):
    """The update folder next to the game: CUSA02344-UPDATE, CUSA02344-patch, ...patch."""
    game = Path(game_dir)
    for candidate in (game.parent / f'{game.name}-UPDATE', game.parent / f'{game.name}-patch',
                      game.parent / f'{game.name}_UPDATE', game.parent / 'patch'):
        if candidate.is_dir() and is_update(candidate):
            return str(candidate)
    return ''


def guess_game(update_dir):
    """The base game next to an update folder that was picked by mistake."""
    update = Path(update_dir)
    for suffix in ('-UPDATE', '-patch', '_UPDATE'):
        if update.name.endswith(suffix):
            base = update.parent / update.name[:-len(suffix)]
            if base.is_dir() and not is_update(base):
                return str(base)
    return ''


def available(settings, executable):
    return bool(settings['game_dir']) and (Path(settings['game_dir']) / executable).is_file()


def ensure_ini(updates=None):
    """ucndc.ini with the tested values, plus what the Options window changed."""
    import graphics_options
    adopt_old_ini()
    if not INI_FILE.is_file():
        graphics_options.write_keys(INI_FILE, INI_DEFAULTS)
    if updates:
        graphics_options.write_keys(INI_FILE, updates)


def adopt_old_ini():
    """Keep the settings of an older install: copy gow3.ini to ucndc.ini once (run.py does the same)."""
    if not INI_FILE.is_file() and OLD_INI_FILE.is_file():
        try:
            shutil.copyfile(OLD_INI_FILE, INI_FILE)
        except OSError:
            pass


def read_ini():
    adopt_old_ini()
    try:
        import graphics_options
        return {**INI_DEFAULTS, **graphics_options.read_ini(INI_FILE)}
    except ImportError:
        return dict(INI_DEFAULTS)


def display_mode():
    try:
        import graphics_options
        mode = graphics_options.read_ini(INI_FILE).get('display_mode', 'windowed')
    except ImportError:
        mode = 'windowed'
    return mode if mode in {m for m, _n in DISPLAY_MODES} else 'windowed'


def public_name(name):
    """The documented name of a toggle: UC_X (the port still accepts the older GOW3_X)."""
    return 'UC_' + name[len('GOW3_'):] if name.startswith('GOW3_') else name


def write_input_ini(controls):
    """input.ini: the keyboard, mouse and controller bindings the game reads at start."""
    keys = {**DEFAULT_KEYS, **(controls.get('keys') or {})}
    pad = {**DEFAULT_PAD, **(controls.get('pad') or {})}
    lines = ['# Uncharted controls (written by the launcher, Controls window)',
             f"mouse_look={1 if controls.get('mouse_look') else 0}",
             f"mouse_sensitivity={float(controls.get('mouse_sensitivity') or 1.0):.2f}",
             f"mouse_aim_sensitivity={float(controls.get('mouse_aim_sensitivity') or 1.0):.2f}",
             f"mouse_invert_y={1 if controls.get('mouse_invert_y') else 0}",
             f"pad_invert_y={1 if controls.get('pad_invert_y') else 0}"]
    lines += [f'key.{action}={keys[action]}' for action in DEFAULT_KEYS]
    lines += [f'pad.{action}={pad[action]}' for action in DEFAULT_PAD]
    INPUT_FILE.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    controls['written'] = dict(line.split('=', 1) for line in lines[1:])
    return INPUT_FILE


def adopt_menu_controls(settings):
    """The port menu (Insert or F1 > Controls) changes input.ini while the game runs; the lines that differ
    from what the launcher wrote last go into launcher.json. True when something changed."""
    controls = settings.get('controls')
    written = (controls or {}).get('written')
    if not written:
        return False
    try:
        text = INPUT_FILE.read_text(encoding='utf-8')
    except OSError:
        return False
    changed = False
    for line in text.splitlines():
        key, sep, value = line.partition('=')
        key, value = key.strip(), value.strip()
        if not sep or key.startswith('#') or written.get(key) == value:
            continue
        if key.startswith('key.') and key[4:] in DEFAULT_KEYS:
            controls.setdefault('keys', {})[key[4:]] = value
        elif key.startswith('pad.') and key[4:] in DEFAULT_PAD:
            controls.setdefault('pad', {})[key[4:]] = value
        elif key in ('mouse_look', 'mouse_invert_y', 'pad_invert_y'):
            controls[key] = value == '1'
        elif key in ('mouse_sensitivity', 'mouse_aim_sensitivity'):
            try:
                controls[key] = round(float(value), 2)
            except ValueError:
                continue
        else:
            continue
        written[key] = value
        changed = True
    return changed


def launcher_option(name):
    """True for a GOW3_/UC_ name this launcher sets or turns off (UC_X is the same option as GOW3_X)."""
    if name.startswith('UC_'):
        name = 'GOW3_' + name[3:]
    elif not name.startswith('GOW3_'):
        return False
    managed = set(COMMON) | set(EXPERIMENTAL) | set(TOGGLE_HELP)
    for game in GAMES:
        managed |= set(game[4])
    for values in UC3_RESOLUTION_ENV.values():
        managed |= set(values)
    managed |= {'GOW3_GAME_DIR', 'GOW3_PATCH_DIR', 'GOW3_EXECUTABLE', 'GOW3_DATA_DIR', 'GOW3_USER_DIR',
                'GOW3_INPUT_CONFIG', 'GOW3_UC3_RT_MB', 'GOW3_ICON_ROOT', 'GOW3_ICON_FILES', 'GOW3_ICON_ERROR'}
    return name in managed


def key_icon_files(settings, key):
    """The game files redrawn with the player's keys or as Xbox buttons (Controls > icons in
    the game), kept in user\\icones\\ under their path in the game folder and made again only when the
    choice or the keys change. Returns the files made (paths in the game folder) and the errors; a file
    that cannot be made stays the game's own."""
    controls = settings.get('controls') or {}
    style = icon_style(controls)
    if style == 'ps' or key not in ICON_FILES:
        return [], []
    font = next((f for f in ICON_FONTS if f.is_file()), None)
    if not font:
        return [], ['no font for the key names']
    keys = {**DEFAULT_KEYS, **(controls.get('keys') or {})}
    made, errors = [], []
    for name in ICON_FILES[key] + (ICON_TEXT_FILES.get(key, ()) if style == 'keys' else ()):
        sources = [Path(d) / name for d in (settings.get('update_dir'), settings.get('game_dir')) if d]
        source = next((p for p in sources if p.is_file()), None)
        if not source:
            errors.append(f'no {name}')
            continue
        stamp = hashlib.sha256(json.dumps([KEY_ICONS_VERSION, str(source), source.stat().st_size, str(font), LANG,
                                           style, keys], sort_keys=True).encode()).hexdigest()
        target = USER_DIR / 'icones' / name
        mark = target.with_name(target.name + '.txt')
        try:
            if not (target.is_file() and mark.is_file() and mark.read_text(encoding='utf-8') == stamp):
                if name.endswith('core.psarc'):
                    from key_icons import make_text_psarc as make
                elif name.endswith('.psarc'):
                    from key_icons import make_icon_psarc as make  # Pillow
                else:
                    from hud_icons import make_hud_pak as make
                target.parent.mkdir(parents=True, exist_ok=True)
                partial = target.with_name(target.name + '.part')
                make(source, partial, keys, str(font), english=LANG != 'pt', style=style)
                os.replace(partial, target)
                mark.write_text(stamp, encoding='utf-8')
            made.append(name)
        except Exception as error:  # Pillow missing, unexpected file: that one keeps the controller icons
            errors.append(f'{name}: {type(error).__name__}: {error}')
    return made, errors


def game_environment(settings, key):
    _k, _title, executable, _status, extra = next(g for g in GAMES if g[0] == key)
    env = dict(os.environ)
    for name in [n for n in env if launcher_option(n)]:
        del env[name]  # only what this launcher decides; other GOW3_/UC_ names (test .bat diagnostics) pass through
    env.update(COMMON)
    env.update(extra)
    for name in settings.get('disabled', []):
        if name not in FIXED:
            env.pop(name, None)  # turned off in Options > Advanced
    for name in settings.get('experimental', []):
        if name in EXPERIMENTAL and key in EXPERIMENTAL[name][0]:
            env[name] = EXPERIMENTAL[name][1]  # turned on in Options > Advanced
    if 'GOW3_SCALAR_COPY_HLE' not in env:
        env.pop('GOW3_HLE_INVALIDATE', None)
    if key in ('uc1', 'uc2', 'uc3'):
        env.update(UC3_RESOLUTION_ENV.get(settings.get('resolution', '1080'), {}))
    env['GOW3_GAME_DIR'] = settings['game_dir']
    if settings.get('update_dir'):
        env['GOW3_PATCH_DIR'] = settings['update_dir']
    env['GOW3_EXECUTABLE'] = executable
    env['GOW3_DATA_DIR'] = str(DATA_DIR)
    env['GOW3_USER_DIR'] = str(USER_DIR)
    if not settings.get('controls'):  # never saved: the Controls window's defaults (mouse look on)
        settings['controls'] = {k: (dict(v) if isinstance(v, dict) else v) for k, v in CONTROL_DEFAULTS.items()}
    try:
        adopt_menu_controls(settings)
        env['GOW3_INPUT_CONFIG'] = str(write_input_ini(settings['controls']))
        save_settings(settings)
    except OSError:
        pass
    made, errors = key_icon_files(settings, key)
    if made:
        env['GOW3_ICON_ROOT'] = str(USER_DIR / 'icones')
        env['GOW3_ICON_FILES'] = ';'.join(made)
    if errors:
        env['GOW3_ICON_ERROR'] = '; '.join(errors)  # printed by the game in the log
    for name, value in os.environ.items():
        if name.startswith('UC_TEST_'):
            env['GOW3_' + name[8:]] = value  # test .bat files: UC_TEST_X=v forces GOW3_X over the launcher's choice
    return env


def run_command():
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'run.py')]


# ---------------------------------------------------------------------------------------------
# Roles of the frozen executable

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited pipes or a console still exist."""
    if not WINDOWS:
        return
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(line_buffering=True)
        except (AttributeError, ValueError):
            pass
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT_DIR / 'run.py')
        sys.argv = [path, *argv[1:]]
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


class GameJob:
    """A Windows job holding run.py and everything it starts (ucndc.exe, in-game restarts),
    so closing the launcher's Stop button ends all of it."""

    def __init__(self, process):
        self.handle = None
        if not WINDOWS:
            return
        kernel32 = ctypes.windll.kernel32
        kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        self.handle = kernel32.CreateJobObjectW(None, None)
        if self.handle:
            kernel32.AssignProcessToJobObject(ctypes.c_void_p(self.handle), ctypes.c_void_p(int(process._handle)))

    def terminate(self):
        if self.handle:
            ctypes.windll.kernel32.TerminateJobObject(ctypes.c_void_p(self.handle), 1)

    def close(self):
        if self.handle:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None


def start_game(settings, key):
    USER_DIR.mkdir(parents=True, exist_ok=True)
    ensure_ini()
    return subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(settings, key),
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            creationflags=NO_WINDOW)


def play_without_window(settings, key):
    """--play uc3: a desktop shortcut starts the game directly; the log goes to user\\last_run.log."""
    attach_stdio()
    process = start_game(settings, key)
    with open(LOG_FILE, 'w', encoding='utf-8', buffering=1) as log:
        for raw in iter(process.stdout.readline, b''):
            log.write(raw.decode('utf-8', errors='replace'))
    return process.wait()


def make_shortcut(key, title):
    """A desktop shortcut that runs `<launcher> --play <key>` (through Windows Script Host)."""
    target = sys.executable if FROZEN else str(Path(sys.executable).with_name('pythonw.exe'))
    arguments = f'--play {key}' if FROZEN else f'"{Path(__file__).resolve()}" --play {key}'
    icon = str(PORT_DIR / 'launcher' / 'uc.ico') if (PORT_DIR / 'launcher' / 'uc.ico').is_file() else target
    script = ('$s=(New-Object -ComObject WScript.Shell);'
              '$d=[Environment]::GetFolderPath("Desktop");'
              f'$l=$s.CreateShortcut((Join-Path $d "{title}.lnk"));'
              f'$l.TargetPath="{target}";$l.Arguments="{arguments}";'
              f'$l.WorkingDirectory="{PORT_DIR}";$l.IconLocation="{icon}";$l.Save()')
    result = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
                            stdin=subprocess.DEVNULL, capture_output=True, creationflags=NO_WINDOW)
    return result.returncode == 0


def version_tuple(text):
    """'v0.3.1' / '0.3.1' / '0.3.5-beta' -> (0, 3, 1) / (0, 3, 5); None when it is not a version."""
    try:
        return tuple(int(n) for n in text.strip().lstrip('vV').split('-')[0].split('.'))
    except (AttributeError, ValueError):
        return None


def latest_release():
    """(tag, page URL) of the newest GitHub Release, or None (offline, rate limit, bad answer)."""
    request = urllib.request.Request(RELEASES_API, headers={'Accept': 'application/vnd.github+json',
                                                            'User-Agent': f'ucndc-launcher/{VERSION}'})
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            data = json.loads(response.read().decode('utf-8'))
        return data['tag_name'], data.get('html_url') or RELEASES_PAGE
    except Exception:  # noqa: BLE001 (any failure just means no notice)
        return None


def open_path(path):
    path = Path(path)
    path.mkdir(parents=True, exist_ok=True)
    if WINDOWS:
        os.startfile(str(path))  # noqa: S606 (opens Explorer / the default text editor)
    else:
        subprocess.Popen(['xdg-open', str(path)])


# ---------------------------------------------------------------------------------------------
# Window

class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.root, self.tk, self.ttk = root, tk, ttk
        self.filedialog, self.messagebox = filedialog, messagebox
        self.settings = load_settings()
        self.process = self.job = None
        self.output = queue.Queue()
        self.log_visible = False
        self.newer = None  # (tag, url) of a newer release, filled by check_updates()
        root.title(f'{APP_NAME} {VERSION}')
        root.configure(bg=BG)
        root.minsize(760, 560)
        icon = PORT_DIR / 'launcher' / 'uc.ico'
        if WINDOWS and icon.is_file():
            try:
                root.iconbitmap(str(icon))
            except tk.TclError:
                pass
        root.protocol('WM_DELETE_WINDOW', self.close)
        self.build()
        self.refresh()
        root.after(100, self.drain_output)
        if self.settings.get('check_updates', True):
            self.check_updates()

    def check_updates(self):
        """Ask GitHub for the latest release in the background; show a button when it is newer."""
        result = []

        def worker():
            result.append(latest_release())

        def poll(tries=0):
            if not result and tries < 100:
                self.root.after(100, poll, tries + 1)
                return
            found = result[0] if result else None
            mine = os.environ.get('UC_PRETEND_VERSION') or VERSION  # test the notice: UC_PRETEND_VERSION=0.3.0
            if found and (version_tuple(found[0]) or ()) > (version_tuple(mine) or ()):
                self.newer = found
                self.show_update()
        threading.Thread(target=worker, daemon=True).start()
        self.root.after(100, poll)

    def show_update(self):
        bar = getattr(self, 'update_bar', None)
        if not self.newer or bar is None:
            return
        tag, url = self.newer
        for child in bar.winfo_children():
            child.destroy()
        mine = os.environ.get('UC_PRETEND_VERSION') or VERSION
        self.label(bar, _(f'Saiu a versão {tag} do port (você tem a {mine}).',
                          f'Port version {tag} is out (you have {mine}).'), 10, TEXT).pack(side='left', padx=12, pady=6)
        self.button(bar, _('Baixar', 'Download'), lambda: webbrowser.open(url), primary=True).pack(side='right', padx=8, pady=4)
        bar.pack(fill='x', padx=24, pady=(0, 4), after=self.top_bar)

    # -- layout -----------------------------------------------------------------------------
    def label(self, parent, text, size=10, color=TEXT, bold=False, **kw):
        return self.tk.Label(parent, text=text, bg=parent['bg'], fg=color,
                             font=('Segoe UI', size, 'bold' if bold else 'normal'), **kw)

    def button(self, parent, text, command, primary=False, **kw):
        return self.tk.Button(parent, text=text, command=command, relief='flat', cursor='hand2',
                              bg=ACCENT if primary else CARD, fg='#1a1408' if primary else TEXT,
                              activebackground='#f0bf5a' if primary else '#3a3226',
                              activeforeground='#1a1408' if primary else TEXT,
                              font=('Segoe UI', 11 if primary else 10, 'bold' if primary else 'normal'),
                              padx=14, pady=6 if primary else 4, bd=0, **kw)

    def build(self):
        tk = self.tk
        for child in self.root.winfo_children():
            child.destroy()
        top = tk.Frame(self.root, bg=BG)
        top.pack(fill='x', padx=24, pady=(20, 6))
        self.label(top, 'UNCHARTED', 22, ACCENT, True).pack(side='left')
        self.label(top, '  The Nathan Drake Collection · PC', 13, MUTED).pack(side='left', pady=(8, 0))
        lang = self.button(top, 'English' if LANG == 'pt' else 'Português', self.toggle_language)
        lang.pack(side='right')
        self.top_bar = top
        self.update_bar = tk.Frame(self.root, bg=CARD)

        # Folders
        box = tk.Frame(self.root, bg=PANEL)
        box.pack(fill='x', padx=24, pady=8)
        inner = tk.Frame(box, bg=PANEL)
        inner.pack(fill='x', padx=16, pady=12)
        self.label(inner, _('1. Seu dump do PS4', '1. Your PS4 dump'), 11, TEXT, True).grid(row=0, column=0, sticky='w', columnspan=4)
        self.game_var = tk.StringVar(value=self.settings['game_dir'])
        self.update_var = tk.StringVar(value=self.settings['update_dir'])
        for row, (title, var, command, key) in enumerate((
                (_('Pasta do jogo', 'Game folder'), self.game_var, self.pick_game, 'game_dir'),
                (_('Atualização (01.02)', 'Update (01.02)'), self.update_var, self.pick_update, 'update_dir')), start=1):
            self.label(inner, title, 10, MUTED).grid(row=row, column=0, sticky='w', pady=3)
            entry = tk.Entry(inner, textvariable=var, bg=CARD, fg=TEXT, insertbackground=TEXT, relief='flat',
                             font=('Segoe UI', 10), state='readonly', readonlybackground=CARD)
            entry.grid(row=row, column=1, sticky='we', padx=10, ipady=4)
            browse = self.button(inner, _('Escolher…', 'Browse…'), command)
            browse.grid(row=row, column=2)
            clear = self.button(inner, '✕', lambda key=key, var=var: self.clear_folder(key, var))
            clear.grid(row=row, column=3, padx=(6, 0))
            if key == 'update_dir':
                self.update_buttons = (browse, clear)
        inner.columnconfigure(1, weight=1)
        self.dump_status = self.label(inner, '', 9, MUTED, justify='left', wraplength=680)
        self.dump_status.grid(row=3, column=0, columnspan=4, sticky='w', pady=(6, 0))

        # Games
        self.label(self.root, _('2. Escolha o jogo', '2. Pick a game'), 11, TEXT, True).pack(anchor='w', padx=24, pady=(10, 4))
        cards = tk.Frame(self.root, bg=BG)
        cards.pack(fill='x', padx=24)
        self.play_buttons = {}
        for column, (key, title, executable, status, _extra) in enumerate(GAMES):
            card = tk.Frame(cards, bg=CARD)
            card.grid(row=0, column=column, sticky='nsew', padx=(0 if column == 0 else 8, 0))
            cards.columnconfigure(column, weight=1, uniform='games')
            self.label(card, title, 11, TEXT, True, wraplength=220, justify='left').pack(anchor='w', padx=12, pady=(12, 2))
            self.label(card, status[0] if LANG == 'pt' else status[1], 9,
                       OK if key != 'uc1' else ACCENT).pack(anchor='w', padx=12)
            button = self.button(card, _('JOGAR', 'PLAY'), lambda k=key: self.play(k), primary=True)
            button.pack(fill='x', padx=12, pady=12)
            self.play_buttons[key] = button

        # Options and tools
        bar = tk.Frame(self.root, bg=BG)
        bar.pack(fill='x', padx=24, pady=(12, 4))
        for text, command in ((_('Opções', 'Options'), self.options),
                              (_('Controles', 'Controls'), self.controls),
                              (_('Pasta de saves', 'Saves folder'), lambda: open_path(USER_DIR)),
                              (_('Log', 'Log'), self.toggle_log),
                              (_('Atalho na área de trabalho', 'Desktop shortcut'), self.shortcut)):
            self.button(bar, text, command).pack(side='left', padx=(0, 6))
        self.stop_button = self.button(bar, _('Parar o jogo', 'Stop the game'), self.stop)
        self.stop_button.pack(side='right')

        self.status = self.label(self.root, '', 10, MUTED, justify='left', wraplength=700)
        self.status.pack(anchor='w', padx=24, pady=(8, 4))
        self.log_frame = tk.Frame(self.root, bg=BG)
        self.log = tk.Text(self.log_frame, bg='#0d0c0a', fg='#cfc6b4', font=('Consolas', 9), relief='flat',
                           height=12, state='disabled', wrap='none')
        self.log.pack(fill='both', expand=True)
        if self.log_visible:
            self.log_frame.pack(fill='both', expand=True, padx=24, pady=(0, 16))
        footer = self.label(self.root, _('Port não oficial, sem relação com a Sony ou a Naughty Dog. Nenhum arquivo do jogo vem neste pacote: use o dump do seu próprio PS4.',
                                         'Unofficial port, not affiliated with Sony or Naughty Dog. No game files are included: use a dump of your own PS4.'), 8, MUTED)
        footer.pack(side='bottom', anchor='w', padx=24, pady=(0, 10))
        self.show_update()

    # -- state ------------------------------------------------------------------------------
    def refresh(self):
        s = self.settings
        lines, ok = [], bool(s['game_dir'])
        # the update is already in the game folder: nothing to pick
        merged = bool(s['game_dir']) and has_update_merged(s['game_dir'])
        if merged and s['update_dir']:
            s['update_dir'] = ''
            save_settings(s)
        self.update_var.set(_('Já incluída na pasta do jogo', 'Already in the game folder') if merged else s['update_dir'])
        for button in getattr(self, 'update_buttons', ()):
            button.configure(state='disabled' if merged else 'normal', cursor='arrow' if merged else 'hand2')
        if not s['game_dir']:
            lines.append(_('Escolha a pasta do jogo (CUSA02344): a que tem eboot.bin, big2-ps4_Shipping.elf e big3-ps4_Shipping.elf.',
                           'Choose the game folder (CUSA02344): the one with eboot.bin, big2-ps4_Shipping.elf and big3-ps4_Shipping.elf.'))
        else:
            found = [title for key, title, exe, _s, _e in GAMES if available(s, exe)]
            if not found:
                ok = False
                lines.append(_('Essa pasta não tem os executáveis da coletânea.', 'This folder has none of the collection executables.'))
            tid = title_id(s['game_dir'])
            if tid and tid != TESTED_TITLE:
                lines.append(_(f'Versão {tid}: só a {TESTED_TITLE} foi testada; pode não funcionar.',
                               f'Version {tid}: only {TESTED_TITLE} was tested; it may not work.'))
            merged = has_update_merged(s['game_dir'])
            if merged and not s['update_dir']:
                lines.append(_(f'Atualização {app_version(s["game_dir"])} já incluída na pasta do jogo. Tudo certo. Escolha um jogo.',
                               f'Update {app_version(s["game_dir"])} already merged into the game folder. All set. Pick a game.'))
            elif not s['update_dir']:
                lines.append(_('Sem a atualização 01.02: o jogo roda, mas os testes foram feitos com ela.',
                               'Without the 01.02 update: the game runs, but it was tested with it.'))
        updated = bool(s['update_dir']) or has_update_merged(s['game_dir'])
        if ok and not lines:
            lines.append(_('Tudo certo. Escolha um jogo.', 'All set. Pick a game.'))
        self.dump_status.configure(text='\n'.join(lines), fg=OK if ok and len(lines) == 1 and updated else MUTED)
        running = self.process is not None
        for key, _t, exe, _s, _e in GAMES:
            self.play_buttons[key].configure(state='normal' if available(s, exe) and not running else 'disabled')
        self.stop_button.configure(state='normal' if running else 'disabled')

    def toggle_language(self):
        global LANG
        LANG = 'en' if LANG == 'pt' else 'pt'
        self.settings['language'] = LANG
        save_settings(self.settings)
        self.build()
        self.refresh()

    def pick_game(self):
        folder = self.filedialog.askdirectory(title=_('Pasta do jogo (CUSA02344)', 'Game folder (CUSA02344)'))
        if not folder:
            return
        folder = inner_dump(folder)
        base = guess_game(folder) if is_update(folder) else ''
        if is_update(folder) and (base or not has_games(folder) or not has_base_data(folder)):
            # the update was picked: use it as such and look for the game
            self.settings['update_dir'] = folder
            folder = base
            if not folder:
                self.messagebox.showinfo(APP_NAME, _('Essa é a pasta da atualização. Agora escolha a pasta do jogo base.',
                                                     'That is the update folder. Now choose the base game folder.'))
        if folder:
            self.settings['game_dir'] = folder
            if is_update(folder) or has_update_merged(folder):
                self.settings['update_dir'] = ''  # merged dump: the update is already inside
            elif not self.settings['update_dir']:
                self.settings['update_dir'] = guess_update(folder)
        self.game_var.set(self.settings['game_dir'])
        self.update_var.set(self.settings['update_dir'])
        save_settings(self.settings)
        self.refresh()

    def clear_folder(self, key, var):
        self.settings[key] = ''
        var.set('')
        save_settings(self.settings)
        self.refresh()

    def pick_update(self):
        folder = self.filedialog.askdirectory(title=_('Pasta da atualização 01.02', 'Update 01.02 folder'))
        if not folder:
            return
        if not is_update(folder):
            self.messagebox.showwarning(APP_NAME, _('Essa pasta não parece uma atualização (falta sce_discmap_patch.plt).',
                                                    'This folder does not look like an update (no sce_discmap_patch.plt).'))
            return
        self.settings['update_dir'] = folder
        self.update_var.set(folder)
        save_settings(self.settings)
        self.refresh()

    def options(self):
        """Options window: display, VSync, FPS limit and overlay (ucndc.ini); Advanced: the
        optimizations each game starts with (launcher.json 'disabled')."""
        tk, ttk = self.tk, self.ttk
        win = tk.Toplevel(self.root, bg=BG)
        win.title(_('Opções', 'Options'))
        win.transient(self.root)
        win.resizable(False, True)
        ini = read_ini()
        body = tk.Frame(win, bg=PANEL)
        body.pack(fill='both', expand=True, padx=16, pady=16)
        pick = lambda pairs: [n[0] if LANG == 'pt' else n[1] for _v, n in pairs]
        self.label(body, _('Imagem e desempenho', 'Display and performance'), 11, TEXT, True).grid(row=0, column=0, columnspan=2, sticky='w', pady=(0, 6))
        mode = tk.StringVar(value=pick(DISPLAY_MODES)[[m for m, _n in DISPLAY_MODES].index(display_mode())])
        limit_values = [v for v, _n in FPS_LIMITS]
        limit = tk.StringVar(value=pick(FPS_LIMITS)[limit_values.index(ini.get('fps_limit', '0'))
                                                    if ini.get('fps_limit', '0') in limit_values else 0])
        res_values = [v for v, _n in RESOLUTIONS]
        resolution = tk.StringVar(value=pick(RESOLUTIONS)[res_values.index(self.settings.get('resolution', '1080'))
                                                          if self.settings.get('resolution', '1080') in res_values else 0])
        vsync = tk.BooleanVar(value=ini.get('vsync', '1') != '0')
        fps = tk.BooleanVar(value=ini.get('overlay_fps', '1') != '0' and ini.get('show_fps', '1') != '0')
        rows = ((_('Tela', 'Display'), ttk.Combobox(body, values=pick(DISPLAY_MODES), state='readonly', width=18, textvariable=mode)),
                (_('Limite de FPS', 'FPS limit'), ttk.Combobox(body, values=pick(FPS_LIMITS), state='readonly', width=18, textvariable=limit)),
                (_('Resolução', 'Resolution'), ttk.Combobox(body, values=pick(RESOLUTIONS), state='readonly', width=18, textvariable=resolution)))
        for row, (title, widget) in enumerate(rows, start=1):
            self.label(body, title, 10, MUTED).grid(row=row, column=0, sticky='w', pady=3)
            widget.grid(row=row, column=1, sticky='w', padx=10)
        check = dict(bg=PANEL, fg=TEXT, selectcolor=CARD, activebackground=PANEL, activeforeground=TEXT,
                     font=('Segoe UI', 10), anchor='w')
        tk.Checkbutton(body, text=_('VSync (sem tearing; recomendado)', 'VSync (no tearing; recommended)'),
                       variable=vsync, **check).grid(row=4, column=0, columnspan=2, sticky='w')
        tk.Checkbutton(body, text=_('Mostrar FPS na tela', 'Show FPS on screen'), variable=fps, **check).grid(row=5, column=0, columnspan=2, sticky='w')
        updates = tk.BooleanVar(value=self.settings.get('check_updates', True))
        tk.Checkbutton(body, text=_('Avisar quando sair versão nova do port (consulta o GitHub ao abrir)',
                                    'Tell me when a new port version is out (asks GitHub at start)'),
                       variable=updates, **check).grid(row=6, column=0, columnspan=2, sticky='w')
        self.label(body, _('Os jogos rodam a no máximo 60 FPS. No jogo, Insert ou F1 (ou L2+R2+L3+R3) abre o menu do port. '
                           'Resolução acima de 1080p pesa mais na placa de vídeo e usa mais memória.',
                           'The games run at 60 FPS at most. In game, Insert or F1 (or L2+R2+L3+R3) opens the port menu. '
                           'Resolutions above 1080p load the graphics card more and use more memory.'),
                   9, MUTED, wraplength=440, justify='left').grid(row=7, column=0, columnspan=2, sticky='w', pady=(4, 10))

        advanced = tk.Frame(body, bg=PANEL)
        perf_list = tk.Frame(advanced, bg=PANEL)
        fix_list = tk.Frame(advanced, bg=PANEL)
        perf_list.grid(row=0, column=0, sticky='nw', padx=(0, 16))
        fix_list.grid(row=0, column=1, sticky='nw')
        disabled = set(self.settings.get('disabled', []))
        toggles = {}
        names = list(COMMON) + [k for g in GAMES for k in g[4] if k not in COMMON]
        self.label(perf_list, _('Desempenho (todas ligadas = configuração testada)',
                                'Performance (all on = tested setup)'), 10, ACCENT, True).pack(anchor='w', pady=(0, 4))
        self.label(fix_list, _('Correções', 'Fixes'), 10, ACCENT, True).pack(anchor='w', pady=(0, 4))

        def toggle(parent, name, var):
            help_text = TOGGLE_HELP.get(name, ('', ''))[0 if LANG == 'pt' else 1]
            tk.Checkbutton(parent, text=f'{help_text}  ({public_name(name)})', variable=var, **check).pack(anchor='w')
        for name in dict.fromkeys(names):
            if name in FIXED:
                continue
            toggles[name] = tk.BooleanVar(value=name not in disabled)
            toggle(fix_list if name in FIX_TOGGLES else perf_list, name, toggles[name])
        chosen = set(self.settings.get('experimental', []))
        trials = {}
        if EXPERIMENTAL:
            self.label(fix_list, _('Experimental', 'Experimental'), 10, ACCENT, True).pack(anchor='w', pady=(10, 4))
        for name in EXPERIMENTAL:
            trials[name] = tk.BooleanVar(value=name in chosen)
            toggle(fix_list, name, trials[name])
        # Open when it was left open; older launcher.json files: open when something differs from the default.
        shown = tk.BooleanVar(value=self.settings.get('show_advanced', bool(disabled) or bool(chosen)))

        def show_advanced():
            if shown.get():
                advanced.grid(row=9, column=0, columnspan=2, sticky='w', pady=(6, 0))
            else:
                advanced.grid_forget()
        tk.Checkbutton(body, text=_('Avançado: escolher otimizações', 'Advanced: choose optimizations'),
                       variable=shown, command=show_advanced, **check).grid(row=8, column=0, columnspan=2, sticky='w')
        show_advanced()

        def reset():
            for var in toggles.values():
                var.set(True)
            for var in trials.values():
                var.set(False)
            vsync.set(True)
            fps.set(True)
            updates.set(True)
            limit.set(pick(FPS_LIMITS)[0])
            resolution.set(pick(RESOLUTIONS)[0])

        def save():
            ensure_ini({'display_mode': DISPLAY_MODES[pick(DISPLAY_MODES).index(mode.get())][0],
                        'vsync': '1' if vsync.get() else '0',
                        'fps_limit': FPS_LIMITS[pick(FPS_LIMITS).index(limit.get())][0],
                        'show_fps': '1' if fps.get() else '0', 'overlay_fps': '1' if fps.get() else '0'})
            self.settings['disabled'] = sorted(k for k, v in toggles.items() if not v.get())
            self.settings['experimental'] = sorted(k for k, v in trials.items() if v.get())
            self.settings['check_updates'] = updates.get()
            self.settings['resolution'] = RESOLUTIONS[pick(RESOLUTIONS).index(resolution.get())][0]
            self.settings['show_advanced'] = shown.get()
            save_settings(self.settings)
            win.destroy()
        buttons = tk.Frame(win, bg=BG)
        buttons.pack(fill='x', padx=16, pady=(0, 16))
        self.button(buttons, _('Salvar', 'Save'), save, primary=True).pack(side='right')
        self.button(buttons, _('Cancelar', 'Cancel'), win.destroy).pack(side='right', padx=6)
        self.button(buttons, _('Restaurar padrão', 'Restore defaults'), reset).pack(side='left')
        win.grab_set()

    def controls(self):
        """Controls window: mouse look, keyboard and mouse keys, controller buttons (launcher.json
        'controls', written to input.ini when a game starts; until it is saved a game starts with
        CONTROL_DEFAULTS, mouse look on). Ported from SuperMedo's launcher (uncharted_pc, GPL)."""
        tk, ttk = self.tk, self.ttk
        win = tk.Toplevel(self.root, bg=BG)
        win.title(_('Controles', 'Controls'))
        win.transient(self.root)
        win.geometry('660x640')
        win.minsize(560, 400)
        if adopt_menu_controls(self.settings):
            save_settings(self.settings)
        saved = self.settings.get('controls')
        self.ctl = json.loads(json.dumps({**CONTROL_DEFAULTS, **(saved or {})}))
        self.ctl_win, self.capturing, self.capture_armed, self.ignore_click = win, None, False, 0.0
        self.key_buttons = {}

        outer = tk.Frame(win, bg=PANEL)
        outer.pack(fill='both', expand=True, padx=16, pady=(16, 8))
        canvas = tk.Canvas(outer, bg=PANEL, highlightthickness=0)
        scroll = ttk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        canvas.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)
        body = tk.Frame(canvas, bg=PANEL)
        canvas.create_window((12, 12), window=body, anchor='nw')
        body.bind('<Configure>', lambda _e: canvas.configure(scrollregion=(0, 0, body.winfo_reqwidth() + 24,
                                                                               body.winfo_reqheight() + 24)))

        def wheel(event):
            if self.capturing:
                return self.captured_wheel(event)
            canvas.yview_scroll(-1 if event.delta > 0 else 1, 'units')
            return 'break'
        win.bind('<MouseWheel>', wheel)
        check = dict(bg=PANEL, fg=TEXT, selectcolor=CARD, activebackground=PANEL, activeforeground=TEXT,
                     font=('Segoe UI', 10), anchor='w')
        row = 0

        def put(widget, top=0, **kw):
            nonlocal row
            widget.grid(row=row, column=0, columnspan=2, sticky='w', pady=(top, 0), **kw)
            row += 1

        put(self.label(body, _('Mouse', 'Mouse'), 11, TEXT, True))
        look = tk.BooleanVar(value=bool(self.ctl['mouse_look']))
        put(tk.Checkbutton(body, text=_('Olhar com o mouse', 'Look around with the mouse'), variable=look, **check), 4)
        put(self.label(body, _('O cursor fica preso na janela do jogo; segure Alt esquerdo (ou troque de janela) '
                               'para soltá-lo. Insert ou F1 abre o menu do port.',
                               'The cursor stays in the game window; hold Left Alt (or switch windows) to free '
                               'it. Insert or F1 opens the port menu.'), 9, MUTED, wraplength=560, justify='left'))
        line = tk.Frame(body, bg=PANEL)
        self.label(line, _('Sensibilidade', 'Sensitivity'), 10, MUTED, width=16, anchor='w').pack(side='left', padx=(0, 10))
        sensitivity = tk.DoubleVar(value=float(self.ctl['mouse_sensitivity'] or 1.0))
        tk.Scale(line, from_=0.1, to=5.0, resolution=0.05, orient='horizontal', length=300,
                 variable=sensitivity, bg=PANEL, fg=TEXT, troughcolor=CARD, activebackground=ACCENT,
                 highlightthickness=0, bd=0, font=('Segoe UI', 9)).pack(side='left')
        put(line, 6)
        line = tk.Frame(body, bg=PANEL)
        self.label(line, _('Na mira (L2) ×', 'While aiming (L2) ×'), 10, MUTED, width=16, anchor='w').pack(side='left', padx=(0, 10))
        aim_sensitivity = tk.DoubleVar(value=float(self.ctl.get('mouse_aim_sensitivity') or 1.0))
        tk.Scale(line, from_=0.1, to=3.0, resolution=0.05, orient='horizontal', length=300,
                 variable=aim_sensitivity, bg=PANEL, fg=TEXT, troughcolor=CARD, activebackground=ACCENT,
                 highlightthickness=0, bd=0, font=('Segoe UI', 9)).pack(side='left')
        put(line)
        put(self.label(body, _('Multiplica a sensibilidade enquanto mira; 1 = igual à normal.',
                               'Multiplies the sensitivity while aiming; 1 = same as normal.'), 9, MUTED))
        invert = tk.BooleanVar(value=bool(self.ctl['mouse_invert_y']))
        put(tk.Checkbutton(body, text=_('Inverter o mouse na vertical', 'Invert the mouse vertically'),
                           variable=invert, **check))
        icons = tk.StringVar(value=icon_style(self.ctl))
        put(self.label(body, _('Ícones no jogo', 'Icons in the game'), 10, TEXT), 6)
        line = tk.Frame(body, bg=PANEL)
        for value, text in (('ps', _('Controle PlayStation', 'PlayStation controller')),
                            ('xbox', _('Controle Xbox', 'Xbox controller')),
                            ('keys', _('Teclado e mouse', 'Keyboard and mouse'))):
            tk.Radiobutton(line, text=text, value=value, variable=icons, **check).pack(side='left', padx=(0, 12))
        put(line)
        put(self.label(body, _('Os botões dos textos e dos avisos na tela (como "Pressione △" e o △ sobre objetos). '
                               'Xbox usa os botões do jogo com A, B, X, Y, LB, RT...; Teclado e mouse mostra as suas '
                               'teclas. Vale ao abrir o jogo: depois de trocar, feche o jogo e abra de novo.',
                               'The buttons in texts and on-screen prompts (such as "Press △" and the △ over objects). '
                               "Xbox uses the game's buttons with A, B, X, Y, LB, RT...; Keyboard and mouse shows your keys. "
                               'It applies when the game starts: after changing it, close the game and start it again.'),
                       9, MUTED, wraplength=560, justify='left'))

        put(self.label(body, _('Teclado e botões do mouse', 'Keyboard and mouse buttons'), 11, TEXT, True), 16)
        put(self.label(body, _('Clique numa caixa e aperte uma tecla ou um botão do mouse (a rodinha também vale). '
                               '✕ apaga. Cada ação pode ter duas.',
                               'Click a box, then press a key or a mouse button (the wheel counts too). '
                               '✕ removes a binding. Each action can have two.'), 9, MUTED, wraplength=560,
                       justify='left'), 2)
        table = tk.Frame(body, bg=PANEL)
        rows = [(a, t) for a, t, _k, _p in BUTTON_ACTIONS] + [(a, t) for a, t, _k in STICK_ACTIONS]
        for r, (action, title) in enumerate(rows):
            self.label(table, _(*title), 10, TEXT).grid(row=r, column=0, sticky='w', padx=(0, 18), pady=2)
            for slot in (0, 1):
                button = tk.Button(table, width=14, relief='flat', bg=CARD, fg=TEXT, activebackground='#3a3226',
                                   activeforeground=TEXT, font=('Segoe UI', 10), cursor='hand2', bd=0, padx=6,
                                   pady=3, takefocus=0, command=lambda a=action, i=slot: self.capture_key(a, i))
                button.grid(row=r, column=1 + slot * 2, sticky='w', pady=2)
                clear = tk.Label(table, text='✕', bg=PANEL, fg=MUTED, font=('Segoe UI', 10), cursor='hand2', padx=6)
                clear.grid(row=r, column=2 + slot * 2, sticky='w', padx=(2, 8))
                clear.bind('<Button-1>', lambda _e, a=action, i=slot: self.set_key(a, i, ''))
                self.key_buttons[(action, slot)] = button
        put(table, 8)
        self.show_keys()
        put(self.button(body, _('Teclas padrão', 'Default keys'), self.reset_keys), 8)

        put(self.label(body, _('Controle', 'Controller'), 11, TEXT, True), 16)
        put(self.label(body, _('Que botão do controle faz cada botão do PS4 (nomes de Xbox / PlayStation).',
                               'Which controller button gives each PS4 button (Xbox / PlayStation names).'),
                       9, MUTED, wraplength=560, justify='left'), 2)
        pads = tk.Frame(body, bg=PANEL)
        values = [v for v, _t in PAD_BUTTONS]
        names = [_(*t) for _v, t in PAD_BUTTONS]
        pad_boxes = {}
        for r, (action, title, _k, default) in enumerate(BUTTON_ACTIONS):
            self.label(pads, _(*title), 10, TEXT).grid(row=r, column=0, sticky='w', padx=(0, 18), pady=2)
            box = ttk.Combobox(pads, values=names, state='readonly', width=30)
            value = self.ctl['pad'].get(action, default)
            box.current(values.index(value) if value in values else values.index('none'))
            box.bind('<MouseWheel>', wheel)  # scroll the window, not the choice
            box.grid(row=r, column=1, sticky='w', pady=2)
            pad_boxes[action] = box
        put(pads, 8)
        pad_invert = tk.BooleanVar(value=bool(self.ctl['pad_invert_y']))
        put(tk.Checkbutton(body, text=_('Inverter o analógico direito na vertical', 'Invert the right stick vertically'),
                           variable=pad_invert, **check), 6)

        def reset_pad():
            for action, box in pad_boxes.items():
                box.current(values.index(DEFAULT_PAD[action]))
        put(self.button(body, _('Botões padrão', 'Default buttons'), reset_pad), 8)

        self.ctl_note = self.label(win, '', 9, ACCENT, wraplength=620, justify='left')
        self.ctl_note.pack(anchor='w', padx=16)

        def close():
            self.release_capture()
            self.capturing = None
            win.destroy()

        def save():
            self.ctl.update(mouse_look=look.get(), mouse_sensitivity=round(sensitivity.get(), 2),
                            mouse_aim_sensitivity=round(aim_sensitivity.get(), 2),
                            mouse_invert_y=invert.get(), pad_invert_y=pad_invert.get(), icons=icons.get(),
                            key_icons=icons.get() == 'keys')
            self.ctl['keys'] = {a: v for a, v in self.ctl['keys'].items() if v != DEFAULT_KEYS[a]}
            self.ctl['pad'] = {a: values[b.current()] for a, b in pad_boxes.items()
                               if values[b.current()] != DEFAULT_PAD[a]}
            self.settings['controls'] = self.ctl
            save_settings(self.settings)
            close()

        def built_in():
            self.settings.pop('controls', None)
            save_settings(self.settings)
            close()
        buttons = tk.Frame(win, bg=BG)
        buttons.pack(fill='x', padx=16, pady=(4, 16))
        self.button(buttons, _('Salvar', 'Save'), save, primary=True).pack(side='right')
        self.button(buttons, _('Cancelar', 'Cancel'), close).pack(side='right', padx=6)
        if saved:
            self.button(buttons, _('Usar os controles originais do port', "Use the port's built-in controls"),
                        built_in).pack(side='left')
        win.protocol('WM_DELETE_WINDOW', close)
        win.grab_set()

    def key_list(self, action):
        value = self.ctl['keys'].get(action, DEFAULT_KEYS[action])
        return [p.strip() for p in (value.split('|') + ['', ''])[:2]]

    def show_keys(self):
        for (action, slot), button in self.key_buttons.items():
            if self.capturing == (action, slot):
                button.configure(text=_('Aperte uma tecla…', 'Press a key…'), bg=ACCENT, fg='#1a1408')
            else:
                name = self.key_list(action)[slot]
                button.configure(text=name or '—', bg=CARD, fg=TEXT if name else MUTED)

    def set_key(self, action, slot, name):
        keys = self.key_list(action)
        keys[slot] = name
        if name and keys[1 - slot] == name:
            keys[1 - slot] = ''
        bindings = self.ctl['keys']
        if name:  # one binding per key: taken from the action that had it
            for other in DEFAULT_KEYS:
                others = self.key_list(other)
                if other != action and name in others:
                    bindings[other] = '|'.join(k for k in others if k and k != name)
        bindings[action] = '|'.join(k for k in keys if k)
        self.capturing = None
        self.release_capture()
        self.show_keys()

    def capture_key(self, action, slot):
        if time.monotonic() < self.ignore_click:
            return  # the click that was just taken as a binding
        self.release_capture()
        self.capturing = (action, slot)
        self.ctl_note.configure(text='')
        self.show_keys()
        self.ctl_win.focus_set()  # keys go to the window, not to a button or a list
        # The click that opened the capture must not be taken as the binding.
        self.root.after(200, self.arm_capture)

    def arm_capture(self):
        if not self.capturing or not self.ctl_win.winfo_exists():
            return
        self.capture_armed = True
        self.ctl_win.bind('<KeyPress>', self.captured_key)
        self.ctl_win.bind('<ButtonPress>', self.captured_button)

    def release_capture(self):
        self.capture_armed = False
        if self.ctl_win.winfo_exists():
            for sequence in ('<KeyPress>', '<ButtonPress>'):
                self.ctl_win.unbind(sequence)

    def captured_key(self, event):
        if not self.capturing:
            return None
        sym = event.keysym
        if sym in RESERVED_KEYS:
            self.ctl_note.configure(text=_(*RESERVED_KEYS[sym]))
            return 'break'
        name = TK_KEYS.get(sym)
        if not name and len(sym) == 1 and sym.isalnum():
            name = sym.upper()
        if not name and sym.startswith('F') and sym[1:].isdigit():
            name = sym
        if not name and sym.startswith('KP_') and sym[3:].isdigit():
            name = f'Keypad {sym[3:]}'
        if name:
            self.set_key(*self.capturing, name)
        return 'break'

    def captured_button(self, event):
        if not self.capturing:
            return None
        widget = next((k for k, b in self.key_buttons.items() if b is event.widget), None)
        if event.num == 1 and widget and widget != self.capturing:
            return None  # another box: its own click moves the capture there
        name = {1: 'Mouse Left', 2: 'Mouse Middle', 3: 'Mouse Right', 4: 'Mouse 4', 5: 'Mouse 5'}.get(event.num)
        if name:
            self.set_key(*self.capturing, name)
            self.ignore_click = time.monotonic() + 0.5
        return 'break'

    def captured_wheel(self, event):
        if self.capture_armed:
            self.set_key(*self.capturing, 'Wheel Up' if event.delta > 0 else 'Wheel Down')
        return 'break'

    def reset_keys(self):
        self.ctl['keys'] = {}
        self.capturing = None
        self.release_capture()
        self.show_keys()

    def toggle_log(self):
        self.log_visible = not self.log_visible
        if self.log_visible:
            self.log_frame.pack(fill='both', expand=True, padx=24, pady=(0, 16))
        else:
            self.log_frame.pack_forget()

    def shortcut(self):
        key = self.settings.get('last_game', 'uc3')
        title = next(t for k, t, *_r in GAMES if k == key)
        if make_shortcut(key, title.split(':')[0] if key != 'uc1' else 'Uncharted'):
            self.messagebox.showinfo(APP_NAME, _(f'Atalho criado na área de trabalho para {title}.',
                                                 f'Desktop shortcut created for {title}.'))
        else:
            self.messagebox.showerror(APP_NAME, _('Não foi possível criar o atalho.', 'Could not create the shortcut.'))

    # -- game -------------------------------------------------------------------------------
    def play(self, key):
        if self.process:
            return
        self.settings['last_game'] = key
        save_settings(self.settings)
        self.set_log('')
        try:
            self.process = start_game(self.settings, key)
        except OSError as error:
            self.status.configure(text=_(f'Não foi possível iniciar: {error}', f'Could not start: {error}'), fg=BAD)
            return
        self.job = GameJob(self.process)
        self.log_file = open(LOG_FILE, 'w', encoding='utf-8', buffering=1)
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.status.configure(text=_('Preparando o jogo… ele abre na própria janela. Na primeira vez demora mais '
                                     'e pode engasgar enquanto os shaders são criados.',
                                     'Preparing the game… it opens in its own window. The first start takes longer '
                                     'and may stutter while shaders are built.'), fg=TEXT)
        self.refresh()

    def read_output(self, process):
        for raw in iter(process.stdout.readline, b''):
            self.output.put(raw.decode('utf-8', errors='replace'))
        self.output.put((process.wait(),))

    def drain_output(self):
        try:
            for _i in range(500):
                item = self.output.get_nowait()
                if isinstance(item, tuple):
                    self.finished(item[0])
                    continue
                self.log_file.write(item)
                if 'Entering original x86-64 code' in item:
                    self.status.configure(text=_('Jogo rodando. Insert ou F1 (ou L2+R2+L3+R3) abre o menu do port.',
                                                 'Game running. Insert or F1 (or L2+R2+L3+R3) opens the port menu.'), fg=OK)
                self.append(item)
        except queue.Empty:
            pass
        self.root.after(100, self.drain_output)

    def finished(self, code):
        self.process = None
        if self.job:
            self.job.close()
            self.job = None
        self.log_file.close()
        if code:
            self.status.configure(text=_(f'O jogo fechou com erro (código {code}). Veja o Log; '
                                         'ao relatar, envie user\\last_run.log.',
                                         f'The game closed with an error (code {code}). See the Log; '
                                         'when reporting, send user\\last_run.log.'), fg=BAD)
            if not self.log_visible:
                self.toggle_log()
        else:
            self.status.configure(text=_('O jogo fechou.', 'The game closed.'), fg=MUTED)
        self.refresh()

    def stop(self):
        if self.job:
            self.job.terminate()

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        lines = int(self.log.index('end-1c').split('.')[0])
        if lines > 4000:
            self.log.delete('1.0', f'{lines - 4000}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def set_log(self, text):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.insert('end', text)
        self.log.configure(state='disabled')

    def close(self):
        if self.process and not self.messagebox.askyesno(
                APP_NAME, _('O jogo ainda está aberto. Fechar o launcher também fecha o jogo. Continuar?',
                            'The game is still running. Closing the launcher also closes the game. Continue?')):
            return
        self.stop()
        self.root.destroy()


def main():
    global LANG
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    settings = load_settings()
    LANG = settings.get('language') or system_language()
    if args[:1] == ['--play'] and len(args) == 2 and settings['game_dir'] and args[1] in {g[0] for g in GAMES}:
        sys.exit(play_without_window(settings, args[1]))
    if WINDOWS:
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    Launcher(root, tk, ttk, filedialog, messagebox)
    root.mainloop()


if __name__ == '__main__':
    main()
