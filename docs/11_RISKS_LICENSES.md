# 11 — Rischi, sicurezza, licenze, rollback

## Rischi tecnici prioritari
- **Compatibilità simultanea ASIO:** multi-client nel README OBS-ASIO = più sorgenti OBS; non significa apertura condivisa garantita per ogni nuovo host/REAPER.
- **Modalità di apertura:** un driver ASIO può richiedere una combinazione input/output, maschere o buffer size specifiche. Non rischiare di scrivere in MON L/R durante test.
- **ASIO callback mapping:** maschere sparse con buffer compattati; documentare indices. UI 1-based (driver) vs buffer/config 0-based: conversione esplicita e testata (selftest).
- **Clock drift:** UAD e WASAPI possono avere clock distinti; FIFO senza adattamento causa xruns nel lungo periodo.
- **Feedback:** MIC/AUX/MON/return possono già contenere componenti comuni; preset default conservativo.
- **Compatibilità piattaforme:** browser, Source-Connect e altre app scelgono endpoint in maniera propria; test reali necessari.
- **Driver F4:** BSoD, firma, Windows Update, test mode, rollback.

## Proprietà intellettuale / licenze
- **OBS-ASIO**: codice distribuito come GPL-3.0, incluse obbligazioni in caso di derivazione/distribuzione; consultare licenza integrale. Non incollare codici terzi nel nuovo repo senza decisione sulla licenza. Studio 2026-10-27 (solo comportamento, nessun codice copiato): un callback per device ASIO, ring per client, selezione arbitraria dei canali; multi-device/multi-client; nessun SRC interno (OBS ricampiona le sorgenti). NL Matrix ha già converter adattivi per device, quindi non serve derivare OBS-ASIO.
- **atkAudio PluginForObsRelease**: distribuito AGPL-3.0 (verificata su GitHub API); offerta PluginHost2/DeviceIo2 con routing ASIO multi-device, conversione di sample rate e compensazione drift. È un riferimento comportamentale per funzionalità e strategia di buffering; nessun codice AGPL copiato o linkato nel prodotto. L'AGPL è incompatibile con una distribuzione closed-source senza decisione di licenza.
- **JUCE**: verificare licenza ufficiale applicabile alla versione e all'uso del progetto; il fork OBS-ASIO è diverso dall'upstream attuale.
- **Steinberg ASIO**: il modulo JUCE espone `JUCE_ASIO`; vedere i file di licenza ASIO del codice JUCE corrente: licenza GPLv3 alternativa a percorso Steinberg proprietario con condizioni/accordi. Decisione legale prima di distribuzione.
- **SysVAD**: consultare licenza repository Microsoft e compatibilità del codice derivato; firma e packaging Win11 non risolti dal sample.
- **VB-CABLE**: dipendenza esterna con licenza VB-Audio separata; non redistribuire installer dentro lo ZIP senza diritti.
- **UAD**: mantenere driver originale, non modificare o distribuire componenti proprietari.

## Sicurezza
- Evitare elevate privileges; niente kernel in F1–F3.
- Non fare automatic system device default set, ASIO registry rewrites, Windows test signing.
- Non raccogliere dati personali/clienti nei log; anonimizzare percorsi e titoli sessioni.
- Mute/panic e prevenzione route loop sempre accessibili.

## Piano rollback
1. Fermare `NL Audio Matrix` e assicurarsi che rilasci ASIO e WASAPI.
2. Riavviare OBS con sorgenti OBS-ASIO attuali.
3. Se necessario reimpostare Source-Connect input su CABLE Output e output su Apollo (impostazioni già operative).
4. Nessun driver o endpoint Windows nuovo da disinstallare in F1–F3.
5. Se F4 sarà sviluppata, scrivere **prima** una procedura di uninstall firmata e un test su macchina non produttiva.

## Nota di accuratezza
Queste note non sostituiscono una verifica legale del progetto eventualmente distribuito a terzi.
