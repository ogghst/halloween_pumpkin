#!/usr/bin/env python3
"""Lint della wiki: frontmatter, valori ammessi, link relativi, index/orfani, obsolescenza.

Uso:
    python3 tools/wiki_lint.py [--wiki-dir docs/wiki]

Output: righe "[ERRORE] percorso: messaggio" e "[AVVISO] percorso: messaggio",
con percorso relativo alla directory della wiki.
Exit code: 0 se nessun errore (avvisi consentiti), 1 se ci sono errori,
2 se la directory wiki non esiste.
"""

from __future__ import annotations

import argparse
import re
import sys
from datetime import date, datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WIKI_DEFAULT = REPO_ROOT / "docs" / "wiki"

# Directory il cui contenuto richiede frontmatter (sezione A del contratto).
TYPED_DIRS = {"hardware", "componenti", "decisioni", "misure", "procedure", "insidie"}
TIPI = {"hardware", "componente", "decisione", "misura", "procedura", "insidia"}
STATI = {"verificato", "assunto", "ipotesi", "obsoleto"}
CAMPI_OBBLIGATORI = ("titolo", "tipo", "stato", "aggiornato", "fonti", "correlate")
# Pagine top-level senza frontmatter richiesto.
SENZA_FRONTMATTER = {"index.md", "log.md", "glossario.md", "domande-aperte.md"}
# Pagine esentate dal controllo orfane.
SENZA_INDICE = {"index.md", "log.md"}
MAX_RIGHE = 80
GIORNI_SCADENZA = 60

_LINK = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
_BULLET = re.compile(r"^\s*-\s+(.+?)\s*$")
_CHIAVE = re.compile(r"^([A-Za-z_][-\w]*)\s*:\s*(.*)$")
_DATA = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def _pulisci(valore: str) -> str:
    """Togli spazi e virgolette di contorno a un valore scalare."""
    v = valore.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        v = v[1:-1].strip()
    return v


def _pieno(campi: dict, chiave: str) -> bool:
    """True se la chiave esiste con valore non vuoto (scalare o lista)."""
    v = campi.get(chiave)
    if isinstance(v, list):
        return len(v) > 0
    return bool(v)


def parse_frontmatter(testo: str):
    """Subset YAML: chiave: valore, liste inline [a, b] o bullet '- x'.

    Ritorna (campi, numero_righe_frontmatter) se il file inizia con '---' e lo
    chiude; (None, 0) se non c'è frontmatter; (None, -1) se non viene chiuso.
    """
    righe = testo.splitlines()
    if not righe or righe[0].strip() != "---":
        return None, 0
    campi: dict = {}
    corrente = None
    for i, riga in enumerate(righe[1:], start=2):
        if riga.strip() == "---":
            return campi, i
        m = _BULLET.match(riga)
        if m and corrente is not None:
            campi.setdefault(corrente, []).append(_pulisci(m.group(1)))
            continue
        m = _CHIAVE.match(riga)
        if m:
            corrente, grezzo = m.group(1), m.group(2).strip()
            if grezzo.startswith("[") and grezzo.endswith("]"):
                interno = grezzo[1:-1].strip()
                voci = [_pulisci(v) for v in interno.split(",")] if interno else []
                campi[corrente] = [v for v in voci if v]
            elif grezzo == "":
                campi[corrente] = []
            else:
                campi[corrente] = _pulisci(grezzo)
    return None, -1


def _percorso_link(uri: str):
    """Percorso da verificare, o None se il target è da ignorare.

    Ignora http(s), mailto: e target solo-ancora; toglie il frammento '#...';
    scarta un eventuale titolo "[t](p \"titolo\")".
    """
    uri = uri.strip()
    if uri.lower().startswith(("http://", "https://", "mailto:")) or uri.startswith("#"):
        return None
    percorso = uri.split("#", 1)[0].strip()
    parti = percorso.split()
    return parti[0] if parti else None


def _leggi(pagina: Path) -> str:
    return pagina.read_text(encoding="utf-8", errors="replace").lstrip("﻿")


