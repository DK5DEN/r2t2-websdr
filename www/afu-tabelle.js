/* Copy of site/js/tabelle.js from afu.tools (stand 2026-10-01): sortable heads, filter row,
   table footer. Do not edit here; take a new copy instead. Needs a global el(), see app.js. */
/* Sortier- und Filterköpfe für Datentabellen.

   Herausgelöst aus der Ausland-Tabelle, damit alle Tabellen der Seite gleich
   aussehen und sich gleich bedienen lassen. Zwei Dinge stecken darin:

   1. Sortierbare Spaltenköpfe. Ein `<th data-sort="feld">` mit Knopf und
      Pfeil, ein Klick wechselt zwischen auf, ab und aus. Der Zustand steht in
      `aria-sort`, davon lebt auch die Darstellung des Pfeils.

   2. Eine Filterzeile unter dem Kopf. Die Filterfelder werden nicht doppelt
      angelegt, sondern **verschoben**: auf breiten Schirmen wandern sie in den
      Tabellenkopf über ihre Spalte, auf schmalen zurück in den Filterkasten
      darüber. Eine zweite Ausfertigung müsste man ständig abgleichen, ein
      verschobenes Element nicht. */

/* Ab hier ist Platz für Filter im Tabellenkopf. Darunter wäre jede Spalte zu
   schmal für ein Eingabefeld.

   Die Grenze muss dieselbe sein wie die der Regel `.field.nur-schmal` im
   Stylesheet, sonst entsteht dazwischen ein Band, in dem die Felder im Kasten
   schon ausgeblendet und im Kopf noch nicht angekommen sind. Genau das war
   zwischen 48 und 64 rem der Fall: dort gab es überhaupt keine Filter. */
const TABELLE_BREIT = window.matchMedia("(min-width: 48rem)");

/** Felder zwischen Filterkasten und Tabellenkopf hin- und herschieben.
 *
 * `felder` ist {feldname: element}. Im Kopf muss es je Feld ein
 * `<th data-filter="feldname">` geben, im Filterkasten ein
 * `#heim-<feldname>`. Fehlt eines von beidem, bleibt das Feld, wo es ist.
 */
function filterPlatz(tabelle, felder) {
  for (const [name, feld] of Object.entries(felder)) {
    if (!feld) continue;
    const ziel = TABELLE_BREIT.matches
      ? tabelle.querySelector(`.filterzeile [data-filter="${name}"]`)
      : document.getElementById(`heim-${name}`);
    if (ziel && feld.parentElement !== ziel) ziel.appendChild(feld);
  }
}

/** Filterfelder einhängen und bei Größenwechsel mitziehen.
 *
 * Wird eine Tabelle neu gezeichnet, bleibt der Hörer der alten stehen. Ohne
 * die Prüfung auf `isConnected` schob er ihre Felder beim nächsten
 * Größenwechsel in den Filterkasten der neuen Tabelle (gleiche Kennung), und
 * dort standen sie dann doppelt und dreifach. */
function filterKopf(tabelle, felder) {
  filterPlatz(tabelle, felder);
  const mitziehen = () => {
    if (!tabelle.isConnected) { TABELLE_BREIT.removeEventListener("change", mitziehen); return; }
    filterPlatz(tabelle, felder);
  };
  TABELLE_BREIT.addEventListener("change", mitziehen);
}

/** Sortierbare Spaltenköpfe.
 *
 * `beiWechsel(feld, richtung)` wird bei jedem Klick gerufen; `richtung` ist
 * "ascending", "descending" oder null. Gibt einen Zugriff auf den Zustand
 * zurück, damit die Seite ihn merken und wiederherstellen kann.
 */
function sortKopf(tabelle, beiWechsel, anfang = {}) {
  let feld = anfang.feld || null;
  let richtung = anfang.richtung || null;

  function zeichne() {
    for (const th of tabelle.querySelectorAll("thead th[data-sort]")) {
      if (th.dataset.sort === feld && richtung) th.setAttribute("aria-sort", richtung);
      else th.removeAttribute("aria-sort");
    }
  }

  for (const th of tabelle.querySelectorAll("thead th[data-sort]")) {
    const knopf = th.querySelector("button");
    if (!knopf) continue;
    knopf.addEventListener("click", () => {
      // Dreimal klicken führt zurück zur ursprünglichen Reihenfolge. Ohne das
      // kommt man nie wieder dahin zurück.
      if (feld !== th.dataset.sort) { feld = th.dataset.sort; richtung = "ascending"; }
      else if (richtung === "ascending") richtung = "descending";
      else if (richtung === "descending") { feld = null; richtung = null; }
      else richtung = "ascending";
      zeichne();
      beiWechsel(feld, richtung);
    });
  }
  zeichne();
  return {
    feld: () => feld,
    richtung: () => richtung,
    setze(neuesFeld, neueRichtung) {
      feld = neuesFeld || null;
      richtung = neueRichtung || null;
      zeichne();
    },
  };
}

