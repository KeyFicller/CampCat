# Bundled UI fonts

The faces the app ships with, committed so the build never touches the network.
They were prepared once, from the sources below; provenance and licence follow.

## Sources

All from [google/fonts](https://github.com/google/fonts) at commit
`bd8f81ddb5c74d5c8897b36ad88b440266245103`, pulled through jsDelivr.

| output | upstream | wght | subset |
|---|---|---|---|
| `Arimo-Regular.ttf` | `ofl/arimo/Arimo[wght].ttf` | 400 | no |
| `Arimo-Bold.ttf` | same | 700 | no |
| `JetBrainsMono-Regular.ttf` | `ofl/jetbrainsmono/JetBrainsMono[wght].ttf` | 400 | no |
| `NotoSansSC-Common.ttf` | `ofl/notosanssc/NotoSansSC[wght].ttf` | 400 | GB2312 1+2, 6763 hanzi |

Variable fonts are instantiated because ImGui ignores variation axes: shipped
as-is, every weight would draw as Regular. The CJK face is additionally subset:
instancing alone still leaves 10.1 MB, while the subset is 2.1 MB.

The 20px title reuses `Arimo-Bold.ttf` and merges no CJK, as it always has.

## Checksums

| file | bytes | sha256 |
|---|---|---|
| `Arimo-Bold.ttf` | 322004 | `6ee331e4367aaa9799e64bbdc79ab4710c2e060b2136f3033125b78060f7d42b` |
| `Arimo-Regular.ttf` | 321444 | `b35e6b7e217258f4c27eace103f3987c6fdb0cb1af08c3d43b68984e27f23fcd` |
| `JetBrainsMono-Regular.ttf` | 115172 | `15e717083a6237478a91f8c7083a030d170d3e5d91718616f82f94806194e75f` |
| `NotoSansSC-Common.ttf` | 2165900 | `a110a49d9e697ce85f2af8346aef1bd7519b66a30b64c542b79d8f3471f31bec` |

## Licence

All three are SIL Open Font License 1.1; full texts in `OFL/`.

- Arimo -- Copyright 2026 The Arimo Project Authors (https://github.com/googlefonts/arimo)
- JetBrainsMono -- Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono)
- NotoSansSC -- Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'

OFL-1.1 permits use, modification, subsetting and redistribution inside an
application. It requires the notice and licence to travel with the binaries
(hence `OFL/`), and that modified copies not carry a Reserved Font Name -- the
outputs here are renamed, and none contains `Source`.

The proprietary faces this replaced (Arial, Menlo, Hiragino Sans GB, PingFang,
Arial Unicode) are not redistributable and are deliberately not in this tree.