def linta_wiki(wiki: Path) -> list[tuple[str, str, str]]:
    """Ritorna esiti (livello, percorso_relativo_alla_wiki, messaggio)."""
    esiti: list[tuple[str, str, str]] = []

    def aggiungi(livello: str, percorso: str, messaggio: str) -> None:
        esiti.append((livello, percorso, messaggio))

    pagine = sorted(p for p in wiki.rglob("*.md") if p.is_file())
    indice = wiki / "index.md"
    altre = [p for p in pagine if p.relative_to(wiki).as_posix() not in SENZA_INDICE]

    indicizzate: set[Path] = set()
    if indice.is_file():
        testo_indice = _leggi(indice)
        for uri in _LINK.findall(testo_indice):
            percorso = _percorso_link(uri)
            if percorso is None:
                continue
            destinazione = wiki / percorso
            if (
                destinazione.is_file()
                and destinazione.suffix == ".md"
                and destinazione.resolve().is_relative_to(wiki.resolve())
            ):
                indicizzate.add(destinazione.resolve())
            else:
                aggiungi(
                    "ERRORE",
                    "index.md",
                    f"link verso pagina wiki inesistente: {percorso}",
                )
    elif altre:
        aggiungi("ERRORE", "index.md", "mancante: impossibile verificare pagine indicizzate e orfane")

    for pagina in pagine:
        rel = pagina.relative_to(wiki).as_posix()
        testo = _leggi(pagina)

        righe = len(testo.splitlines())
        if righe > MAX_RIGHE:
            aggiungi("AVVISO", rel, f"{righe} righe (> {MAX_RIGHE})")

        parti = pagina.relative_to(wiki).parts
        tipata = len(parti) > 1 and parti[0] in TYPED_DIRS
        if tipata:
            campi, fine = parse_frontmatter(testo)
            if campi is None:
                if fine == -1:
                    aggiungi("ERRORE", rel, "frontmatter non chiuso (manca '---' di chiusura)")
                else:
                    aggiungi("ERRORE", rel, "frontmatter mancante")
            else:
                stato = campi.get("stato")
                obsoleto = isinstance(stato, str) and stato == "obsoleto"

                for chiave in ("titolo", "tipo", "stato", "aggiornato"):
                    if isinstance(campi.get(chiave), list):
                        aggiungi("ERRORE", rel, f"campo '{chiave}' deve essere un valore singolo, non una lista")
                for chiave in CAMPI_OBBLIGATORI:
                    if _pieno(campi, chiave):
                        continue
                    if chiave == "correlate" and obsoleto:
                        continue  # segnalata dal controllo dedicato
                    aggiungi("ERRORE", rel, f"campo '{chiave}' mancante o vuoto")

                tipo = campi.get("tipo")
                if isinstance(tipo, str) and tipo and tipo not in TIPI:
                    aggiungi(
                        "ERRORE",
                        rel,
                        f"valore non ammesso per 'tipo': {tipo} (ammessi: {', '.join(sorted(TIPI))})",
                    )
                if isinstance(stato, str) and stato and stato not in STATI:
                    aggiungi(
                        "ERRORE",
                        rel,
                        f"valore non ammesso per 'stato': {stato} (ammessi: {', '.join(sorted(STATI))})",
                    )

                aggiornato = campi.get("aggiornato")
                data = None
                if aggiornato:
                    if not (isinstance(aggiornato, str) and _DATA.match(aggiornato)):
                        aggiungi("ERRORE", rel, f"'aggiornato' non in formato AAAA-MM-GG: {aggiornato}")
                    else:
                        try:
                            data = datetime.strptime(aggiornato, "%Y-%m-%d").date()
                        except ValueError:
                            aggiungi("ERRORE", rel, f"'aggiornato' non è una data valida: {aggiornato}")
                if (
                    isinstance(stato, str)
                    and stato == "verificato"
                    and data is not None
                    and (date.today() - data).days > GIORNI_SCADENZA
                ):
                    aggiungi(
                        "AVVISO",
                        rel,
                        f"'verificato' ma aggiornato al {aggiornato} (più di {GIORNI_SCADENZA} giorni fa)",
                    )

                if obsoleto and not _pieno(campi, "correlate"):
                    aggiungi(
                        "ERRORE",
                        rel,
                        "stato 'obsoleto' senza voce in 'correlate' (manca il link di sostituzione)",
                    )

        if rel == "index.md":
            continue  # i link dell'index sono coperti dal controllo index/orfane
        for uri in _LINK.findall(testo):
            percorso = _percorso_link(uri)
            if percorso is None:
                continue
            if not (pagina.parent / percorso).exists():
                aggiungi("ERRORE", rel, f"link relativo rotto: {percorso}")

        if indice.is_file() and rel not in SENZA_INDICE and pagina.resolve() not in indicizzate:
            aggiungi("ERRORE", rel, "pagina orfana: assente da index.md")

    ordine = {"ERRORE": 0, "AVVISO": 1}
    esiti.sort(key=lambda e: (e[1], ordine[e[0]], e[2]))
    return esiti


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Lint della wiki (frontmatter, link, index, obsolescenza).")
    parser.add_argument(
        "--wiki-dir",
        default=None,
        help="directory della wiki (default: docs/wiki nella radice del repo)",
    )
    args = parser.parse_args(argv)
    wiki = Path(args.wiki_dir).resolve() if args.wiki_dir else WIKI_DEFAULT
    if not wiki.is_dir():
        print(f"wiki_lint: directory wiki non trovata: {wiki}", file=sys.stderr)
        return 2
    esiti = linta_wiki(wiki)
    for livello, percorso, messaggio in esiti:
        print(f"[{livello}] {percorso}: {messaggio}")
    return 1 if any(livello == "ERRORE" for livello, _, _ in esiti) else 0


if __name__ == "__main__":
    sys.exit(main())
