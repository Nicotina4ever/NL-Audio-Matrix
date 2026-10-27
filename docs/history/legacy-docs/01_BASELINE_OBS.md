# 01 — Baseline: il setup funzionante è la specifica

## Schema attestato dall'utente
```text
MIC FISICO 1 ─┬─> UAD Console ─> cuffia locale (monitor diretto, NON cambiare)
              │
              ├─> REAPER (input 1) ─> routing UAD/Console ESISTENTE ─> AUX 1/2
              │                                                  │
              └─> OBS-ASIO Mic 1 -------------------------------┐ │
                                                               v v
                                                OBS-ASIO AUX 1/2 + Mic 1
                                                               |
                                                Monitoraggio OBS verso
                                                       CABLE Input
                                                               |
                                                VB-CABLE => CABLE Output
                                                               |
                                               Source-Connect input
                                                               |
                                             Cliente / sistema remoto

RITORNO dal client remoto ---> Apollo Solo ---> UAD Console / cuffie
```

**ATTENZIONE:** lo schema non significa che MIC 1 e AUX debbano essere sempre sommati; sono due **sorgenti indipendenti**. Il loro contenuto reale e l'intenzionalità della somma vanno verificati, per evitare duplicazione della voce/comb filtering.

## Cosa l'utente NON vuole
- Installare una VST da inserire ogni volta sulle tracce, bus o sul Master di REAPER.
- Monitorare la propria voce passando da REAPER/WASAPI/software per sentire il cliente.
- Usare OBS come ponte permanente.
- Vedersi proporre di nuovo VBMatrix o Voicemeeter come soluzione certa: già provati, non soddisfacenti con questa Apollo.
- Modificare la Console o il routing REAPER già funzionanti.

## Perché OBS-ASIO conta
- È l'unico setup che **sul computer reale** accede ai canali ASIO Apollo selezionabili uno a uno e li invia a VB-CABLE.
- La versione **v3.2.1f** è un benchmark empirico; il plugin distribuisce sorgenti audio OBS, non un driver Windows generico.
- La funzione di monitoraggio OBS produce un endpoint WASAPI di playback che VB-CABLE riceve; non è un driver UAD aggiuntivo.
- L'app standalone deve realizzare **capture + bus + render WASAPI**, non semplicemente caricare il file plugin OBS fuori da OBS (è dipendente da libobs/OBS SDK).

## Baseline da documentare durante test
1. Schermate/mapping UAD Console Mic/Virtual/AUX (oscurare informazioni private).
2. OBS-ASIO sorgenti attive: channel indices/names, formato mono/stereo.
3. Impostazioni OBS: Monitor only vs Monitor and Output, output Monitoring Device esatto, sample rate, buffer.
4. Setup REAPER input/output di un progetto di prova e solo routing tecnico indispensabile.
5. Source-Connect input/output della sessione test; registrare un loopback controllato, mai registrare terzi senza consenso.
6. Risultati prima/dopo per evitare regressioni.

## Vincolo di latenza
Monitor in cuffia **invariato** (UAD Console). Il bridge aggiunge latenza sulla **mandata remota**, misurabile e da ottimizzare, non sul monitor locale. Non promettere zero-latency end-to-end.
