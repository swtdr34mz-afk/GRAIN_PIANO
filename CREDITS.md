# Credits

GRAIN PIANO by swtdr34mz, under GPL-3.0-only.

| Part | Author | Licence | Used for |
| --- | --- | --- | --- |
| Salamander Grand Piano V3 (https://archive.org/details/SalamanderGrandPianoV3, as remapped in https://github.com/sfzinstruments/SalamanderGrandPiano) | Alexander Holm | CC-BY 3.0 | `res/piano.bin`: nine notes, mono, 22,050 Hz, levelled, shortened with a crossfaded loop, 4-bit ADPCM (`tools/make_piano.py`, from CHORD RUN) |
| [fm1-base](https://github.com/charlesvestal/fm1-base) | Charles Vestal | MPL-2.0 | the platform: screen, panel, audio, MIDI, storage, simulator, build tools |
| [felucca-bsp](https://github.com/hugelton/felucca-bsp) | Leo Kuroshita (Hügelton Instruments) and contributors | MPL-2.0 | the hardware layer, update loader, packaging |
| [Tonitor](https://github.com/nzimas/tonitor) | Nuno Zimas | GPL-3.0-only | the idea and the page layout (FX, SEL, ENV, LFO, EDIT); no code copied |
| [CHORD RUN](https://github.com/swtdr34mz-afk/CHORD_RUN) | swtdr34mz (after Blop / OSR-1 by Obsimo) | GPL-3.0-only | the piano sampler's ADPCM reader, the build workflow |
| [UnifrakturMaguntia](https://github.com/google/fonts/tree/main/ofl/unifrakturmaguntia) | j. 'mach' wust (after Peter Wiegel) | SIL OFL 1.1 | the splash's title (`assets/fonts/`, rasterised by `tools/gen_title.py`) |
| JieLi AC79 SDK | JieLi Technology | Apache-2.0 | three files in each `.fwsc` package (fetched at build time) |
