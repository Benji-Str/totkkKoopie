# TotK Testbank

Eine Oberfläche, die `totk-pc-recomp` auf deinem Windows-PC einrichtet, baut und für den Koop-Test vorbereitet.
Sie führt die Schritte aus `docs/building.md` der Reihe nach aus und zeigt deren Ausgabe live an.

## Einfach: eine Datei

Für einen zweiten PC reicht `TotK-Koop-Setup.cmd` (gebaut mit `python make_installer.py`). Doppelklick, Basisspiel,
Update, `prod.keys` und einen Ordner wählen, **Installieren**. Sie packt die Testbank nach
`%LOCALAPPDATA%\TotK-Koop\Testbank` aus und erledigt alle Reiter der Reihe nach: Werkzeuge, Quellcode mit Koop-Patch,
Bauen, Einstellungen, Firewall, Verknüpfung „TotK Koop“ auf dem Desktop. Spieldateien und Schlüssel sind nicht darin.

## Automatische Updates

Die Verknüpfung „TotK Koop“ schaut bei jedem Start nach, ob es einen neuen Koop-Stand gibt (Zweig `koop` im
GitHub-Repo aus `koop-update.json`). Wenn ja, erscheint kurz das Fenster „Update wird installiert“: Es holt den neuen
Stand, baut nur das Geänderte neu (meist 1 bis 5 Minuten) und startet dann das Spiel. Ohne Internet startet einfach
die vorhandene Version. Schlägt ein Update fehl, startet die bisherige Version, und beim nächsten Start wird es erneut
versucht.

Veröffentlichen (nur am Entwickler-PC): `python publish_update.py "Was sich geändert hat"`. Das Skript kopiert den
Stand aus `totk-pc-recomp` nach `koop-publish`, prüft ihn mit `tools/check_content.py` auf Spieldaten, committet und
lädt ihn hoch. Einmalig vorher: `python publish_update.py --setup https://github.com/NAME/REPO`.

## Start

1. Den Ordner aus der ZIP-Datei irgendwohin entpacken, zum Beispiel `C:\TotK-Testbank`.
2. `TotK-Testbank.cmd` doppelklicken. Beim ersten Mal installiert sie `uv` (das Python-Werkzeug von Astral) und lädt
   Python 3.14. Danach öffnet sich das Fenster.

## Reiter

| Reiter | Was du tust |
|---|---|
| 1 · Werkzeuge | Auf **Alles Fehlende installieren** klicken. Installiert werden Git, uv, MSYS2 mit seinen Paketen, das .NET SDK 10 und die VS Build Tools, alles über winget. Manche Installer fragen nach Admin-Rechten. Danach **Neu prüfen**. |
| 2 · Repo | Einen Ordner mit rund **60 GB** freiem Platz wählen, dann **Klonen** und **Koop-Patch anwenden**. Der Patch liegt schon im Ordner. |
| 3 · Spieldateien | Basisspiel (XCI oder NSP), Update 1.4.3 (NSP) und `prod.keys` wählen, Version 1.4.3 lassen, dann **Speichern**. |
| 4 · Bauen | **Alles bauen**. Beim ersten Mal dauert das etwa 20 bis 60 Minuten. Fertige Schritte werden später übersprungen. Am Ende laufen die Selbsttests; wichtig ist die Zeile `coop: probe passed`. |
| 5 · Koop & Spielen | „Allein testen (loop)“ und „Testmodus“ wählen, dann **Spiel starten**. Erwartet: Der orange Marker sitzt auf dem gelben Kreuz in der Bildmitte. |

## Datenschutz

- Die Testbank speichert nur Pfade und Einstellungen in `testbank.json`.
- Die Spielpfade landen zusätzlich in `config.local.toml` des Repos; git ignoriert diese Datei.
- Den Inhalt von `prod.keys` liest die Testbank nie. Das tun die Werkzeuge des Repos selbst.
- Jeder Lauf schreibt ein Protokoll in den Unterordner `logs`.

## Wenn etwas schiefgeht

- **Schritt rot:** Die Ausgabe im unteren Fenster zeigt den Grund. Mit **Build-Logs öffnen** kommst du an die Logs der
  Werkzeuge.
- **Werkzeug nach der Installation noch „fehlt“:** Die Testbank schließen und neu starten, weil sich der PATH erst
  dann aktualisiert.
- **Kreuz nicht in der Mitte, oder „camera: none“:** Über **Spiel-Log öffnen** die Zeilen `coop: test` und
  `coop: scene camera exported` heraussuchen und schicken.

Selbsttest ohne Fenster: `uv run --no-project --python 3.14 testbank.py --selftest`