/** Zeilen nach einem Feld sortieren. `wert(zeile, feld)` liefert den Wert.
 *
 * Zahlen werden als Zahlen verglichen, alles andere als Text mit deutscher
 * Sortierreihenfolge. Leere Werte stehen immer hinten, egal in welcher
 * Richtung: eine leere Zelle ist keine kleine, sondern eine fehlende.
 */
function sortiereZeilen(zeilen, feld, richtung, wert) {
  if (!feld || !richtung) return zeilen;
  const vz = richtung === "descending" ? -1 : 1;
  const samm = new Intl.Collator("de", { numeric: true, sensitivity: "base" });
  return [...zeilen].sort((a, b) => {
    const x = wert(a, feld);
    const y = wert(b, feld);
    const xLeer = x === null || x === undefined || x === "";
    const yLeer = y === null || y === undefined || y === "";
    if (xLeer && yLeer) return 0;
    if (xLeer) return 1;
    if (yLeer) return -1;
    if (typeof x === "number" && typeof y === "number") return (x - y) * vz;
    return samm.compare(String(x), String(y)) * vz;
  });
}

/* Tabellenfuß: Anzahl, Blättern, Einträge je Seite.

   Aufbau wie in der Ausland-Tabelle. Der Behälter braucht drei Kinder:
   `.fuss-info` für die Zahlen, `.seiten-nav` für die Knöpfe und ein
   `select.fuss-anzahl` für die Seitengröße.

   Die Seitengröße bleibt im Browser, nicht in der Adresse: sie ist eine
   Gewohnheit des Lesers, kein Teil dessen, was er gerade ansieht. Ein
   geteilter Link soll beim Empfänger mit dessen Einstellung aufgehen. */
function seitenFuss(behaelter, { schluessel, beiWechsel, obenAnkern }) {
  const info = behaelter.querySelector(".fuss-info");
  const nav = behaelter.querySelector(".seiten-nav");
  const menge = behaelter.querySelector("select");
  let seite = 1;

  try {
    const gemerkt = localStorage.getItem(schluessel);
    if (gemerkt && [...menge.options].some((o) => o.value === gemerkt)) menge.value = gemerkt;
  } catch (err) { /* privater Modus */ }

  menge.addEventListener("change", () => {
    try { localStorage.setItem(schluessel, menge.value); } catch (err) { /* egal */ }
    seite = 1;
    beiWechsel();
  });

  function groesse() {
    return menge.value === "alle" ? Infinity : Number(menge.value);
  }

  function springe(ziel) {
    seite = ziel;
    beiWechsel();
    if (obenAnkern) obenAnkern.scrollIntoView({ block: "start", behavior: "smooth" });
  }

  function knopf(nr, text, seiten, titel) {
    const aus = nr < 1 || nr > seiten;
    const b = el("button", {
      type: "button", title: titel || `Seite ${nr}`,
      ...(nr === seite ? { "aria-current": "page" } : {}),
      ...(aus ? { disabled: "disabled" } : {}),
    }, text);
    if (!aus) b.addEventListener("click", () => springe(nr));
    return b;
  }

  function zeichneNav(seiten) {
    if (seiten <= 1) { nav.replaceChildren(); return; }
    // Immer erste und letzte Seite zeigen, dazu die Nachbarn der aktuellen.
    // Dazwischen Auslassungspunkte, sonst wird die Leiste bei vielen Seiten
    // breiter als die Tabelle.
    const nummern = [...new Set([1, seiten, seite, seite - 1, seite + 1])]
      .filter((n) => n >= 1 && n <= seiten).sort((a, b) => a - b);
    const teile = [knopf(seite - 1, "‹", seiten, "eine Seite zurück")];
    nummern.forEach((n, i) => {
      if (i && n - nummern[i - 1] > 1) teile.push(el("span", { class: "luecke" }, "…"));
      teile.push(knopf(n, String(n), seiten));
    });
    teile.push(knopf(seite + 1, "›", seiten, "eine Seite weiter"));
    nav.replaceChildren(...teile);
  }

  return {
    /** Den Ausschnitt für die aktuelle Seite; setzt Fußzeile und Knöpfe. */
    ausschnitt(zeilen) {
      const g = groesse();
      const seiten = g === Infinity ? 1 : Math.max(1, Math.ceil(zeilen.length / g));
      if (seite > seiten) seite = seiten;
      const von = g === Infinity ? 0 : (seite - 1) * g;
      const teil = g === Infinity ? zeilen : zeilen.slice(von, von + g);
      info.textContent = !zeilen.length ? ""
        : (g === Infinity || zeilen.length <= g
           ? `${zeilen.length} ${zeilen.length === 1 ? "Eintrag" : "Einträge"}`
           : `${von + 1} bis ${von + teil.length} von ${zeilen.length}`);
      zeichneNav(seiten);
      return teil;
    },
    /** Nach einer neuen Abfrage wieder auf Seite eins. */
    zuruecksetzen() { seite = 1; },
    /** Aktuelle Seite, und eine gemerkte wieder aufschlagen (ausschnitt() kappt sie). */
    seite: () => seite,
    geheZu(nr) { seite = Math.max(1, nr || 1); },
  };
}
