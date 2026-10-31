"""Approved Light/Slate and Dark/Obsidian appearance; no window creation on import."""
import os
from pathlib import Path
import sys
import tkinter as tk
from tkinter import ttk

THEMES = {'light': {'bg': '#e9edf1',
           'panel': '#f6f8fa',
           'ink': '#25323d',
           'muted': '#4e5d6b',
           'line': '#6b7d8d',
           'field': '#ffffff',
           'accent': '#345b79',
           'hover': '#29485f',
           'selected': '#dbe5ee',
           'error': '#922e2e',
           'disabled': '#607080',
           'accent_ink': '#ffffff',
           'header_bg': '#202e3b',
           'header_kicker': '#bdd1e4',
           'header_ink': '#f3f6f9',
           'header_muted': '#c3cfda'},
 'dark': {'bg': '#292929',
          'panel': '#363433',
          'ink': '#f3eee6',
          'muted': '#c9bfb4',
          'line': '#a99c90',
          'field': '#202020',
          'accent': '#d5a27d',
          'hover': '#e1b997',
          'selected': '#4d4037',
          'error': '#ffb2a1',
          'disabled': '#b8ada2',
          'accent_ink': '#281e16',
          'header_bg': '#1d1c1b',
          'header_kicker': '#d5a27d',
          'header_ink': '#f5eee7',
          'header_muted': '#ccc0b4'}}


def apply_theme(root, name):
    """Apply the named ttk appearance and return its semantic header palette."""
    p = THEMES[name]
    root.configure(bg=p['bg'])
    s = ttk.Style(root)
    if s.theme_use() != "clam":
        s.theme_use("clam")
    s.configure('.', font=('Segoe UI', 10), background=p['bg'], foreground=p['ink'], bordercolor=p['line'], lightcolor=p['line'], darkcolor=p['line'], focuscolor=p['accent'])
    for name, bg in [('TFrame', p['bg']), ('Panel.TFrame', p['panel']), ('Bar.TFrame', p['selected']), ('Selected.TFrame', p['selected'])]:
        s.configure(name, background=bg)
    for name, bg, fg, font in [
        ('TLabel', p['bg'], p['ink'], ('Segoe UI', 10)),
        ('Muted.TLabel', p['bg'], p['muted'], ('Segoe UI', 9)),
        ('Strong.TLabel', p['bg'], p['ink'], ('Segoe UI', 10, 'bold')),
        ('Section.TLabel', p['bg'], p['ink'], ('Segoe UI', 13, 'bold')),
        ('Result.TLabel', p['bg'], p['ink'], ('Segoe UI', 22, 'bold')),
        ('Bar.TLabel', p['selected'], p['ink'], ('Segoe UI', 9)),
        ('Selected.TLabel', p['selected'], p['ink'], ('Segoe UI', 9, 'bold')),
        ('Panel.TLabel', p['panel'], p['ink'], ('Segoe UI', 10)),
        ('PanelTitle.TLabel', p['panel'], p['ink'], ('Segoe UI', 13, 'bold')),
        ('PanelMuted.TLabel', p['panel'], p['muted'], ('Segoe UI', 9)),
        ('PanelStrong.TLabel', p['panel'], p['ink'], ('Segoe UI', 10, 'bold'))]:
        s.configure(name, background=bg, foreground=fg, font=font)
    s.configure('TEntry', fieldbackground=p['field'], foreground=p['ink'], padding=(7, 5), insertcolor=p['ink'])
    s.configure('TCombobox', fieldbackground=p['field'], background=p['panel'], foreground=p['ink'], padding=4, arrowcolor=p['ink'])
    s.map('TCombobox', fieldbackground=[('readonly', p['field'])], foreground=[('readonly', p['ink']), ('disabled', p['disabled'])])
    s.configure('TButton', background=p['panel'], foreground=p['ink'], padding=(10, 5), borderwidth=1)
    s.map('TButton', background=[('active', p['selected']), ('pressed', p['selected'])], foreground=[('disabled', p['disabled'])])
    accent_ink = p['accent_ink']
    s.configure('Accent.TButton', background=p['accent'], foreground=accent_ink, font=('Segoe UI', 10, 'bold'), padding=(16, 9))
    s.map('Accent.TButton', background=[('disabled', p['panel']), ('active', p['hover']), ('pressed', p['hover'])], foreground=[('disabled', p['disabled'])])
    s.configure('TCheckbutton', background=p['bg'], foreground=p['ink'], padding=2)
    s.configure('TRadiobutton', background=p['bg'], foreground=p['ink'], padding=2)
    for name in ('Panel.TCheckbutton', 'Panel.TRadiobutton'):
        s.configure(name, background=p['panel'], foreground=p['ink'], padding=2)
        s.map(name, background=[('active', p['panel'])], foreground=[('disabled', p['disabled'])])
    for name in ('TCheckbutton', 'TRadiobutton'):
        s.map(name, background=[('active', p['bg'])], foreground=[('disabled', p['disabled'])])
    s.configure('Horizontal.TProgressbar', background=p['accent'], troughcolor=p['panel'], borderwidth=0, thickness=5)
    root.option_add('*TCombobox*Listbox.background', p['field'])
    root.option_add('*TCombobox*Listbox.foreground', p['ink'])
    return p


def _branding_directory():
    if getattr(sys, "frozen", False):
        return Path(__file__).resolve().parent / "resources" / "branding"
    return Path(__file__).resolve().parents[2] / "tools" / "pack_helper" / "art"


def load_branding(root):
    """Load approved pixels; the caller retains both returned PhotoImages."""
    directory = _branding_directory()
    logo = tk.PhotoImage(master=root, file=str(directory / "logo.png")).zoom(2, 2)
    icon = tk.PhotoImage(master=root, file=str(directory / "computer.png"))
    root.iconphoto(True, icon)
    if os.name == "nt":
        root.iconbitmap(str(directory / "computer.ico"))
    return logo, icon
